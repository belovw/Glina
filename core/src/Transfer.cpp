#include "glina/Transfer.h"

#include <algorithm>

namespace glina {

namespace {
double lerp(double from, double to, double share) { return from + share * (to - from); }
double clamp01(double v) { return std::clamp(v, 0.0, 1.0); }
}

bool transferNoteEdits(const Document& src, Document& dst, const Analysis& eff, const TransferShare& share)
{
    const size_t n = eff.notes.size();
    if (src.edits.notes.size() != n) return false;
    conform(dst, eff);
    const double pitchShare = clamp01(share.pitch);
    const double timingShare = clamp01(share.timing);
    for (size_t i = 0; i < n; ++i) {
        const NoteEdit& from = src.edits.notes[i];
        NoteEdit& to = dst.edits.notes[i];
        to.pitchOffset = lerp(to.pitchOffset, from.pitchOffset, pitchShare);
        to.timeOffsetSec = lerp(to.timeOffsetSec, from.timeOffsetSec, timingShare);
        to.stretch = lerp(to.stretch, from.stretch, timingShare);
    }
    return true;
}

} // namespace glina
