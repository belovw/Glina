#include "glina/Harmony.h"
#include "glina/Editing.h"
#include "glina/Tuning.h"

#include <cmath>
#include <cstdio>
#include <limits>

namespace glina {

namespace {

const int kMajor[7] = {0, 2, 4, 5, 7, 9, 11};
const int kMinor[7] = {0, 2, 3, 5, 7, 8, 10};

int floorDiv(int a, int b) { return (a >= 0) ? a / b : -((-a + b - 1) / b); }

const NoteEdit kNone{};

// Deterministic per-note random in [-1, 1].
double noteRand(uint32_t seed, size_t i)
{
    uint32_t h = seed * 2654435761u ^ (uint32_t)(i * 0x9E3779B9u + 0x7F4A7C15u);
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return (h / 4294967295.0) * 2.0 - 1.0;
}

} // namespace

double scaleStep(double midi, const Key& key, int degrees)
{
    const int* steps = key.minor ? kMinor : kMajor;
    const int n = (int)std::lround(nearestTarget(midi, key, true));
    const int rel = n - key.tonic;
    const int oct = floorDiv(rel, 12);
    const int pc = rel - 12 * oct;
    int deg = 0;
    for (int d = 0; d < 7; ++d) if (steps[d] == pc) deg = d;
    const int idx = oct * 7 + deg + degrees;
    const int oct2 = floorDiv(idx, 7);
    const int deg2 = idx - 7 * oct2;
    return key.tonic + 12 * oct2 + steps[deg2];
}

std::string harmonyIntervalName(const HarmonyVoice& v)
{
    if (!v.diatonic) {
        char b[32];
        std::snprintf(b, sizeof b, "%+.0f пт", v.semitones);
        return b;
    }
    static const char* names[] = {"унисон", "секунда", "терция", "кварта", "квинта", "секста", "септима", "октава"};
    const int d = std::abs(v.degrees);
    const std::string nm = d <= 7 ? names[d] : std::to_string(d + 1) + "-я";
    if (d == 0) return nm;
    return nm + (v.degrees > 0 ? " выше" : " ниже");
}

static double leadPitch(const Analysis& a, const EditSet& lead, size_t i)
{
    return editedCenter(a, lead, (int)i); // perceived pitch, tilt included
}

static double harmonyPitch(const Analysis& a, const EditSet& lead, const Key& key, const HarmonyVoice& v, size_t i)
{
    const double m = leadPitch(a, lead, i);
    double target;
    if (v.diatonic) {
        const double snapped = nearestTarget(m, key, true);
        target = scaleStep(m, key, v.degrees) + (m - snapped); // keep the lead's intonation
    } else {
        target = m + v.semitones;
    }
    return target + noteRand(v.seed, i) * v.detuneCents / 100.0;
}

EditSet harmonyEdits(const Analysis& a, const EditSet& lead, const Key& key, const HarmonyVoice& v)
{
    EditSet e = lead;
    e.notes.resize(a.notes.size());
    for (size_t i = 0; i < a.notes.size(); ++i) {
        const NoteEdit& l = i < lead.notes.size() ? lead.notes[i] : kNone;
        NoteEdit& h = e.notes[i];
        h = l;
        // The voice keeps the lead's tilt: its offset aims the tilted note's centre.
        h.pitchOffset = harmonyPitch(a, lead, key, v, i) - a.notes[i].centerMidi - tiltMean(a, (int)i, h);
        h.vibratoAmount = l.vibratoAmount * v.vibratoAmount;
        h.vibrato.depthCents *= v.vibratoAmount; // own auto vibrato, scaled like the singer's
        h.driftAmount = l.driftAmount * v.driftAmount;
        h.formantShift = l.formantShift + v.formantShift;
        const bool on = v.noteMask.empty() || (i < v.noteMask.size() && v.noteMask[i]);
        if (!on) h.gainDb = -120;
    }
    // Auto vibrato: scaled like the singer's vibrato, and out of step with the lead.
    e.autoVibrato.depthCents *= v.vibratoAmount;
    e.vibratoSeed = lead.vibratoSeed ^ (v.seed * 0x9E3779B9u + 0x6A09E667u);
    if (!v.consonants) {
        e.sibilantGainDb = -90;
        e.breathGainDb = -90;
    }
    e.unassignedGainDb = -90;
    return e;
}

std::vector<double> harmonyNotePitches(const Analysis& a, const EditSet& lead, const Key& key, const HarmonyVoice& v)
{
    std::vector<double> out(a.notes.size(), std::numeric_limits<double>::quiet_NaN());
    for (size_t i = 0; i < a.notes.size(); ++i) {
        const bool on = v.noteMask.empty() || (i < v.noteMask.size() && v.noteMask[i]);
        if (on) out[i] = harmonyPitch(a, lead, key, v, i);
    }
    return out;
}

} // namespace glina
