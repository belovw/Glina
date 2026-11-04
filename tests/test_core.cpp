// Engine tests on synthetic material with known ground truth.
#include "Synth.h"
#include "glina/Analysis.h"
#include "glina/Renderer.h"
#include "glina/Tuning.h"
#include "glina/Document.h"
#include "glina/Notes.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace glina;

static int failures = 0;
#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) { ++failures; std::printf("  FAIL: " __VA_ARGS__); std::printf("\n"); } \
    } while (0)

static double centsErr(double a, double b) { return 1200.0 * std::log2(a / b); }

static void testSines()
{
    std::printf("[sines] pitch accuracy on pure tones\n");
    const double sr = 48000;
    for (double f : {82.4, 110.0, 196.0, 329.6, 523.3, 880.0}) {
        std::vector<float> x((size_t)(sr * 0.6));
        for (size_t i = 0; i < x.size(); ++i) x[i] = (float)(0.4 * std::sin(2 * M_PI * f * i / sr) + 0.1 * std::sin(4 * M_PI * f * i / sr));
        YinViterbiDetector det;
        const PitchTrack tr = det.detect(x.data(), x.size(), sr);
        double worst = 0;
        int voiced = 0;
        for (size_t k = 10; k + 10 < tr.f0.size(); ++k) {
            if (tr.f0[k] <= 0) continue;
            ++voiced;
            worst = std::max(worst, std::abs(centsErr(tr.f0[k], f)));
        }
        std::printf("  %7.1f Hz: worst %.2f cents, voiced %d/%zu\n", f, worst, voiced, tr.f0.size() - 20);
        CHECK(worst < 2.0, "%.1f Hz worst error %.2f cents", f, worst);
        CHECK(voiced > (int)(tr.f0.size() - 20) * 9 / 10, "%.1f Hz voicing too low", f);
    }
}

static void testSilenceAndNoise()
{
    std::printf("[noise] no pitch in white noise / silence\n");
    const double sr = 44100;
    std::vector<float> x((size_t)sr);
    uint32_t rng = 7;
    for (size_t i = 0; i < x.size(); ++i) {
        rng = rng * 1664525u + 1013904223u;
        x[i] = i < x.size() / 2 ? 0.f : (float)(0.2 * (((rng >> 8) / 8388608.0) - 1.0));
    }
    YinViterbiDetector det;
    const PitchTrack tr = det.detect(x.data(), x.size(), sr);
    int voiced = 0;
    for (double f : tr.f0) voiced += f > 0;
    std::printf("  voiced frames: %d of %zu\n", voiced, tr.f0.size());
    CHECK(voiced < (int)tr.f0.size() / 50, "noise detected as voiced in %d frames", voiced);
}

static Analysis analyseSynth(const test::SynthResult& s, double sr) { return analyze(s.audio.data(), s.audio.size(), sr); }

static void testNotes(const test::SynthResult& s, const Analysis& a)
{
    std::printf("[notes] segmentation and note centres on synthetic singer\n");
    std::printf("  truth %zu notes, detected %zu, key %s\n", s.noteMidi.size(), a.notes.size(), a.key.name().c_str());
    CHECK(a.notes.size() == s.noteMidi.size(), "note count %zu != %zu", a.notes.size(), s.noteMidi.size());
    // Match each true note to the detected note with the largest time overlap.
    double worst = 0;
    for (size_t t = 0; t < s.noteMidi.size(); ++t) {
        double bestOv = 0; int bi = -1;
        for (size_t i = 0; i < a.notes.size(); ++i) {
            const double ov = std::min(s.noteEnd[t], a.notes[i].endSec) - std::max(s.noteStart[t], a.notes[i].startSec);
            if (ov > bestOv) { bestOv = ov; bi = (int)i; }
        }
        CHECK(bi >= 0, "true note %zu not found", t);
        if (bi < 0) continue;
        const double e = (a.notes[bi].centerMidi - s.noteMidi[t]) * 100.0;
        worst = std::max(worst, std::abs(e));
        std::printf("  note %zu: true %.2f detected %.2f (%+.1f cents)\n", t, s.noteMidi[t], a.notes[bi].centerMidi, e);
    }
    CHECK(worst < 6.0, "worst note centre error %.1f cents", worst);
    CHECK(a.key.tonic == 9 && a.key.minor, "key %s, expected A minor", a.key.name().c_str());
}

static void testNull(const test::SynthResult& s, const Analysis& a)
{
    std::printf("[null] render with no edits reproduces the input\n");
    const std::vector<float> y = render(s.audio.data(), s.audio.size(), a, EditSet{});
    CHECK(y.size() == s.audio.size(), "length %zu != %zu", y.size(), s.audio.size());
    double se = 0, ss = 0;
    for (size_t i = 0; i < std::min(y.size(), s.audio.size()); ++i) {
        const double d = y[i] - s.audio[i];
        se += d * d; ss += (double)s.audio[i] * s.audio[i];
    }
    const double snr = 10 * std::log10(ss / (se + 1e-30));
    std::printf("  SNR %.1f dB\n", snr);
    CHECK(snr > 80, "null-test SNR %.1f dB", snr);
}

static void testTranspose(const test::SynthResult& s, const Analysis& a, double sr)
{
    std::printf("[shift] transpose +3 and -5 semitones, re-analyse output\n");
    for (double st : {3.0, -5.0}) {
        const std::vector<float> y = render(s.audio.data(), s.audio.size(), a, makeTranspose(a, st));
        const Analysis b = analyze(y.data(), y.size(), sr);
        double worst = 0;
        size_t matched = 0;
        for (const Note& n : a.notes) {
            for (const Note& m : b.notes) {
                const double ov = std::min(n.endSec, m.endSec) - std::max(n.startSec, m.startSec);
                if (ov > 0.5 * (n.endSec - n.startSec)) {
                    worst = std::max(worst, std::abs((m.centerMidi - n.centerMidi - st) * 100));
                    ++matched;
                    break;
                }
            }
        }
        std::printf("  %+g st: matched %zu/%zu notes, worst %.1f cents\n", st, matched, a.notes.size(), worst);
        CHECK(matched == a.notes.size(), "%+g st: only %zu notes matched", st, matched);
        CHECK(worst < 8, "%+g st: worst %.1f cents", st, worst);
    }
}

static void testTune(const test::SynthResult& s, const Analysis& a, double sr)
{
    std::printf("[tune] snap to A minor, flatten drift by 50%%\n");
    TuneParams tp;
    tp.driftAmount = 0.5;
    const EditSet e = makeAutoTune(a, tp);
    const std::vector<float> y = render(s.audio.data(), s.audio.size(), a, e);
    const Analysis b = analyze(y.data(), y.size(), sr);
    double worst = 0, before = 0;
    for (const Note& n : a.notes) {
        const double target = nearestTarget(n.centerMidi, a.key, true);
        before = std::max(before, std::abs(n.centerMidi - target) * 100);
        for (const Note& m : b.notes) {
            const double ov = std::min(n.endSec, m.endSec) - std::max(n.startSec, m.startSec);
            if (ov > 0.5 * (n.endSec - n.startSec)) { worst = std::max(worst, std::abs(m.centerMidi - target) * 100); break; }
        }
    }
    std::printf("  worst deviation from target: before %.1f cents, after %.1f cents\n", before, worst);
    CHECK(worst < 6, "tuned notes off by %.1f cents", worst);
}

static void testStretch(const test::SynthResult& s, const Analysis& a, double sr)
{
    std::printf("[stretch] time-stretch x1.25 keeps pitch\n");
    EditSet e;
    e.notes.resize(a.notes.size());
    for (size_t i = 0; i < a.notes.size(); ++i) {
        e.notes[i].timeOffsetSec = a.notes[i].startSec * 0.25;
        e.notes[i].stretch = 1.25;
    }
    const std::vector<float> y = render(s.audio.data(), s.audio.size(), a, e);
    // Everything up to the end of the last note is stretched; the trailing silence is not.
    const double expected = s.audio.size() + 0.25 * a.notes.back().endSec * sr;
    const double ratio = (double)y.size() / expected;
    const Analysis b = analyze(y.data(), y.size(), sr);
    double worst = 0, worstDur = 0;
    for (size_t i = 0; i < a.notes.size() && i < b.notes.size(); ++i) {
        worst = std::max(worst, std::abs(b.notes[i].centerMidi - a.notes[i].centerMidi) * 100);
        const double d0 = a.notes[i].endSec - a.notes[i].startSec, d1 = b.notes[i].endSec - b.notes[i].startSec;
        worstDur = std::max(worstDur, std::abs(d1 / d0 - 1.25));
    }
    std::printf("  length vs expected %.4f, notes %zu -> %zu, worst pitch change %.1f cents, worst duration error %.3f\n",
                ratio, a.notes.size(), b.notes.size(), worst, worstDur);
    CHECK(std::abs(ratio - 1.0) < 0.002, "length ratio %.4f", ratio);
    CHECK(worstDur < 0.08, "note duration off by %.3f", worstDur);
    CHECK(b.notes.size() == a.notes.size(), "note count changed %zu -> %zu", a.notes.size(), b.notes.size());
    CHECK(worst < 8, "pitch changed by %.1f cents", worst);
}


static void testLayout(const Analysis& a)
{
    std::printf("[layout] re-deriving notes from their own spans changes nothing\n");
    const Analysis b = withLayout(a, spansOf(a));
    double worst = 0;
    CHECK(b.notes.size() == a.notes.size(), "note count %zu -> %zu", a.notes.size(), b.notes.size());
    for (size_t i = 0; i < a.notes.size() && i < b.notes.size(); ++i)
        worst = std::max(worst, std::abs(a.notes[i].centerMidi - b.notes[i].centerMidi) * 100);
    std::printf("  worst centre change %.2f cents\n", worst);
    CHECK(worst < 0.5, "centre moved %.2f cents", worst);
}

static void testSplitJoin(const test::SynthResult& s, const Analysis& a, double sr)
{
    std::printf("[split/join] split a note, move only its second half, join back\n");
    Document d;
    conform(d, a);
    const int i = 2;
    const Note n = a.notes[i];
    const int mid = (n.startFrame + n.endFrame) / 2;
    CHECK(splitNote(d, a, i, mid), "split refused");
    Analysis eff = withLayout(a, d.edits.spans);
    CHECK(eff.notes.size() == a.notes.size() + 1, "split gave %zu notes", eff.notes.size());
    d.edits.notes[(size_t)i + 1].pitchOffset = 2.0;
    const Mix m = renderMix(s.audio.data(), s.audio.size(), eff, d);
    const Analysis b = analyze(m.lead.data(), m.lead.size(), sr);
    // Pitch in the middle of each half (frames away from the transition).
    auto pitchAround = [&](int f0, int f1) {
        double sum = 0; int c = 0;
        for (int f = f0; f < f1 && f < (int)b.frames.size(); ++f) if (b.frames[f].f0 > 0) { sum += b.frames[f].midi; ++c; }
        return c ? sum / c : 0.0;
    };
    const int q = (n.endFrame - n.startFrame) / 8;
    const double first = pitchAround(n.startFrame + q, mid - q);
    const double second = pitchAround(mid + q, n.endFrame - q);
    std::printf("  first half %+.2f st, second half %+.2f st (want 0 / +2)\n", first - a.notes[i].centerMidi,
                second - a.notes[i].centerMidi);
    CHECK(std::abs(first - a.notes[i].centerMidi) < 0.25, "first half moved");
    CHECK(std::abs(second - a.notes[i].centerMidi - 2.0) < 0.25, "second half not +2");
    CHECK(joinNotes(d, eff, i), "join refused");
    eff = withLayout(a, d.edits.spans);
    CHECK(eff.notes.size() == a.notes.size(), "join gave %zu notes", eff.notes.size());
}

static void testHarmony(const test::SynthResult& s, const Analysis& a, double sr)
{
    std::printf("[harmony] diatonic third above follows the scale\n");
    Document d;
    conform(d, a);
    // Correct the lead first so every note sits on the scale.
    TuneParams tp;
    d.edits = makeAutoTune(a, tp);
    HarmonyVoice v;
    v.degrees = 2;
    v.detuneCents = 0;
    v.delayMs = 0;
    v.gainDb = 0;
    d.voices.push_back(v);
    const Mix m = renderMix(s.audio.data(), s.audio.size(), a, d);
    CHECK(m.voices.size() == 1, "no voice rendered");
    if (m.voices.empty()) return;
    const Analysis b = analyze(m.voices[0].data(), m.voices[0].size(), sr);
    const Key key = d.key(a);
    double worst = 0;
    size_t matched = 0;
    for (size_t k = 0; k < a.notes.size(); ++k) {
        const Note& n = a.notes[k];
        const double lead = nearestTarget(n.centerMidi, key, true);
        const double want = scaleStep(lead, key, 2);
        for (const Note& h : b.notes) {
            const double ov = std::min(n.endSec, h.endSec) - std::max(n.startSec, h.startSec);
            if (ov > 0.5 * (n.endSec - n.startSec)) { worst = std::max(worst, std::abs(h.centerMidi - want) * 100); ++matched; break; }
        }
        const int iv = (int)std::lround(want - lead);
        CHECK(iv == 3 || iv == 4, "third above %d is %d semitones", (int)lead, iv);
    }
    std::printf("  matched %zu/%zu notes, worst %.1f cents from the diatonic third\n", matched, a.notes.size(), worst);
    CHECK(matched == a.notes.size(), "matched only %zu", matched);
    CHECK(worst < 8, "harmony off by %.1f cents", worst);
}

static void testDocumentIO(const Analysis& a)
{
    std::printf("[document] save / load round trip\n");
    Document d;
    conform(d, a);
    d.keyTonic = 7; d.keyMinor = true; d.chromatic = false; d.leadGainDb = -1.5;
    d.edits.notes[1].pitchOffset = 0.37; d.edits.notes[1].formantShift = -1.25; d.edits.notes[2].stretch = 1.1;
    d.edits.spans = spansOf(a);
    HarmonyVoice v; v.degrees = -2; v.pan = -0.4; v.noteMask.assign(a.notes.size(), 1); v.noteMask[0] = 0;
    d.voices.push_back(v);
    Document r;
    CHECK(deserialize(serialize(d), r), "parse failed");
    CHECK(serialize(r) == serialize(d), "round trip differs");
    CHECK(!deserialize("garbage", r), "garbage accepted");
}

int main()
{
    testSines();
    testSilenceAndNoise();
    for (double sr : {44100.0, 48000.0}) {
        std::printf("=== synthetic singer @ %.0f Hz ===\n", sr);
        const test::SynthResult s = test::synthesize(test::demoMelody(), sr);
        const Analysis a = analyseSynth(s, sr);
        testNotes(s, a);
        testNull(s, a);
        testTranspose(s, a, sr);
        testTune(s, a, sr);
        testStretch(s, a, sr);
        testLayout(a);
        testSplitJoin(s, a, sr);
        testHarmony(s, a, sr);
        testDocumentIO(a);
    }
    std::printf(failures ? "\n%d FAILURE(S)\n" : "\nALL PASSED\n", failures);
    return failures ? 1 : 0;
}
