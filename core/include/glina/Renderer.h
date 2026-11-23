#pragma once

#include "glina/Types.h"

#include <utility>
#include <vector>

namespace glina {

// Version of the sound the engine produces for a given document. Bump it whenever
// analysis, synthesis, harmonies or mixing change the rendered audio of an existing
// project: the plug-in keeps renders next to the DAW project and marks those made by
// another version (they stay as they are until the user re-renders or edits the clip).
inline constexpr int kRenderVersion = 2; // 2: gaps opened between notes are silent

// Monotonic piecewise-linear mapping between input and output sample time.
// Built from note timing edits; identity when nothing was moved/stretched.
//
// Where notes are pulled further apart than they were in the recording, the audio
// between them (a pause, a breath, a consonant) keeps its length and stays just before
// the later note; the rest of the gap is silence. Between notes that touched in the
// recording the whole gap is silence. After the last note the tail keeps its length and
// silence fills the clip to its end. Over a silence the input time stands still.
class TimeMap {
public:
    TimeMap() = default;
    static TimeMap build(const Analysis& a, const EditSet& e);
    // At an input time where a silence was opened this gives the output time after the
    // silence (where the later note starts); inToOutBefore gives the time before it.
    double inToOut(double t) const;
    double inToOutBefore(double t) const;
    double outToIn(double t) const;
    bool isIdentity() const { return identity_; }

    // Output stretches [start, end) in samples that are silent, in order.
    const std::vector<std::pair<double, double>>& silences() const { return silence_; }
    // True if a silence was opened at an input time in (inA, inB].
    bool silenceBetween(double inA, double inB) const;
    // Level at an output time: 0 in a silence, short raised-cosine fades (`fade` samples)
    // in the audio on either side of it, 1 elsewhere.
    double gainAt(double out, double fade) const;
    // Output start and end (samples) of note i as the renderer places it.
    double noteStartOut(size_t i) const { return i < noteOut_.size() ? noteOut_[i].first : 0.0; }
    double noteEndOut(size_t i) const { return i < noteOut_.size() ? noteOut_[i].second : 0.0; }
    size_t noteCount() const { return noteOut_.size(); }

private:
    std::vector<double> in_, out_;
    std::vector<std::pair<double, double>> silence_;
    std::vector<double> silenceIn_; // input time at which each silence stands
    std::vector<std::pair<double, double>> noteOut_;
    bool identity_ = true;
};

// Shortest silence the time map opens (seconds): a gap that grows by less than this
// stretches the audio between the notes instead.
inline constexpr double kMinSilenceSec = 0.010;
// Fade into and out of a silence (seconds), in the audio on either side.
inline constexpr double kSilenceFadeSec = 0.005;

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
