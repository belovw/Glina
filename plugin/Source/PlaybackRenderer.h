#pragma once

#include "GlinaModel.h"

#include <array>

namespace glina::plugin {

// Realtime-safe: reads pre-rendered samples (or the dry source while analysis
// runs), maps song time -> modification time per playback region, resamples on
// the fly if the source rate differs from the host rate. No allocation, no locks
// that can block, no DSP heavier than cubic interpolation.
class GlinaPlaybackRenderer final : public juce::ARAPlaybackRenderer, private juce::Timer {
public:
    GlinaPlaybackRenderer(ARA::PlugIn::DocumentController* dc, GlinaDocumentController& controller);
    ~GlinaPlaybackRenderer() override;

    void prepareToPlay(double sampleRate, int maxBlock, int numChannels, juce::AudioProcessor::ProcessingPrecision,
                       AlwaysNonRealtime) override
    {
        sampleRate_ = sampleRate;
        maxBlock_ = maxBlock;
        numChannels_ = numChannels;
        mixL_.assign((size_t)std::max(1, maxBlock), 0.f);
        mixR_.assign((size_t)std::max(1, maxBlock), 0.f);
    }

    void releaseResources() override {}

    bool processBlock(juce::AudioBuffer<float>& buffer, juce::AudioProcessor::Realtime,
                      const juce::AudioPlayHead::PositionInfo& pos) noexcept override;

private:
    // ---- diagnostics: which cache the audio thread plays, logged once per change -------------
    // The audio thread only compares pointers and pushes into a preallocated FIFO; the
    // message thread (timer) writes the lines to glina_diag.txt.
    enum class DiagKind { region, playStart, playStop };
    struct DiagEvent {
        DiagKind kind = DiagKind::region;
        const void* region = nullptr;
        const void* mod = nullptr;
        const void* cache = nullptr;
        uint64_t key = 0, cacheEditVersion = 0;
        double songSec = 0, playStart = 0, modStart = 0;
        int numRegions = 0;
        bool firstSeen = false, dry = false, haveCache = false, bypass = false;
    };
    struct DiagSeen { const void* region; const void* mod; const void* cache; bool dry; };
    void diagPush(const DiagEvent& e) noexcept;
    void timerCallback() override { diagDrain(); }
    void diagDrain();

    juce::AbstractFifo diagFifo_ { 512 };
    std::array<DiagEvent, 512> diagEvents_ {};
    std::array<DiagSeen, 64> diagSeen_ {};   // audio thread only
    int diagSeenCount_ = 0;                 // audio thread only
    bool diagWasPlaying_ = false;           // audio thread only
    std::atomic<int> diagDropped_ { 0 };

    GlinaDocumentController& controller_;
    double sampleRate_ = 48000;
    int maxBlock_ = 0, numChannels_ = 2;
    std::vector<float> mixL_, mixR_;
};

} // namespace glina::plugin
