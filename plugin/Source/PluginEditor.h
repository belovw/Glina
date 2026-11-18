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
    bool valid() const { return mod_ != nullptr; }

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
    std::vector<GridLine> hostGrid(double t0, double t1) const override;
    bool hasHostTempo() const override;

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override { changes.sendChangeMessage(); }
    void willDestroyPlaybackRegion(juce::ARAPlaybackRegion*) override { setRegion(nullptr); }
    void willDestroyAudioModification(juce::ARAAudioModification*) override { setRegion(nullptr); }
    ARA::PlugIn::HostPlaybackController* playback() const;
    juce::ARAMusicalContext* musicalContext() const;
    double toSong(double clipSec) const;
    double fromSong(double songSec) const;

    GlinaDocumentController* dc_;
    GlinaProcessor& proc_;
    juce::ARAPlaybackRegion* region_ = nullptr;
    GlinaAudioModification* mod_ = nullptr;
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
