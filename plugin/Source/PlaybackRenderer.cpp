#include "PlaybackRenderer.h"

namespace glina::plugin {

static inline float cubic(const float* x, size_t n, double t) noexcept
{
    const long i = (long)std::floor(t);
    const float f = (float)(t - (double)i);
    auto s = [&](long k) { return (k >= 0 && (size_t)k < n) ? x[k] : 0.f; };
    const float y0 = s(i - 1), y1 = s(i), y2 = s(i + 1), y3 = s(i + 2);
    const float a = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
    const float b = y0 - 2.5f * y1 + 2.f * y2 - 0.5f * y3;
    const float c = -0.5f * y0 + 0.5f * y2;
    return ((a * f + b) * f + c) * f + y1;
}

bool GlinaPlaybackRenderer::processBlock(juce::AudioBuffer<float>& buffer, juce::AudioProcessor::Realtime,
                                         const juce::AudioPlayHead::PositionInfo& pos) noexcept
{
    buffer.clear();
    const int numSamples = buffer.getNumSamples();
    if (numSamples > (int)mixL_.size()) return true; // host broke its promise; stay silent rather than allocate

    const auto lock = controller_.tryProcessingLock();
    if (!lock.isLocked() || !pos.getIsPlaying()) return true;

    const double blockStart = (double)pos.getTimeInSamples().orFallback(0) / sampleRate_;
    std::fill(mixL_.begin(), mixL_.begin() + numSamples, 0.f);
    std::fill(mixR_.begin(), mixR_.begin() + numSamples, 0.f);
    bool any = false;

    for (auto* region : getPlaybackRegions()) {
        auto* mod = static_cast<GlinaAudioModification*>(region->getAudioModification());
        auto& data = *mod->getGlinaSource()->data;

        // Edited audio if rendered (and not in A/B bypass); the dry source otherwise.
        std::shared_ptr<const RenderCache> cache;
        const bool haveCache = mod->tryGetCache(cache);
        const bool dry = !haveCache || mod->bypass.load(std::memory_order_relaxed);
        if (dry && !data.samplesReady) continue;
        const double srcRate = dry ? data.sampleRate : cache->sampleRate;
        if (srcRate <= 0) continue;

        const double playStart = region->getStartInPlaybackTime();
        const double playEnd = region->getEndInPlaybackTime();
        const double modStart = region->getStartInAudioModificationTime();
        const double playDur = region->getDurationInPlaybackTime();
        const double ratio = playDur > 0 ? region->getDurationInAudioModificationTime() / playDur : 1.0;

        for (int i = 0; i < numSamples; ++i) {
            const double t = blockStart + i / sampleRate_;
            if (t < playStart || t >= playEnd) continue;
            const double modT = modStart + (t - playStart) * ratio; // output (edited) time of the clip
            if (dry) {
                // A/B and while rendering: original audio at the same musical position.
                const double inPos = haveCache ? cache->map.outToIn(modT * cache->sampleRate) * (data.sampleRate / cache->sampleRate)
                                               : modT * data.sampleRate;
                const float s = cubic(data.mono.data(), data.mono.size(), inPos);
                mixL_[(size_t)i] += s;
                mixR_[(size_t)i] += s;
            } else {
                const double p = modT * srcRate;
                mixL_[(size_t)i] += cubic(cache->left.data(), cache->left.size(), p);
                mixR_[(size_t)i] += cubic(cache->right.data(), cache->right.size(), p);
            }
            any = true;
        }
    }

    if (any) {
        if (buffer.getNumChannels() >= 2) {
            buffer.copyFrom(0, 0, mixL_.data(), numSamples);
            buffer.copyFrom(1, 0, mixR_.data(), numSamples);
            for (int c = 2; c < buffer.getNumChannels(); ++c) buffer.copyFrom(c, 0, mixL_.data(), numSamples);
        } else if (buffer.getNumChannels() == 1) {
            float* o = buffer.getWritePointer(0);
            for (int i = 0; i < numSamples; ++i) o[i] = 0.5f * (mixL_[(size_t)i] + mixR_[(size_t)i]);
        }
    }
    return true;
}

} // namespace glina::plugin
