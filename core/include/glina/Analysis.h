#pragma once

#include "glina/PitchDetector.h"
#include "glina/Types.h"

namespace glina {

struct AnalysisParams {
    PitchParams pitch;
    double noteMinMs = 60;          // shortest note blob
    double notePenalty = 2.0;       // cost of starting a new note (higher = fewer notes)
    double noteTruncSt = 0.6;       // deviation cap per frame (semitones): vibrato/glides tolerance
    double noteMinIntervalSt = 0.5; // smallest pitch step that can start a new note
    double dipSplitDb = 9.0;        // level dip that splits a note even on the same pitch
    double contourSmoothMs = 150;   // vibrato-free contour (segmentation, drift/vibrato split)
    double unvoicedMarkMs = 4.0;    // grain spacing in unvoiced regions
    double sibilantHfRatio = 0.45;  // unvoiced frame with more HF energy than this = sibilant
    // Consonants belong to their notes (see attachConsonants): in the sound between two notes,
    // a frame this far (dB) below the louder note, or silent, is pause; the sound between the
    // pause and a note is that note's consonant, up to the length below (longer is not a consonant).
    double consPauseDb = 27;
    double consMaxSec = 0.20;       // a consonant of breath-like noise (х, ф, п, the onset of the voice)
    double sibilantMaxSec = 0.40;   // a consonant that is mostly sibilant (с, ш, ч)
    double consMinPauseSec = 0.04;  // a shorter quiet run between sounds is a closure inside a consonant
};

// Moves each note's edges over its consonants: the sound between a pause and the note (in front
// of it: its consonant and the onset of the voice; after it: its final consonant and release)
// becomes part of the note, so it is cut, muted and moved with the note. What stays between the
// notes is the pause: silence and breath. Where the sound between two notes has no pause (a
// consonant inside a word), nothing changes. Notes must be in time order.
void attachConsonants(Analysis& a, const AnalysisParams& params = {});

// Full analysis of one mono clip. `detector` may be null (default detector used).
Analysis analyze(const float* mono, size_t n, double sampleRate,
                 const AnalysisParams& params = {}, IPitchDetector* detector = nullptr);

// Krumhansl–Kessler key estimate from the detected notes.
Key detectKey(const Analysis& a);

// Pitch-synchronous marks: one per period in voiced regions (period-tracked by
// cross-correlation so neighbouring grains stay phase-coherent), fixed spacing
// in unvoiced regions. Exposed for tests / visualisation.
std::vector<PitchMark> placePitchMarks(const float* x, size_t n, const Analysis& a,
                                       double unvoicedSpacingSamples);

double hzToMidi(double hz);
double midiToHz(double midi);

} // namespace glina
