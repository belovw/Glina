#include "glina/Renderer.h"
#include "glina/Editing.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace glina {

namespace {

inline double dbToLin(double db) { return std::pow(10.0, db / 20.0); }

// Raised-cosine smoothing (two moving-average passes) inside [b, e).
void smoothRange(std::vector<float>& v, int b, int e, int half)
{
    if (half <= 0 || e - b < 3) return;
    std::vector<double> tmp(v.begin() + b, v.begin() + e);
    for (int pass = 0; pass < 2; ++pass) {
        const int n = (int)tmp.size();
        std::vector<double> pre(n + 1, 0.0);
        for (int i = 0; i < n; ++i) pre[i + 1] = pre[i] + tmp[i];
        for (int i = 0; i < n; ++i) {
            const int h = std::min({half, i, n - 1 - i});
            tmp[i] = (pre[i + h + 1] - pre[i - h]) / (2 * h + 1);
        }
    }
    for (int i = b; i < e; ++i) v[i] = (float)tmp[i - b];
}

inline double cubicAt(const float* x, size_t n, double t)
{
    const long i = (long)std::floor(t);
    const double f = t - i;
    auto s = [&](long k) -> double { return (k >= 0 && (size_t)k < n) ? x[k] : 0.0; };
    const double y0 = s(i - 1), y1 = s(i), y2 = s(i + 1), y3 = s(i + 2);
    if (f == 0.0) return y1;
    const double a = -0.5 * y0 + 1.5 * y1 - 1.5 * y2 + 0.5 * y3;
    const double b = y0 - 2.5 * y1 + 2.0 * y2 - 0.5 * y3;
    const double c = -0.5 * y0 + 0.5 * y2;
    return ((a * f + b) * f + c) * f + y1;
}

inline float ctrlAt(const std::vector<float>& v, double frame)
{
    if (v.empty()) return 0.f;
    const int nf = (int)v.size();
    const int k0 = std::clamp((int)std::floor(frame), 0, nf - 1);
    const int k1 = std::min(k0 + 1, nf - 1);
    const float w = (float)std::clamp(frame - k0, 0.0, 1.0);
    return v[k0] * (1 - w) + v[k1] * w;
}

const NoteEdit kNoEdit{};

} // namespace

// ---------------------------------------------------------------------------
TimeMap TimeMap::build(const Analysis& a, const EditSet& e)
{
    TimeMap m;
    const double sr = a.sampleRate;
    m.in_.push_back(0);
    m.out_.push_back(0);
    double lastIn = 0, lastOut = 0;
    for (size_t i = 0; i < a.notes.size(); ++i) {
        const NoteEdit& ed = i < e.notes.size() ? e.notes[i] : kNoEdit;
        if (ed.timeOffsetSec != 0 || ed.stretch != 1) m.identity_ = false;
        const double ai = std::max(lastIn + 1, a.notes[i].startSec * sr);
        const double bi = std::max(ai + 1, a.notes[i].endSec * sr);
        // A note keeps its own position unless moved; a stretched or moved
        // neighbour that would overlap pushes it later instead.
        double ao = ai + ed.timeOffsetSec * sr;
        ao = std::max(ao, lastOut + 0.1 * (ai - lastIn));
        double bo = ao + (bi - ai) * std::max(0.05, ed.stretch);
        bo = std::max(bo, ao + 0.1 * (bi - ai));
        m.in_.push_back(ai); m.out_.push_back(ao);
        // Only the steady middle of a note is stretched: the attack and the
        // transition into the next note keep their timing, so transients stay crisp.
        const double din = bi - ai, dout = bo - ao;
        const double att = std::min(0.040 * sr, 0.30 * din), rel = std::min(0.030 * sr, 0.20 * din);
        if (std::abs(dout - din) > 1 && dout - att - rel > 0.15 * (din - att - rel)) {
            m.in_.push_back(ai + att); m.out_.push_back(ao + att);
            m.in_.push_back(bi - rel); m.out_.push_back(bo - rel);
        }
        m.in_.push_back(bi); m.out_.push_back(bo);
        lastIn = bi; lastOut = bo;
    }
    const double end = std::max((double)a.numSamples, lastIn + 1);
    m.in_.push_back(end);
    // The clip keeps its length (in a DAW the item does not grow): the tail after
    // the last note absorbs any change, unless notes were pushed past the end.
    m.out_.push_back(e.keepLength ? std::max(end, lastOut + 0.1 * (end - lastIn)) : lastOut + (end - lastIn));
    if (m.identity_) { m.in_ = {0, end}; m.out_ = {0, end}; }
    return m;
}

static double interpMap(const std::vector<double>& xs, const std::vector<double>& ys, double t)
{
    if (xs.size() < 2) return t;
    if (t <= xs.front()) return ys.front() + (t - xs.front());
    if (t >= xs.back()) return ys.back() + (t - xs.back());
    const size_t j = std::upper_bound(xs.begin(), xs.end(), t) - xs.begin();
    const double x0 = xs[j - 1], x1 = xs[j], y0 = ys[j - 1], y1 = ys[j];
    return x1 > x0 ? y0 + (y1 - y0) * (t - x0) / (x1 - x0) : y0;
}

double TimeMap::inToOut(double t) const { return identity_ ? t : interpMap(in_, out_, t); }
double TimeMap::outToIn(double t) const { return identity_ ? t : interpMap(out_, in_, t); }

// ---------------------------------------------------------------------------
Controls buildControls(const Analysis& a, const EditSet& e)
{
    const int nf = (int)a.frames.size();
    const double hopMs = 1000.0 * a.hop / a.sampleRate;
    Controls c;
    c.pitchSemis.assign(nf, 0.f);
    c.formantSemis.assign(nf, 0.f);
    c.gainDb.assign(nf, 0.f);

    std::vector<float> offset(nf, 0.f), shape(nf, 0.f), formant(nf, (float)e.globalFormantShift);
    const double taper = std::max(1.0, e.edgeTaperMs / hopMs);

    for (int k = 0; k < nf; ++k) {
        const Frame& f = a.frames[k];
        const int ni = a.noteOfFrame.empty() ? -1 : a.noteOfFrame[k];
        if (f.kind == FrameKind::Sibilant) c.gainDb[k] = (float)e.sibilantGainDb;
        if (f.kind == FrameKind::Breath) c.gainDb[k] = (float)e.breathGainDb;
        if (ni < 0) {
            if (f.kind == FrameKind::Voiced) c.gainDb[k] = (float)e.unassignedGainDb;
            continue;
        }
        const NoteEdit& ed = ni < (int)e.notes.size() ? e.notes[ni] : kNoEdit;
        const Note& nt = a.notes[ni];
        // Drift/vibrato edits fade in over the note edges so natural glides survive.
        const double edge = std::min(k - nt.startFrame + 0.5, nt.endFrame - k - 0.5);
        const double t = std::clamp(edge / taper, 0.0, 1.0);
        const double w = 0.5 - 0.5 * std::cos(M_PI * t);
        const double drift = 1.0 + (ed.driftAmount - 1.0) * w;
        const double vib = 1.0 + (ed.vibratoAmount - 1.0) * w;
        // Scaling drift/vibrato around their own mean keeps the perceived centre where it was.
        offset[k] = (float)ed.pitchOffset;
        shape[k] = (float)((drift - 1.0) * (a.slowDev[k] - nt.meanSlow) + (vib - 1.0) * (a.fastDev[k] - nt.meanFast));
        formant[k] += (float)ed.formantShift;
        c.gainDb[k] += (float)ed.gainDb;
    }

    // Pitch-offset transitions between adjacent notes within a voiced run: a
    // raised-cosine ramp centred on the boundary, as wide as the later note's
    // smoothness asks (0 = instant step ... 1 = long glide), never wider than
    // half of either neighbouring note. Formant changes use a fixed short blend.
    const int half = std::max(1, (int)std::lround(e.transitionMs / hopMs / 4.0));
    const std::vector<float> raw = offset;
    auto noteAt = [&](int k) { return a.noteOfFrame.empty() ? -1 : a.noteOfFrame[(size_t)k]; };
    for (int k = 0; k < nf;) {
        if (a.frames[k].f0 <= 0) { ++k; continue; }
        int en = k;
        while (en < nf && a.frames[en].f0 > 0) ++en;
        for (int b = k + 1; b < en; ++b) {
            if (noteAt(b) == noteAt(b - 1)) continue;
            int segL = b - 1, segR = b;
            while (segL - 1 >= k && noteAt(segL - 1) == noteAt(b - 1)) --segL;
            while (segR + 1 < en && noteAt(segR + 1) == noteAt(b)) ++segR;
            const int r = noteAt(b) >= 0 ? noteAt(b) : noteAt(b - 1);
            const double smooth = (r >= 0 && r < (int)e.notes.size()) ? e.notes[(size_t)r].transition : 0.5;
            const double widthFrames = transitionWidthMs(smooth, e.transitionMs) / hopMs;
            const int h = (int)std::floor(std::min({0.5 * widthFrames, 0.5 * (b - segL), 0.5 * (segR + 1 - b)}));
            if (h <= 0) continue; // hard step
            const float vL = raw[(size_t)b - 1], vR = raw[(size_t)b];
            for (int j = b - h; j < b + h; ++j) {
                const double t = (j - (b - h) + 0.5) / (2.0 * h);
                offset[(size_t)j] = (float)(vL + (vR - vL) * (0.5 - 0.5 * std::cos(M_PI * t)));
            }
        }
        smoothRange(formant, k, en, half);
        k = en;
    }
    smoothRange(c.gainDb, 0, nf, std::max(1, (int)std::lround(8.0 / hopMs)));

    for (int k = 0; k < nf; ++k) {
        if (a.frames[k].f0 > 0) {
            c.pitchSemis[k] = offset[k] + shape[k];
            c.formantSemis[k] = formant[k];
        }
    }

    // Pencil: drawn pitch replaces the computed curve, blended in over ~30 ms at
    // the ends of each drawn stretch so it joins the rest without a jump.
    for (const DrawRun& r : e.draw) {
        const int len = (int)r.delta.size();
        const double blend = std::max(1.0, 30.0 / hopMs);
        for (int q = 0; q < len; ++q) {
            const int k = r.start + q;
            if (k < 0 || k >= nf || a.frames[k].f0 <= 0 || std::isnan(r.delta[(size_t)q])) continue;
            const int ni = noteAt(k);
            const double noteOff = (ni >= 0 && ni < (int)e.notes.size()) ? e.notes[(size_t)ni].pitchOffset : 0.0;
            const double drawn = noteOff + r.delta[(size_t)q];
            const double edgeDist = std::min(q, len - 1 - q) + 1.0;
            const double w = edgeDist >= blend ? 1.0 : 0.5 - 0.5 * std::cos(M_PI * edgeDist / blend);
            c.pitchSemis[k] = (float)((1.0 - w) * c.pitchSemis[k] + w * drawn);
        }
    }
    return c;
}

std::vector<double> editedPitchCurve(const Analysis& a, const EditSet& e)
{
    const Controls c = buildControls(a, e);
    std::vector<double> out(a.frames.size(), 0.0);
    for (size_t k = 0; k < a.frames.size(); ++k)
        if (a.frames[k].f0 > 0) out[k] = a.frames[k].midi + c.pitchSemis[k];
    return out;
}

// ---------------------------------------------------------------------------
std::vector<float> render(const float* x, size_t n, const Analysis& a, const EditSet& e)
{
    const auto& M = a.marks;
    if (M.empty() || n == 0) return std::vector<float>(x, x + n);

    const Controls c = buildControls(a, e);
    const TimeMap map = TimeMap::build(a, e);
    const double outLen = map.inToOut((double)n);
    const size_t outN = (size_t)std::max(0.0, std::ceil(outLen));
    std::vector<double> acc(outN + 4096, 0.0);

    uint32_t rng = 0x9E3779B9u;
    size_t i = 0;
    long lastGrain = -1;
    double ts = map.inToOut(M[0].pos);

    while (ts < outLen) {
        const double tau = map.outToIn(ts);
        while (i + 1 < M.size() && std::abs(M[i + 1].pos - tau) <= std::abs(M[i].pos - tau)) ++i;

        size_t g = i;
        // Time-stretched noise: avoid repeating the very same unvoiced grain (buzz).
        if (!M[g].voiced && (long)g == lastGrain) {
            rng = rng * 1664525u + 1013904223u;
            const size_t alt = (rng >> 16) & 1 ? g + 1 : (g > 0 ? g - 1 : g + 1);
            if (alt < M.size() && !M[alt].voiced) g = alt;
        }
        lastGrain = (long)g;
        const PitchMark& mk = M[g];

        double halfR = g + 1 < M.size() ? M[g + 1].pos - mk.pos : 0.0;
        double halfL = g > 0 ? mk.pos - M[g - 1].pos : halfR;
        if (halfR <= 0) halfR = halfL;
        if (halfL <= 0 || halfR <= 0) { ts += 1.0; continue; }
        const double maxHalf = 0.03 * a.sampleRate;
        halfL = std::min(halfL, maxHalf);
        halfR = std::min(halfR, maxHalf);

        const double frame = mk.pos / a.hop;
        const bool voiced = mk.voiced;
        const double beta = voiced ? std::pow(2.0, ctrlAt(c.pitchSemis, frame) / 12.0) : 1.0;
        const double gamma = voiced ? std::pow(2.0, ctrlAt(c.formantSemis, frame) / 12.0) : 1.0;
        double gain = dbToLin(ctrlAt(c.gainDb, frame));
        if (voiced) gain *= std::sqrt(gamma / beta); // keep loudness when pulse rate / grain width change

        const long o0 = (long)std::ceil(ts - halfL / gamma);
        const long o1 = (long)std::floor(ts + halfR / gamma);
        for (long o = std::max(0L, o0); o <= o1 && (size_t)o < acc.size(); ++o) {
            const double u = (o - ts) * gamma;
            double w;
            if (u < 0) w = 0.5 - 0.5 * std::cos(M_PI * std::clamp(1.0 + u / halfL, 0.0, 1.0));
            else       w = 0.5 + 0.5 * std::cos(M_PI * std::clamp(u / halfR, 0.0, 1.0));
            if (w <= 0) continue;
            acc[o] += gain * w * cubicAt(x, n, mk.pos + u);
        }

        ts += voiced ? halfR / beta : halfR;
    }

    std::vector<float> out(outN);
    for (size_t k = 0; k < outN; ++k) out[k] = (float)acc[k];
    return out;
}

} // namespace glina
