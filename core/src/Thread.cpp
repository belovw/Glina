#include "glina/Thread.h"
#include "glina/Editing.h"
#include "glina/Renderer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace glina {

std::vector<ThreadFrame> threadOf(const Analysis& a, const EditSet& e, const std::function<double(double)>& toCommon,
                                  double lo, double hi, double step)
{
    std::vector<ThreadFrame> out;
    const int nf = (int)a.frames.size();
    if (nf == 0 || hi <= lo || step <= 0) return out;
    const TimeMap map = TimeMap::build(a, e);
    const std::vector<double> curve = editedPitchCurve(a, e);
    const double sr = a.sampleRate;
    const size_t n = (size_t)std::floor((hi - lo) / step) + 1;
    out.reserve(n);
    for (size_t q = 0; q < n; ++q) {
        const double t = lo + q * step;
        const double in = map.outToIn(t * sr) / sr;
        const int k = std::clamp((int)std::lround(in * sr / a.hop), 0, nf - 1);
        const Frame& f = a.frames[(size_t)k];
        ThreadFrame tf;
        tf.t = toCommon ? toCommon(t) : t;
        tf.midi = (f.f0 > 0 && curve[(size_t)k] > 0) ? (float)curve[(size_t)k] : 0.f;
        tf.kind = f.kind;
        tf.rmsDb = f.rmsDb;
        out.push_back(tf);
    }
    return out;
}

namespace {
// Pins closer than this (seconds) count as the same point.
constexpr double kPinEps = 1e-3;
}

bool addPin(std::vector<Pin>& pins, const Pin& p)
{
    size_t k = 0;
    while (k < pins.size() && pins[k].tgt < p.tgt) ++k;
    if (k < pins.size() && pins[k].tgt - p.tgt < kPinEps) return false;      // on a pin already
    if (k > 0 && p.tgt - pins[k - 1].tgt < kPinEps) return false;
    if (k > 0 && p.ref <= pins[k - 1].ref + kPinEps) return false;           // would cross a neighbour
    if (k < pins.size() && p.ref >= pins[k].ref - kPinEps) return false;
    pins.insert(pins.begin() + (std::ptrdiff_t)k, p);
    return true;
}

bool movePin(std::vector<Pin>& pins, size_t i, bool refEnd, double sec)
{
    if (i >= pins.size()) return false;
    // Neighbours in target order; the reference order follows it (addPin keeps that).
    const double lo = i > 0 ? (refEnd ? pins[i - 1].ref : pins[i - 1].tgt) + kPinEps : -1e300;
    const double hi = i + 1 < pins.size() ? (refEnd ? pins[i + 1].ref : pins[i + 1].tgt) - kPinEps : 1e300;
    if (lo > hi) return false;
    const double v = std::clamp(sec, lo, hi);
    (refEnd ? pins[i].ref : pins[i].tgt) = v;
    return true;
}

double Warp::map(double t) const
{
    if (tgt.empty()) return t;
    if (tgt.size() == 1 || t <= tgt.front()) return ref.front() + (t - tgt.front());
    if (t >= tgt.back()) return ref.back() + (t - tgt.back());
    const size_t j = (size_t)(std::upper_bound(tgt.begin(), tgt.end(), t) - tgt.begin());
    const double x0 = tgt[j - 1], x1 = tgt[j];
    return x1 > x0 ? ref[j - 1] + (ref[j] - ref[j - 1]) * (t - x0) / (x1 - x0) : ref[j - 1];
}

namespace {

// Frames on a uniform grid t0 + k * step (nearest frame of a time-sorted thread).
std::vector<ThreadFrame> resample(const std::vector<ThreadFrame>& in, double t0, double step, size_t n)
{
    std::vector<ThreadFrame> out(n);
    size_t j = 0;
    for (size_t k = 0; k < n; ++k) {
        const double t = t0 + k * step;
        while (j + 1 < in.size() && std::abs(in[j + 1].t - t) <= std::abs(in[j].t - t)) ++j;
        out[k] = in[j];
        out[k].t = t;
    }
    return out;
}

// Blocks of `f` frames: voiced if at least half is voiced (mean pitch), else the
// commonest other kind; level = the block's maximum.
std::vector<ThreadFrame> decimate(const std::vector<ThreadFrame>& in, size_t f)
{
    std::vector<ThreadFrame> out;
    for (size_t b = 0; b < in.size(); b += f) {
        const size_t e = std::min(in.size(), b + f);
        ThreadFrame o;
        o.t = in[b].t;
        int cnt[4] = {0, 0, 0, 0};
        double sum = 0;
        int voiced = 0;
        for (size_t k = b; k < e; ++k) {
            ++cnt[(int)in[k].kind & 3];
            o.rmsDb = std::max(o.rmsDb, in[k].rmsDb);
            if (in[k].midi > 0) { sum += in[k].midi; ++voiced; }
        }
        if (2 * voiced >= (int)(e - b)) {
            o.kind = FrameKind::Voiced;
            o.midi = (float)(sum / voiced);
        } else {
            int best = 0;
            for (int k = 0; k < 4; ++k) if (k != (int)FrameKind::Voiced && cnt[k] > cnt[best]) best = k;
            o.kind = (FrameKind)best;
            o.midi = 0;
        }
        out.push_back(o);
    }
    return out;
}

// How different two kinds of sound are (0 = same).
float kindCost(FrameKind a, FrameKind b)
{
    static const float table[4][4] = {
        // Silence Voiced Sibilant Breath
        {0.0f, 1.0f, 0.6f, 0.3f},  // Silence
        {1.0f, 0.0f, 1.0f, 0.8f},  // Voiced
        {0.6f, 1.0f, 0.0f, 0.5f},  // Sibilant
        {0.3f, 0.8f, 0.5f, 0.0f},  // Breath
    };
    return table[(int)a & 3][(int)b & 3];
}

float level01(float db) { return std::clamp((db + 60.f) / 60.f, 0.f, 1.f); }

// One DTW pass (symmetric, slope 1:2 .. 2:1) in a band of +-B columns around
// centre(row); open start on the first aligned row, open end on the last.
// Rows whose band centre lies on the reference are aligned.
struct Pass {
    Pass(const std::vector<ThreadFrame>& t, const std::vector<ThreadFrame>& r) : T(t), R(r) {}
    const std::vector<ThreadFrame>& T;
    const std::vector<ThreadFrame>& R;
    bool unison = true;
    float shift = 0;                        // 12 * octave
    long onsetLag = 3, slopeLag = 2;        // frames
    std::vector<std::pair<double, double>> pins; // (row, column)
    std::function<double(long)> centre;
    long B = 50;

    long iBeg = -1, iEnd = -1;
    std::vector<double> col; // mean reference column per aligned row
    double cost = 0;         // per row

    bool run()
    {
        const long N = (long)T.size(), M = (long)R.size();
        const long W = 2 * B + 1;
        std::vector<long> start((size_t)N);
        for (long i = 0; i < N; ++i) {
            const double c = centre(i);
            start[(size_t)i] = std::lround(c) - B;
            if (c >= 0 && c <= (double)M - 1) { if (iBeg < 0) iBeg = i; iEnd = i; }
        }
        if (iBeg < 0 || iEnd - iBeg < 3) return false;

        auto onset = [&](const std::vector<ThreadFrame>& v, long k) {
            return std::clamp(v[(size_t)k].rmsDb - v[(size_t)std::max(0L, k - onsetLag)].rmsDb, -30.f, 30.f);
        };
        auto slope = [&](const std::vector<ThreadFrame>& v, long k) -> float {
            const long k0 = std::max(0L, k - slopeLag);
            if (v[(size_t)k].midi <= 0 || v[(size_t)k0].midi <= 0) return 0.f;
            return v[(size_t)k].midi - v[(size_t)k0].midi;
        };
        const float big = 1e4f;
        auto dist = [&](long i, long j) -> float {
            if (j < 0 || j >= M) return big;
            const ThreadFrame& a = T[(size_t)i];
            const ThreadFrame& b = R[(size_t)j];
            float d = kindCost(a.kind, b.kind);
            d += 0.4f * std::min(std::abs(onset(T, i) - onset(R, j)) / 12.f, 1.f);
            if (a.midi > 0 && b.midi > 0) {
                if (unison) d += std::min(std::abs(b.midi - shift - a.midi), 2.f) / 2.f;
                else d += 0.7f * std::min(std::abs(slope(R, j) - slope(T, i)), 2.f) / 2.f;
            }
            for (const auto& p : pins)
                if (std::lround(p.first) == i && std::abs((double)j - p.second) > 1.0) d += big;
            return d;
        };

        const long rows = iEnd - iBeg + 1;
        const float inf = std::numeric_limits<float>::infinity();
        std::vector<float> D((size_t)(rows * W), inf), Dc((size_t)(rows * W), big);
        std::vector<uint8_t> how((size_t)(rows * W), 0);
        auto at = [&](long i, long j) -> long {
            if (i < iBeg || i > iEnd) return -1;
            const long wv = j - start[(size_t)i];
            return (wv < 0 || wv >= W) ? -1 : (i - iBeg) * W + wv;
        };
        for (long i = iBeg; i <= iEnd; ++i)
            for (long wv = 0; wv < W; ++wv) Dc[(size_t)((i - iBeg) * W + wv)] = dist(i, start[(size_t)i] + wv);
        auto dc = [&](long i, long j) -> float { const long x = at(i, j); return x < 0 ? big : Dc[(size_t)x]; };
        auto Dv = [&](long i, long j) -> float { const long x = at(i, j); return x < 0 ? inf : D[(size_t)x]; };
        for (long i = iBeg; i <= iEnd; ++i)
            for (long wv = 0; wv < W; ++wv) {
                const long j = start[(size_t)i] + wv;
                if (j < 0 || j >= M) continue;
                const long x = (i - iBeg) * W + wv;
                const float d = Dc[(size_t)x];
                if (i == iBeg) { D[(size_t)x] = 2 * d; how[(size_t)x] = 3; continue; }
                float best = Dv(i - 1, j - 1) + 2 * d;
                uint8_t h = 0;
                const float b = Dv(i - 1, j - 2) + 2 * dc(i, j - 1) + d;
                if (b < best) { best = b; h = 1; }
                const float c = Dv(i - 2, j - 1) + 2 * dc(i - 1, j) + d;
                if (c < best) { best = c; h = 2; }
                D[(size_t)x] = best;
                how[(size_t)x] = h;
            }
        long jEnd = -1;
        float endV = inf;
        for (long wv = 0; wv < W; ++wv) {
            const float v = Dv(iEnd, start[(size_t)iEnd] + wv);
            if (v < endV) { endV = v; jEnd = start[(size_t)iEnd] + wv; }
        }
        if (jEnd < 0 || !std::isfinite(endV)) return false;

        std::vector<double> sum((size_t)rows, 0.0);
        std::vector<int> cnt((size_t)rows, 0);
        auto visit = [&](long i, long j) { if (i >= iBeg && i <= iEnd) { sum[(size_t)(i - iBeg)] += j; ++cnt[(size_t)(i - iBeg)]; } };
        long i = iEnd, j = jEnd;
        while (true) {
            visit(i, j);
            const long x = at(i, j);
            if (x < 0) break;
            const uint8_t h = how[(size_t)x];
            if (h == 3) break;
            if (h == 0) { --i; --j; }
            else if (h == 1) { visit(i, j - 1); --i; j -= 2; }
            else { visit(i - 1, j); i -= 2; --j; }
            if (i < iBeg) break;
        }
        col.assign((size_t)rows, -1.0);
        for (long r = 0; r < rows; ++r) if (cnt[(size_t)r]) col[(size_t)r] = sum[(size_t)r] / cnt[(size_t)r];
        for (long r = 1; r < rows; ++r) if (col[(size_t)r] < 0 && col[(size_t)r - 1] >= 0) col[(size_t)r] = col[(size_t)r - 1] + 1;
        for (long r = rows - 2; r >= 0; --r) if (col[(size_t)r] < 0 && col[(size_t)r + 1] >= 0) col[(size_t)r] = col[(size_t)r + 1] - 1;
        cost = endV / (2.0 * (double)rows);
        return true;
    }

    // Column at a (fractional) row, slope 1 beyond the aligned rows.
    double colAt(double row) const
    {
        if (col.empty()) return row;
        const double r = row - iBeg;
        if (r <= 0) return col.front() + r;
        const double last = (double)col.size() - 1;
        if (r >= last) return col.back() + (r - last);
        const size_t k = (size_t)r;
        return col[k] + (col[k + 1] - col[k]) * (r - k);
    }
};

} // namespace

Warp alignThreads(const std::vector<ThreadFrame>& tgtIn, const std::vector<ThreadFrame>& refIn, const AlignOptions& opt,
                  const std::vector<Pin>& pins, AlignInfo* info)
{
    Warp w;
    if (tgtIn.size() < 4 || refIn.size() < 4) return w;
    const double step = (tgtIn.back().t - tgtIn.front().t) / (double)(tgtIn.size() - 1);
    if (!(step > 0)) return w;
    const size_t N = tgtIn.size();
    const double t0 = tgtIn.front().t;
    const std::vector<ThreadFrame> tgt = resample(tgtIn, t0, step, N);
    const double r0 = refIn.front().t;
    const size_t M = (size_t)std::floor((refIn.back().t - r0) / step) + 1;
    if (M < 4) return w;
    const std::vector<ThreadFrame> ref = resample(refIn, r0, step, M);

    // ---- coarse shift: correlation of the level envelopes ------------------------------
    std::vector<float> xa(N), ya(M);
    double mx = 0, my = 0;
    for (size_t i = 0; i < N; ++i) { xa[i] = tgt[i].kind == FrameKind::Silence ? 0.f : level01(tgt[i].rmsDb); mx += xa[i]; }
    for (size_t j = 0; j < M; ++j) { ya[j] = ref[j].kind == FrameKind::Silence ? 0.f : level01(ref[j].rmsDb); my += ya[j]; }
    mx /= N; my /= M;
    const long base = std::lround((t0 - r0) / step); // reference index of target frame i = i + base (no shift)
    const long C = std::max(0L, std::lround(opt.coarseSec / step));
    long lag = 0;
    double best = -std::numeric_limits<double>::infinity();
    for (long L = -C; L <= C; ++L) {
        double s = 0;
        long cnt = 0;
        for (size_t i = 0; i < N; ++i) {
            const long j = (long)i + base + L;
            if (j < 0 || j >= (long)M) continue;
            s += (xa[i] - mx) * (ya[(size_t)j] - my);
            ++cnt;
        }
        if (cnt < (long)std::min(N, M) / 4) continue;
        s /= cnt;
        if (s > best + 1e-12 || (std::abs(s - best) <= 1e-12 && std::abs(L) < std::abs(lag))) { best = s; lag = L; }
    }

    // ---- octave (unison): median of reference minus target over voiced pairs ------------
    int octave = opt.octave == AlignOptions::kAutoOctave ? 0 : opt.octave;
    if (opt.unison && opt.octave == AlignOptions::kAutoOctave) {
        std::vector<float> d;
        for (size_t i = 0; i < N; ++i) {
            const long j = (long)i + base + lag;
            if (j < 0 || j >= (long)M || tgt[i].midi <= 0 || ref[(size_t)j].midi <= 0) continue;
            d.push_back(ref[(size_t)j].midi - tgt[i].midi);
        }
        if (!d.empty()) {
            std::nth_element(d.begin(), d.begin() + (long)d.size() / 2, d.end());
            octave = (int)std::lround(d[d.size() / 2] / 12.0);
        }
    }

    // Another part? Median distance of the lines, octave folded.
    bool unison = opt.unison;
    double medianDiff = 0;
    {
        std::vector<float> d;
        for (size_t i = 0; i < N; ++i) {
            const long j = (long)i + base + lag;
            if (j < 0 || j >= (long)M || tgt[i].midi <= 0 || ref[(size_t)j].midi <= 0) continue;
            d.push_back(std::abs(ref[(size_t)j].midi - 12.f * (float)octave - tgt[i].midi));
        }
        if (!d.empty()) {
            std::nth_element(d.begin(), d.begin() + (long)d.size() / 2, d.end());
            medianDiff = d[d.size() / 2];
        }
        if (unison && opt.autoPart && medianDiff > 1.0) unison = false;
    }

    std::vector<std::pair<double, double>> pinRows; // (target row, reference column), sorted
    for (const Pin& p : pins) {
        const double ip = (p.tgt - t0) / step, jp = (p.ref - r0) / step;
        if (ip >= 0 && ip < (double)N) pinRows.push_back({ip, jp});
    }
    std::sort(pinRows.begin(), pinRows.end());
    // The coarse diagonal, bent through the pins.
    auto diagonal = [&](double i, double scale) -> double {
        const double diag = i + (double)(base + lag) / scale;
        if (pinRows.empty()) return diag;
        auto off = [&](size_t k) { return (pinRows[k].second - (pinRows[k].first + base + lag)) / scale; };
        const double ii = i * scale;
        if (ii <= pinRows.front().first) return diag + off(0);
        if (ii >= pinRows.back().first) return diag + off(pinRows.size() - 1);
        size_t k = 1;
        while (k < pinRows.size() && pinRows[k].first < ii) ++k;
        const double u = (ii - pinRows[k - 1].first) / std::max(1e-9, pinRows[k].first - pinRows[k - 1].first);
        return diag + off(k - 1) + (off(k) - off(k - 1)) * u;
    };

    // ---- pass 1: 100 ms blocks, wide band (follows a take that drifts in tempo) -----------
    const size_t F = std::max<size_t>(1, (size_t)std::lround(0.1 / step));
    const std::vector<ThreadFrame> T1 = decimate(tgt, F), R1 = decimate(ref, F);
    Pass p1(T1, R1);
    p1.unison = unison;
    p1.shift = 12.f * (float)octave;
    p1.onsetLag = 1;
    p1.slopeLag = 1;
    for (const auto& pr : pinRows) p1.pins.push_back({pr.first / F, pr.second / F});
    p1.centre = [&](long i) { return diagonal((double)i, (double)F); };
    const double len = std::max(N, M) * step;
    p1.B = std::max(3L, std::lround(std::max({2.0, 0.05 * len, opt.bandSec}) / (step * F)));
    const bool coarseOk = F > 1 && p1.run();

    // ---- pass 2: frame by frame, +-bandSec around the coarse path ---------------------------
    Pass p2(tgt, ref);
    p2.unison = unison;
    p2.shift = 12.f * (float)octave;
    p2.pins = pinRows;
    if (coarseOk) p2.centre = [&](long i) { return p1.colAt((double)i / F) * F; };
    else p2.centre = [&](long i) { return diagonal((double)i, 1.0); };
    p2.B = std::max(2L, std::lround(opt.bandSec / step));
    if (!p2.run()) return w;
    const long iBeg = p2.iBeg, iEnd = p2.iEnd, rows = iEnd - iBeg + 1;
    const std::vector<double>& col = p2.col;

    // Smooth the staircase (+-2 rows) and keep it monotone.
    std::vector<double> sm((size_t)rows);
    for (long r = 0; r < rows; ++r) {
        double s = 0; int c = 0;
        for (long q = std::max(0L, r - 2); q <= std::min(rows - 1, r + 2); ++q) { s += col[(size_t)q]; ++c; }
        sm[(size_t)r] = s / c;
    }
    for (long r = 1; r < rows; ++r) sm[(size_t)r] = std::max(sm[(size_t)r], sm[(size_t)r - 1]);
    // Anchors every 4 rows (40 ms at 10 ms), at both ends, and exactly at the pins.
    std::vector<std::pair<double, double>> anchors; // (target row, reference column)
    for (long r = 0; r < rows; r += 4) anchors.push_back({(double)(iBeg + r), sm[(size_t)r]});
    if ((rows - 1) % 4 != 0) anchors.push_back({(double)iEnd, sm[(size_t)rows - 1]});
    for (const auto& pr : pinRows)
        if (pr.first >= iBeg && pr.first <= iEnd) {
            // Drop the regular anchors right next to the pin, so the line goes straight through it.
            anchors.erase(std::remove_if(anchors.begin(), anchors.end(),
                                         [&](const auto& an) { return std::abs(an.first - pr.first) < 2.0; }),
                          anchors.end());
            anchors.push_back(pr);
        }
    std::sort(anchors.begin(), anchors.end());
    for (const auto& an : anchors) { w.tgt.push_back(t0 + an.first * step); w.ref.push_back(r0 + an.second * step); }
    for (size_t k = 1; k < w.ref.size(); ++k) w.ref[k] = std::max(w.ref[k], w.ref[k - 1]);

    if (info != nullptr) {
        info->octave = octave;
        info->coarseSec = lag * step; // reference minus target
        info->frames = (int)rows;
        info->meanCost = p2.cost;
        info->part = !unison;
        info->medianDiff = medianDiff;
    }
    return w;
}

// ---- step 2: wrong notes ---------------------------------------------------------------
std::vector<NoteFix> planPitchFix(const Analysis& a, const EditSet& e, const std::vector<ThreadFrame>& ref,
                                  const Warp& warp, const AlignInfo& info, const FixOptions& opt)
{
    std::vector<NoteFix> out;
    if (warp.empty() || ref.empty()) return out;
    const TimeMap map = TimeMap::build(a, e);
    const double sr = a.sampleRate;
    const int nf = (int)a.frames.size();
    const std::vector<float> drawn = drawnDense(e, nf);
    // Reference pitch at a time (nearest frame within 15 ms; 0 = none).
    auto refAt = [&](double t) -> float {
        const auto it = std::lower_bound(ref.begin(), ref.end(), t, [](const ThreadFrame& f, double x) { return f.t < x; });
        const ThreadFrame* best = nullptr;
        if (it != ref.end()) best = &*it;
        if (it != ref.begin() && (best == nullptr || t - std::prev(it)->t < best->t - t)) best = &*std::prev(it);
        return (best != nullptr && std::abs(best->t - t) <= 0.015) ? best->midi : 0.f;
    };
    const NoteEdit none{};
    for (int i = 0; i < (int)a.notes.size(); ++i) {
        NoteFix f;
        f.note = i;
        const Note& n = a.notes[(size_t)i];
        const NoteEdit& ed = i < (int)e.notes.size() ? e.notes[(size_t)i] : none;
        bool edited = ed.pitchOffset != 0 || ed.tiltL != 0 || ed.tiltR != 0;
        for (int k = std::max(0, n.startFrame); k < std::min(n.endFrame, nf) && !edited; ++k)
            if (!std::isnan(drawn[(size_t)k])) edited = true;
        f.edited = edited;
        if (edited || ed.consonant || info.part) { out.push_back(f); continue; }
        // The middle of the note (glides at its ends belong to the transitions).
        const int len = n.endFrame - n.startFrame;
        const int k0 = n.startFrame + len / 5, k1 = n.endFrame - len / 5;
        std::vector<float> d;
        int voiced = 0;
        for (int k = std::max(0, k0); k < std::min(k1, nf); ++k) {
            if (a.frames[(size_t)k].f0 <= 0) continue;
            ++voiced;
            const double t = map.inToOut(k * (double)a.hop) / sr;
            const float r = refAt(warp.map(t));
            if (r > 0) d.push_back(r - 12.f * (float)info.octave);
        }
        if (voiced == 0 || (double)d.size() < opt.minCover * voiced) { out.push_back(f); continue; }
        std::nth_element(d.begin(), d.begin() + (long)d.size() / 2, d.end());
        f.refMidi = d[d.size() / 2];
        f.diff = f.refMidi - n.centerMidi;
        int steady = 0;
        for (float x : d) if (std::abs(x - f.refMidi) <= 0.5f) ++steady;
        if ((double)steady < opt.minSteady * (double)d.size()) { f.unsteady = true; out.push_back(f); continue; }
        if (std::abs(f.diff) * 100.0 >= opt.wrongCents) f.semis = (int)std::lround(f.diff);
        out.push_back(f);
    }
    return out;
}

void applyPitchFix(EditSet& e, const std::vector<NoteFix>& fixes)
{
    for (const NoteFix& f : fixes)
        if (f.semis != 0 && !f.edited && f.note >= 0 && f.note < (int)e.notes.size()) e.notes[(size_t)f.note].pitchOffset += f.semis;
}

} // namespace glina
