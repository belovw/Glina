// Editing operations shared by every front end: time edits by note
// boundaries (the rest of the take stays locked to the backing track) and
// pencil-drawn pitch. All functions are pure: they take the current edits and
// return/modify a copy, so a UI gesture can always re-apply from its start state.
#pragma once

#include "glina/Types.h"

#include <set>
#include <string>
#include <vector>

namespace glina {

// Output (edited) placement of each note, in seconds.
struct NoteTimes {
    double inStart = 0, inEnd = 0;   // original
    double outStart = 0, outEnd = 0; // after edits
};
std::vector<NoteTimes> noteTimes(const Analysis& a, const EditSet& e);
void applyNoteTimes(EditSet& e, const Analysis& a, const std::vector<NoteTimes>& t);
// The time offset and stretch that put one note at t.outStart..t.outEnd.
void setNoteTimes(NoteEdit& e, const NoteTimes& t);
// The shortest a note may become: ~30 ms, or half its recorded length if that is shorter.
double minNoteLength(const NoteTimes& t);

// Time edits are limited only by the conditions they change: no note shorter than ~30 ms,
// no note starting before the previous one ends, notes inside their item (track view) and the
// clip. A fault the edit does not touch (an overlap elsewhere, left by an earlier build) does
// not block it, and a note that overlaps its neighbour can still move away from it.

// Move the boundary after note `left` (between `left` and `left + 1`) by `dt`
// seconds. `left` = -1 is the start of the first note, `left` = n-1 the end of
// the last. Selected notes touching the boundary act as one block on each side:
// the far ends of both blocks stay put, the blocks stretch/compress
// proportionally, and the consonants/pauses between notes keep their length.
// `dt` is clamped so no note becomes shorter than ~30 ms. Returns the dt applied.
// Where the notes do not touch (a gap), only the note whose edge is dragged moves:
// `startEdge` = the start of note left+1 (else the end of note `left`).
double moveBoundary(EditSet& e, const Analysis& a, int left, double dt, const std::set<int>& selection,
                    bool startEdge = false);

// Move notes (each contiguous run of `notes`) by `dt` seconds together with
// the consonant before them; the neighbouring notes absorb the change.
double moveNotes(EditSet& e, const Analysis& a, const std::set<int>& notes, double dt);

// True if going from layout `before` to `after` (same notes) breaks none of the conditions above
// that held and makes no broken one worse.
bool noNewLayoutFaults(const Analysis& a, const std::vector<NoteTimes>& before, const std::vector<NoteTimes>& after);

// True if the separation between note `i` and `i + 1` is soft (see NoteEdit::sepBefore).
bool softSeparation(const EditSet& e, const Analysis& a, int i);
// The same for every boundary at once: element i is the separation between note i and i + 1.
std::vector<bool> softSeparations(const EditSet& e, const Analysis& a);
// Switch the separation between note `i` and `i + 1` between soft and hard.
void toggleSeparation(EditSet& e, const Analysis& a, int i);

// Reset `notes` to no edit (pitch, timing, pencil and the rest). Each run of consecutive notes
// goes back to its recorded time as one block, touching neighbours following its edges; if that
// would overlap or squeeze a neighbour, the run keeps its current timing.
void resetNoteEdits(EditSet& e, const Analysis& a, const std::set<int>& notes);

// Delete notes (Alt+Delete): each becomes silent and hidden, its audio kept. Neighbours and
// the consonants between them stay where they are; nothing is joined.
void deleteNotes(EditSet& e, const Analysis& a, const std::set<int>& notes);
// Delete the breath in the unvoiced sound before each of `notes` (their consonants stay).
void deleteBreaths(EditSet& e, const Analysis& a, const std::set<int>& notes);
// Alt + dragging the boundary between note `left` and `left + 1` brings back a deleted note on either side,
// and the breath deleted before note left + 1.
void restoreDeleted(EditSet& e, int left);

// TEMP-MOVE-DIAG: names the first check the current layout fails ("valid" if none). Remove later.
std::string whyLayoutInvalid(const EditSet& e, const Analysis& a);
// TEMP-MOVE-DIAG: the condition that limited the last move or edge drag (empty if none). Remove later.
std::string moveLimitReason();

// Set note `i`'s output duration (seconds) by moving its end boundary.
void setNoteDuration(EditSet& e, const Analysis& a, int i, double seconds);

// Mark note `i` as a consonant (or back to a vowel). A consonant keeps its
// length and its own pitch: it is passed through unprocessed, only moved.
void setConsonant(EditSet& e, const Analysis& a, int i, bool on);

// Width (ms) of the pitch ramp into a note for a smoothness value 0..1.
double transitionWidthMs(double smoothness, double baseMs);

// ---- tilt (drag a top corner of a note) ---------------------------------------------
// Semitones the tilt adds at analysis frame `frame` of note `n` (linear from
// tiltL at the start edge to tiltR at the end edge; 0 for consonants).
double tiltAt(const Note& n, const NoteEdit& e, double frame);
// Energy-weighted mean of the tilt over the note: how far it moves the perceived centre.
double tiltMean(const Analysis& a, int note, const NoteEdit& e);
// Perceived centre (MIDI) of note `i` after its edits: centre + pitch offset + tilt.
// The editor draws the note box here; correction to the scale aims it.
double editedCenter(const Analysis& a, const EditSet& e, int i);
// Tilt `notes` as ONE note: a roof over the group (first start .. last end): 0 at both
// ends, `semis` at the apex, the right (left) edge of note `apex` when rightCorner (left).
// Without an apex (or for one note) the apex is the group's dragged end, so one note
// tilts its own right (left) edge by `semis`. Each note gets its piece of the line on
// top of its own tilt. Consonants are skipped.
void addTilt(EditSet& e, const Analysis& a, const std::set<int>& notes, bool rightCorner, double semis,
             int apex = -1);

// ---- auto vibrato -----------------------------------------------------------------------
// Settings that apply to note `i` (its own, or the track's).
const AutoVibrato& vibratoOf(const EditSet& e, int i);
// Track settings. `resetNotes` drops every note's own settings (all notes selected).
void setTrackVibrato(EditSet& e, const AutoVibrato& v, bool resetNotes);
// Own settings for the given notes.
void setNoteVibrato(EditSet& e, const std::set<int>& notes, const AutoVibrato& v);

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
