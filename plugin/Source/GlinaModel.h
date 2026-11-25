// ARA model: audio sources carry the (shared, immutable) analysis, audio
// modifications carry the user's document and a pre-rendered stereo result.
//
// Threading rules (the core of "no glitches in the DAW"):
//   * analysis and rendering run on a background ThreadPool, never on the audio thread;
//   * the audio thread only copies samples out of an immutable RenderCache, taking a
//     try-lock (never blocks) and never freeing memory;
//   * documents live on the message thread; render jobs work on a copy.
#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include "../../ui/ClipHost.h"
#include "ReaperHost.h"
#include "glina/Analysis.h"
#include "glina/Document.h"
#include "glina/Renderer.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>

namespace glina::plugin {

// Shared between an audio source and its background jobs (kept alive by shared_ptr).
struct SourceData {
    std::atomic<bool> alive { true };
    std::mutex readMutex;                       // held by the analysis job while it reads a block
    std::atomic<bool> readFinished { false };   // the job no longer needs the reader
    std::atomic<bool> samplesReady { false };   // `mono` valid (dry playback possible)
    std::atomic<bool> analysisReady { false };  // `analysis` valid
    double sampleRate = 0;
    uint64_t contentHash = 0;                   // of `mono`; set before samplesReady
    std::vector<float> mono;                    // immutable once samplesReady
    Analysis analysis;                          // immutable once analysisReady (automatic note layout)
};

class GlinaAudioSource final : public juce::ARAAudioSource {
public:
    using juce::ARAAudioSource::ARAAudioSource;
    ~GlinaAudioSource() override
    {
        data->alive = false;
        { const std::lock_guard<std::mutex> lk(data->readMutex); } // wait for a block read in progress
        reader.reset();
    }

    std::shared_ptr<SourceData> data = std::make_shared<SourceData>();
    std::atomic<bool> analysisStarted { false };
    // ARA requires host audio readers to be created and destroyed on the document
    // controller's thread, so the source owns it and only ARA callbacks touch it.
    std::unique_ptr<juce::ARAAudioSourceReader> reader;
    void releaseReaderIfDone()
    {
        if (reader != nullptr && data->readFinished) reader.reset();
    }
};

struct RenderCache {
    double sampleRate = 0;
    std::vector<float> left, right;             // edited lead + harmonies
    TimeMap map;                                // for A/B: output time -> original time
    uint64_t key = 0;                           // source samples + document it was made from
    int version = kRenderVersion;               // engine version that made it
    bool fromDisk = false;                      // loaded from <project>/Glina instead of rendered
};

class GlinaAudioModification final : public juce::ARAAudioModification {
public:
    GlinaAudioModification(juce::ARAAudioSource* source, ARA::ARAAudioModificationHostRef ref,
                           const GlinaAudioModification* clone);
    ~GlinaAudioModification() override { alive->store(false); }

    GlinaAudioSource* getGlinaSource() const { return static_cast<GlinaAudioSource*>(getAudioSource()); }

    // ---- message thread ------------------------------------------------------
    Document doc;
    ui::DocumentHistory history;
    std::shared_ptr<const Analysis> effective;  // source analysis + the user's note layout
    uint64_t editVersion = 0;      // bumped on every change
    uint64_t renderedVersion = 0;  // version currently in the cache
    bool renderRunning = false;
    bool forceRender = false;      // next render: ignore renders saved in the project folder
    bool restoredFromArchive = false; // next render is the first after loading the project
    bool renderFromDisk = false;   // the cache came from the project folder...
    int renderEngineVersion = kRenderVersion; // ...made by this engine version
    juce::String renderError;      // the last render failed (why); empty once one succeeds
    void setCache(std::shared_ptr<const RenderCache> c);
    std::shared_ptr<const RenderCache> currentCache() const; // message thread
    juce::String sourceName() const;

    // ---- audio thread (non-blocking) -------------------------------------------
    bool tryGetCache(std::shared_ptr<const RenderCache>& out) const;
    std::atomic<bool> bypass { false };         // A/B: play the original

    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);

private:
    mutable juce::SpinLock cacheLock;
    std::shared_ptr<const RenderCache> cache;
    std::vector<std::shared_ptr<const RenderCache>> retired; // freed on the message thread only
};

class GlinaDocumentController final : public juce::ARADocumentControllerSpecialisation {
public:
    using juce::ARADocumentControllerSpecialisation::ARADocumentControllerSpecialisation;
    ~GlinaDocumentController() override;

    // Called by the editor after it changed `mod->doc` (message thread).
    void documentChanged(GlinaAudioModification* mod, bool layoutChanged);

    // ---- renders kept in <project folder>/Glina (REAPER only) ------------------------
    // Plug-in instances bound to a document controller register their REAPER link, so the
    // controller knows which project it belongs to. Any thread; links unregister themselves.
    static void registerHostLink(GlinaDocumentController* dc, reaper::HostLink* link);
    static void unregisterHostLink(reaper::HostLink* link);
    bool canSaveRenders() const { return reaper::available(); }
    // Render the clip again with the current engine (drops a render from an older version).
    void rerender(GlinaAudioModification* mod);

    // Broadcast (async) when analysis or a render finishes, so editors repaint.
    juce::ChangeBroadcaster modelChanged;

    // Undo history of a track shown as a whole (track view). One step per edit, however many
    // takes it touched: per take, the document as it was before. It lives here, not in the editor
    // window, so it survives closing and reopening the window and is shared by every window on
    // the track; tied to the takes themselves, it survives moving items too. Message thread.
    struct TrackStepPart {
        GlinaAudioModification* mod = nullptr;
        std::shared_ptr<std::atomic<bool>> alive; // false once the take is gone: the part is skipped
        Document before;
    };
    using TrackStep = std::vector<TrackStepPart>;
    struct TrackHistory {
        std::vector<TrackStep> undo, redo;
        void push(TrackStep s)
        {
            undo.push_back(std::move(s));
            if (undo.size() > 300) undo.erase(undo.begin());
            redo.clear();
        }
    };
    TrackHistory& trackHistory(const void* sequence) { return trackHistories[sequence]; }

    juce::ScopedTryReadLock tryProcessingLock() { return juce::ScopedTryReadLock(processLock); }

    // ARA listener callbacks (the specialisation receives them automatically).
    void willBeginEditing(juce::ARADocument*) override { processLock.enterWrite(); }
    void didEndEditing(juce::ARADocument*) override;
    void didEnableAudioSourceSamplesAccess(juce::ARAAudioSource*, bool enable) override;

protected:
    juce::ARAAudioSource* doCreateAudioSource(juce::ARADocument*, ARA::ARAAudioSourceHostRef) noexcept override;
    juce::ARAAudioModification* doCreateAudioModification(juce::ARAAudioSource*, ARA::ARAAudioModificationHostRef,
                                                          const juce::ARAAudioModification*) noexcept override;
    juce::ARAPlaybackRenderer* doCreatePlaybackRenderer() noexcept override;
    bool doRestoreObjectsFromStream(juce::ARAInputStream&, const juce::ARARestoreObjectsFilter*) noexcept override;
    bool doStoreObjectsToStream(juce::ARAOutputStream&, const juce::ARAStoreObjectsFilter*) noexcept override;

private:
    std::map<const void*, TrackHistory> trackHistories; // by region sequence; entries never erased (pointers stay valid)
    void startAnalysisIfPossible(GlinaAudioSource* src);
    void analysisFinished(GlinaAudioSource* src);
    void rebuildEffective(GlinaAudioModification* mod);
    void startRender(GlinaAudioModification* mod);

    // REAPER project of this document (via a bound plug-in instance); may be null.
    void* reaperProject() const;
    // Folders that may hold saved renders: this project's first, then every open project's.
    std::vector<juce::File> renderFolders() const;
    // After a full store: wait until REAPER has really written the project file
    // (stores also happen without a save), then write the renders next to it.
    void watchSave(const std::vector<const GlinaAudioModification*>& mods);
    struct PendingSave;
    class SaveWatcher;
    std::shared_ptr<SaveWatcher> saveWatcher; // shared_ptr: SaveWatcher is complete only in the .cpp

    juce::ThreadPool pool { juce::ThreadPoolOptions{}.withNumberOfThreads(2).withThreadName("Glina worker") };
    juce::ReadWriteLock processLock;
};

} // namespace glina::plugin
