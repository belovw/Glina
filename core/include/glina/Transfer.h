// Note transfer between clips (plan: claude/glina-note-transfer-plan.md).
// Stage 1, case A: the same recording on several tracks. Notes match one to
// one, so the lead track's edits are copied note by note, each parameter by its
// own share: 1 = as in the source, 0 = keep the target's own value.
#pragma once

#include "glina/Document.h"

namespace glina {

struct TransferShare {
    double pitch = 1;   // pitch offset of the note centre (semitones)
    double timing = 1;  // note start (timeOffsetSec) and duration (stretch)
};

// Copies note edits from `src` into `dst` for the analysis `eff` (both documents
// must describe the same notes). Returns false and changes nothing when the note
// counts do not match what `eff` has.
bool transferNoteEdits(const Document& src, Document& dst, const Analysis& eff, const TransferShare& share);

} // namespace glina
