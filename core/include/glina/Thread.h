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

// Adds a pin keeping the list sorted by tgt. False (nothing added) when it sits on a pin already or
// would cross a neighbour in either time.
bool addPin(std::vector<Pin>& pins, const Pin& p);
// Moves one end of pin i (refEnd: the reference end, else the target end) to `sec`, clamped so the
// neighbours keep their order. False when pin i does not exist.
bool movePin(std::vector<Pin>& pins, size_t i, bool refEnd, double sec);

struct AlignOptions {
    double bandSec = 0.5;    // how far the alignment may wander from the coarse shift
    double coarseSec = 1.0;  // search range of the coarse shift
    bool unison = true;      // false: another part (alto, tenor): only the shape of the line counts
    bool autoPart = true;    // unison asked, but the lines are > 1 semitone apart (octave folded): treat as another part
    int octave = kAutoOctave; // reference minus target, octaves
    static constexpr int kAutoOctave = 0x7fff;
    // Leaving the coarse shift costs: free up to driftFreeSec, then driftCostPerSec per second of
    // distance, per frame (1 = a whole mismatch of kind). Two tracks of one song are sung to the same
    // backing: where nothing tells them apart (a long silence on one side, phrases that repeat, a
    // part whose line only resembles the other) the links stay where the shift puts them instead of
    // wandering off by seconds; a real drift still wins where the sound shows it.
    double driftFreeSec = 0.3;
    double driftCostPerSec = 0.3;
};

// Monotone, piecewise-linear target -> reference time map (slope 1 beyond its ends).
struct Warp {
    std::vector<double> tgt, ref; // anchors, both increasing
    double map(double tgtSec) const;
    double unmap(double refSec) const; // the inverse: the target time that maps to refSec
    bool empty() const { return tgt.size() < 2; }
};

struct AlignInfo {
    int octave = 0;          // reference minus target, octaves (found or given)
    double coarseSec = 0;    // coarse shift, reference minus target, seconds
    double meanCost = 0;     // per matched frame; 0 = identical threads
    int frames = 0;          // target frames that were aligned
    bool part = false;       // aligned as another part (shape only)
    double medianDiff = 0;   // median |reference - target| in semitones, octave folded (after the coarse shift)
};

// DTW over the frames' kind, level onsets and pitch (octave-folded in unison,
// shape only for another part), in a band around the coarse shift, slope 1:2..2:1,
// through the pins. Empty when the threads do not overlap.
Warp alignThreads(const std::vector<ThreadFrame>& tgt, const std::vector<ThreadFrame>& ref, const AlignOptions& opt = {},
                  const std::vector<Pin>& pins = {}, AlignInfo* info = nullptr);

// ---- step 2: wrong notes ---------------------------------------------------------------
// For each note of the target: what the reference sings over it (through the warp),
// and whether the note is a wrong note. A note within `keepCents` of the reference is
// the singer's own intonation and stays; one at least `wrongCents` away is moved by a
// whole number of semitones toward the reference, keeping its own cents. In between:
// left alone. Notes the user already edited (pitch, tilt, pencil) and consonants are
// never touched, nor anything when the reference is another part.
struct NoteFix {
    int note = -1;
    int semis = 0;          // whole semitones to add (0 = leave the note)
    double refMidi = 0;     // reference over the note, octave-folded to the target's octave
    double diff = 0;        // reference minus the note's centre, semitones
    bool edited = false;    // the user already edited it: never touched
    bool unsteady = false;  // the reference changes notes over it (our note spans two of theirs): left alone
};
struct FixOptions {
    double keepCents = 50;
    double wrongCents = 100;
    double minCover = 0.4;  // share of the note's middle where the reference sings
    double minSteady = 0.7; // share of those frames within half a semitone of the reference's median
};
// `tgtA`/`tgtE`: the target clip (its threads were taken on its own output timeline);
// `ref`, `warp`, `info`: from alignThreads(threadOf(tgtA, tgtE, ...), ref, ...).
std::vector<NoteFix> planPitchFix(const Analysis& tgtA, const EditSet& tgtE, const std::vector<ThreadFrame>& ref,
                                  const Warp& warp, const AlignInfo& info, const FixOptions& opt = {});
// Adds the planned semitones to the notes' pitch offsets.
void applyPitchFix(EditSet& e, const std::vector<NoteFix>& fixes);

// ---- interval: pull notes onto the reference (a non-singer under a clean reference) -----
// Every note asked for is pulled, edited or not, whatever the reference is (unison or another
// part); nothing is left alone for being close. Pitch: the note's centre goes to a clean pitch
// `steps` degrees of the key's scale from what the reference sings over the note's middle
// (0 = unison: the reference's own note, see scaleInterval()), in the note's own octave, by the
// share `pitchAmount`. Consonants keep their pitch. Time: each edge of the note moves toward
// where the alignment puts it on the reference, by the share `timeAmount`; edges closer than
// `timeTolSec` stay. A note under which the reference does not sing keeps its pitch.
struct IntervalOptions {
    int steps = 0;             // scale degrees: 0 unison, ±2 third, ±3 fourth, ±4 fifth, ±5 sixth
    double pitchAmount = 1.0;  // share of the way to the target pitch («Высота»)
    double timeAmount = 0.92;  // share of the way to the reference in time (0 = time untouched)
    double timeTolSec = 0;     // edges closer than this to their place stay put
    bool useKey = true;        // false (chromatic mode): unison goes to the nearest semitone
};
struct IntervalFix {
    int note = -1;
    bool pitched = false;      // the reference sings over the note: it gets a pitch target
    double refMidi = 0;        // the reference over the note's middle (median, as sung, its own octave)
    double target = 0;         // where the note's centre goes (pitched only)
    double pitchDelta = 0;     // semitones to add to the note's pitch offset (share applied)
    double outStart = 0, outEnd = 0; // the note's new output placement (share applied)
    bool timed = false;        // the placement changes
};
// How to align for an interval: unison (steps 0) is known to be the same part, however far off the
// customer sings; any other interval is another part (only the shape of the line counts).
AlignOptions intervalAlignOptions(int steps);
// `notes`: the notes to pull (empty = every note). `key`: the clip's key (Document::key).
// `ref`, `warp`: as for planPitchFix. One entry per note asked for, in note order.
std::vector<IntervalFix> planIntervalFix(const Analysis& tgtA, const EditSet& tgtE, const Key& key,
                                         const std::vector<ThreadFrame>& ref, const Warp& warp,
                                         const std::vector<int>& notes, const IntervalOptions& opt = {});
// ---- melismas: one note of ours where the reference sings several -----------------------
// The reference's notes (after its edits) on the same timeline as its thread.
struct RefNote {
    double t0 = 0, t1 = 0, midi = 0;
};
// Where to cut a note of ours: analysis frames (input timeline), ascending.
struct NoteCuts {
    int note = -1;
    std::vector<int> frames;
};
// For each note asked for (empty = every note; consonants never): the reference notes the warp
// lays over it, each over at least `minSec`. Where two of them follow each other at different
// pitches (half a semitone or more), the note is cut where the second one starts (through the
// warp), so each piece can be pulled to its own note. Pieces are at least `minSec` long.
std::vector<NoteCuts> planMelismaCuts(const Analysis& a, const EditSet& e, const std::vector<RefNote>& ref,
                                      const Warp& warp, const std::vector<int>& notes, double minSec = 0.05);

// Applies the planned pitch and time (time through placeNotes: order and lengths stay valid).
// False when the time could not be placed (the pitch is applied anyway).
bool applyIntervalFix(EditSet& e, const Analysis& a, const std::vector<IntervalFix>& fixes);

} // namespace glina
