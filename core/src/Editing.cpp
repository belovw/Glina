#include "glina/Editing.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace glina {

namespace {
const NoteEdit kNoEditE{};
constexpr double kMinNoteSec = 0.03;

double minDur(const NoteTimes& t) { return std::min(kMinNoteSec, 0.5 * (t.inEnd - t.inStart)); }

// Every note keeps a minimal length, nothing overlaps, nothing leaves the clip.
bool valid(const std::vector<NoteTimes>& t, double clipLen)
{
    double last = 0;
    for (const auto& n : t) {
        if (n.outEnd - n.outStart < minDur(n) - 1e-9) return false;
        if (n.outStart < last - 1e-9) return false;
        last = n.outEnd;
    }
    return t.empty() || last <= clipLen + 1e-9;
}

// Largest part of `dt` (shrinking towards 0) for which `make` gives a valid layout.
template <typename Make>
double clampDt(double dt, double clipLen, Make&& make)
{
    if (valid(make(dt), clipLen)) return dt;
    double lo = 0, hi = 1;
    for (int it = 0; it < 40; ++it) {
        const double mid = 0.5 * (lo + hi);
        if (valid(make(dt * mid), clipLen)) lo = mid; else hi = mid;
    }
    return dt * lo;
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

void applyNoteTimes(EditSet& e, const Analysis& a, const std::vector<NoteTimes>& t)
{
    if (e.notes.size() < a.notes.size()) e.notes.resize(a.notes.size());
    for (size_t i = 0; i < t.size() && i < e.notes.size(); ++i) {
        const double inDur = std::max(1e-6, t[i].inEnd - t[i].inStart);
        double off = t[i].outStart - t[i].inStart;
        double st = (t[i].outEnd - t[i].outStart) / inDur;
        if (std::abs(off) < 1e-7) off = 0;
        if (std::abs(st - 1) < 1e-7) st = 1;
        e.notes[i].timeOffsetSec = off;
        e.notes[i].stretch = st;
    }
}

double moveBoundary(EditSet& e, const Analysis& a, int left, double dt, const std::set<int>& sel)
{
    const int n = (int)a.notes.size();
    if (n == 0 || left < -1 || left > n - 1 || dt == 0) return 0;
    const std::vector<NoteTimes> base = noteTimes(a, e);
    const double clipLen = (double)a.numSamples / a.sampleRate;

    // Selected notes touching the boundary form one block on each side.
    int L0 = left, R1 = left + 1;
    if (left >= 0 && sel.count(left)) while (L0 - 1 >= 0 && sel.count(L0 - 1)) --L0;
    if (left + 1 < n && sel.count(left + 1)) while (R1 + 1 < n && sel.count(R1 + 1)) ++R1;

    // Notes that touch at the boundary move together. If they do not touch (a gap
    // between them), only the note whose edge is dragged changes; the neighbour stays.
    const bool touching = left >= 0 && left + 1 < n &&
                          std::abs(base[(size_t)left + 1].outStart - base[(size_t)left].outEnd) < 0.005;

    auto make = [&](double d) {
        std::vector<NoteTimes> t = base;
        if (left >= 0) // left block: start fixed, end follows the boundary
            layBlock(t, base, e, L0, left, base[(size_t)L0].outStart, base[(size_t)left].outEnd + d);
        if (left + 1 < n && touching) // right block: end fixed; its start (and the consonant before it) follows
            layBlock(t, base, e, left + 1, R1, base[(size_t)left + 1].outStart + d, base[(size_t)R1].outEnd);
        else if (left + 1 < n && left < 0) // dragging the start of the first note
            layBlock(t, base, e, left + 1, R1, base[(size_t)left + 1].outStart + d, base[(size_t)R1].outEnd);
        return t;
    };
    const double applied = clampDt(dt, clipLen, make);
    applyNoteTimes(e, a, make(applied));
    return applied;
}

double moveNotes(EditSet& e, const Analysis& a, const std::set<int>& notes, double dt)
{
    const int n = (int)a.notes.size();
    if (n == 0 || notes.empty() || dt == 0) return 0;
    const std::vector<NoteTimes> base = noteTimes(a, e);
    const double clipLen = (double)a.numSamples / a.sampleRate;
    auto moved = [&](int i) { return i >= 0 && i < n && notes.count(i) > 0; };

    auto make = [&](double d) {
        std::vector<NoteTimes> t = base;
        for (int i = 0; i < n; ++i) {
            if (moved(i)) { t[(size_t)i].outStart += d; t[(size_t)i].outEnd += d; continue; }
            // Neighbours absorb: the gap next to a moved note travels with it.
            if (moved(i - 1)) t[(size_t)i].outStart += d;
            if (moved(i + 1)) t[(size_t)i].outEnd += d;
        }
        return t;
    };
    const double applied = clampDt(dt, clipLen, make);
    applyNoteTimes(e, a, make(applied));
    return applied;
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
        const double off = ni < (int)e.notes.size() ? e.notes[(size_t)ni].pitchOffset : 0.0;
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
