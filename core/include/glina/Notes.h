// Note layout: computing note statistics for arbitrary frame spans, so the user
// can split and join notes on top of the automatic segmentation.
#pragma once

#include "glina/Types.h"

namespace glina {

// Appends a note covering frames [start, end) to `a` and fills its centre,
// drift/vibrato decomposition and noteOfFrame entries. `smoothed` (optional)
// is a jitter-filtered MIDI contour aligned with `start`; if null it is
// computed from the frames. Unvoiced frames inside the span are ignored.
void appendNote(Analysis& a, int start, int end, double contourSmoothMs = 150, const double* smoothed = nullptr);

// The base analysis with a user note layout applied (identity when `spans` is empty).
Analysis withLayout(const Analysis& base, const std::vector<NoteSpan>& spans);

// Current layout as spans (from the analysis' notes).
std::vector<NoteSpan> spansOf(const Analysis& a);

} // namespace glina
