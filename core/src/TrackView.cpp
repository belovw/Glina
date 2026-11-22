#include "glina/TrackView.h"

#include "glina/Editing.h"
#include "glina/Notes.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace glina {

namespace {

bool sameSpans(const std::vector<NoteSpan>& a, const std::vector<NoteSpan>& b)
{
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](const NoteSpan& x, const NoteSpan& y) {
               return x.start == y.start && x.end == y.end;
           });
}

// A voice's settings without its note mask (the mask is per note, not track-wide).
bool sameVoiceSettings(HarmonyVoice a, HarmonyVoice b)
{
    a.noteMask.clear();
    b.noteMask.clear();
    Document x, y;
    x.voices = {a};
    y.voices = {b};
    return serialize(x) == serialize(y);
}

bool sameVoices(const std::vector<HarmonyVoice>& a, const std::vector<HarmonyVoice>& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (!sameVoiceSettings(a[i], b[i])) return false;
    return true;
}

uint8_t maskAt(const std::vector<uint8_t>& m, size_t i) { return m.empty() ? 1 : (i < m.size() ? m[i] : 1); }

void compactMask(std::vector<uint8_t>& m)
{
    if (std::all_of(m.begin(), m.end(), [](uint8_t v) { return v != 0; })) m.clear();
}

} // namespace

bool buildTrackView(const std::vector<TrackPiece>& pieces, int primary, TrackView& out)
{
    if (pieces.empty()) return false;
    for (const auto& p : pieces)
        if (p.eff == nullptr || p.eff->frames.empty()) return false;
    const Analysis& a0 = *pieces.front().eff;
    for (const auto& p : pieces)
        if (p.eff->sampleRate != a0.sampleRate || p.eff->hop != a0.hop) return false;
    primary = std::clamp(primary, 0, (int)pieces.size() - 1);

    const double hopSec = (double)a0.hop / a0.sampleRate;
    const size_t np = pieces.size();
    std::vector<TrackPieceMap> maps(np);
    int total = 0;
    for (size_t k = 0; k < np; ++k) {
        const auto& p = pieces[k];
        auto& m = maps[k];
        const int nf = (int)p.eff->frames.size();
        m.frameOffset = (int)std::lround(p.placeSec / hopSec);
        const int i0 = std::clamp((int)std::ceil(p.winStart / hopSec - 1e-6), 0, nf);
        const int i1 = std::clamp((int)std::floor(p.winEnd / hopSec + 1e-6), i0, nf);
        m.frameBegin = std::max(0, i0 + m.frameOffset);
        m.frameEnd = std::max(m.frameBegin, i1 + m.frameOffset);
    }
    // Pieces own consecutive stretches: a piece overlapped by the next one ends where it starts.
    for (size_t k = 0; k < np; ++k) {
        if (k > 0) maps[k].frameBegin = std::max(maps[k].frameBegin, maps[k - 1].frameEnd);
        maps[k].frameEnd = std::max(maps[k].frameEnd, maps[k].frameBegin);
        if (k + 1 < np) maps[k].frameEnd = std::min(maps[k].frameEnd, std::max(maps[k].frameBegin, maps[k + 1].frameBegin));
        total = std::max(total, maps[k].frameEnd);
    }

    auto view = std::make_shared<Analysis>();
    Analysis& v = *view;
    v.sampleRate = a0.sampleRate;
    v.hop = a0.hop;
    v.numSamples = (size_t)total * (size_t)a0.hop;
    v.key = pieces[(size_t)primary].eff->key;
    v.frames.resize((size_t)total);
    for (int f = 0; f < total; ++f) v.frames[(size_t)f].timeSec = f * hopSec;
    v.slowDev.assign((size_t)total, 0.f);
    v.fastDev.assign((size_t)total, 0.f);
    v.noteOfFrame.assign((size_t)total, -1);

    Document doc = pieces[(size_t)primary].doc;
    doc.edits.notes.clear();
    doc.edits.spans.clear();
    std::vector<float> draw((size_t)total, std::numeric_limits<float>::quiet_NaN());
    // Masks per voice of the view (the primary piece's voices), built note by note.
    std::vector<std::vector<uint8_t>> masks(doc.voices.size());

    for (size_t k = 0; k < np; ++k) {
        const Analysis& a = *pieces[k].eff;
        Document pd = pieces[k].doc;
        conform(pd, a);
        auto& m = maps[k];
        const int off = m.frameOffset;
        v.segments.emplace_back(m.frameBegin * hopSec, m.frameEnd * hopSec);
        for (int f = m.frameBegin; f < m.frameEnd; ++f) {
            const size_t src = (size_t)(f - off);
            v.frames[(size_t)f] = a.frames[src];
            v.frames[(size_t)f].timeSec = f * hopSec;
            if (src < a.slowDev.size()) v.slowDev[(size_t)f] = a.slowDev[src];
            if (src < a.fastDev.size()) v.fastDev[(size_t)f] = a.fastDev[src];
        }
        // Notes lying entirely inside the piece's part (they are consecutive).
        m.firstNote = (int)a.notes.size();
        m.count = 0;
        for (int j = 0; j < (int)a.notes.size(); ++j) {
            const Note& n = a.notes[(size_t)j];
            const bool in = n.startFrame + off >= m.frameBegin && n.endFrame + off <= m.frameEnd;
            if (in && m.count == 0) m.firstNote = j;
            if (in) ++m.count;
            else if (m.count > 0) break;
        }
        if (m.count == 0) // where the piece's notes would go: after those that start before its part
            m.firstNote = (int)std::count_if(a.notes.begin(), a.notes.end(),
                                             [&](const Note& n) { return n.startFrame + off < m.frameBegin; });
        m.viewFirst = (int)v.notes.size();
        for (int j = m.firstNote; j < m.firstNote + m.count; ++j) {
            Note n = a.notes[(size_t)j];
            const int vi = (int)v.notes.size();
            for (int f = n.startFrame; f < n.endFrame; ++f)
                if ((size_t)f < a.noteOfFrame.size() && a.noteOfFrame[(size_t)f] == j) v.noteOfFrame[(size_t)(f + off)] = vi;
            n.startFrame += off;
            n.endFrame += off;
            n.startSec += off * hopSec;
            n.endSec += off * hopSec;
            v.notes.push_back(n);
            doc.edits.notes.push_back(pd.edits.notes[(size_t)j]);
            for (size_t vv = 0; vv < masks.size(); ++vv)
                masks[vv].push_back(vv < pd.voices.size() ? maskAt(pd.voices[vv].noteMask, (size_t)j) : 1);
        }
        const std::vector<float> pdraw = drawnDense(pd.edits, (int)a.frames.size());
        for (int f = m.frameBegin; f < m.frameEnd; ++f) draw[(size_t)f] = pdraw[(size_t)(f - off)];
    }
    doc.edits.spans = spansOf(v);
    setDrawnDense(doc.edits, draw);
    for (size_t vv = 0; vv < masks.size(); ++vv) {
        compactMask(masks[vv]);
        doc.voices[vv].noteMask = masks[vv];
    }

    out.analysis = view;
    out.doc = doc;
    out.built = doc;
    out.maps = std::move(maps);
    out.primary = primary;
    return true;
}

std::vector<Document> splitTrackView(const TrackView& view, const Document& edited, const std::vector<TrackPiece>& pieces,
                                     std::vector<bool>* layoutChanged)
{
    std::vector<Document> docs;
    if (layoutChanged != nullptr) layoutChanged->assign(pieces.size(), false);
    if (view.analysis == nullptr || pieces.size() != view.maps.size()) return docs;
    const Analysis& v = *view.analysis;
    const std::vector<NoteSpan> vspans = edited.edits.spans.empty() ? spansOf(v) : edited.edits.spans;
    const size_t vn = std::min(vspans.size(), edited.edits.notes.size());
    const std::vector<float> vdraw = drawnDense(edited.edits, (int)v.frames.size());
    const Document& built = view.built;
    const bool voicesChanged = !sameVoices(edited.voices, built.voices);

    for (size_t k = 0; k < pieces.size(); ++k) {
        const Analysis& a = *pieces[k].eff;
        const auto& m = view.maps[k];
        const int off = m.frameOffset;
        Document d = pieces[k].doc;
        conform(d, a);
        const std::vector<NoteSpan> orig = spansOf(a);
        const int first = std::clamp(m.firstNote, 0, (int)orig.size());
        const int after = std::clamp(m.firstNote + m.count, first, (int)orig.size());

        // The view's notes in this piece's part (a span crossing into another piece is cut there).
        std::vector<NoteSpan> mid;
        std::vector<size_t> midIdx;
        for (size_t i = 0; i < vn; ++i) {
            const int s = std::max(vspans[i].start, m.frameBegin), e = std::min(vspans[i].end, m.frameEnd);
            if (e <= s) continue;
            mid.push_back({s - off, e - off});
            midIdx.push_back(i);
        }

        std::vector<NoteSpan> spans(orig.begin(), orig.begin() + first);
        spans.insert(spans.end(), mid.begin(), mid.end());
        spans.insert(spans.end(), orig.begin() + after, orig.end());
        std::vector<NoteEdit> notes(d.edits.notes.begin(), d.edits.notes.begin() + first);
        for (size_t i : midIdx) notes.push_back(edited.edits.notes[i]);
        notes.insert(notes.end(), d.edits.notes.begin() + after, d.edits.notes.end());

        const bool layout = !sameSpans(spans, orig);
        if (layout) d.edits.spans = spans; // unchanged: the piece keeps its own (possibly empty) layout
        if (layoutChanged != nullptr) (*layoutChanged)[k] = layout;

        // Harmony masks per note, in the new note order.
        auto newMask = [&](const std::vector<uint8_t>& old, const std::vector<uint8_t>& viewMask) {
            std::vector<uint8_t> nm;
            for (int j = 0; j < first; ++j) nm.push_back(maskAt(old, (size_t)j));
            for (size_t i : midIdx) nm.push_back(maskAt(viewMask, i));
            for (int j = after; j < (int)orig.size(); ++j) nm.push_back(maskAt(old, (size_t)j));
            compactMask(nm);
            return nm;
        };
        if (voicesChanged) {
            std::vector<HarmonyVoice> voices = edited.voices;
            for (size_t vv = 0; vv < voices.size(); ++vv)
                voices[vv].noteMask = newMask(vv < d.voices.size() ? d.voices[vv].noteMask : std::vector<uint8_t>{},
                                              edited.voices[vv].noteMask);
            d.voices = std::move(voices);
        } else {
            for (size_t vv = 0; vv < d.voices.size() && vv < edited.voices.size(); ++vv)
                d.voices[vv].noteMask = newMask(d.voices[vv].noteMask, edited.voices[vv].noteMask);
        }
        d.edits.notes = std::move(notes);

        // Pencil: the piece's part comes from the view, the rest of the item stays as it was.
        std::vector<float> dense = drawnDense(d.edits, (int)a.frames.size());
        bool drawChanged = false;
        for (int f = m.frameBegin; f < m.frameEnd; ++f) {
            float& dst = dense[(size_t)(f - off)];
            const float src = vdraw[(size_t)f];
            if (std::isnan(dst) != std::isnan(src) || (!std::isnan(src) && dst != src)) { dst = src; drawChanged = true; }
        }
        if (drawChanged) setDrawnDense(d.edits, dense);

        // Track-wide settings: only those changed in the view are written to every piece.
        auto take = [](auto& dst, const auto& now, const auto& was) { if (!(now == was)) dst = now; };
        take(d.keyTonic, edited.keyTonic, built.keyTonic);
        take(d.keyMinor, edited.keyMinor, built.keyMinor);
        take(d.chromatic, edited.chromatic, built.chromatic);
        take(d.keyMode, edited.keyMode, built.keyMode);
        take(d.snapAmount, edited.snapAmount, built.snapAmount);
        take(d.leadGainDb, edited.leadGainDb, built.leadGainDb);
        take(d.tempoBpm, edited.tempoBpm, built.tempoBpm);
        take(d.beatsPerBar, edited.beatsPerBar, built.beatsPerBar);
        take(d.barOffsetSec, edited.barOffsetSec, built.barOffsetSec);
        const EditSet& ne = edited.edits;
        const EditSet& be = built.edits;
        take(d.edits.sibilantGainDb, ne.sibilantGainDb, be.sibilantGainDb);
        take(d.edits.breathGainDb, ne.breathGainDb, be.breathGainDb);
        take(d.edits.unassignedGainDb, ne.unassignedGainDb, be.unassignedGainDb);
        take(d.edits.globalFormantShift, ne.globalFormantShift, be.globalFormantShift);
        take(d.edits.transitionMs, ne.transitionMs, be.transitionMs);
        take(d.edits.edgeTaperMs, ne.edgeTaperMs, be.edgeTaperMs);
        take(d.edits.keepLength, ne.keepLength, be.keepLength);
        take(d.edits.autoVibrato, ne.autoVibrato, be.autoVibrato);
        take(d.edits.vibratoSeed, ne.vibratoSeed, be.vibratoSeed);
        docs.push_back(std::move(d));
    }
    return docs;
}

} // namespace glina
