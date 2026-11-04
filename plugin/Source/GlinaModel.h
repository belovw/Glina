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
#include "glina/Analysis.h"
#include "glina/Document.h"
#include "glina/Renderer.h"

#include <atomic>
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
    void setCache(std::shared_ptr<const RenderCache> c);

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

    // Broadcast (async) when analysis or a render finishes, so editors repaint.
    juce::ChangeBroadcaster modelChanged;

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
    void startAnalysisIfPossible(GlinaAudioSource* src);
    void analysisFinished(GlinaAudioSource* src);
    void rebuildEffective(GlinaAudioModification* mod);
    void startRender(GlinaAudioModification* mod);

    juce::ThreadPool pool { juce::ThreadPoolOptions{}.withNumberOfThreads(2).withThreadName("Glina worker") };
    juce::ReadWriteLock processLock;
};

} // namespace glina::plugin
