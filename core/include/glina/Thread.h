// Transfer by the pitch "thread", step 1: align two threads in time
// (plan: claude/glina-thread-transfer.md in the Glina project).
//
// A thread is the pitch line of a clip after its edits, with what lies under it
// (voiced / sibilant / breath / silence, level), sampled on a uniform time grid.
// Notes play no part: the target and the reference may split a phrase into
// notes differently. The result is a monotone map from the target's time to the
// reference's time; nothing is changed in either clip yet.
#pragma once

#include "glina/Types.h"

#include <functional>
#include <vector>

namespace glina {

struct ThreadFrame {
    double t = 0;     // seconds on the common timeline
    float midi = 0;   // pitch after the clip's edits; 0 = no pitch here
    FrameKind kind = FrameKind::Silence;
    float rmsDb = -120;
};

// The clip's edited thread, sampled every `step` seconds of its OUTPUT time over
// [lo, hi]; `toCommon` maps clip output time to the common timeline (the edited
// clip's own output time, or project time).
std::vector<ThreadFrame> threadOf(const Analysis& a, const EditSet& e, const std::function<double(double)>& toCommon,
                                  double lo, double hi, double step = 0.010);

// The user's "this point of the target goes with that point of the reference".
struct Pin {
    double tgt = 0, ref = 0; // common timeline, seconds
};

struct AlignOptions {
    double bandSec = 0.5;    // how far the alignment may wander from the coarse shift
    double coarseSec = 1.0;  // search range of the coarse shift
    bool unison = true;      // false: another part (alto, tenor): only the shape of the line counts
    int octave = kAutoOctave; // reference minus target, octaves
    static constexpr int kAutoOctave = 0x7fff;
};

// Monotone, piecewise-linear target -> reference time map (slope 1 beyond its ends).
struct Warp {
    std::vector<double> tgt, ref; // anchors, both increasing
    double map(double tgtSec) const;
    bool empty() const { return tgt.size() < 2; }
};

struct AlignInfo {
    int octave = 0;          // reference minus target, octaves (found or given)
    double coarseSec = 0;    // coarse shift, reference minus target, seconds
    double meanCost = 0;     // per matched frame; 0 = identical threads
    int frames = 0;          // target frames that were aligned
};

// DTW over the frames' kind, level onsets and pitch (octave-folded in unison,
// shape only for another part), in a band around the coarse shift, slope 1:2..2:1,
// through the pins. Empty when the threads do not overlap.
Warp alignThreads(const std::vector<ThreadFrame>& tgt, const std::vector<ThreadFrame>& ref, const AlignOptions& opt = {},
                  const std::vector<Pin>& pins = {}, AlignInfo* info = nullptr);

} // namespace glina
