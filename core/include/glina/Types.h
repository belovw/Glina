// Glina — vocal pitch & time editor engine.
// Core data types shared by analysis, editing and rendering.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace glina {

// What a short analysis frame (hop ≈ 5 ms) contains.
enum class FrameKind : uint8_t {
    Silence = 0,   // below the noise floor
    Voiced  = 1,   // periodic voice (vowels, voiced consonants)
    Sibilant = 2,  // unvoiced, high-frequency noise: s, sh, t, ch, f
    Breath  = 3    // unvoiced, broadband / low-frequency noise: breaths, h
};

struct Frame {
    double timeSec = 0;     // frame centre
    double f0 = 0;          // Hz, 0 when unvoiced
    double midi = 0;        // f0 as MIDI note number (69 = A4 = 440 Hz), 0 when unvoiced
    float voicedProb = 0;   // 0..1, from the pitch detector
    float rmsDb = -120;     // short-term level, dBFS
    float hfRatio = 0;      // share of energy above ~3.5 kHz (sibilant indicator)
    FrameKind kind = FrameKind::Silence;
};

// Pitch-synchronous analysis mark (one per glottal period in voiced regions,
// fixed spacing in unvoiced ones). Positions are in samples, fractional.
struct PitchMark {
    double pos = 0;
    bool voiced = false;
};

// A note "blob": a contiguous voiced segment with one perceived pitch.
struct Note {
    int startFrame = 0;     // inclusive
    int endFrame = 0;       // exclusive
    double centerMidi = 0;  // perceived pitch centre (energy-weighted)
    double meanSlow = 0;    // energy-weighted mean of slowDev / fastDev over the centre window:
    double meanFast = 0;    // used so drift/vibrato edits never move the perceived centre
    double startSec = 0;
    double endSec = 0;
};

struct Key {
    int tonic = 0;          // 0 = C, 1 = C#, ... 11 = B
    bool minor = false;
    int minorForm = 0;      // minor only: 0 natural, 1 melodic, 2 harmonic
    bool just = false;      // natural tuning: pure intervals (ratios), not equal steps
    double confidence = 0;  // correlation with the best key profile
    std::string name() const;
};

// Everything the editor needs about one audio clip. Produced once per clip
// (in the plugin: once per ARA audio source) and kept with the document.
struct Analysis {
    double sampleRate = 48000;
    size_t numSamples = 0;
    int hop = 240;                    // samples per frame
    std::vector<Frame> frames;
    std::vector<PitchMark> marks;
    std::vector<Note> notes;
    // Per-frame decomposition of pitch inside notes (semitones):
    //   midi = note.centerMidi + slowDev + fastDev
    // slowDev = drift (glides, sag), fastDev = vibrato and jitter.
    std::vector<float> slowDev;
    std::vector<float> fastDev;
    std::vector<int> noteOfFrame;     // note index per frame, -1 outside notes
    Key key;
    // Track view only (several items of a track shown as one clip): the [start, end) seconds of
    // each item's part of the timeline. Time edits keep a note inside its item. Empty = one clip.
    std::vector<std::pair<double, double>> segments;

    double frameTime(int i) const { return (double)i * hop / sampleRate; }
};

// ---------------------------------------------------------------------------
// Editing model (what the user changes). All edits are non-destructive and
// applied by the renderer on top of the original audio + analysis.

// Synthetic vibrato that replaces the singer's own (Auto-Tune style). A note
// uses the track's settings (EditSet::autoVibrato) unless it has its own.
struct AutoVibrato {
    double depthCents = 0;     // ± around the note's pitch line; 0 = no pitch vibrato
    double rateHz = 5.5;
    double delayMs = 300;      // from the note start; a note shorter than this gets none (0 = operatic)
    double onsetPeriods = 1.5; // S-curve fade-in after the delay, in vibrato periods
    double variation = 20;     // %: slow wander of rate and depth (not jitter)
    double tremoloDb = 0;      // ± level modulation, same rate and phase as the pitch vibrato
    bool active() const { return depthCents > 0 || tremoloDb > 0; }
    bool operator==(const AutoVibrato& o) const
    {
        return depthCents == o.depthCents && rateHz == o.rateHz && delayMs == o.delayMs &&
               onsetPeriods == o.onsetPeriods && variation == o.variation && tremoloDb == o.tremoloDb;
    }
};

struct NoteEdit {
    double pitchOffset = 0;    // semitones added to the note centre
    double driftAmount = 1;    // 1 = original pitch drift, 0 = flattened
    double vibratoAmount = 1;  // 1 = original, 0 = removed, 2 = doubled
    double formantShift = 0;   // semitones, 0 = natural formants
    double gainDb = 0;
    bool muted = false;        // silent, but kept in place (shown as a ghost); harmonies follow
    bool consMuted = false;    // the unvoiced sound (consonant) before this note is silent
    // Deleted (Alt+Delete): silent and not drawn, its audio kept. Dragging an edge that touches
    // it brings it back (restoreDeleted). Set together with muted and consMuted.
    bool deleted = false;
    // The breath (Breath frames) in the unvoiced sound before this note is deleted: silent and
    // hidden, its consonants kept. Dragging this note's left edge brings it back.
    bool breathDeleted = false;
    double timeOffsetSec = 0;  // move note in time
    double stretch = 1;        // note duration multiplier
    double transition = 0.5;   // pitch move INTO this note: 0 = instant step, 0.5 = natural, 1 = long glide
    bool consonant = false;    // treated as a consonant: no pitch change, no stretch (moves only)
    // Tilt of the pitch line: semitones added at the note's left / right edge,
    // linear in between. Added on top of the line, so drift and vibrato ride on it.
    double tiltL = 0, tiltR = 0;
    bool ownVibrato = false;   // true: `vibrato` below; false: the track's EditSet::autoVibrato
    AutoVibrato vibrato;
    // The separation from the previous note. Soft: the two are linked, a time edit of one
    // edge moves the other note's edge too (no gap opens, none closes). Hard: each moves alone.
    // Auto: soft where they touch (gap under 5 ms), hard where there is a gap.
    enum class Sep : uint8_t { Auto = 0, Soft = 1, Hard = 2 };
    Sep sepBefore = Sep::Auto;
};

// A pencil-drawn stretch of pitch over analysis frames [start, start + delta.size()).
// Stored relative to (original pitch + the note's pitch offset + its tilt), so the
// drawing travels with its note when the note is moved or tilted. NaN = not drawn.
struct DrawRun {
    int start = 0;
    std::vector<float> delta;
};

// A user-defined note boundary (frames, [start, end)). Used when the user splits
// or joins notes; empty layout = the analysis' own segmentation.
struct NoteSpan {
    int start = 0;
    int end = 0;
};

struct EditSet {
    std::vector<NoteEdit> notes;      // one per (effective) note (may be empty = no edits)
    std::vector<NoteSpan> spans;      // user note layout; empty = analysis default
    std::vector<DrawRun> draw;        // pencil-drawn pitch, sorted, non-overlapping
    double sibilantGainDb = 0;        // s / sh / t level
    double breathGainDb = 0;          // breath / noise level
    double unassignedGainDb = 0;      // voiced frames that belong to no note (harmonies mute them)
    double globalFormantShift = 0;    // semitones, added to every note
    double transitionMs = 45;         // ramp width of a note transition at smoothness 0.5
    double edgeTaperMs = 40;          // drift/vibrato edits fade in over note edges
    bool keepLength = true;           // the take keeps its length: the tail after the last note absorbs time edits
    AutoVibrato autoVibrato;          // track settings for notes without their own (default: off)
    uint32_t vibratoSeed = 1;         // per-track randomness of the auto vibrato (phase, wander)
};

} // namespace glina
