// Standalone session: one audio file, its analysis, the user's document,
// background rendering and playback. Implements the editor's ClipHost.
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "../ui/ClipHost.h"
#include "glina/Renderer.h"

#include <atomic>
#include <memory>

namespace glina::app {

class Session final : public ui::ClipHost, public juce::AudioSource {
public:
    Session();
    ~Session() override;

    // ---- files (message thread) -------------------------------------------------
    bool openAudio(const juce::File& file, juce::String& error);
    bool openProject(const juce::File& file, juce::String& error);
    bool saveProject(const juce::File& file, juce::String& error);
    bool exportMix(const juce::File& file, juce::String& error) const;      // stereo WAV, 24-bit
    bool exportStems(const juce::File& folder, juce::String& error) const;  // lead + each harmony voice, mono
    bool hasAudio() const { return mono_ != nullptr; }
    bool isDirty() const { return dirty_; }
    bool isIdle() const { return eff_ != nullptr && !renderRunning_ && renderedVersion_ == editVersion_; }
    juce::File audioFile() const { return audioFile_; }
    juce::File projectFile() const { return projectFile_; }
    double lengthSec() const;

    // ---- ClipHost ------------------------------------------------------------------
    const Analysis* analysis() const override { return eff_.get(); }
    Document& document() override { return doc_; }
    ui::DocumentHistory& history() override { return history_; }
    void documentChanged(bool layoutChanged) override;
    juce::String clipName() const override { return audioFile_.getFileName(); }
    juce::String busyText() const override { return busy_; }
    bool isRendering() const override { return renderRunning_ || renderedVersion_ != editVersion_; }
    double playheadSec() const override { return mono_ != nullptr ? pos_.load() : -1.0; }
    bool isPlaying() const override { return playing_.load(); }
    bool canControlTransport() const override { return mono_ != nullptr; }
    void setPlaying(bool p) override;
    void setPlayheadSec(double t) override { pos_.store(std::max(0.0, t)); }
    bool isBypassed() const override { return bypass_.load(); }
    void setBypassed(bool b) override { bypass_.store(b); }
    // The edited file and, if loaded, a reference recording shown as a read-only ghost.
    std::vector<TrackRef> tracks() const override;
    bool ghost(const juce::String& id, Ghost& out) const override;
    bool thread(const juce::String& id, std::vector<ThreadFrame>& out) const override;
    bool canOpenReference() const override { return true; }
    void openReference() override;

    // ---- reference recording (message thread) ---------------------------------------
    bool loadReference(const juce::File& file, juce::String& error);
    bool hasReference() const { return ref_ != nullptr; }

    // ---- AudioSource (audio thread) ---------------------------------------------------
    void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override;
    void releaseResources() override {}
    void getNextAudioBlock(const juce::AudioSourceChannelInfo&) override;

private:
    struct Rendered {
        double sampleRate = 48000;
        std::vector<float> left, right, lead;
        std::vector<std::vector<float>> voices;
        TimeMap map;
    };
    struct Playback { // what the audio thread sees
        std::shared_ptr<const std::vector<float>> mono;
        std::shared_ptr<const Rendered> rendered;
        double sampleRate = 48000;
    };

    void reset();
    bool readMono(const juce::File& file, std::shared_ptr<std::vector<float>>& mono, double& sampleRate, juce::String& error);
    void startAnalysis();
    void rebuildEffective();
    void startRender();
    void publish(std::shared_ptr<const Rendered> r);

    juce::ThreadPool pool_ { juce::ThreadPoolOptions{}.withNumberOfThreads(2).withThreadName("Glina worker") };
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    juce::AudioFormatManager formats_;

    std::shared_ptr<const std::vector<float>> mono_;
    double sampleRate_ = 48000;
    std::shared_ptr<const Analysis> base_, eff_;
    Document doc_;
    ui::DocumentHistory history_;
    std::string pendingDocument_;          // from a project file, applied after analysis
    uint64_t generation_ = 0;              // bumps on every file load; stale jobs are ignored
    uint64_t editVersion_ = 0, renderedVersion_ = 0;
    bool renderRunning_ = false;
    std::shared_ptr<const Rendered> lastRender_;
    juce::String busy_;
    juce::File audioFile_, projectFile_;
    bool dirty_ = false;

    // Reference: analysed in the background; independent of the edited file.
    std::shared_ptr<const Analysis> ref_;
    juce::File refFile_;
    uint64_t refGen_ = 0;
    bool refLoading_ = false;
    std::unique_ptr<juce::FileChooser> refChooser_;

    juce::SpinLock playLock_;
    std::shared_ptr<const Playback> play_;
    std::vector<std::shared_ptr<const Playback>> retired_;
    std::atomic<double> pos_ { 0 };
    std::atomic<bool> playing_ { false }, bypass_ { false };
    double deviceRate_ = 48000;
};

} // namespace glina::app
