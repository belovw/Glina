#include "glina/Editing.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace glina {

namespace {
const NoteEdit kNoEditE{};
constexpr double kMinNoteSec = 0.03;

double minDur(const NoteTimes& t) { return std::min(kMinNoteSec, 0.5 * (t.inEnd - t.inStart)); }

// Overlaps and the clip end are judged with 1 µs of slack in the diagnostic check below.
constexpr double kTolSec = 1e-6;

// Per note, where its output may lie: the item (segment) it belongs to in a track view, widened
// to where it already is (so a layout that starts out of bounds does not block every move).
// No limits for a single clip.
using Bounds = std::vector<std::pair<double, double>>;
Bounds boundsOf(const Analysis& a, const std::vector<NoteTimes>& base)
{
    constexpr double inf = std::numeric_limits<double>::infinity();
    Bounds b(base.size(), {-inf, inf});
    if (a.segments.empty()) return b;
    for (size_t i = 0; i < base.size(); ++i)
        for (const auto& s : a.segments)
            // By the note's middle: its start sits half a frame before its first frame, so a note
            // starting on an item's first frame would otherwise belong to no item (and have no bounds).
            if (const double mid = 0.5 * (base[i].inStart + base[i].inEnd); mid >= s.first && mid < s.second) {
                b[i] = {std::min(s.first, base[i].outStart), std::max(s.second, base[i].outEnd)};
                break;
            }
    return b;
}

// The conditions a layout must meet, each as a margin that is >= 0 when it holds: every note
// keeps its minimal length, starts after the end of every earlier note and after `floorSec`,
// stays inside its item (track view) and ends inside the clip.
// "Every earlier note", pair by pair, not just the previous one: in a layout an earlier build
// left out of order, the previous note may lie far back, and a note could otherwise be put
// onto others. Each condition is a difference of two note edges, so it changes linearly with
// the dt of a time edit (every edit moves edges linearly).
struct Limits {
    double clipLen = 0, floorSec = 0;
    Bounds bounds;
    std::vector<char> ignore; // deleted notes: they do not limit the order of the others
    // A note whose edge, dragged past a hard separation, may go over other notes (they overlap):
    // its end over the later notes (ovLater), or its start back over the earlier ones.
    int ovNote = -1;
    bool ovLater = true;
};

static bool ignored(const Limits& L, size_t i) { return i < L.ignore.size() && L.ignore[i]; }

// A change below this (seconds) is rounding, not a change.
constexpr double kChangeEps = 1e-9;

// Calls f(before, after, note, condition) for every condition whose margin differs between the
// layouts `a` and `b` (same notes). Conditions on edges that did not move are skipped: a pair
// of notes neither of which moved keeps its margin.
template <typename F>
void eachChange(const std::vector<NoteTimes>& a, const std::vector<NoteTimes>& b, const Limits& L, F&& f)
{
    constexpr double kFree = 1e9; // a limit that does not exist (single clip: no item bounds)
    const size_t n = std::min(a.size(), b.size());
    auto cond = [&](double ma, double mb, size_t note, const char* what) {
        if (std::abs(mb - ma) > kChangeEps) f(ma, mb, note, what);
    };
    std::vector<char> startMoved(n), endMoved(n);
    for (size_t i = 0; i < n; ++i) {
        startMoved[i] = std::abs(b[i].outStart - a[i].outStart) > kChangeEps;
        endMoved[i] = std::abs(b[i].outEnd - a[i].outEnd) > kChangeEps;
        if (!startMoved[i] && !endMoved[i]) continue;
        if (!ignored(L, i)) cond(a[i].outEnd - a[i].outStart - minDur(a[i]), b[i].outEnd - b[i].outStart - minDur(b[i]), i, "length");
        cond(a[i].outStart - L.floorSec, b[i].outStart - L.floorSec, i, "clip start");
        cond(L.clipLen - a[i].outEnd, L.clipLen - b[i].outEnd, i, "clip end");
        const bool hasB = i < L.bounds.size();
        const double lo = hasB && std::isfinite(L.bounds[i].first) ? L.bounds[i].first : -kFree;
        const double hi = hasB && std::isfinite(L.bounds[i].second) ? L.bounds[i].second : kFree;
        cond(a[i].outStart - lo, b[i].outStart - lo, i, "item start");
        cond(hi - a[i].outEnd, hi - b[i].outEnd, i, "item end");
    }
    // Note k starts after note j (j < k) ends. Only pairs with a moved edge.
    auto order = [&](size_t j, size_t k) {
        if (ignored(L, j) || ignored(L, k)) return;
        if (L.ovNote >= 0 && (L.ovLater ? (int)j == L.ovNote : (int)k == L.ovNote)) return; // may overlap
        cond(a[k].outStart - a[j].outEnd, b[k].outStart - b[j].outEnd, k, "order");
    };
    for (size_t k = 0; k < n; ++k)
        if (startMoved[k])
            for (size_t j = 0; j < k; ++j) order(j, k);
    for (size_t j = 0; j < n; ++j)
        if (endMoved[j])
            for (size_t k = j + 1; k < n; ++k)
                if (!startMoved[k]) order(j, k);
}

// The full move takes layout t0 to t1, and every margin moves linearly in between:
// m(x) = m0 + x (m1 - m0) for the share x of the move. The largest x in [0, 1] that breaks no
// condition that held and makes no broken one worse is found directly, condition by condition.
// A condition the move does not change does not limit it: a fault elsewhere in the clip (an
// overlap left by an earlier build) does not block moves; a note that overlaps its neighbour
// can still move away from it.
double allowedShare(const std::vector<NoteTimes>& t0, const std::vector<NoteTimes>& t1, const Limits& L)
{
    double x = 1;
    eachChange(t0, t1, L, [&](double m0, double m1, size_t note, const char* what) {
        if (m1 >= m0) return; // better
        const double xk = std::max(0.0, m0 / (m0 - m1));
        if (xk < x) x = xk;
    });
    return x;
}

// True if going from `before` to `after` breaks no condition that held and makes no broken one worse.
bool noNewFaults(const std::vector<NoteTimes>& before, const std::vector<NoteTimes>& after, const Limits& L)
{
    bool ok = true;
    eachChange(before, after, L, [&](double m0, double m1, size_t, const char*) {
        if (m1 < -kChangeEps && m1 < m0) ok = false;
    });
    return ok;
}

// The largest part of `dt` that `make` may apply (see allowedShare).
template <typename Make>
double limitDt(double dt, const Limits& L, Make&& make)
{
    const double x = allowedShare(make(0.0), make(dt), L);
    return x >= 1 ? dt : dt * x;
}

// The last note may end a fraction of a millisecond past the clip (frame centres), and an
// earlier build may have left a note past it: the limit is the latest of these.
double clipLenOf(const Analysis& a, const std::vector<NoteTimes>& base)
{
    double len = (double)a.numSamples / a.sampleRate;
    for (const NoteTimes& n : base) len = std::max(len, n.outEnd);
    return len;
}

// The layout may not start before the clip, nor before where it already starts.
double floorOf(const std::vector<NoteTimes>& base)
{
    double f = 0;
    for (const NoteTimes& n : base) f = std::min(f, n.outStart);
    return f;
}

Limits limitsOf(const Analysis& a, const std::vector<NoteTimes>& base)
{
    Limits L;
    L.clipLen = clipLenOf(a, base);
    L.floorSec = floorOf(base);
    L.bounds = boundsOf(a, base);
    return L;
}

// Lay out the block of notes [b, e] (taken from `base`) so that it starts at
// `start` and ends at `end`. The gaps between its notes (consonants, pauses)
// keep their length; the notes share the change in proportion to their durations.
// Consonant notes keep their own duration; only the vowel notes share the change.
void layBlock(std::vector<NoteTimes>& t, const std::vector<NoteTimes>& base, const EditSet& ed, int b, int e,
              double start, double end)
{
    auto fixed = [&](int i) { return i < (int)ed.notes.size() && ed.notes[(size_t)i].consonant; };
    double notes = 0, gaps = 0;
    for (int i = b; i <= e; ++i) {
        if (!fixed(i)) notes += base[(size_t)i].outEnd - base[(size_t)i].outStart;
        if (i > b) gaps += base[(size_t)i].outStart - base[(size_t)i - 1].outEnd;
    }
    const double k = notes > 0 ? (end - start - gaps) / notes : 1.0;
    double pos = start;
    for (int i = b; i <= e; ++i) {
        if (i > b) pos += base[(size_t)i].outStart - base[(size_t)i - 1].outEnd;
        const double scale = fixed(i) ? 1.0 : k;
        const double dur = (base[(size_t)i].outEnd - base[(size_t)i].outStart) * scale;
        t[(size_t)i].outStart = pos;
        t[(size_t)i].outEnd = pos + dur;
        pos += dur;
    }
}
} // namespace

std::vector<NoteTimes> noteTimes(const Analysis& a, const EditSet& e)
{
    std::vector<NoteTimes> t(a.notes.size());
    for (size_t i = 0; i < a.notes.size(); ++i) {
        const NoteEdit& ed = i < e.notes.size() ? e.notes[i] : kNoEditE;
        NoteTimes& n = t[i];
        n.inStart = a.notes[i].startSec;
        n.inEnd = a.notes[i].endSec;
        n.outStart = n.inStart + ed.timeOffsetSec;
        n.outEnd = n.outStart + (n.inEnd - n.inStart) * ed.stretch;
    }
    return t;
}

double minNoteLength(const NoteTimes& t) { return minDur(t); }

bool notesInOneItem(const Analysis& a, int i)
{
    if (i < 0 || i + 1 >= (int)a.notes.size()) return false;
    if (a.segments.empty()) return true;
    auto segOf = [&](const Note& m) {
        const double mid = 0.5 * (m.startSec + m.endSec);
        for (size_t s = 0; s < a.segments.size(); ++s)
            if (mid >= a.segments[s].first && mid < a.segments[s].second) return (int)s;
        return -1;
    };
    return segOf(a.notes[(size_t)i]) == segOf(a.notes[(size_t)i + 1]);
}

namespace {
// Soft separation between note i and i + 1 in layout t (see NoteEdit::sepBefore).
bool soft(const EditSet& e, const Analysis& a, const std::vector<NoteTimes>& t, int i)
{
    if (i < 0 || i + 1 >= (int)t.size()) return false;
    // A deleted note is not linked to its neighbours: its boundaries are hard, so it does not move with them.
    if ((size_t)i + 1 < e.notes.size() && (e.notes[(size_t)i].deleted || e.notes[(size_t)i + 1].deleted)) return false;
    const NoteEdit::Sep s = (size_t)i + 1 < e.notes.size() ? e.notes[(size_t)i + 1].sepBefore : NoteEdit::Sep::Auto;
    if (s != NoteEdit::Sep::Auto) return s == NoteEdit::Sep::Soft;
    // Auto is soft: only the user makes hard separations. Notes of different items (track view)
    // stay apart: each item is its own recording.
    return notesInOneItem(a, i);
}
} // namespace

Limits limitsOf(const Analysis& a, const std::vector<NoteTimes>& base, const EditSet& e)
{
    Limits L = limitsOf(a, base);
    L.ignore.assign(base.size(), 0);
    for (size_t k = 0; k < base.size() && k < e.notes.size(); ++k) L.ignore[k] = e.notes[k].deleted;
    return L;
}

bool softSeparation(const EditSet& e, const Analysis& a, int i)
{
    return soft(e, a, noteTimes(a, e), i);
}

bool noNewLayoutFaults(const Analysis& a, const std::vector<NoteTimes>& before, const std::vector<NoteTimes>& after)
{
    return noNewFaults(before, after, limitsOf(a, before));
}

bool noNewLayoutFaults(const Analysis& a, const EditSet& e, const std::vector<NoteTimes>& before,
                       const std::vector<NoteTimes>& after)
{
    return noNewFaults(before, after, limitsOf(a, before, e));
}

std::vector<bool> softSeparations(const EditSet& e, const Analysis& a)
{
    const std::vector<NoteTimes> t = noteTimes(a, e);
    std::vector<bool> out(t.empty() ? 0 : t.size() - 1);
    for (size_t i = 0; i < out.size(); ++i) out[i] = soft(e, a, t, (int)i);
    return out;
}

void toggleSeparation(EditSet& e, const Analysis& a, int i)
{
    if (i < 0 || i + 1 >= (int)a.notes.size()) return;
    if (e.notes.size() < a.notes.size()) e.notes.resize(a.notes.size());
    const bool wasSoft = softSeparation(e, a, i);
    e.notes[(size_t)i + 1].sepBefore = wasSoft ? NoteEdit::Sep::Hard : NoteEdit::Sep::Soft;
}

void setNoteTimes(NoteEdit& e, const NoteTimes& t)
{
    const double inDur = std::max(1e-6, t.inEnd - t.inStart);
    double off = t.outStart - t.inStart;
    double st = (t.outEnd - t.outStart) / inDur;
    if (std::abs(off) < 1e-7) off = 0;
    if (std::abs(st - 1) < 1e-7) st = 1;
    e.timeOffsetSec = off;
    e.stretch = st;
}

void applyNoteTimes(EditSet& e, const Analysis& a, const std::vector<NoteTimes>& t)
{
    if (e.notes.size() < a.notes.size()) e.notes.resize(a.notes.size());
    for (size_t i = 0; i < t.size() && i < e.notes.size(); ++i) setNoteTimes(e.notes[i], t[i]);
}

bool placeNotes(EditSet& e, const Analysis& a, std::vector<NoteTimes> t)
{
    const std::vector<NoteTimes> base = noteTimes(a, e);
    const size_t n = base.size();
    if (t.size() != n) return false;
    if (n == 0) return true;
    const Limits L = limitsOf(a, base, e); // the clip, the items; deleted notes are ignored
    auto consonant = [&](size_t i) { return i < e.notes.size() && e.notes[i].consonant; };
    auto dur = [&](size_t i) { return base[i].outEnd - base[i].outStart; }; // a consonant keeps it
    // As in the time edits: a deleted note gives way to every other (it keeps its own times), and
    // two notes that already overlap (an overlay, or a layout left by an earlier build) may go on
    // overlapping. Every other pair keeps its order.
    auto ordered = [&](size_t j, size_t k) { // j < k
        return !ignored(L, j) && !ignored(L, k) && base[k].outStart >= base[j].outEnd - kTolSec;
    };
    const double slack = 2e-6; // inside the check's tolerance
    // Forward: order, minimal length, the start limits.
    for (size_t k = 0; k < n; ++k) {
        NoteTimes& x = t[k];
        if (ignored(L, k)) { x = base[k]; continue; }
        double lo = std::max(L.floorSec, L.bounds[k].first);
        for (size_t j = 0; j < k; ++j)
            if (ordered(j, k)) lo = std::max(lo, t[j].outEnd);
        x.outStart = std::max(x.outStart, lo);
        if (consonant(k)) x.outEnd = x.outStart + dur(k);
        else x.outEnd = std::max(x.outEnd, x.outStart + minDur(x) + slack);
    }
    // Backward: the end limits (the clip, the item); a note pushed back pushes the ones before it.
    for (size_t k = n; k-- > 0;) {
        if (ignored(L, k)) continue;
        NoteTimes& x = t[k];
        double hi = std::min(L.clipLen, L.bounds[k].second);
        for (size_t m = k + 1; m < n; ++m)
            if (ordered(k, m)) hi = std::min(hi, t[m].outStart);
        if (x.outEnd > hi) {
            x.outEnd = hi;
            const double len = consonant(k) ? dur(k) : minDur(x) + slack;
            x.outStart = std::min(x.outStart, x.outEnd - len);
        }
    }
    // The result must meet every condition the passes keep.
    for (size_t k = 0; k < n; ++k) {
        if (ignored(L, k)) continue;
        const NoteTimes& x = t[k];
        if (x.outEnd - x.outStart < minDur(x) - kTolSec) return false;
        if (x.outStart < L.floorSec - kTolSec || x.outEnd > L.clipLen + kTolSec) return false;
        if (x.outStart < L.bounds[k].first - kTolSec || x.outEnd > L.bounds[k].second + kTolSec) return false;
        for (size_t j = 0; j < k; ++j)
            if (ordered(j, k) && x.outStart < t[j].outEnd - kTolSec) return false;
    }
    applyNoteTimes(e, a, t);
    return true;
}

double moveBoundary(EditSet& e, const Analysis& a, int left, double dt, const std::set<int>& sel, bool startEdge,
                    bool overlap)
{
    const int n = (int)a.notes.size();
    if (n == 0 || left < -1 || left > n - 1 || dt == 0) return 0;
    const std::vector<NoteTimes> base = noteTimes(a, e);

    // Selected notes touching the boundary form one block on each side.
    int L0 = left, R1 = left + 1;
    if (left >= 0 && sel.count(left)) while (L0 - 1 >= 0 && sel.count(L0 - 1)) --L0;
    if (left + 1 < n && sel.count(left + 1)) while (R1 + 1 < n && sel.count(R1 + 1)) ++R1;

    // Across a soft separation the two notes move together. Across a hard one (by default
    // where there is a gap) only the note whose edge is dragged changes; the neighbour stays.
    const bool touching = soft(e, a, base, left);

    const bool moveLeft = left >= 0 && (touching || !startEdge);
    // A selected note across a hard separation moves with the dragged edge too; unselected
    // neighbours behind a hard separation stay put.
    const bool rightSelected = left + 1 < n && sel.count(left + 1) > 0;
    const bool moveRight = left + 1 < n && (touching || startEdge || left < 0 || rightSelected);
    auto make = [&](double d) {
        std::vector<NoteTimes> t = base;
        if (moveLeft) // left block: start fixed, end follows the boundary
            layBlock(t, base, e, L0, left, base[(size_t)L0].outStart, base[(size_t)left].outEnd + d);
        if (moveRight) // right block: end fixed; its start (and the consonant before it) follows
            layBlock(t, base, e, left + 1, R1, base[(size_t)left + 1].outStart + d, base[(size_t)R1].outEnd);
        return t;
    };
    // Past a hard separation (a gap, or deleted notes) the dragged edge may go over the notes
    // beyond: they overlap; those notes are neither moved nor squeezed.
    Limits lim = limitsOf(a, base, e);
    if (overlap && !touching) {
        if (!startEdge && left >= 0 && !rightSelected) {
            lim.ovNote = left;
            lim.ovLater = true;
        } else if (startEdge && left + 1 < n && !(left >= 0 && sel.count(left))) {
            lim.ovNote = left + 1;
            lim.ovLater = false;
        }
    }
    const double applied = limitDt(dt, lim, make);
    applyNoteTimes(e, a, make(applied));
    return applied;
}

namespace {
// Places notes with one or two edges moved, the move clamped to what the layout allows; halves it
// until placeNotes accepts (a consonant keeps its length, items and the clip bound the notes).
double placeEdges(EditSet& e, const Analysis& a, int left, double dt, bool moveEnd, bool moveStart, double lo, double hi)
{
    dt = std::clamp(dt, lo, hi);
    const std::vector<NoteTimes> base = noteTimes(a, e);
    for (int tries = 0; tries < 12 && std::abs(dt) > 1e-5; ++tries, dt *= 0.5) {
        std::vector<NoteTimes> t = base;
        if (moveEnd) t[(size_t)left].outEnd += dt;
        if (moveStart) t[(size_t)left + 1].outStart += dt;
        EditSet trial = e;
        if (placeNotes(trial, a, t)) { e = trial; return dt; }
    }
    return 0;
}
constexpr double kMinPauseSec = 0.010;
} // namespace

double movePause(EditSet& e, const Analysis& a, int left, double dt)
{
    const int n = (int)a.notes.size();
    if (left < 0 || left + 1 >= n || dt == 0) return 0;
    if (e.notes.size() < (size_t)n) e.notes.resize((size_t)n);
    const std::vector<NoteTimes> t = noteTimes(a, e);
    const NoteTimes& l = t[(size_t)left];
    const NoteTimes& r = t[(size_t)left + 1];
    // The left note keeps its minimal length, so does the right one.
    const double lo = l.outStart + minNoteLength(l) - l.outEnd;
    const double hi = r.outEnd - minNoteLength(r) - r.outStart;
    return placeEdges(e, a, left, dt, true, true, std::min(0.0, lo), std::max(0.0, hi));
}

double stretchPause(EditSet& e, const Analysis& a, int left, bool rightEdge, double dt)
{
    const int n = (int)a.notes.size();
    if (left < 0 || left + 1 >= n || dt == 0) return 0;
    if (e.notes.size() < (size_t)n) e.notes.resize((size_t)n);
    const std::vector<NoteTimes> t = noteTimes(a, e);
    const NoteTimes& l = t[(size_t)left];
    const NoteTimes& r = t[(size_t)left + 1];
    const double pause = r.outStart - l.outEnd;
    double lo, hi;
    if (rightEdge) { // the next note's start: not into the pause's last 10 ms, the note keeps its length
        lo = std::min(0.0, kMinPauseSec - pause);
        hi = std::max(0.0, r.outEnd - minNoteLength(r) - r.outStart);
    } else {         // the last note's end
        lo = std::min(0.0, l.outStart + minNoteLength(l) - l.outEnd);
        hi = std::max(0.0, pause - kMinPauseSec);
    }
    return placeEdges(e, a, left, dt, !rightEdge, rightEdge, lo, hi);
}

double moveNotes(EditSet& e, const Analysis& a, const std::set<int>& notes, double dt)
{
    const int n = (int)a.notes.size();
    if (n == 0 || notes.empty() || dt == 0) return 0;
    const std::vector<NoteTimes> base = noteTimes(a, e);
    auto moved = [&](int i) { return i >= 0 && i < n && notes.count(i) > 0; };

    // Across a soft separation (by default where notes touch) the neighbour follows the move.
    // Across a hard one (by default where there is a gap) the neighbour stays put.
    auto touch = [&](int i) { return soft(e, a, base, i); };
    auto make = [&](double d) {
        std::vector<NoteTimes> t = base;
        for (int i = 0; i < n; ++i) {
            if (moved(i)) { t[(size_t)i].outStart += d; t[(size_t)i].outEnd += d; continue; }
            if (moved(i - 1) && touch(i - 1)) t[(size_t)i].outStart += d;
            if (moved(i + 1) && touch(i)) t[(size_t)i].outEnd += d;
        }
        return t;
    };
    const double applied = limitDt(dt, limitsOf(a, base, e), make);
    applyNoteTimes(e, a, make(applied));
    return applied;
}

void resetNoteEdits(EditSet& e, const Analysis& a, const std::set<int>& notes)
{
    const int n = (int)a.notes.size();
    if (n == 0 || notes.empty()) return;
    if (e.notes.size() < a.notes.size()) e.notes.resize(a.notes.size());
    const std::vector<NoteTimes> base = noteTimes(a, e);
    const Limits L = limitsOf(a, base, e);

    // Timing: each run of consecutive notes goes back to where it was recorded, as one block;
    // a neighbour touching the run follows its edge, as in a move. If that would overlap or
    // squeeze a neighbour (the run was moved as part of a larger block), the run keeps its timing.
    std::vector<NoteTimes> t = base;
    for (auto it = notes.begin(); it != notes.end();) {
        const int b = *it;
        int r = b;
        for (++it; it != notes.end() && *it == r + 1; ++it) ++r;
        if (b < 0 || r >= n) continue;
        std::vector<NoteTimes> c = t;
        for (int i = b; i <= r; ++i) { c[(size_t)i].outStart = c[(size_t)i].inStart; c[(size_t)i].outEnd = c[(size_t)i].inEnd; }
        // A neighbour across a soft separation follows the run's edge, keeping the gap (none if they touch).
        if (b > 0 && soft(e, a, t, b - 1)) c[(size_t)b - 1].outEnd = c[(size_t)b].outStart - (t[(size_t)b].outStart - t[(size_t)b - 1].outEnd);
        if (r + 1 < n && soft(e, a, t, r)) c[(size_t)r + 1].outStart = c[(size_t)r].outEnd + (t[(size_t)r + 1].outStart - t[(size_t)r].outEnd);
        if (noNewFaults(t, c, L)) t = c;
    }

    // Everything else goes back to no edit; the drawn pitch on the notes is erased.
    for (int i : notes) {
        if (i < 0 || i >= n) continue;
        const NoteEdit::Sep sep = e.notes[(size_t)i].sepBefore; // the separation is layout, not a sound edit
        e.notes[(size_t)i] = NoteEdit{};
        e.notes[(size_t)i].sepBefore = sep;
        eraseDrawn(e, a.notes[(size_t)i].startFrame, a.notes[(size_t)i].endFrame);
    }
    for (int i = 0; i < n; ++i) {
        const NoteTimes& was = base[(size_t)i];
        const NoteTimes& now = t[(size_t)i];
        if (notes.count(i) || now.outStart != was.outStart || now.outEnd != was.outEnd) setNoteTimes(e.notes[(size_t)i], now);
    }
}

void setNoteDuration(EditSet& e, const Analysis& a, int i, double seconds)
{
    if (i < 0 || i >= (int)a.notes.size()) return;
    const auto t = noteTimes(a, e);
    moveBoundary(e, a, i, seconds - (t[(size_t)i].outEnd - t[(size_t)i].outStart), {});
}

void setConsonant(EditSet& e, const Analysis& a, int i, bool on)
{
    if (i < 0 || i >= (int)a.notes.size()) return;
    if (e.notes.size() < a.notes.size()) e.notes.resize(a.notes.size());
    if (on) {
        // Bring the note back to its original length first, so the flag freezes it there.
        const double inDur = a.notes[(size_t)i].endSec - a.notes[(size_t)i].startSec;
        setNoteDuration(e, a, i, inDur);
        e.notes[(size_t)i].stretch = 1;
    }
    e.notes[(size_t)i].consonant = on;
}

double transitionWidthMs(double s, double baseMs)
{
    if (baseMs <= 0) baseMs = 45;
    s = std::clamp(s, 0.0, 1.0);
    if (s <= 0.5) return baseMs * s / 0.5;
    return baseMs * std::pow(300.0 / baseMs, (s - 0.5) / 0.5); // up to a 300 ms glide
}

// ---- tilt ---------------------------------------------------------------------------
double tiltAt(const Note& n, const NoteEdit& e, double frame)
{
    if (e.consonant || (e.tiltL == 0 && e.tiltR == 0)) return 0.0;
    const double len = std::max(1, n.endFrame - n.startFrame);
    const double u = std::clamp((frame + 0.5 - n.startFrame) / len, 0.0, 1.0);
    return e.tiltL + (e.tiltR - e.tiltL) * u;
}

double tiltMean(const Analysis& a, int i, const NoteEdit& e)
{
    if (i < 0 || i >= (int)a.notes.size() || e.consonant || (e.tiltL == 0 && e.tiltR == 0)) return 0.0;
    const Note& n = a.notes[(size_t)i];
    double sum = 0, wsum = 0;
    for (int k = std::max(0, n.startFrame); k < std::min(n.endFrame, (int)a.frames.size()); ++k) {
        const Frame& f = a.frames[(size_t)k];
        if (f.f0 <= 0) continue;
        const double w = std::pow(10.0, f.rmsDb / 20.0);
        sum += w * tiltAt(n, e, k);
        wsum += w;
    }
    return wsum > 0 ? sum / wsum : 0.5 * (e.tiltL + e.tiltR);
}

double editedCenter(const Analysis& a, const EditSet& e, int i)
{
    if (i < 0 || i >= (int)a.notes.size()) return 0.0;
    const NoteEdit& ed = i < (int)e.notes.size() ? e.notes[(size_t)i] : kNoEditE;
    return a.notes[(size_t)i].centerMidi + ed.pitchOffset + tiltMean(a, i, ed);
}

void addTilt(EditSet& e, const Analysis& a, const std::set<int>& notes, bool rightCorner, double semis, int apex)
{
    auto usable = [&](int i) {
        return i >= 0 && i < (int)e.notes.size() && i < (int)a.notes.size() && !e.notes[(size_t)i].consonant;
    };
    int g0 = std::numeric_limits<int>::max(), g1 = std::numeric_limits<int>::min();
    for (int i : notes)
        if (usable(i)) { g0 = std::min(g0, a.notes[(size_t)i].startFrame); g1 = std::max(g1, a.notes[(size_t)i].endFrame); }
    if (g1 <= g0) return;
    // The line in frames, as tiltAt() measures each note: edges at startFrame and endFrame.
    // A roof: 0 at both ends of the group, `semis` at the apex, which is the dragged note's
    // edge (apex >= 0), or the group's dragged end when the apex is not given (one note).
    double apexF = rightCorner ? g1 : g0;
    if (usable(apex)) apexF = rightCorner ? a.notes[(size_t)apex].endFrame : a.notes[(size_t)apex].startFrame;
    apexF = std::clamp(apexF, (double)g0, (double)g1);
    auto line = [&](double frame) {
        if (apexF > g0 && frame <= apexF) return semis * (frame - g0) / (apexF - g0);
        if (apexF < g1) return semis * (g1 - frame) / (g1 - apexF);
        return semis * (frame - g0) / (double)(g1 - g0);
    };
    for (int i : notes) {
        if (!usable(i)) continue;
        NoteEdit& n = e.notes[(size_t)i];
        n.tiltL += line(a.notes[(size_t)i].startFrame);
        n.tiltR += line(a.notes[(size_t)i].endFrame);
    }
}

// ---- auto vibrato -------------------------------------------------------------------------
const AutoVibrato& vibratoOf(const EditSet& e, int i)
{
    if (i >= 0 && i < (int)e.notes.size() && e.notes[(size_t)i].ownVibrato) return e.notes[(size_t)i].vibrato;
    return e.autoVibrato;
}

void deleteNotes(EditSet& e, const Analysis& a, const std::set<int>& notes)
{
    if (e.notes.size() < a.notes.size()) e.notes.resize(a.notes.size());
    for (int i : notes) {
        if (i < 0 || i >= (int)a.notes.size()) continue;
        NoteEdit& n = e.notes[(size_t)i];
        n.deleted = true;
        n.consMuted = true;
    }
}

void restoreDeleted(EditSet& e, int left)
{
    for (int i : {left, left + 1}) {
        if (i < 0 || i >= (int)e.notes.size() || !e.notes[(size_t)i].deleted) continue;
        e.notes[(size_t)i].deleted = false;
        e.notes[(size_t)i].consMuted = false;
    }
    // The breath before note left + 1 (the unvoiced sound at this boundary) comes back too.
    if (left + 1 >= 0 && left + 1 < (int)e.notes.size()) e.notes[(size_t)left + 1].breathDeleted = false;
}

void deleteBreaths(EditSet& e, const Analysis& a, const std::set<int>& notes)
{
    if (e.notes.size() < a.notes.size()) e.notes.resize(a.notes.size());
    for (int i : notes)
        if (i >= 0 && i < (int)a.notes.size()) e.notes[(size_t)i].breathDeleted = true;
}

void setTrackVibrato(EditSet& e, const AutoVibrato& v, bool resetNotes)
{
    e.autoVibrato = v;
    if (resetNotes)
        for (auto& n : e.notes) { n.ownVibrato = false; n.vibrato = AutoVibrato{}; }
}

void setNoteVibrato(EditSet& e, const std::set<int>& notes, const AutoVibrato& v)
{
    for (int i : notes) {
        if (i < 0 || i >= (int)e.notes.size()) continue;
        e.notes[(size_t)i].ownVibrato = true;
        e.notes[(size_t)i].vibrato = v;
    }
}

// ---- pencil -------------------------------------------------------------------------
std::vector<float> drawnDense(const EditSet& e, int numFrames)
{
    std::vector<float> d((size_t)std::max(0, numFrames), std::numeric_limits<float>::quiet_NaN());
    for (const auto& r : e.draw)
        for (size_t k = 0; k < r.delta.size(); ++k) {
            const long f = r.start + (long)k;
            if (f >= 0 && f < numFrames) d[(size_t)f] = r.delta[k];
        }
    return d;
}

void setDrawnDense(EditSet& e, const std::vector<float>& d)
{
    e.draw.clear();
    for (size_t f = 0; f < d.size();) {
        if (std::isnan(d[f])) { ++f; continue; }
        DrawRun r;
        r.start = (int)f;
        while (f < d.size() && !std::isnan(d[f])) r.delta.push_back(d[f++]);
        e.draw.push_back(std::move(r));
    }
}

void drawPitch(EditSet& e, const Analysis& a, int firstFrame, const std::vector<float>& midi)
{
    const int nf = (int)a.frames.size();
    std::vector<float> d = drawnDense(e, nf);
    for (size_t k = 0; k < midi.size(); ++k) {
        const int f = firstFrame + (int)k;
        if (f < 0 || f >= nf || std::isnan(midi[k])) continue;
        const int ni = a.noteOfFrame.empty() ? -1 : a.noteOfFrame[(size_t)f];
        if (ni < 0 || a.frames[(size_t)f].f0 <= 0) continue;
        const double off = ni < (int)e.notes.size()
                               ? e.notes[(size_t)ni].pitchOffset + tiltAt(a.notes[(size_t)ni], e.notes[(size_t)ni], f)
                               : 0.0;
        d[(size_t)f] = (float)(midi[k] - (a.frames[(size_t)f].midi + off));
    }
    setDrawnDense(e, d);
}

void eraseDrawn(EditSet& e, int f0, int f1)
{
    if (e.draw.empty()) return;
    int nf = 0;
    for (const auto& r : e.draw) nf = std::max(nf, r.start + (int)r.delta.size());
    std::vector<float> d = drawnDense(e, nf);
    for (int f = std::max(0, f0); f < std::min(f1, nf); ++f) d[(size_t)f] = std::numeric_limits<float>::quiet_NaN();
    setDrawnDense(e, d);
}

} // namespace glina
