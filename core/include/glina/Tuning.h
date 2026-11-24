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

// Degrees of the key's scale above its tonic (7 steps, semitones 0..11).
void scaleSteps(const Key& key, int steps[7]);
// The same degrees in cents above the tonic: equal steps, or pure intervals when key.just.
void scaleCents(const Key& key, double cents[7]);

// Nearest allowed MIDI note for `midi` (scale of `key`, or chromatic).
double nearestTarget(double midi, const Key& key, bool useKey);

// A clean pitch `steps` degrees of the key's scale above `midi` (below when negative), the
// interval counted in the scale (C major: a third above C is E, above E is G).
// steps != 0: `midi` is first taken to its nearest degree (a note outside the scale, e.g. in a
// passing chord, goes to the closest step), then moved by `steps` degrees.
// steps == 0 (unison): the nearest semitone to `midi`; when that semitone is a degree of the
// scale, its exact pitch in the key (pure intervals in natural tuning). A note outside the
// scale stays its own semitone. useKey = false (chromatic): unison is the nearest semitone.
double scaleInterval(double midi, const Key& key, bool useKey, int steps);

EditSet makeAutoTune(const Analysis& a, const TuneParams& p);

// Shifts every note by the same interval (transpose).
EditSet makeTranspose(const Analysis& a, double semitones, double formantShift = 0);

} // namespace glina
