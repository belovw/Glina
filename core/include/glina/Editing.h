// Editing operations shared by every front end: time edits by note
// boundaries (the rest of the take stays locked to the backing track) and
// pencil-drawn pitch. All functions are pure: they take the current edits and
// return/modify a copy, so a UI gesture can always re-apply from its start state.
#pragma once

#include "glina/Types.h"

#include <set>
#include <vector>

namespace glina {

// Output (edited) placement of each note, in seconds.
struct NoteTimes {
    double inStart = 0, inEnd = 0;   // original
    double outStart = 0, outEnd = 0; // after edits
};
std::vector<NoteTimes> noteTimes(const Analysis& a, const EditSet& e);
void applyNoteTimes(EditSet& e, const Analysis& a, const std::vector<NoteTimes>& t);

// Move the boundary after note `left` (between `left` and `left + 1`) by `dt`
// seconds. `left` = -1 is the start of the first note, `left` = n-1 the end of
// the last. Selected notes touching the boundary act as one block on each side:
// the far ends of both blocks stay put, the blocks stretch/compress
// proportionally, and the consonants/pauses between notes keep their length.
// `dt` is clamped so no note becomes shorter than ~30 ms. Returns the dt applied.
double moveBoundary(EditSet& e, const Analysis& a, int left, double dt, const std::set<int>& selection);

// Move notes (each contiguous run of `notes`) by `dt` seconds together with
// the consonant before them; the neighbouring notes absorb the change.
double moveNotes(EditSet& e, const Analysis& a, const std::set<int>& notes, double dt);

// Set note `i`'s output duration (seconds) by moving its end boundary.
void setNoteDuration(EditSet& e, const Analysis& a, int i, double seconds);

// Width (ms) of the pitch ramp into a note for a smoothness value 0..1.
double transitionWidthMs(double smoothness, double baseMs);

// ---- pencil -------------------------------------------------------------------------
// Draw absolute pitch values (MIDI) onto frames firstFrame.. (one value per frame).
// Frames without pitch or outside notes are skipped.
void drawPitch(EditSet& e, const Analysis& a, int firstFrame, const std::vector<float>& midi);
// Remove drawn pitch on frames [f0, f1).
void eraseDrawn(EditSet& e, int f0, int f1);
// Dense per-frame view of the drawing (NaN = not drawn).
std::vector<float> drawnDense(const EditSet& e, int numFrames);
void setDrawnDense(EditSet& e, const std::vector<float>& dense);

} // namespace glina
