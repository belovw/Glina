#include "glina/Notes.h"

#include <algorithm>
#include <cmath>

namespace glina {

namespace {

std::vector<double> smoothTri(const std::vector<double>& v, int half)
{
    auto ma = [&](const std::vector<double>& in) {
        const int n = (int)in.size();
        std::vector<double> pre(n + 1, 0.0), out(n);
        for (int i = 0; i < n; ++i) pre[i + 1] = pre[i] + in[i];
        for (int i = 0; i < n; ++i) {
            const int h = std::min({half, i, n - 1 - i});
            out[i] = (pre[i + h + 1] - pre[i - h]) / (2 * h + 1);
        }
        return out;
    };
    if (half <= 0 || v.size() < 3) return v;
    return ma(ma(v));
}

} // namespace

void appendNote(Analysis& a, int start, int end, double contourSmoothMs, const double* smoothedIn)
{
    const int nf = (int)a.frames.size();
    start = std::clamp(start, 0, nf);
    end = std::clamp(end, start, nf);
    const int nl = end - start;
    if (nl <= 0) return;
    if (a.slowDev.size() != (size_t)nf) a.slowDev.assign(nf, 0.f);
    if (a.fastDev.size() != (size_t)nf) a.fastDev.assign(nf, 0.f);
    if (a.noteOfFrame.size() != (size_t)nf) a.noteOfFrame.assign(nf, -1);

    // Voiced frames of the span (a joined note may contain a short unvoiced gap).
    std::vector<int> idx;
    idx.reserve(nl);
    for (int i = 0; i < nl; ++i)
        if (a.frames[start + i].f0 > 0) idx.push_back(i);

    Note nt;
    nt.startFrame = start;
    nt.endFrame = end;
    const double hopSec = (double)a.hop / a.sampleRate;
    nt.startSec = start * hopSec - 0.5 * hopSec;
    nt.endSec = end * hopSec - 0.5 * hopSec;
    const int noteIndex = (int)a.notes.size();
    for (int i = 0; i < nl; ++i) {
        a.noteOfFrame[start + i] = noteIndex;
        a.slowDev[start + i] = 0.f;
        a.fastDev[start + i] = 0.f;
    }
    if (idx.empty()) { // no pitch at all: keep it as a placeholder at its neighbours' pitch
        nt.centerMidi = 60;
        a.notes.push_back(nt);
        return;
    }

    // Jitter-filtered contour over voiced frames.
    const int nv = (int)idx.size();
    std::vector<double> s(nv);
    if (smoothedIn != nullptr) {
        for (int j = 0; j < nv; ++j) s[j] = smoothedIn[idx[j]];
    } else {
        for (int j = 0; j < nv; ++j) {
            double w[5];
            int c = 0;
            for (int q = std::max(0, j - 2); q <= std::min(nv - 1, j + 2); ++q) {
                const double v = a.frames[start + idx[q]].midi;
                int p = c++;
                while (p > 0 && w[p - 1] > v) { w[p] = w[p - 1]; --p; }
                w[p] = v;
            }
            s[j] = w[c / 2];
        }
    }

    // Perceived centre: most populated pitch level (energy-weighted kernel
    // density), then the weighted mean of frames near it. Scoops and falls do not bias it.
    std::vector<double> wts(nv);
    for (int j = 0; j < nv; ++j) wts[j] = std::pow(10.0, a.frames[start + idx[j]].rmsDb / 20.0);
    double mode = s[nv / 2], bestScore = -1;
    {
        const double mn = *std::min_element(s.begin(), s.end());
        const double mx = *std::max_element(s.begin(), s.end());
        for (double lv = mn; lv <= mx + 1e-9; lv += 0.05) {
            double sc = 0;
            for (int j = 0; j < nv; ++j) {
                const double d = (s[j] - lv) / 0.25;
                sc += wts[j] * std::exp(-0.5 * d * d);
            }
            if (sc > bestScore) { bestScore = sc; mode = lv; }
        }
    }
    std::vector<char> inCentre(nv, 0);
    double wsum = 0, msum = 0;
    for (int j = 0; j < nv; ++j) {
        if (std::abs(s[j] - mode) > 0.6) continue;
        inCentre[j] = 1;
        wsum += wts[j];
        msum += wts[j] * a.frames[start + idx[j]].midi;
    }
    nt.centerMidi = wsum > 0 ? msum / wsum : mode;

    const double hopMs = 1000.0 * hopSec;
    const int smoothHalf = std::max(1, (int)std::lround(contourSmoothMs / hopMs / 2));
    std::vector<double> dev(nv);
    for (int j = 0; j < nv; ++j) dev[j] = a.frames[start + idx[j]].midi - nt.centerMidi;
    const std::vector<double> slow = smoothTri(dev, smoothHalf);
    double ms = 0, mf = 0;
    for (int j = 0; j < nv; ++j) {
        const int f = start + idx[j];
        a.slowDev[f] = (float)slow[j];
        a.fastDev[f] = (float)(dev[j] - slow[j]);
        if (inCentre[j]) { ms += wts[j] * a.slowDev[f]; mf += wts[j] * a.fastDev[f]; }
    }
    nt.meanSlow = wsum > 0 ? ms / wsum : 0;
    nt.meanFast = wsum > 0 ? mf / wsum : 0;
    a.notes.push_back(nt);
}

std::vector<NoteSpan> spansOf(const Analysis& a)
{
    std::vector<NoteSpan> s;
    s.reserve(a.notes.size());
    for (const Note& n : a.notes) s.push_back({n.startFrame, n.endFrame});
    return s;
}

Analysis withLayout(const Analysis& base, const std::vector<NoteSpan>& spans)
{
    if (spans.empty()) return base;
    Analysis a = base;
    const int nf = (int)a.frames.size();
    a.notes.clear();
    a.slowDev.assign(nf, 0.f);
    a.fastDev.assign(nf, 0.f);
    a.noteOfFrame.assign(nf, -1);
    int last = 0;
    for (const NoteSpan& sp : spans) {
        const int s = std::max(sp.start, last), e = std::min(sp.end, nf);
        if (e - s < 1) continue;
        appendNote(a, s, e);
        last = e;
    }
    return a;
}

} // namespace glina
