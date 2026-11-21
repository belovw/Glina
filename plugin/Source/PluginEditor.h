#pragma once

#include "../../ui/PitchEditor.h"
#include "PluginProcessor.h"

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

    const Analysis* analysis() const override;
    Document& document() override { return mod_ != nullptr ? mod_->doc : dummyDoc_; }
    ui::DocumentHistory& history() override { return mod_ != nullptr ? mod_->history : dummyHistory_; }
    void documentChanged(bool layoutChanged) override;
    juce::String busyText() const override;
    bool isRendering() const override;
    double playheadSec() const override;
    bool isPlaying() const override { return proc_.playing.load(); }
    bool canControlTransport() const override;
    void setPlaying(bool) override;
    void setPlayheadSec(double) override;
    bool isBypassed() const override { return mod_ != nullptr && mod_->bypass.load(); }
    void setBypassed(bool b) override { if (mod_ != nullptr) mod_->bypass.store(b); }
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
    void changeListenerCallback(juce::ChangeBroadcaster*) override { changes.sendChangeMessage(); }
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

    GlinaProcessor& proc_;
    GlinaDocumentController* dc_ = nullptr;
    std::unique_ptr<PluginClip> clip_;
    ui::PitchEditor editor_;
};

} // namespace glina::plugin
