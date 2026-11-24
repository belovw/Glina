#include "glina/Tuning.h"

#include <cmath>

namespace glina {

void scaleSteps(const Key& key, int steps[7])
{
    static const int major[7] = {0, 2, 4, 5, 7, 9, 11};
    static const int natural[7] = {0, 2, 3, 5, 7, 8, 10};
    static const int melodic[7] = {0, 2, 3, 5, 7, 9, 11};
    static const int harmonic[7] = {0, 2, 3, 5, 7, 8, 11};
    const int* src = !key.minor ? major : key.minorForm == 1 ? melodic : key.minorForm == 2 ? harmonic : natural;
    for (int s = 0; s < 7; ++s) steps[s] = src[s];
}

void scaleCents(const Key& key, double cents[7])
{
    // Pure intervals as ratios: major 1, 9/8, 5/4, 4/3, 3/2, 5/3, 15/8; minor 1, 9/8, 6/5, 4/3, 3/2, 8/5, 9/5.
    static const double justMajor[7] = {0, 203.910, 386.314, 498.045, 701.955, 884.359, 1088.269};
    static const double justMinor[7] = {0, 203.910, 315.641, 498.045, 701.955, 813.686, 1017.596};
    if (key.just) {
        for (int s = 0; s < 7; ++s) cents[s] = key.minor ? justMinor[s] : justMajor[s];
        return;
    }
    int steps[7];
    scaleSteps(key, steps);
    for (int s = 0; s < 7; ++s) cents[s] = 100.0 * steps[s];
}

double nearestTarget(double midi, const Key& key, bool useKey)
{
    if (!useKey) return std::round(midi);
    double cents[7];
    scaleCents(key, cents);
    double best = std::round(midi), bestDist = 1e9;
    const int base = (int)std::floor(midi) - 13;
    for (int m = base; m <= base + 26; ++m) {
        const int pc = ((m - key.tonic) % 12 + 12) % 12;
        if (pc != 0) continue; // every octave of the tonic; the degrees are added above it
        for (int s = 0; s < 7; ++s) {
            const double cand = m + cents[s] / 100.0;
            const double d = std::abs(midi - cand);
            if (d < bestDist) { bestDist = d; best = cand; }
        }
    }
    return best;
}

double scaleInterval(double midi, const Key& key, bool useKey, int steps)
{
    double cents[7];
    scaleCents(key, cents);
    int sc[7];
    scaleSteps(key, sc);
    const int tonic = ((key.tonic % 12) + 12) % 12;
    if (steps == 0) {
        const double r = std::round(midi);
        if (!useKey) return r;
        const int pc = (((int)r - tonic) % 12 + 12) % 12;
        for (int s = 0; s < 7; ++s)
            if (sc[s] == pc) return r - pc + cents[s] / 100.0; // the degree's own pitch (pure in natural tuning)
        return r; // outside the scale: its own semitone
    }
    // The nearest degree: an octave of the tonic `base` and a degree `deg` above it.
    int bestBase = 0, bestDeg = 0;
    double bestDist = 1e9;
    const int lo = (int)std::floor(midi) - 13;
    for (int m = lo; m <= lo + 26; ++m) {
        if (((m - tonic) % 12 + 12) % 12 != 0) continue;
        for (int s = 0; s < 7; ++s) {
            const double d = std::abs(midi - (m + cents[s] / 100.0));
            if (d < bestDist - 1e-9) { bestDist = d; bestBase = m; bestDeg = s; }
        }
    }
    const int idx = bestDeg + steps;
    const int oct = idx >= 0 ? idx / 7 : -((-idx + 6) / 7);
    const int deg = idx - 7 * oct;
    return bestBase + 12 * oct + cents[deg] / 100.0;
}

EditSet makeAutoTune(const Analysis& a, const TuneParams& p)
{
    EditSet e;
    e.notes.resize(a.notes.size());
    Key key = a.key;
    if (p.tonic >= 0) { key.tonic = p.tonic; key.minor = p.minor; }
    for (size_t i = 0; i < a.notes.size(); ++i) {
        const double c = a.notes[i].centerMidi;
        const double target = nearestTarget(c, key, p.useKey);
        NoteEdit& ed = e.notes[i];
        const double diffCents = (target - c) * 100.0;
        if (std::abs(diffCents) > p.toleranceCents) ed.pitchOffset = (target - c) * p.pitchAmount;
        ed.driftAmount = 1.0 - p.driftAmount;
        ed.vibratoAmount = p.vibratoScale;
        ed.formantShift = p.formantShift;
    }
    return e;
}

EditSet makeTranspose(const Analysis& a, double semitones, double formantShift)
{
    EditSet e;
    e.notes.resize(a.notes.size());
    for (auto& ed : e.notes) { ed.pitchOffset = semitones; ed.formantShift = formantShift; }
    return e;
}

} // namespace glina
