#pragma once

#include "glina/Types.h"

#include <vector>

namespace glina {

// Monotonic piecewise-linear mapping between input and output sample time.
// Built from note timing edits; identity when nothing was moved/stretched.
class TimeMap {
public:
    TimeMap() = default;
    static TimeMap build(const Analysis& a, const EditSet& e);
    double inToOut(double t) const;
    double outToIn(double t) const;
    bool isIdentity() const { return identity_; }

private:
    std::vector<double> in_, out_;
    bool identity_ = true;
};

// Per-frame control curves derived from the edit set (all on the INPUT timeline).
struct Controls {
    std::vector<float> pitchSemis;   // pitch change to apply (voiced frames)
    std::vector<float> formantSemis; // formant change (voiced frames)
    std::vector<float> gainDb;       // level change (all frames)
};

Controls buildControls(const Analysis& a, const EditSet& e);

// Pitch-synchronous overlap-add resynthesis (TD-PSOLA family). Formants are
// preserved by construction; formant shift resamples each grain; unvoiced
// grains are never pitch-shifted, so consonants stay natural.
std::vector<float> render(const float* x, size_t n, const Analysis& a, const EditSet& e);

// Output pitch curve (MIDI per input frame) that the renderer will produce —
// used by the editor to draw the edited pitch line without rendering audio.
std::vector<double> editedPitchCurve(const Analysis& a, const EditSet& e);

} // namespace glina
