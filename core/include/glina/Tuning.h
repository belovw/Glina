#pragma once

#include "glina/Types.h"

namespace glina {

// "Correct pitch" macro: snaps note centres to a scale and reduces drift,
// leaving vibrato and transitions alone unless asked.
struct TuneParams {
    double pitchAmount = 1.0;   // 0..1, how far each note centre moves to its target
    double driftAmount = 0.0;   // 0..1, how much pitch drift is flattened (0 = keep all)
    double vibratoScale = 1.0;  // multiplier for vibrato
    double toleranceCents = 0;  // notes already this close to target are left alone
    bool useKey = true;         // false = chromatic snapping
    int tonic = -1;             // -1 = use detected key
    bool minor = false;
    double formantShift = 0;    // semitones, applied to every note
};

// Nearest allowed MIDI note for `midi` (scale of `key`, or chromatic).
double nearestTarget(double midi, const Key& key, bool useKey);

EditSet makeAutoTune(const Analysis& a, const TuneParams& p);

// Shifts every note by the same interval (transpose).
EditSet makeTranspose(const Analysis& a, double semitones, double formantShift = 0);

} // namespace glina
