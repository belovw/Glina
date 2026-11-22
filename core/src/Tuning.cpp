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

double nearestTarget(double midi, const Key& key, bool useKey)
{
    if (!useKey) return std::round(midi);
    int steps[7];
    scaleSteps(key, steps);
    double best = std::round(midi), bestDist = 1e9;
    const int base = (int)std::floor(midi) - 13;
    for (int m = base; m <= base + 26; ++m) {
        const int pc = ((m - key.tonic) % 12 + 12) % 12;
        bool inScale = false;
        for (int s = 0; s < 7; ++s) if (steps[s] == pc) inScale = true;
        if (!inScale) continue;
        const double d = std::abs(midi - m);
        if (d < bestDist) { bestDist = d; best = m; }
    }
    return best;
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
