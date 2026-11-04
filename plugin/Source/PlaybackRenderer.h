#pragma once

#include "GlinaModel.h"

namespace glina::plugin {

// Realtime-safe: reads pre-rendered samples (or the dry source while analysis
// runs), maps song time -> modification time per playback region, resamples on
// the fly if the source rate differs from the host rate. No allocation, no locks
// that can block, no DSP heavier than cubic interpolation.
class GlinaPlaybackRenderer final : public juce::ARAPlaybackRenderer {
public:
    GlinaPlaybackRenderer(ARA::PlugIn::DocumentController* dc, GlinaDocumentController& controller)
        : juce::ARAPlaybackRenderer(dc), controller_(controller) {}

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
    GlinaDocumentController& controller_;
    double sampleRate_ = 48000;
    int maxBlock_ = 0, numChannels_ = 2;
    std::vector<float> mixL_, mixR_;
};

} // namespace glina::plugin
