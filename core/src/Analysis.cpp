#include "glina/Analysis.h"
#include "glina/FFT.h"
#include "glina/Notes.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace glina {

double hzToMidi(double hz) { return hz > 0 ? 69.0 + 12.0 * std::log2(hz / 440.0) : 0.0; }
double midiToHz(double m) { return 440.0 * std::pow(2.0, (m - 69.0) / 12.0); }

std::string Key::name() const
{
    static const char* names[12] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
    return std::string(names[((tonic % 12) + 12) % 12]) + (minor ? " minor" : " major");
}

namespace {

double highFreqRatio(const float* x, size_t n, long centre, double sr, const FFT& fft,
                     const std::vector<double>& win)
{
    const int N = fft.size();
    std::vector<std::complex<double>> buf(N);
    for (int j = 0; j < N; ++j) {
        const long i = centre - N / 2 + j;
        buf[j] = (i >= 0 && (size_t)i < n) ? x[i] * win[j] : 0.0;
    }
    fft.forward(buf);
    const int lo = std::max(1, (int)std::lround(80.0 * N / sr));
    const int hf = (int)std::lround(3500.0 * N / sr);
    double total = 0, high = 0;
    for (int k = lo; k < N / 2; ++k) {
        const double p = std::norm(buf[k]);
        total += p;
        if (k >= hf) high += p;
    }
    return total > 0 ? high / total : 0.0;
}

// Robust piecewise-constant segmentation of one voiced run into notes.
// Viterbi over a 10-cent level grid: per-frame cost is a TRUNCATED squared
// deviation (vibrato, scoops and glides cost at most trunc^2 per frame, so they
// do not spawn notes); changing level costs `notePenalty` and must jump at least
// `noteMinIntervalSt`. Then: split at deep level dips (repeated syllables on the
// same pitch) and fold segments shorter than minLen into the closer neighbour.
std::vector<int> segmentRun(const std::vector<double>& s, const Frame* fr, int minLen, const AnalysisParams& P)
{
    const int len = (int)s.size();
    const double grid = 0.1;
    const double lo = *std::min_element(s.begin(), s.end()) - 0.5;
    const double hi = *std::max_element(s.begin(), s.end()) + 0.5;
    const int L = std::max(1, (int)std::ceil((hi - lo) / grid) + 1);
    const int minJump = std::max(1, (int)std::lround(P.noteMinIntervalSt / grid));
    const double T2 = P.noteTruncSt * P.noteTruncSt;
    auto emit = [&](int i, int l) { const double d = s[i] - (lo + l * grid); return std::min(d * d, T2); };

    std::vector<double> D(L), Dn(L), pre(L), suf(L);
    std::vector<int> preA(L), sufA(L);
    std::vector<int16_t> bp((size_t)len * L, -1);
    for (int l = 0; l < L; ++l) D[l] = emit(0, l);
    for (int i = 1; i < len; ++i) {
        pre[0] = D[0]; preA[0] = 0;
        for (int l = 1; l < L; ++l) { if (D[l] < pre[l - 1]) { pre[l] = D[l]; preA[l] = l; } else { pre[l] = pre[l - 1]; preA[l] = preA[l - 1]; } }
        suf[L - 1] = D[L - 1]; sufA[L - 1] = L - 1;
        for (int l = L - 2; l >= 0; --l) { if (D[l] < suf[l + 1]) { suf[l] = D[l]; sufA[l] = l; } else { suf[l] = suf[l + 1]; sufA[l] = sufA[l + 1]; } }
        double mn = 1e300;
        for (int l = 0; l < L; ++l) {
            double sw = 1e300; int arg = -1;
            if (l - minJump >= 0 && pre[l - minJump] < sw) { sw = pre[l - minJump]; arg = preA[l - minJump]; }
            if (l + minJump < L && suf[l + minJump] < sw) { sw = suf[l + minJump]; arg = sufA[l + minJump]; }
            sw += P.notePenalty;
            if (arg >= 0 && sw < D[l]) { Dn[l] = sw; bp[(size_t)i * L + l] = (int16_t)arg; }
            else Dn[l] = D[l];
            Dn[l] += emit(i, l);
            mn = std::min(mn, Dn[l]);
        }
        for (int l = 0; l < L; ++l) D[l] = Dn[l] - mn;
    }
    std::vector<int> level(len);
    level[len - 1] = (int)(std::min_element(D.begin(), D.end()) - D.begin());
    for (int i = len - 1; i > 0; --i) {
        const int b = bp[(size_t)i * L + level[i]];
        level[i - 1] = b >= 0 ? b : level[i];
    }

    std::vector<int> cuts{0};
    for (int i = 1; i < len; ++i) if (level[i] != level[i - 1]) cuts.push_back(i);
    cuts.push_back(len);

    // Deep level dips start a new syllable even on the same pitch.
    std::vector<int> dips;
    for (int i = 3; i + 3 < len; ++i) {
        const double r = fr[i].rmsDb;
        bool localMin = true;
        for (int j = -3; j <= 3; ++j) if (fr[i + j].rmsDb < r) localMin = false;
        if (!localMin) continue;
        double lm = -200, rm = -200;
        for (int j = std::max(0, i - 16); j < i; ++j) lm = std::max(lm, (double)fr[j].rmsDb);
        for (int j = i + 1; j < std::min(len, i + 17); ++j) rm = std::max(rm, (double)fr[j].rmsDb);
        if (std::min(lm, rm) - r >= P.dipSplitDb) dips.push_back(i);
    }
    for (int d : dips) {
        auto it = std::upper_bound(cuts.begin(), cuts.end(), d);
        const int a = *(it - 1), b = *it;
        if (d - a >= minLen && b - d >= minLen) cuts.insert(it, d);
    }

    // Fold short pieces (glides, scoops) into the neighbour with the closer pitch.
    auto median = [&](int a, int b) {
        std::vector<double> v(s.begin() + a, s.begin() + b);
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        return v[v.size() / 2];
    };
    // A short piece where the pitch never settles is a transition, not a note.
    auto unstable = [&](int a, int b) {
        if (b - a >= 3 * minLen) return false;
        const double med = median(a, b);
        int near = 0;
        for (int i = a; i < b; ++i) near += std::abs(s[i] - med) < 0.3;
        return near < 0.6 * (b - a);
    };
    for (;;) {
        int worst = -1, wl = 1 << 30;
        for (size_t c = 0; c + 1 < cuts.size(); ++c) {
            const int l = cuts[c + 1] - cuts[c];
            if ((l < minLen || unstable(cuts[c], cuts[c + 1])) && l < wl) { wl = l; worst = (int)c; }
        }
        if (worst < 0 || cuts.size() <= 2) break;
        const int c = worst;
        const bool hasPrev = c > 0, hasNext = c + 2 < (int)cuts.size();
        bool intoNext = hasNext;
        if (hasPrev && hasNext) {
            const double me = median(cuts[c], cuts[c + 1]);
            const double dp = std::abs(me - median(cuts[c - 1], cuts[c]));
            const double dn = std::abs(me - median(cuts[c + 1], cuts[c + 2]));
            intoNext = dn <= dp + 0.3; // onset glides belong to the note they arrive at
        }
        cuts.erase(cuts.begin() + (intoNext ? c + 1 : c));
    }
    return cuts;
}

} // namespace

std::vector<PitchMark> placePitchMarks(const float* x, size_t n, const Analysis& a, double U)
{
    std::vector<PitchMark> marks;
    const int nf = (int)a.frames.size();
    const int hop = a.hop;
    auto sample = [&](long i) -> double { return (i >= 0 && (size_t)i < n) ? x[i] : 0.0; };

    // Period (samples) at an arbitrary sample time, interpolated in log-frequency.
    auto periodAt = [&](double t) {
        double fi = t / hop;
        int k0 = std::clamp((int)std::floor(fi), 0, nf - 1), k1 = std::min(k0 + 1, nf - 1);
        double f0a = a.frames[k0].f0, f0b = a.frames[k1].f0;
        if (f0a <= 0) f0a = f0b;
        if (f0b <= 0) f0b = f0a;
        if (f0a <= 0) return a.sampleRate / 200.0;
        const double w = std::clamp(fi - k0, 0.0, 1.0);
        return a.sampleRate / std::exp(std::log(f0a) * (1 - w) + std::log(f0b) * w);
    };

    // Normalised cross-correlation of the period starting around `m` with the one around `c`.
    auto ncc = [&](long m, long c, int len) {
        double xy = 0, xx = 0, yy = 0;
        for (int j = -len / 2; j < len - len / 2; ++j) {
            const double p = sample(m + j), q = sample(c + j);
            xy += p * q; xx += p * p; yy += q * q;
        }
        return (xx > 0 && yy > 0) ? xy / std::sqrt(xx * yy) : 0.0;
    };

    double pos = 0;
    marks.push_back({0.0, false});

    int k = 0;
    while (k < nf) {
        if (a.frames[k].f0 <= 0) { ++k; continue; }
        int e = k;
        while (e < nf && a.frames[e].f0 > 0) ++e;
        const double rs = std::max(0.0, k * (double)hop - hop / 2.0);
        const double re = std::min((double)n - 1, (e - 1) * (double)hop + hop / 2.0);

        // Unvoiced fill up to the voiced run.
        while (pos + U < rs - U * 0.5) { pos += U; marks.push_back({pos, false}); }

        // First voiced mark: strongest peak within the first period.
        const double T0 = periodAt(rs);
        long best = (long)std::ceil(std::max(rs, pos + 0.4 * std::min(U, T0)));
        double bestAbs = -1;
        for (long t = best; t < (long)(rs + T0) && t < (long)n; ++t)
            if (std::abs(x[t]) > bestAbs) { bestAbs = std::abs(x[t]); best = t; }
        double m = (double)best;
        marks.push_back({m, true});

        // Track period by period.
        for (;;) {
            const double T = periodAt(m);
            const double p = m + T;
            if (p > re) break;
            const int len = std::max(8, (int)std::lround(T));
            const long mi = std::lround(m);
            const long lo = (long)std::floor(p - 0.12 * T), hi = (long)std::ceil(p + 0.12 * T);
            long bc = std::lround(p);
            double bv = -2;
            std::vector<double> vals(hi - lo + 1);
            for (long c = lo; c <= hi; ++c) {
                vals[c - lo] = ncc(mi, c, len);
                if (vals[c - lo] > bv) { bv = vals[c - lo]; bc = c; }
            }
            double off = 0;
            if (bc > lo && bc < hi) {
                const double y0 = vals[bc - 1 - lo], y1 = vals[bc - lo], y2 = vals[bc + 1 - lo];
                const double den = y0 - 2 * y1 + y2;
                if (den < -1e-12) off = std::clamp(0.5 * (y0 - y2) / den, -0.5, 0.5);
            }
            // Weak correlation (noisy onset, creak): fall back to the predicted position.
            m = bv > 0.3 ? bc + off : p;
            marks.push_back({m, true});
        }
        pos = m;
        k = e;
    }
    while (pos + U < (double)n) { pos += U; marks.push_back({pos, false}); }
    if ((double)n - 1 - pos > U * 0.25) marks.push_back({(double)n - 1, false});
    return marks;
}

Key detectKey(const Analysis& a)
{
    static const double maj[12] = {6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88};
    static const double mnr[12] = {6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17};
    double hist[12] = {0};
    for (const auto& nt : a.notes) {
        const int pc = ((int)std::lround(nt.centerMidi) % 12 + 12) % 12;
        double amp = 0;
        for (int f = nt.startFrame; f < nt.endFrame; ++f) amp += std::pow(10.0, a.frames[f].rmsDb / 20.0);
        hist[pc] += amp;
    }
    auto corr = [](const double* p, const double* q) {
        double mp = 0, mq = 0;
        for (int i = 0; i < 12; ++i) { mp += p[i]; mq += q[i]; }
        mp /= 12; mq /= 12;
        double xy = 0, xx = 0, yy = 0;
        for (int i = 0; i < 12; ++i) {
            xy += (p[i] - mp) * (q[i] - mq); xx += (p[i] - mp) * (p[i] - mp); yy += (q[i] - mq) * (q[i] - mq);
        }
        return (xx > 0 && yy > 0) ? xy / std::sqrt(xx * yy) : 0.0;
    };
    Key best;
    best.confidence = -2;
    for (int t = 0; t < 12; ++t) {
        double rot[12];
        for (int i = 0; i < 12; ++i) rot[i] = hist[(i + t) % 12];
        const double cM = corr(rot, maj), cm = corr(rot, mnr);
        if (cM > best.confidence) { best = Key{}; best.tonic = t; best.minor = false; best.confidence = cM; }
        if (cm > best.confidence) { best = Key{}; best.tonic = t; best.minor = true; best.confidence = cm; }
    }
    return best;
}

Analysis analyze(const float* x, size_t n, double sr, const AnalysisParams& P, IPitchDetector* det)
{
    Analysis a;
    a.sampleRate = sr;
    a.numSamples = n;

    std::unique_ptr<IPitchDetector> own;
    if (!det) { own = makeDefaultPitchDetector(P.pitch); det = own.get(); }
    const PitchTrack tr = det->detect(x, n, sr);
    a.hop = tr.hop;
    const int nf = (int)tr.f0.size();
    a.frames.resize(nf);

    const double gateDb = std::max(P.pitch.silenceDb, (double)levelReference(tr.rmsDb) + P.pitch.relSilenceDb);

    const int hfN = FFT::nextPow2((int)std::lround(0.021 * sr));
    FFT hfFft(hfN);
    std::vector<double> hann(hfN);
    for (int j = 0; j < hfN; ++j) hann[j] = 0.5 - 0.5 * std::cos(2 * M_PI * j / hfN);

    for (int k = 0; k < nf; ++k) {
        Frame& f = a.frames[k];
        f.timeSec = (double)k * a.hop / sr;
        f.f0 = tr.f0[k];
        f.midi = hzToMidi(f.f0);
        f.voicedProb = tr.voicedProb[k];
        f.rmsDb = tr.rmsDb[k];
        if (f.f0 > 0) f.kind = FrameKind::Voiced;
        else if (f.rmsDb < gateDb) f.kind = FrameKind::Silence;
        else {
            f.hfRatio = (float)highFreqRatio(x, n, (long)k * a.hop, sr, hfFft, hann);
            f.kind = f.hfRatio > P.sibilantHfRatio ? FrameKind::Sibilant : FrameKind::Breath;
        }
    }

    // ---- Note segmentation ---------------------------------------------------
    const double hopMs = 1000.0 * a.hop / sr;
    const int minLen = std::max(2, (int)std::lround(P.noteMinMs / hopMs));
    a.slowDev.assign(nf, 0.f);
    a.fastDev.assign(nf, 0.f);
    a.noteOfFrame.assign(nf, -1);

    for (int k = 0; k < nf;) {
        if (a.frames[k].f0 <= 0) { ++k; continue; }
        int e = k;
        while (e < nf && a.frames[e].f0 > 0) ++e;
        const int len = e - k;
        if (len < minLen) { k = e; continue; }

        // Light median against jitter and single-frame blips (vibrato is kept).
        std::vector<double> s(len);
        for (int i = 0; i < len; ++i) {
            double w[5]; int c = 0;
            for (int j = std::max(0, i - 2); j <= std::min(len - 1, i + 2); ++j) {
                double v = a.frames[k + j].midi;
                int p = c++;
                while (p > 0 && w[p - 1] > v) { w[p] = w[p - 1]; --p; }
                w[p] = v;
            }
            s[i] = w[c / 2];
        }
        const std::vector<int> cuts = segmentRun(s, &a.frames[k], minLen, P);
        for (size_t c = 0; c + 1 < cuts.size(); ++c)
            appendNote(a, k + cuts[c], k + cuts[c + 1], P.contourSmoothMs, &s[(size_t)cuts[c]]);
        k = e;
    }

    attachConsonants(a, P);
    a.key = detectKey(a);
    a.marks = placePitchMarks(x, n, a, std::max(16.0, P.unvoicedMarkMs / 1000.0 * sr));
    return a;
}

void attachConsonants(Analysis& a, const AnalysisParams& P)
{
    const int nf = (int)a.frames.size();
    const int n = (int)a.notes.size();
    if (n == 0 || nf == 0 || a.sampleRate <= 0) return;
    if (a.noteOfFrame.size() != (size_t)nf) a.noteOfFrame.assign((size_t)nf, -1);
    const double hopSec = (double)a.hop / a.sampleRate;
    // A note's level by the voiced frames next to the pause (up to 30), the loud ones (75th percentile).
    auto levelNear = [&](int from, int to, bool fromEnd) {
        std::vector<double> v;
        for (int k = 0; k < to - from && v.size() < 30; ++k) {
            const int f = fromEnd ? to - 1 - k : from + k;
            if (a.frames[(size_t)f].f0 > 0) v.push_back(a.frames[(size_t)f].rmsDb);
        }
        if (v.empty()) return -1e9;
        std::sort(v.begin(), v.end());
        return v[v.size() * 3 / 4];
    };
    // A run of frames is a consonant if it is not too long for one.
    auto consonant = [&](int s, int e) {
        int sib = 0;
        for (int f = s; f < e; ++f) sib += a.frames[(size_t)f].kind == FrameKind::Sibilant;
        return (e - s) * hopSec <= (2 * sib > e - s ? P.sibilantMaxSec : P.consMaxSec) + 1e-9;
    };
    auto setStart = [&](Note& m, int f) { m.startFrame = f; m.startSec = f * hopSec - 0.5 * hopSec; };
    auto setEnd = [&](Note& m, int f) { m.endFrame = f; m.endSec = f * hopSec - 0.5 * hopSec; };
    // The gaps are taken from the original edges: a note is changed only at the gaps beside it.
    std::vector<std::pair<int, int>> edges;
    for (const Note& m : a.notes) edges.emplace_back(m.startFrame, m.endFrame);
    for (int i = 0; i <= n; ++i) { // the gap in front of note i (i == n: after the last note)
        const int g0 = std::clamp(i > 0 ? edges[(size_t)i - 1].second : 0, 0, nf);
        const int g1 = std::clamp(i < n ? edges[(size_t)i].first : nf, g0, nf);
        if (g1 <= g0) continue;
        double L = -1e9;
        if (i > 0) L = std::max(L, levelNear(edges[(size_t)i - 1].first, g0, true));
        if (i < n) L = std::max(L, levelNear(g1, edges[(size_t)i].second, false));
        if (L < -1e8) continue;
        std::vector<char> pause((size_t)(g1 - g0));
        for (int f = g0; f < g1; ++f)
            pause[(size_t)(f - g0)] = a.frames[(size_t)f].kind == FrameKind::Silence || a.frames[(size_t)f].rmsDb <= L - P.consPauseDb;
        // A short dip between sounds (between a consonant and the vowel, a closure) is not a pause;
        // a short blip of sound inside a pause does not end it.
        auto bridge = [&](char val, int maxLen) {
            for (int s = 0; s < (int)pause.size();) {
                if (pause[(size_t)s] != val) { ++s; continue; }
                int e = s;
                while (e < (int)pause.size() && pause[(size_t)e] == val) ++e;
                if (s > 0 && e < (int)pause.size() && e - s < maxLen)
                    for (int k = s; k < e; ++k) pause[(size_t)k] = (char)!val;
                s = e;
            }
        };
        bridge(1, std::max(1, (int)std::lround(P.consMinPauseSec / hopSec)));
        bridge(0, 2);
        int p0 = -1, p1 = -1;
        for (int k = 0; k < (int)pause.size(); ++k)
            if (pause[(size_t)k]) { if (p0 < 0) p0 = k; p1 = k + 1; }
        if (p0 < 0) continue; // no pause: a consonant inside a word stays between its notes
        p0 += g0;
        p1 += g0;
        if (i > 0 && p0 > g0 && consonant(g0, p0)) {
            Note& m = a.notes[(size_t)i - 1];
            for (int f = g0; f < p0; ++f) a.noteOfFrame[(size_t)f] = i - 1;
            setEnd(m, p0);
        }
        if (i < n && g1 > p1 && consonant(p1, g1)) {
            Note& m = a.notes[(size_t)i];
            for (int f = p1; f < g1; ++f) a.noteOfFrame[(size_t)f] = i;
            setStart(m, p1);
        }
    }
}

} // namespace glina
