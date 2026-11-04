// Glina — vocal pitch & time editor engine.
// Core data types shared by analysis, editing and rendering.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
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

    double frameTime(int i) const { return (double)i * hop / sampleRate; }
};

// ---------------------------------------------------------------------------
// Editing model (what the user changes). All edits are non-destructive and
// applied by the renderer on top of the original audio + analysis.

struct NoteEdit {
    double pitchOffset = 0;    // semitones added to the note centre
    double driftAmount = 1;    // 1 = original pitch drift, 0 = flattened
    double vibratoAmount = 1;  // 1 = original, 0 = removed, 2 = doubled
    double formantShift = 0;   // semitones, 0 = natural formants
    double gainDb = 0;
    double timeOffsetSec = 0;  // move note in time
    double stretch = 1;        // note duration multiplier
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
    double sibilantGainDb = 0;        // s / sh / t level
    double breathGainDb = 0;          // breath / noise level
    double unassignedGainDb = 0;      // voiced frames that belong to no note (harmonies mute them)
    double globalFormantShift = 0;    // semitones, added to every note
    double transitionMs = 45;         // pitch-offset crossfade between adjacent notes
    double edgeTaperMs = 40;          // drift/vibrato edits fade in over note edges
};

} // namespace glina
