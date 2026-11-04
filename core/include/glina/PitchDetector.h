#pragma once

#include <cstddef>
#include <memory>
#include <vector>

namespace glina {

struct PitchParams {
    double minF0 = 60;                // Hz, lowest expected voice pitch
    double maxF0 = 1100;              // Hz, highest
    double hopSec = 0.005;            // frame step
    double windowSec = 0.025;         // YIN integration window
    double silenceDb = -60;           // absolute gate, dBFS
    double relSilenceDb = -45;        // gate relative to the loudest frame
    double binCents = 20;             // pitch-tracking grid
    double maxJumpCentsPerFrame = 260;// fastest allowed pitch movement between frames
    double voicingSwitchProb = 0.02;  // voiced<->unvoiced transition probability per frame
    double minVoicedMs = 30;          // shorter voiced islands are dropped
};

struct PitchTrack {
    int hop = 0;                      // samples
    std::vector<double> f0;           // Hz per frame, 0 = unvoiced
    std::vector<float> voicedProb;    // 0..1
    std::vector<float> rmsDb;         // frame level
};

// Interface so a neural detector (ONNX model, à la Vovious/RMVPE) can be
// dropped in later and A/B-compared against the classic one.
class IPitchDetector {
public:
    virtual ~IPitchDetector() = default;
    virtual PitchTrack detect(const float* x, size_t n, double sampleRate) = 0;
};

// Classic, deterministic detector: YIN difference function computed via FFT,
// pYIN-style multi-threshold candidate probabilities, Viterbi pitch tracking
// over a 20-cent grid, then a 2-state voicing HMM.
class YinViterbiDetector : public IPitchDetector {
public:
    explicit YinViterbiDetector(PitchParams p = {}) : p_(p) {}
    PitchTrack detect(const float* x, size_t n, double sampleRate) override;

private:
    PitchParams p_;
};

std::unique_ptr<IPitchDetector> makeDefaultPitchDetector(const PitchParams& p = {});

// Reference level for relative gating (95th percentile of frame levels, dB).
float levelReference(const std::vector<float>& rmsDb);

} // namespace glina
