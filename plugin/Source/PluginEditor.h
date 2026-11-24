#pragma once

#include "../../ui/PitchEditor.h"
#include "PluginProcessor.h"
#include "glina/TrackView.h"

namespace glina::plugin {

// Adapts the audio modification behind the selected ARA playback region to the
// shared editor's ClipHost interface.
class PluginClip final : public ui::ClipHost,
                         private juce::ChangeListener,
                         private juce::ARAPlaybackRegion::Listener,
                         private juce::ARAAudioModification::Listener {
public:
    PluginClip(GlinaDocumentController* dc, GlinaProcessor& proc);
    ~PluginClip() override;

    void setRegion(juce::ARAPlaybackRegion* region);
    juce::ARAPlaybackRegion* region() const { return region_; }
    GlinaAudioModification* modification() const { return mod_; }
    bool valid() const { return mod_ != nullptr; }
    // Keeps the edited track after the host replaced its item or switched its take.
    void followTrack();
    // The ARA track (with Glina items) that is this REAPER track: matched by where its items start,
    // then by name and number. Null if none matches.
    juce::ARARegionSequence* sequenceOfTrack(const reaper::TrackInfo& t) const;
    // Track view: every Glina item of the edited track shown and edited as one clip (when the
    // track has two or more). Rebuilt when items, takes, positions or other editors' edits change.
    void refreshView(bool force = false);
    bool trackView() const { return viewOn_; }
    juce::ARARegionSequence* sequence() const { return seq_; }

    const Analysis* analysis() const override;
    Document& document() override;
    ui::DocumentHistory& history() override;
    void recordEdit(const Document& before) override;
    bool canUndo() override;
    bool canRedo() override;
    bool undoStep(bool redo) override;
    juce::String noticeText() const override;
    bool summary(ClipHost::Summary& out) const override;
    void documentChanged(bool layoutChanged) override;
    juce::String busyText() const override;
    bool isRendering() const override;
    double playheadSec() const override;
    bool isPlaying() const override { return proc_.playing.load(); }
    bool canControlTransport() const override;
    void setPlaying(bool) override;
    void setPlayheadSec(double) override;
    bool isBypassed() const override { return mod_ != nullptr && mod_->bypass.load(); }
    void setBypassed(bool b) override;
    // REAPER ignores ARA start/stop requests: space & Home go back to REAPER instead.
    bool handlesTransportKeys() const override { return false; }
    double toProjectSec(double clipSec) const override { return region_ != nullptr ? toSong(clipSec) : clipSec; }
    double fromProjectSec(double songSec) const override { return region_ != nullptr ? fromSong(songSec) : songSec; }
    void log(const juce::String& line) const override;
    std::vector<GridLine> hostGrid(double t0, double t1) const override;
    bool hasHostTempo() const override;
    std::vector<TrackRef> tracks() const override;
    bool ghost(const juce::String& id, Ghost& out) const override;
    bool thread(const juce::String& id, std::vector<ThreadFrame>& out) const override;
    void setEditedTrack(const juce::String& id) override;
    bool hasRenderOptions() const override;
    bool savesRender() const override;
    void setSavesRender(bool on) override;
    juce::String renderOrigin() const override;
    bool canRerender() const override;
    void rerender() override;

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override
    {
        refreshView();
        changes.sendChangeMessage();
    }
    void willDestroyPlaybackRegion(juce::ARAPlaybackRegion*) override { setRegion(nullptr); }
    void willDestroyAudioModification(juce::ARAAudioModification*) override { setRegion(nullptr); }
    ARA::PlugIn::HostPlaybackController* playback() const;
    juce::ARAMusicalContext* musicalContext() const;
    double toSong(double clipSec) const;
    double fromSong(double songSec) const;
    juce::ARADocument* arDoc() const;
    // Every track (region sequence) in the ARA document that has Glina items.
    std::vector<juce::ARARegionSequence*> glinaSequences() const;
    juce::ARAPlaybackRegion* itemAt(const std::vector<juce::ARAPlaybackRegion*>& items) const;
    bool ghostOfItem(juce::ARAPlaybackRegion* r, Ghost& out) const;
    static juce::String idOf(const void* p) { return juce::String::toHexString((juce::pointer_sized_int)p); }

    GlinaDocumentController* dc_;
    GlinaProcessor& proc_;
    // A locator set by a click: the host ignores that while stopped, so keep showing it
    // until the host's own cursor moves.
    mutable bool locatorSet_ = false;
    mutable double locatorSec_ = -1;
    mutable double locatorHostSong_ = 0;
    juce::ARAPlaybackRegion* region_ = nullptr;
    GlinaAudioModification* mod_ = nullptr;
    juce::ARARegionSequence* seq_ = nullptr; // the edited track; kept while its item changes
    juce::ARAAudioModification* lastMod_ = nullptr; // the take we last edited; preferred when the host recreates its region
    Document dummyDoc_;
    ui::DocumentHistory dummyHistory_;

    // ---- track view ----
    struct ViewItem {
        juce::ARAPlaybackRegion* region = nullptr;
        GlinaAudioModification* mod = nullptr;
        const Analysis* eff = nullptr;
        uint64_t version = 0;
        double playStart = 0, modStart = 0, modEnd = 0;
        bool operator==(const ViewItem& o) const
        {
            return region == o.region && mod == o.mod && eff == o.eff && version == o.version &&
                   playStart == o.playStart && modStart == o.modStart && modEnd == o.modEnd;
        }
    };
    std::vector<ViewItem> viewItemsNow() const; // empty: no view (fewer than two items, or they don't fit)
    bool viewOn_ = false;
    bool viewPending_ = false;           // the track has a view, but an item is still being analysed
    TrackView view_;
    std::vector<TrackPiece> viewPieces_; // the items as the view was built from them
    std::vector<ViewItem> viewItems_;
    int viewPrimary_ = -1;
    juce::ARARegionSequence* viewSeq_ = nullptr;
    GlinaDocumentController::TrackHistory* viewHist_ = nullptr; // the track's shared undo history
    GlinaDocumentController::TrackStep openStep_; // takes changed by the edit in progress (closed by recordEdit)
    juce::String notice_;                         // shown in the status line for a few seconds
    juce::uint32 noticeMs_ = 0;
    double viewStart_ = 0, viewEnd_ = 0;  // project time of the view's start and end
    juce::String viewRefused_;            // why the last track could not be shown as a view (logged once)
};

class GlinaEditor final : public juce::AudioProcessorEditor,
                          public juce::AudioProcessorEditorARAExtension,
                          private juce::ARAEditorView::Listener,
                          private juce::Timer {
public:
    explicit GlinaEditor(GlinaProcessor& p);
    ~GlinaEditor() override;
    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void onNewSelection(const juce::ARAViewSelection&) override;
    void timerCallback() override;
    void pickFallbackRegion();
    bool pinOwnRegion();

    GlinaProcessor& proc_;
    GlinaDocumentController* dc_ = nullptr;
    std::unique_ptr<PluginClip> clip_;
    ui::PitchEditor editor_;
    bool ownPinned_ = false; // bound to the item this instance sits on (Take FX)
    bool centreOnOpen_ = false; // view still to be centred on the cursor once it can be
    double openSec_ = 0;        // the cursor (project seconds) the editor opened at
    juce::uint32 openedMs_ = 0; // when the editor opened (stale host selection is ignored shortly after)
};

} // namespace glina::plugin
