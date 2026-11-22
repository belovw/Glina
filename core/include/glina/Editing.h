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
// Where the notes do not touch (a gap), only the note whose edge is dragged moves:
// `startEdge` = the start of note left+1 (else the end of note `left`).
double moveBoundary(EditSet& e, const Analysis& a, int left, double dt, const std::set<int>& selection,
                    bool startEdge = false);

// Move notes (each contiguous run of `notes`) by `dt` seconds together with
// the consonant before them; the neighbouring notes absorb the change.
double moveNotes(EditSet& e, const Analysis& a, const std::set<int>& notes, double dt);

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
