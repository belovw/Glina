#include "glina/PitchDetector.h"
#include "glina/FFT.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <limits>
#include <thread>

namespace glina {

namespace {

struct Candidate {
    double f0;      // Hz
    double cmnd;    // cumulative-mean-normalised difference at the dip (0 = perfectly periodic)
    double prob;    // pYIN-style threshold probability
};

inline double hzToMidi(double hz) { return 69.0 + 12.0 * std::log2(hz / 440.0); }
inline double midiToHz(double m) { return 440.0 * std::pow(2.0, (m - 69.0) / 12.0); }

// Beta(2, 18) prior over YIN thresholds, as in pYIN (Mauch & Dixon, 2014).
std::vector<double> thresholdWeights(std::vector<double>& thresholds)
{
    const int n = 100;
    thresholds.resize(n);
    std::vector<double> w(n);
    double sum = 0;
    for (int i = 0; i < n; ++i) {
        const double t = (i + 1) / 100.0;
        thresholds[i] = t;
        w[i] = t * std::pow(1.0 - t, 17.0);
        sum += w[i];
    }
    for (auto& v : w) v /= sum;
    return w;
}

} // namespace

float levelReference(const std::vector<float>& rmsDb)
{
    // 95th percentile of non-silent frames: robust to isolated clicks, plosives
    // and loud sibilants that would make a max-based gate swallow the vocal.
    std::vector<float> v;
    v.reserve(rmsDb.size());
    for (float d : rmsDb) if (d > -100) v.push_back(d);
    if (v.empty()) return -120.f;
    const size_t k = (size_t)(0.95 * (v.size() - 1));
    std::nth_element(v.begin(), v.begin() + k, v.end());
    return v[k];
}

PitchTrack YinViterbiDetector::detect(const float* x, size_t n, double sr)
{
    PitchTrack out;
    const int hop = std::max(1, (int)std::lround(p_.hopSec * sr));
    const int W = std::max(32, (int)std::lround(p_.windowSec * sr));
    const int tauMax = (int)std::ceil(sr / p_.minF0) + 1;
    const int tauMin = std::max(2, (int)std::floor(sr / p_.maxF0));
    const int lenB = W + tauMax + 2;
    const int N = FFT::nextPow2(lenB);
    FFT fft(N);

    const int numFrames = (int)(n / hop) + 1;
    out.hop = hop;
    out.f0.assign(numFrames, 0.0);
    out.voicedProb.assign(numFrames, 0.f);
    out.rmsDb.assign(numFrames, -120.f);

    std::vector<double> thr;
    const std::vector<double> thrW = thresholdWeights(thr);

    std::vector<std::vector<Candidate>> cands(numFrames);
    auto sample = [&](long i) -> double { return (i >= 0 && (size_t)i < n) ? x[i] : 0.0; };

    // Frames are independent: analyse them in parallel, each worker with its own buffers.
    auto processRange = [&](int kBegin, int kEnd) {
        std::vector<std::complex<double>> Z(N), R(N);
        std::vector<double> b(lenB), prefix(lenB + 1), d(tauMax + 2), cm(tauMax + 2), prob;
        std::vector<std::pair<int, double>> minima;
        for (int k = kBegin; k < kEnd; ++k) {
            const long centre = (long)k * hop;

            // Level over a W-sample window centred on the frame.
            double e = 0;
            for (int j = 0; j < W; ++j) { const double s = sample(centre - W / 2 + j); e += s * s; }
            out.rmsDb[k] = (float)(10.0 * std::log10(e / W + 1e-14));

            const long s0 = centre - (W + tauMax) / 2;
            for (int j = 0; j < lenB; ++j) b[j] = sample(s0 + j);
            prefix[0] = 0;
            for (int j = 0; j < lenB; ++j) prefix[j + 1] = prefix[j] + b[j] * b[j];
            const double E0 = prefix[W];
            if (E0 < 1e-10) continue;

            // r[tau] = sum_j a[j] * b[j + tau], a = first W samples of b. Both real
            // signals go through one complex FFT (a in the real part, b in the imaginary).
            for (int j = 0; j < N; ++j) Z[j] = {(j < W) ? b[j] : 0.0, (j < lenB) ? b[j] : 0.0};
            fft.forward(Z);
            for (int j = 0; j < N; ++j) {
                const std::complex<double> zk = Z[j], zc = std::conj(Z[(N - j) & (N - 1)]);
                // A = (zk + zc) / 2, B = (zk - zc) / 2i ; R = conj(A) * B
                const double ar = 0.5 * (zk.real() + zc.real()), ai = 0.5 * (zk.imag() + zc.imag());
                const double br = 0.5 * (zk.imag() - zc.imag()), bi = -0.5 * (zk.real() - zc.real());
                R[j] = {ar * br + ai * bi, ar * bi - ai * br};
            }
            fft.inverse(R);

            // YIN difference and cumulative-mean normalisation.
            d[0] = 0;
            cm[0] = 1;
            double running = 0;
            for (int tau = 1; tau <= tauMax; ++tau) {
                const double Etau = prefix[tau + W] - prefix[tau];
                d[tau] = std::max(0.0, E0 + Etau - 2.0 * R[tau].real());
                running += d[tau];
                cm[tau] = running > 0 ? d[tau] * tau / running : 1.0;
            }

            // Local minima of the normalised difference = period candidates.
            minima.clear();
            for (int tau = tauMin; tau < tauMax; ++tau)
                if (cm[tau] < cm[tau - 1] && cm[tau] <= cm[tau + 1])
                    minima.push_back({tau, cm[tau]});
            if (minima.empty()) continue;

            prob.assign(minima.size(), 0.0);
            int globalMin = 0;
            for (size_t i = 1; i < minima.size(); ++i)
                if (minima[i].second < minima[globalMin].second) globalMin = (int)i;
            // For each threshold the first minimum below it wins: walk thresholds and
            // minima together instead of rescanning.
            for (size_t t = 0; t < thr.size(); ++t) {
                bool found = false;
                for (size_t i = 0; i < minima.size(); ++i) {
                    if (minima[i].second < thr[t]) { prob[i] += thrW[t]; found = true; break; }
                }
                if (!found) prob[globalMin] += thrW[t] * 0.01;
            }

            for (size_t i = 0; i < minima.size(); ++i) {
                const int tau = minima[i].first;
                // Parabolic refinement of the dip position.
                const double y0 = cm[tau - 1], y1 = cm[tau], y2 = cm[tau + 1];
                const double den = y0 - 2.0 * y1 + y2;
                double off = den > 1e-12 ? 0.5 * (y0 - y2) / den : 0.0;
                off = std::clamp(off, -0.5, 0.5);
                const double tauF = tau + off;
                const double c = std::max(0.0, y1 - 0.25 * (y0 - y2) * off);
                cands[k].push_back({sr / tauF, c, prob[i]});
            }
        }
    };
    {
        const int hw = (int)std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
        const int workers = std::max(1, std::min(hw, numFrames / 2000));
        std::vector<std::thread> pool;
        const int chunk = (numFrames + workers - 1) / workers;
        for (int w = 1; w < workers; ++w)
            pool.emplace_back(processRange, w * chunk, std::min(numFrames, (w + 1) * chunk));
        processRange(0, std::min(numFrames, chunk));
        for (auto& t : pool) t.join();
    }

    // ---- Level gate ------------------------------------------------------
    const double gateDb = std::max(p_.silenceDb, (double)levelReference(out.rmsDb) + p_.relSilenceDb);
    std::vector<double> gate(numFrames);
    for (int k = 0; k < numFrames; ++k)
        gate[k] = std::clamp((out.rmsDb[k] - gateDb) / 6.0, 0.0, 1.0);

    // ---- Stage 1: Viterbi pitch path over a cents grid --------------------
    const double minMidi = hzToMidi(p_.minF0), maxMidi = hzToMidi(p_.maxF0);
    const double step = p_.binCents / 100.0;
    const int nb = (int)std::floor((maxMidi - minMidi) / step) + 1;
    const int wb = std::max(1, (int)std::lround(p_.maxJumpCentsPerFrame / p_.binCents));
    std::vector<double> logT(wb + 1);
    {
        double sum = 0;
        for (int dd = -wb; dd <= wb; ++dd) sum += (wb + 1 - std::abs(dd));
        for (int dd = 0; dd <= wb; ++dd) logT[dd] = std::log((wb + 1 - dd) / sum);
    }
    auto binOf = [&](double hz) { return std::clamp((int)std::lround((hzToMidi(hz) - minMidi) / step), 0, nb - 1); };

    std::vector<int8_t> back((size_t)numFrames * nb);
    std::vector<double> delta(nb, 0.0), next(nb), mass(nb);
    const double floorObs = 1e-4;
    for (int k = 0; k < numFrames; ++k) {
        std::fill(mass.begin(), mass.end(), 0.0);
        for (const auto& c : cands[k]) {
            const int bi = binOf(c.f0);
            const double pm = c.prob * gate[k];
            mass[bi] += pm;
            if (bi > 0) mass[bi - 1] += 0.4 * pm;
            if (bi + 1 < nb) mass[bi + 1] += 0.4 * pm;
        }
        double best = -std::numeric_limits<double>::infinity();
        for (int bi = 0; bi < nb; ++bi) {
            double m = -std::numeric_limits<double>::infinity();
            int arg = 0;
            if (k == 0) { m = 0; }
            else {
                const int lo = std::max(0, bi - wb), hi = std::min(nb - 1, bi + wb);
                for (int pj = lo; pj <= hi; ++pj) {
                    const double v = delta[pj] + logT[std::abs(pj - bi)];
                    if (v > m) { m = v; arg = pj - bi; }
                }
            }
            back[(size_t)k * nb + bi] = (int8_t)arg;
            next[bi] = m + std::log(floorObs + mass[bi]);
            best = std::max(best, next[bi]);
        }
        for (int bi = 0; bi < nb; ++bi) delta[bi] = next[bi] - best;
    }
    std::vector<int> path(numFrames);
    path[numFrames - 1] = (int)(std::max_element(delta.begin(), delta.end()) - delta.begin());
    for (int k = numFrames - 1; k > 0; --k)
        path[k - 1] = path[k] + back[(size_t)k * nb + path[k]];

    // ---- Stage 2: voicing HMM along the path -----------------------------
    // Evidence = how periodic the signal is at the tracked pitch.
    std::vector<double> ev(numFrames, 0.0);
    std::vector<int> pick(numFrames, -1);
    for (int k = 0; k < numFrames; ++k) {
        double bestC = 1.0;
        for (size_t i = 0; i < cands[k].size(); ++i) {
            const int bi = binOf(cands[k][i].f0);
            if (std::abs(bi - path[k]) <= 2 && cands[k][i].cmnd < bestC) {
                bestC = cands[k][i].cmnd;
                pick[k] = (int)i;
            }
        }
        const double periodic = 1.0 / (1.0 + std::exp(-(0.35 - bestC) / 0.06));
        ev[k] = std::clamp(periodic * gate[k], 1e-4, 1.0 - 1e-4);
        out.voicedProb[k] = (float)ev[k];
    }
    {
        const double ls = std::log(1.0 - p_.voicingSwitchProb), lw = std::log(p_.voicingSwitchProb);
        std::vector<std::array<int8_t, 2>> bp(numFrames);
        double dv = std::log(ev[0]), du = std::log(1.0 - ev[0]);
        for (int k = 1; k < numFrames; ++k) {
            const double vFromV = dv + ls, vFromU = du + lw;
            const double uFromU = du + ls, uFromV = dv + lw;
            bp[k][1] = vFromV >= vFromU ? 1 : 0;
            bp[k][0] = uFromU >= uFromV ? 0 : 1;
            const double nv = std::max(vFromV, vFromU) + std::log(ev[k]);
            const double nu = std::max(uFromU, uFromV) + std::log(1.0 - ev[k]);
            const double m = std::max(nv, nu);
            dv = nv - m; du = nu - m;
        }
        int st = dv >= du ? 1 : 0;
        std::vector<int> voiced(numFrames);
        for (int k = numFrames - 1; k >= 0; --k) {
            voiced[k] = st;
            if (k > 0) st = bp[k][st];
        }
        for (int k = 0; k < numFrames; ++k) {
            if (!voiced[k]) { out.f0[k] = 0; continue; }
            out.f0[k] = pick[k] >= 0 ? cands[k][pick[k]].f0 : midiToHz(minMidi + path[k] * step);
        }
    }

    // ---- Clean-up ------------------------------------------------------------
    // Fill 1–2 frame holes inside a steady voiced line.
    for (int k = 1; k + 1 < numFrames; ++k) {
        if (out.f0[k] > 0) continue;
        int e = k;
        while (e < numFrames && out.f0[e] <= 0 && e - k < 3) ++e;
        if (e - k <= 2 && e < numFrames && out.f0[k - 1] > 0 && out.f0[e] > 0 &&
            std::abs(1200.0 * std::log2(out.f0[e] / out.f0[k - 1])) < 100.0) {
            const double a = std::log(out.f0[k - 1]), bb = std::log(out.f0[e]);
            for (int j = k; j < e; ++j)
                out.f0[j] = std::exp(a + (bb - a) * (j - k + 1) / (double)(e - k + 1));
        }
        k = e - 1;
    }
    // Drop voiced islands that are too short to be sung notes.
    const int minRun = std::max(1, (int)std::lround(p_.minVoicedMs / 1000.0 * sr / hop));
    for (int k = 0; k < numFrames;) {
        if (out.f0[k] <= 0) { ++k; continue; }
        int e = k;
        while (e < numFrames && out.f0[e] > 0) ++e;
        if (e - k < minRun)
            for (int j = k; j < e; ++j) out.f0[j] = 0;
        k = e;
    }
    return out;
}

std::unique_ptr<IPitchDetector> makeDefaultPitchDetector(const PitchParams& p)
{
    return std::make_unique<YinViterbiDetector>(p);
}

} // namespace glina
