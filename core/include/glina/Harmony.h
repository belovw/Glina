// Harmony voices generated from the lead vocal. Intervals follow the scale
// (a "third above" is a major or minor third depending on the note), unlike
// fixed-semitone harmonisers.
#pragma once

#include "glina/Types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace glina {

struct HarmonyVoice {
    bool enabled = true;
    bool diatonic = true;        // true: `degrees` scale steps; false: fixed `semitones`
    int degrees = 2;             // +2 = third above, -2 = third below, +4 = fifth above, ±7 = octave
    double semitones = 0;        // used when !diatonic
    double formantShift = 0;     // semitones: negative = darker/bigger singer, positive = lighter
    double gainDb = -6;
    double pan = 0;              // -1 (left) .. +1 (right)
    double delayMs = 12;         // humanise: the double comes in slightly late
    double detuneCents = 5;      // humanise: random per-note intonation spread (±)
    double vibratoAmount = 0.5;  // relative to the lead's vibrato
    double driftAmount = 0.6;    // relative to the lead's drift
    bool consonants = false;     // also sing s / sh / t and breaths (usually off)
    uint32_t seed = 1;
    std::vector<uint8_t> noteMask; // per note: 1 = sing it; empty = every note
};

// Short human-readable interval name ("3 выше", "+5 пт").
std::string harmonyIntervalName(const HarmonyVoice& v);

// MIDI note `degrees` scale steps away from `midi` (snapped to the scale first).
double scaleStep(double midi, const Key& key, int degrees);

// The edit set that turns the original take into this harmony voice, following
// all of the lead's edits (pitch, timing, formants).
EditSet harmonyEdits(const Analysis& a, const EditSet& lead, const Key& key, const HarmonyVoice& v);

// Pitch (MIDI) of each note of the harmony voice, for drawing; NaN where muted.
std::vector<double> harmonyNotePitches(const Analysis& a, const EditSet& lead, const Key& key, const HarmonyVoice& v);

} // namespace glina
