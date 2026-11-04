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
};

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
