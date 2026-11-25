#pragma once

#include "glina/Types.h"

#include <utility>
#include <vector>

namespace glina {

// Version of the sound the engine produces for a given document. Bump it whenever
// analysis, synthesis, harmonies or mixing change the rendered audio of an existing
// project: the plug-in keeps renders next to the DAW project and marks those made by
// another version (they stay as they are until the user re-renders or edits the clip).
inline constexpr int kRenderVersion = 5; // 2: gaps opened between notes are silent; 3: overlaps;
                                         // 4: consonants copied whole after time edits, overlays without a seam;
                                         // 5: silence only inside a pause of kWordGapSec, never inside a word

// Monotonic piecewise-linear mapping between input and output sample time.
// Built from note timing edits; identity when nothing was moved/stretched.
//
// Where notes are pulled further apart than they were in the recording, the audio
// between them (a pause, a breath, a consonant) keeps its length and stays just before
// the later note; the rest of the gap is silence. Between notes that touched in the
// recording the whole gap is silence. After the last note the tail keeps its length and
// silence fills the clip to its end. Over a silence the input time stands still.
//
// A note that reaches over the next one (an overlap, past a hard separation) does not push
// it: the next note keeps its place, with the pause (consonant, breath) before it, and the
// part of the earlier note that lies over it is an overlay, played on top. In the main map
// that part is squeezed to nothing; inToOut gives its overlay position.
class TimeMap {
public:
    // The end of a note played over what follows it: input [in.front(), in.back()] to output.
    struct Overlay {
        size_t note = 0;
        std::vector<double> in, out;
    };
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
    // Where the note ends in the main map (before its overlay, if it has one).
    double noteMainEndOut(size_t i) const { return i < mainEnd_.size() ? mainEnd_[i] : noteEndOut(i); }
    size_t noteCount() const { return noteOut_.size(); }
    const std::vector<Overlay>& overlays() const { return overlays_; }
    // Output position of an input time under the main map alone (overlays left out).
    double mainInToOut(double t) const;

private:
    std::vector<double> in_, out_;
    std::vector<std::pair<double, double>> silence_;
    std::vector<double> silenceIn_; // input time at which each silence stands
    std::vector<std::pair<double, double>> noteOut_;
    std::vector<double> mainEnd_;
    std::vector<Overlay> overlays_;
    bool identity_ = true;
};

// Shortest silence the time map opens (seconds): a gap that grows by less than this
// stretches the audio between the notes instead.
inline constexpr double kMinSilenceSec = 0.010;
// A gap opened between notes becomes silence only inside a silent run of the recording of
// kWordGapSec (Types.h) or more: between words or phrases, never inside a word.
// A pause with sound all through (a consonant, a glide) stretches at most this much when the notes
// around it move apart; the note before it holds on for the rest.
inline constexpr double kMaxPauseStretch = 1.5;
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
