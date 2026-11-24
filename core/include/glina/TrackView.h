// Track view: several items of one track shown and edited as one clip.
//
// Each item keeps its own analysis, document and render. The view joins their notes onto one
// timeline (gaps between items are silence) so the editor can work on the whole track at once,
// and splits the edited view document back into one document per item.
//
// The view's frame grid is the first item's; another item's frames are placed on it by
// rounding to the nearest frame (at most half a frame, ~2.5 ms, of drawing offset). Nothing
// is resampled and no audio moves: edits go back to each item in its own frames and seconds.
#pragma once

#include "glina/Document.h"
#include "glina/Types.h"

#include <memory>
#include <vector>

namespace glina {

// One item of the track as the view sees it.
struct TrackPiece {
    std::shared_ptr<const Analysis> eff; // the item's effective analysis (its note layout applied)
    Document doc;                        // the item's document
    double placeSec = 0;                 // view time of the item's clip time 0 (may be negative)
    double winStart = 0, winEnd = 0;     // the heard part of the item, in its own clip time
};

// Where a piece landed in the view.
struct TrackPieceMap {
    int frameOffset = 0;              // view frame = item frame + frameOffset
    int frameBegin = 0, frameEnd = 0; // view frames owned by this piece [begin, end)
    int firstNote = 0, count = 0;     // item notes [firstNote, firstNote + count) are shown...
    int viewFirst = 0;                // ...as view notes [viewFirst, viewFirst + count)
};

struct TrackView {
    std::shared_ptr<Analysis> analysis; // all pieces' frames and notes on one timeline
    Document doc;                       // the view document (spans always explicit)
    Document built;                     // `doc` as it was built: tells which track-wide settings changed
    std::vector<TrackPieceMap> maps;
    int primary = 0;                    // the piece whose track-wide settings the view shows
};

// Builds the view. Pieces must be sorted by placeSec, analysed, with the same sample rate and
// hop. A piece overlapped by the next one ends where the next one starts; a note is shown only
// if it lies entirely inside its piece's part. False (and `out` untouched) if pieces don't fit.
bool buildTrackView(const std::vector<TrackPiece>& pieces, int primary, TrackView& out);

// The edited view document split back into one document per piece (same order as `pieces`).
// Notes, layout (split/join), pencil and harmony note masks go to the piece they lie in. A
// track-wide setting (key, globals, auto vibrato, harmony voices...) changed in the view is
// written to every piece; one left unchanged keeps each piece's own value. `layoutChanged`
// (optional) tells per piece whether its note layout changed.
std::vector<Document> splitTrackView(const TrackView& view, const Document& edited,
                                     const std::vector<TrackPiece>& pieces, std::vector<bool>* layoutChanged = nullptr);

} // namespace glina
