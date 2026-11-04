// Synthetic "singer" for tests: glottal pulse source + formant filter,
// with glides, vibrato, jitter and sibilant bursts between phrases.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace glina::test {

struct SynthNote {
    double midi;        // target pitch
    double dur;         // seconds
    double detuneCents; // how far the "singer" misses the note
    bool sibilantBefore;// 's' noise burst + gap before this note
    int vowel;          // 0 = a, 1 = o, 2 = i, 3 = e
};

struct SynthResult {
    std::vector<float> audio;
    std::vector<double> trueF0;      // per sample, 0 = unvoiced
    std::vector<double> noteStart, noteEnd, noteMidi; // ground truth (sung pitch incl. detune)
};

class Resonator {
public:
    void set(double f, double bw, double sr)
    {
        const double r = std::exp(-M_PI * bw / sr);
        a1 = 2 * r * std::cos(2 * M_PI * f / sr);
        a2 = -r * r;
        g = 1 - r;
    }
    double tick(double x)
    {
        const double y = g * x + a1 * y1 + a2 * y2;
        y2 = y1; y1 = y;
        return y;
    }
private:
    double a1 = 0, a2 = 0, g = 1, y1 = 0, y2 = 0;
};

inline SynthResult synthesize(const std::vector<SynthNote>& notes, double sr, double vibratoCents = 35,
                              double vibratoHz = 5.6, uint32_t seed = 12345)
{
    static const double F[4][4] = {{750, 1200, 2600, 3500}, {500, 850, 2500, 3400},
                                   {300, 2300, 3000, 3700}, {480, 1900, 2600, 3500}};
    static const double BW[4] = {80, 100, 140, 200};
    SynthResult r;
    uint32_t rng = seed;
    auto noise = [&]() { rng = rng * 1664525u + 1013904223u; return ((rng >> 8) / 8388608.0) - 1.0; };

    Resonator res[4];
    double phase = 0, prevMidi = notes.empty() ? 60 : notes[0].midi + notes[0].detuneCents / 100;
    double hpState = 0, lpS = 0;
    double t = 0.15; // leading silence
    r.audio.assign((size_t)(t * sr), 0.f);
    r.trueF0.assign(r.audio.size(), 0.0);
    std::vector<uint8_t> sib(r.audio.size(), 0);

    for (const auto& n : notes) {
        if (n.sibilantBefore) {
            const size_t len = (size_t)(0.09 * sr), gap = (size_t)(0.03 * sr);
            for (size_t i = 0; i < len; ++i) {
                const double w = std::sin(M_PI * i / len);
                const double v = noise();
                hpState = 0.6 * hpState + 0.4 * v;
                r.audio.push_back((float)(0.08 * w * (v - hpState)));
                r.trueF0.push_back(0);
                sib.push_back(1);
            }
            for (size_t i = 0; i < gap; ++i) { r.audio.push_back(0.f); r.trueF0.push_back(0); sib.push_back(0); }
            t += 0.12;
        }
        for (int k = 0; k < 4; ++k) res[k].set(F[n.vowel][k], BW[k], sr);
        const double sung = n.midi + n.detuneCents / 100.0;
        const size_t len = (size_t)(n.dur * sr);
        r.noteStart.push_back(r.audio.size() / sr);
        r.noteMidi.push_back(sung);
        for (size_t i = 0; i < len; ++i) {
            const double tt = i / sr;
            // 40 ms glide from the previous pitch, vibrato fades in after 0.2 s.
            const double glide = std::min(1.0, tt / 0.04);
            double m = prevMidi + (sung - prevMidi) * (0.5 - 0.5 * std::cos(M_PI * glide));
            const double vibAmt = std::clamp((tt - 0.2) / 0.25, 0.0, 1.0);
            m += vibAmt * vibratoCents / 100.0 * std::sin(2 * M_PI * vibratoHz * tt);
            m += 0.03 * noise() * 0.1; // tiny jitter
            const double f0 = 440.0 * std::pow(2.0, (m - 69) / 12.0);
            phase += f0 / sr;
            if (phase >= 1) phase -= 1;
            // Rosenberg-like glottal pulse (open 60 %), differentiated by the lip radiation.
            const double op = 0.6;
            double g = phase < op * 0.66 ? 0.5 * (1 - std::cos(M_PI * phase / (op * 0.66)))
                     : phase < op ? std::cos(M_PI * (phase - op * 0.66) / (op * 0.68) * 0.5) : 0.0;
            g += 0.02 * noise(); // aspiration
            double s = g;
            for (int k = 0; k < 4; ++k) s = res[k].tick(s);
            const double y = s - lpS; lpS = s; // radiation (first difference)
            const double env = std::min({1.0, i / (0.015 * sr), (len - i) / (0.02 * sr)});
            r.audio.push_back((float)(0.35 * env * y * 6.0));
            r.trueF0.push_back(env > 0.3 ? f0 : 0.0);
            sib.push_back(0);
        }
        r.noteEnd.push_back(r.audio.size() / sr);
        prevMidi = sung;
    }
    for (size_t i = 0; i < (size_t)(0.2 * sr); ++i) { r.audio.push_back(0.f); r.trueF0.push_back(0); sib.push_back(0); }
    // Voice peaks at -6 dBFS, sibilants at a realistic -16 dBFS.
    double pv = 0, ps = 0;
    for (size_t i = 0; i < r.audio.size(); ++i)
        (sib[i] ? ps : pv) = std::max(sib[i] ? ps : pv, (double)std::abs(r.audio[i]));
    for (size_t i = 0; i < r.audio.size(); ++i)
        r.audio[i] = (float)(r.audio[i] * (sib[i] ? (ps > 0 ? 0.16 / ps : 0) : (pv > 0 ? 0.5 / pv : 0)));
    return r;
}

inline std::vector<SynthNote> demoMelody()
{
    // A short phrase in A minor with typical singer errors (cents off).
    return {
        {57, 0.45, -18, false, 0}, {60, 0.35, +25, false, 0}, {64, 0.6, -32, false, 1},
        {62, 0.3, +12, true, 3},   {60, 0.3, -22, false, 0},  {59, 0.35, +38, false, 2},
        {57, 0.9, -15, true, 0},   {64, 0.5, +28, false, 1},  {69, 0.8, -26, false, 0},
    };
}

} // namespace glina::test
