// Engine tests on synthetic material with known ground truth.
#include "Synth.h"
#include "glina/Analysis.h"
#include "glina/Renderer.h"
#include "glina/Tuning.h"
#include "glina/Document.h"
#include "glina/Notes.h"
#include "glina/Editing.h"

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
    e.keepLength = false; // global time stretch: the take gets longer
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

// Output time (s) of the analysed onset nearest to `want` in a re-analysis.
static double onsetNear(const Analysis& b, double want)
{
    double best = 1e9;
    for (const Note& n : b.notes) if (std::abs(n.startSec - want) < std::abs(best - want)) best = n.startSec;
    return best;
}

static void testBoundaryBlocks(const test::SynthResult& s, const Analysis& a, double sr)
{
    std::printf("[boundary] 5 selected notes, drag the edge between the 2nd and 3rd: two blocks\n");
    if (a.notes.size() < 7) { CHECK(false, "need 7 notes"); return; }
    EditSet e;
    e.notes.resize(a.notes.size());
    const auto base = noteTimes(a, e);
    const std::set<int> sel = {0, 1, 2, 3, 4};
    const double applied = moveBoundary(e, a, 1, 0.08, sel);
    const auto t = noteTimes(a, e);
    std::printf("  applied %.3f s; note1 end %+.3f, note2 start %+.3f, note0 start %+.4f, note4 end %+.4f, note5 %+.4f\n",
                applied, t[1].outEnd - base[1].outEnd, t[2].outStart - base[2].outStart, t[0].outStart - base[0].outStart,
                t[4].outEnd - base[4].outEnd, t[5].outStart - base[5].outStart);
    CHECK(std::abs(applied - 0.08) < 1e-9, "dt clamped to %.3f", applied);
    CHECK(std::abs(t[1].outEnd - base[1].outEnd - 0.08) < 1e-6, "boundary did not move");
    CHECK(std::abs(t[2].outStart - base[2].outStart - 0.08) < 1e-6, "gap at the boundary changed length");
    CHECK(std::abs(t[0].outStart - base[0].outStart) < 1e-6, "left block start moved");
    CHECK(std::abs(t[4].outEnd - base[4].outEnd) < 1e-6, "right block end moved");
    for (int i = 5; i < (int)t.size(); ++i)
        CHECK(std::abs(t[i].outStart - base[i].outStart) < 1e-6 && std::abs(t[i].outEnd - base[i].outEnd) < 1e-6,
              "note %d outside the blocks moved", i);
    for (int i = 1; i <= 4; ++i) {
        if (i == 2) continue;
        CHECK(std::abs((t[i].outStart - t[i - 1].outEnd) - (base[i].outStart - base[i - 1].outEnd)) < 1e-6,
              "gap before note %d changed", i);
    }
    const double kL = (t[0].outEnd - t[0].outStart) / (base[0].outEnd - base[0].outStart);
    const double kL1 = (t[1].outEnd - t[1].outStart) / (base[1].outEnd - base[1].outStart);
    CHECK(std::abs(kL - kL1) < 1e-6 && kL > 1, "left block not stretched as one (%.3f vs %.3f)", kL, kL1);

    const std::vector<float> y = render(s.audio.data(), s.audio.size(), a, e);
    CHECK(y.size() == s.audio.size(), "clip length changed %zu -> %zu", s.audio.size(), y.size());
    const Analysis b = analyze(y.data(), y.size(), sr);
    const double on2 = onsetNear(b, t[2].outStart), on6 = onsetNear(b, t[6].outStart);
    std::printf("  rendered: note 3 onset %+.1f ms from target, note 7 onset %+.1f ms (should stay)\n",
                (on2 - t[2].outStart) * 1000, (on6 - base[6].outStart) * 1000);
    CHECK(std::abs(on2 - t[2].outStart) < 0.015, "note 3 onset off by %.1f ms", (on2 - t[2].outStart) * 1000);
    CHECK(std::abs(on6 - base[6].outStart) < 0.015, "note 7 moved by %.1f ms", (on6 - base[6].outStart) * 1000);
}

static void testMoveNote(const test::SynthResult& s, const Analysis& a, double sr)
{
    std::printf("[move] move one note +50 ms: touching neighbours follow, across a gap they stay\n");
    EditSet e;
    e.notes.resize(a.notes.size());
    const auto base = noteTimes(a, e);
    const double applied = moveNotes(e, a, {3}, 0.05);
    const auto t = noteTimes(a, e);
    CHECK(std::abs(applied - 0.05) < 1e-9, "clamped to %.3f", applied);
    CHECK(std::abs(t[3].outStart - base[3].outStart - 0.05) < 1e-6, "note did not move");
    const bool touchPrev = std::abs(base[3].outStart - base[2].outEnd) < 0.005;
    const bool touchNext = std::abs(base[4].outStart - base[3].outEnd) < 0.005;
    CHECK(std::abs(t[2].outEnd - base[2].outEnd - (touchPrev ? 0.05 : 0.0)) < 1e-6 &&
              std::abs(t[2].outStart - base[2].outStart) < 1e-6,
          "previous note: touching=%d should lengthen only when touching", (int)touchPrev);
    CHECK(std::abs(t[4].outStart - base[4].outStart - (touchNext ? 0.05 : 0.0)) < 1e-6 &&
              std::abs(t[4].outEnd - base[4].outEnd) < 1e-6,
          "next note: touching=%d should shorten only when touching", (int)touchNext);
    const std::vector<float> y = render(s.audio.data(), s.audio.size(), a, e);
    const Analysis b = analyze(y.data(), y.size(), sr);
    const double on3 = onsetNear(b, t[3].outStart);
    std::printf("  rendered note 4 onset %+.1f ms from target\n", (on3 - t[3].outStart) * 1000);
    // The onset detector works on a ~5 ms frame grid, so allow one frame of error.
    CHECK(std::abs(on3 - t[3].outStart) < 0.02, "moved note onset off by %.1f ms", (on3 - t[3].outStart) * 1000);
    // Clamping: pushing a note far into its neighbour stops at the neighbour's minimum length.
    EditSet f;
    f.notes.resize(a.notes.size());
    const double big = moveNotes(f, a, {3}, 10.0);
    CHECK(big < 10.0 && big > 0, "huge move not clamped (%.3f)", big);
}

static void testAttackKept(const Analysis& a)
{
    std::printf("[attack] stretching a note leaves its attack and tail untouched\n");
    EditSet e;
    e.notes.resize(a.notes.size());
    const int i = 4;
    const double dur = a.notes[i].endSec - a.notes[i].startSec;
    setNoteDuration(e, a, i, dur * 1.6);
    const TimeMap m = TimeMap::build(a, e);
    const double sr = a.sampleRate, ai = a.notes[i].startSec * sr, bi = a.notes[i].endSec * sr;
    const double attackSlope = (m.inToOut(ai + 0.02 * sr) - m.inToOut(ai)) / (0.02 * sr);
    const double tailSlope = (m.inToOut(bi) - m.inToOut(bi - 0.015 * sr)) / (0.015 * sr);
    const double midSlope = (m.inToOut(0.5 * (ai + bi) + 0.01 * sr) - m.inToOut(0.5 * (ai + bi))) / (0.01 * sr);
    std::printf("  slopes: attack %.3f, middle %.3f, tail %.3f\n", attackSlope, midSlope, tailSlope);
    CHECK(std::abs(attackSlope - 1) < 1e-6, "attack stretched (%.3f)", attackSlope);
    CHECK(std::abs(tailSlope - 1) < 1e-6, "tail stretched (%.3f)", tailSlope);
    CHECK(midSlope > 1.6, "middle not stretched enough (%.3f)", midSlope);
}

static void testTransitions(const Analysis& a)
{
    std::printf("[transition] the sung glide between two notes: 0 = step, 0.5 = as sung, 1 = long glide\n");
    int pair = -1;
    for (int i = 0; i + 1 < (int)a.notes.size(); ++i)
        if (a.notes[(size_t)i + 1].startFrame - a.notes[(size_t)i].endFrame <= 1 &&
            std::abs(a.notes[(size_t)i + 1].centerMidi - a.notes[(size_t)i].centerMidi) >= 1.5) { pair = i; break; }
    if (pair < 0) { std::printf("  (no contiguous notes with a pitch change, skipped)\n"); return; }
    const Note& L = a.notes[(size_t)pair];
    const Note& R = a.notes[(size_t)pair + 1];
    const int b = R.startFrame;
    const int reach = std::min(b - L.startFrame, R.endFrame - b) / 2;
    auto curveFor = [&](double smooth, double offL = 0, double offR = 0) {
        EditSet e;
        e.notes.resize(a.notes.size());
        e.notes[(size_t)pair].pitchOffset = offL;
        e.notes[(size_t)pair + 1].pitchOffset = offR;
        e.notes[(size_t)pair + 1].transition = smooth;
        return editedPitchCurve(a, e);
    };
    // Frames between 10 % and 90 % of the way from one note to the next.
    auto rampFrames = [&](const std::vector<double>& c, double offL = 0, double offR = 0) {
        const double lvL = L.centerMidi + offL, lvR = R.centerMidi + offR;
        int n = 0;
        for (int k = b - reach; k < b + reach; ++k) {
            if (a.frames[(size_t)k].f0 <= 0) continue;
            const double frac = (c[(size_t)k] - lvL) / (lvR - lvL);
            if (frac > 0.1 && frac < 0.9) ++n;
        }
        return n;
    };
    // 0.5 = untouched: exactly the sung curve.
    const auto c5 = curveFor(0.5);
    double worst = 0;
    for (int k = b - reach; k < b + reach; ++k)
        if (a.frames[(size_t)k].f0 > 0) worst = std::max(worst, std::abs(c5[(size_t)k] - a.frames[(size_t)k].midi));
    CHECK(worst < 1e-6, "smoothness 0.5 changed the sung curve by %.3f st", worst);
    const int r0 = rampFrames(curveFor(0.0)), r5 = rampFrames(c5), r1 = rampFrames(curveFor(1.0));
    const int q1 = rampFrames(curveFor(1.0, 1.0, -1.0), 1.0, -1.0);
    std::printf("  frames in the move: smoothness 0 -> %d, 0.5 (as sung) -> %d, 1 -> %d; 1 with edited notes -> %d\n",
                r0, r5, r1, q1);
    CHECK(r0 <= 1, "step still has %d frames in the move", r0);
    CHECK(r1 > std::max(r5, 4) + 4, "glide not longer than sung (%d vs %d)", r1, r5);
    CHECK(q1 > std::max(r5, 4) + 4, "glide between edited notes too short (%d)", q1);
}

static void testPencil(const test::SynthResult& s, const Analysis& a, double sr)
{
    std::printf("[pencil] drawn pitch is rendered and moves with its note\n");
    const int i = 4;
    const Note& n = a.notes[(size_t)i];
    const int f0 = n.startFrame + (n.endFrame - n.startFrame) / 4, f1 = n.endFrame - (n.endFrame - n.startFrame) / 4;
    const float target = (float)(n.centerMidi + 0.5);
    EditSet e;
    e.notes.resize(a.notes.size());
    drawPitch(e, a, f0, std::vector<float>((size_t)(f1 - f0), target));
    CHECK(e.draw.size() == 1, "draw runs %zu", e.draw.size());
    auto measure = [&](const EditSet& ed) {
        const std::vector<float> y = render(s.audio.data(), s.audio.size(), a, ed);
        const Analysis b = analyze(y.data(), y.size(), sr);
        double sum = 0; int c = 0;
        for (int f = f0 + 8; f < f1 - 8; ++f) if (b.frames[(size_t)f].f0 > 0) { sum += b.frames[(size_t)f].midi; ++c; }
        return c ? sum / c : 0.0;
    };
    const double got = measure(e);
    std::printf("  drawn %.2f, rendered %.2f (%+.1f cents)\n", target, got, (got - target) * 100);
    CHECK(std::abs(got - target) < 0.06, "pencil pitch off by %.1f cents", (got - target) * 100);
    e.notes[(size_t)i].pitchOffset = 1.0;
    const double moved = measure(e);
    std::printf("  note +1 st: rendered %.2f (want %.2f)\n", moved, target + 1.0);
    CHECK(std::abs(moved - target - 1.0) < 0.06, "drawing did not follow the note (%.2f)", moved);
    eraseDrawn(e, 0, (int)a.frames.size());
    CHECK(e.draw.empty(), "erase left %zu runs", e.draw.size());
}

static void testConsonant(const test::SynthResult& s, const Analysis& a, double sr)
{
    std::printf("[consonant] a consonant keeps its length and pitch; unlinked neighbours stay\n");
    EditSet e;
    e.notes.resize(a.notes.size());
    setNoteDuration(e, a, 3, (a.notes[3].endSec - a.notes[3].startSec) * 1.5);
    setConsonant(e, a, 3, true);
    // A stretched neighbour cannot change the consonant's length.
    const auto before = noteTimes(a, e);
    moveBoundary(e, a, 2, 0.03, {});
    const auto t = noteTimes(a, e);
    CHECK(std::abs((t[3].outEnd - t[3].outStart) - (a.notes[3].endSec - a.notes[3].startSec)) < 1e-4,
          "consonant changed length");
    CHECK(std::abs((before[3].outEnd - before[3].outStart) - (t[3].outEnd - t[3].outStart)) < 1e-4,
          "consonant stretched by neighbour");
    // Rendering: the consonant's pitch is not shifted.
    EditSet c = e;
    c.notes[3].pitchOffset = 3.0;
    const std::vector<double> curve = editedPitchCurve(a, c);
    double worst = 0;
    for (int k = 0; k < (int)a.frames.size(); ++k)
        if (a.noteOfFrame[(size_t)k] == 3 && a.frames[(size_t)k].f0 > 0)
            worst = std::max(worst, std::abs(curve[(size_t)k] - a.frames[(size_t)k].midi));
    CHECK(worst < 1e-6, "consonant pitch changed by %.3f st", worst);
    // Gap rule: notes that do not touch. Dragging the end of note 2 moves only its end;
    // dragging the start of note 3 moves only its start; neighbours stay in place.
    EditSet g;
    g.notes.resize(a.notes.size());
    g.notes[2].stretch = 0.9; // shortens note 2: opens a gap between notes 2 and 3
    const auto g0 = noteTimes(a, g);
    const double gap = g0[3].outStart - g0[2].outEnd;
    CHECK(gap > 0.04, "test setup: no gap (%.3f)", gap);
    EditSet ge = g;
    moveBoundary(ge, a, 2, 0.02, {}, false);
    const auto ge_t = noteTimes(a, ge);
    CHECK(std::abs(ge_t[2].outEnd - g0[2].outEnd - 0.02) < 1e-6, "end edge did not move");
    CHECK(std::abs(ge_t[3].outStart - g0[3].outStart) < 1e-6, "end drag moved the next note");
    EditSet gs = g;
    moveBoundary(gs, a, 2, 0.02, {}, true);
    const auto gs_t = noteTimes(a, gs);
    CHECK(std::abs(gs_t[3].outStart - g0[3].outStart - 0.02) < 1e-6, "start edge did not move");
    CHECK(std::abs(gs_t[2].outEnd - g0[2].outEnd) < 1e-6, "start drag moved the previous note");
    // Moving a whole note (body drag) across a gap leaves the neighbour where it is.
    EditSet gm = g;
    moveNotes(gm, a, {3}, 0.02);
    const auto gm_t = noteTimes(a, gm);
    CHECK(std::abs(gm_t[2].outEnd - g0[2].outEnd) < 1e-6, "body drag across a gap moved the neighbour");
    CHECK(std::abs(gm_t[3].outStart - g0[3].outStart - 0.02) < 1e-6, "body drag did not move the note");
    // Touching notes: the neighbour follows, as before.
    EditSet gt = {};
    gt.notes.resize(a.notes.size());
    const auto t0 = noteTimes(a, gt);
    moveNotes(gt, a, {3}, 0.01);
    const auto t1 = noteTimes(a, gt);
    CHECK(std::abs(t1[2].outEnd - t0[2].outEnd - 0.01) < 1e-6 || std::abs(t1[3].outStart - t0[3].outStart - 0.01) > 0.0,
          "touching neighbour did not follow");
    std::printf("  checked gap %.1f ms\n", gap * 1000);
}

static void testDocumentV2(const Analysis& a)
{
    std::printf("[document v2] smoothness + pencil round trip, v1 still loads\n");
    Document d;
    conform(d, a);
    d.edits.notes[2].transition = 0.85;
    drawPitch(d.edits, a, a.notes[3].startFrame + 5, std::vector<float>(20, (float)a.notes[3].centerMidi + 0.3f));
    Document r;
    CHECK(deserialize(serialize(d), r), "v2 parse failed");
    CHECK(serialize(r) == serialize(d), "v2 round trip differs");
    CHECK(r.edits.draw.size() == d.edits.draw.size(), "draw runs lost");
    const std::string v1 = "glina-document 1\nkey -1 0 0\nlead 0\nglobal 0 0 0 45 40\nspans 0\nnotes 1\n"
                           "n 0.5 1 1 0 0 0 1\nend\n";
    Document o;
    CHECK(deserialize(v1, o) && o.edits.notes.size() == 1 && o.edits.notes[0].pitchOffset == 0.5 &&
              o.edits.notes[0].transition == 0.5, "v1 document did not load");
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
        testBoundaryBlocks(s, a, sr);
        testMoveNote(s, a, sr);
        testAttackKept(a);
        testTransitions(a);
        testPencil(s, a, sr);
        testConsonant(s, a, sr);
        testDocumentV2(a);
    }
    std::printf(failures ? "\n%d FAILURE(S)\n" : "\nALL PASSED\n", failures);
    return failures ? 1 : 0;
}
