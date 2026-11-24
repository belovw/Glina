// Engine tests on synthetic material with known ground truth.
#include "Synth.h"
#include "glina/Analysis.h"
#include "glina/Renderer.h"
#include "glina/Tuning.h"
#include "glina/Document.h"
#include "glina/Notes.h"
#include "glina/Editing.h"
#include "glina/Transfer.h"
#include "glina/Thread.h"
#include "glina/TrackView.h"

#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
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
    d.keyMode = 2; d.snapAmount = 0.6; // melodic minor, pulled 60 % of the way
    d.edits.notes[1].pitchOffset = 0.37; d.edits.notes[1].formantShift = -1.25; d.edits.notes[2].stretch = 1.1;
    d.edits.spans = spansOf(a);
    HarmonyVoice v; v.degrees = -2; v.pan = -0.4; v.noteMask.assign(a.notes.size(), 1); v.noteMask[0] = 0;
    d.voices.push_back(v);
    Document r;
    CHECK(deserialize(serialize(d), r), "parse failed");
    CHECK(serialize(r) == serialize(d), "round trip differs");
    CHECK(!deserialize("garbage", r), "garbage accepted");
    CHECK(r.keyMode == 2 && std::abs(r.snapAmount - 0.6) < 1e-9, "scale mode lost");
    Document plain; // default mode and snap: no new record, same bytes as before
    CHECK(serialize(plain).find("\nmode ") == std::string::npos, "default mode written");
    CHECK(serialize(plain).find("leadmix") == std::string::npos, "default lead mix written");
    Document cm = d; if (cm.edits.notes.size() > 1) cm.edits.notes[1].consMuted = true;
    Document cr;
    CHECK(deserialize(serialize(cm), cr) && cr.edits.notes.size() > 1 && cr.edits.notes[1].consMuted, "consonant mute lost on save");
    Document mu = d; if (mu.edits.notes.size() > 1) mu.edits.notes[1].muted = true;
    Document mr;
    CHECK(deserialize(serialize(mu), mr), "mute parse failed");
    CHECK(mr.edits.notes.size() > 1 && mr.edits.notes[1].muted, "mute lost on save");
    Document lm = d; lm.leadEnabled = false; lm.leadPan = 0.5;
    Document lr;
    CHECK(deserialize(serialize(lm), lr), "lead mix parse failed");
    CHECK(!lr.leadEnabled && std::abs(lr.leadPan - 0.5) < 1e-9, "lead enable / pan lost");
}

// Mute: a muted note goes silent and keeps its place; the others are unchanged.
static void testDelete(const test::SynthResult& s, const Analysis& a, double sr)
{
    std::printf("[delete] a deleted note is silent; pulling its edge brings it back\n");
    if (a.notes.size() < 3) { std::printf("  skipped: too few notes\n"); return; }
    EditSet plain;
    plain.notes.resize(a.notes.size());
    const size_t k = 1;
    EditSet del = plain;
    deleteNotes(del, a, {(int)k});
    CHECK(del.notes[k].deleted && del.notes[k].consMuted, "delete did not set the flags");
    const auto y0 = render(s.audio.data(), s.audio.size(), a, plain);
    const auto y1 = render(s.audio.data(), s.audio.size(), a, del);
    auto rms = [&](const std::vector<float>& y) {
        const size_t from = (size_t)(a.notes[k].startSec * sr), to = (size_t)(a.notes[k].endSec * sr);
        double acc = 0;
        for (size_t t = from; t < to && t < y.size(); ++t) acc += (double)y[t] * y[t];
        return std::sqrt(acc / std::max<size_t>(1, to - from));
    };
    const double before = rms(y0), after = rms(y1);
    CHECK(before > 1e-3, "note %zu silent even without delete", k);
    // Neighbours' grain windows reach ~30 ms into it; their own audio, not the deleted note.
    CHECK(after < before * 1e-2, "deleted note still sounds: %.6f vs %.6f", after, before);
    // Alt-drag of its edge brings it back.
    EditSet back = del;
    restoreDeleted(back, (int)k - 1);
    CHECK(!back.notes[k].deleted, "alt-drag did not bring the deleted note back");
    EditSet br = plain;
    deleteBreaths(br, a, {(int)k});
    restoreDeleted(br, (int)k - 1);
    CHECK(!br.notes[k].breathDeleted, "alt-drag did not bring the breath back");
    // The note before a deleted one stretched over it: the notes after stay where they were.
    if (a.notes.size() >= 4) {
        EditSet over = plain;
        deleteNotes(over, a, {1, 2});
        const std::vector<NoteTimes> t0 = noteTimes(a, over);
        const double want = t0[3].outStart - 1e-3; // just before the next live note
        moveBoundary(over, a, 0, want - t0[0].outEnd, {0}, false);
        const std::vector<NoteTimes> t1 = noteTimes(a, over);
        CHECK(t1[0].outEnd > t0[2].outStart, "the stretch did not reach over the deleted notes (%.3f)", t1[0].outEnd);
        const TimeMap m0 = TimeMap::build(a, plain), m1 = TimeMap::build(a, over);
        for (size_t i = 3; i < a.notes.size(); ++i)
            CHECK(std::abs(m1.noteStartOut(i) - m0.noteStartOut(i)) < 1e-3 * sr,
                  "note %zu pushed by the stretch over deleted notes: %.4f s", i,
                  (m1.noteStartOut(i) - m0.noteStartOut(i)) / sr);
    }
}

static void testMute(const test::SynthResult& s, const Analysis& a, double sr)
{
    std::printf("[mute] a muted note is silent, the rest keep sounding\n");
    (void)s;
    if (a.notes.size() < 3) { std::printf("  skipped: too few notes\n"); return; }
    EditSet plain;
    plain.notes.resize(a.notes.size());
    EditSet muted = plain;
    const size_t k = 1;
    muted.notes[k].muted = true;
    const auto y0 = render(s.audio.data(), s.audio.size(), a, plain);
    const auto y1 = render(s.audio.data(), s.audio.size(), a, muted);
    CHECK(y0.size() == y1.size(), "length changed by mute");
    auto rms = [&](const std::vector<float>& y, size_t i) {
        const size_t from = (size_t)(a.notes[i].startSec * sr), to = (size_t)(a.notes[i].endSec * sr);
        double acc = 0;
        for (size_t t = from; t < to && t < y.size(); ++t) acc += (double)y[t] * y[t];
        return std::sqrt(acc / std::max<size_t>(1, to - from));
    };
    const double before = rms(y0, k), after = rms(y1, k);
    CHECK(before > 1e-3, "note %zu silent even without mute", k);
    CHECK(after < before * 1e-3, "muted note still sounds: %.6f vs %.6f", after, before);
    // Muted consonant in front of note k: the gap before it is silent.
    EditSet cons = plain;
    cons.notes[k].consMuted = true;
    const auto y2 = render(s.audio.data(), s.audio.size(), a, cons);
    const double g0 = a.notes[k - 1].endFrame * (double)a.hop / sr, g1 = a.notes[k].startFrame * (double)a.hop / sr;
    const size_t from = (size_t)(g0 * sr), to = (size_t)(g1 * sr);
    double gacc = 0;
    for (size_t t = from; t < to && t < y2.size(); ++t) gacc += (double)y2[t] * y2[t];
    CHECK(to <= from || gacc < 1e-6, "muted consonant still sounds: %.3g", gacc);
    const double other0 = rms(y0, 0), other1 = rms(y1, 0);
    CHECK(std::abs(other0 - other1) < 0.02 * other0, "neighbour changed by mute: %.6f vs %.6f", other0, other1);
    std::printf("  note %zu rms %.4f -> %.6f when muted; neighbour %.4f -> %.4f\n", k, before, after, other0, other1);
}

// Scale steps per mode, and the key a document resolves to.
static void testScaleModes()
{
    std::printf("[scale] modes and steps\n");
    auto steps = [](Key k) { int s[7]; scaleSteps(k, s); return std::vector<int>(s, s + 7); };
    CHECK((steps({0, false}) == std::vector<int>{0, 2, 4, 5, 7, 9, 11}), "major steps");
    CHECK((steps({0, true, 0}) == std::vector<int>{0, 2, 3, 5, 7, 8, 10}), "natural minor steps");
    CHECK((steps({0, true, 1}) == std::vector<int>{0, 2, 3, 5, 7, 9, 11}), "melodic minor steps");
    CHECK((steps({0, true, 2}) == std::vector<int>{0, 2, 3, 5, 7, 8, 11}), "harmonic minor steps");
    Analysis a; a.key = Key{0, false};
    Document d;
    d.keyTonic = 9; d.keyMode = 3;
    CHECK(d.key(a).tonic == 9 && d.key(a).minor && d.key(a).minorForm == 2, "harmonic A minor");
    d.keyMode = -1; d.keyTonic = -1;
    CHECK(!d.key(a).minor && d.key(a).tonic == 0, "analysed key kept in auto mode");
    // F# (6) is a step of A melodic minor (raised 6th), not of A natural minor, where 6.4 goes to G (7).
    Key mel, nat;
    mel.tonic = nat.tonic = 9; mel.minor = nat.minor = true;
    mel.minorForm = 1; nat.minorForm = 0;
    CHECK(nearestTarget(6.4, mel, true) == 6.0 && nearestTarget(6.4, nat, true) == 7.0, "F# in melodic minor only");
    // Pure major third above C: 386.3 cents, not 400; the tempered key still snaps to 4.
    Key pure, tempered;
    pure.just = true;
    CHECK(std::abs(nearestTarget(3.9, pure, true) - 3.86314) < 1e-4, "pure third %.5f", nearestTarget(3.9, pure, true));
    CHECK(nearestTarget(3.9, tempered, true) == 4.0, "tempered third");
    Document dp;
    dp.keyMode = 4;
    CHECK(dp.key(a).just && !dp.key(a).minor, "pure major mode");
    dp.keyMode = 5;
    CHECK(dp.key(a).just && dp.key(a).minor, "pure minor mode");
}

// Output time (s) of the analysed onset nearest to `want` in a re-analysis.
static double onsetNear(const Analysis& b, double want)
{
    double best = 1e9;
    for (const Note& n : b.notes) if (std::abs(n.startSec - want) < std::abs(best - want)) best = n.startSec;
    return best;
}

// A clip that starts singing on the very first sample: note 0 begins at frame 0, so its
// boundary sits just below zero (boundaries are frame centres). The layout must still be
// movable — taking 0 as the floor made every drag a no-op (found on a real REAPER item).
static void testNoteAtFrameZero(const test::SynthResult& s, double sr)
{
    std::printf("[frame0] first note starts at frame 0: time edits still work\n");
    // Drop the leading silence so the first note starts at the first frame.
    const Analysis probe = analyze(s.audio.data(), s.audio.size(), sr);
    if (probe.notes.empty()) { CHECK(false, "no notes"); return; }
    const size_t cut = (size_t)probe.notes[0].startFrame * (size_t)probe.hop;
    if (cut >= s.audio.size()) { CHECK(false, "clip too short"); return; }
    const std::vector<float> trimmed(s.audio.begin() + (long)cut, s.audio.end());
    const Analysis a = analyze(trimmed.data(), trimmed.size(), sr);
    if (a.notes.size() < 3) { CHECK(false, "need 3 notes"); return; }
    EditSet e;
    e.notes.resize(a.notes.size());
    const auto base = noteTimes(a, e);
    std::printf("  note0 starts at frame %d, outStart %.4f s\n", a.notes[0].startFrame, base[0].outStart);
    const int mid = (int)a.notes.size() / 2;
    const double right = moveNotes(e, a, {mid}, 0.05);
    EditSet e2;
    e2.notes.resize(a.notes.size());
    const double left = moveNotes(e2, a, {mid}, -0.05);
    std::printf("  moveNotes +50 ms -> %.4f, -50 ms -> %.4f\n", right, left);
    CHECK(std::abs(right) > 0.001, "note moves right");
    CHECK(std::abs(left) > 0.001, "note moves left");
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

// Mean rendered pitch (MIDI) over frames [f0, f1) of an analysis of `y`.
static double renderedPitch(const std::vector<float>& y, double sr, int f0, int f1)
{
    const Analysis b = analyze(y.data(), y.size(), sr);
    double sum = 0; int c = 0;
    for (int f = f0; f < f1 && f < (int)b.frames.size(); ++f) if (b.frames[(size_t)f].f0 > 0) { sum += b.frames[(size_t)f].midi; ++c; }
    return c ? sum / c : 0.0;
}

static void testTilt(const test::SynthResult& s, const Analysis& a, double sr)
{
    std::printf("[tilt] corner drag: the opposite edge stays, the middle moves half, drift/vibrato ride on it\n");
    const int i = 6; // the longest note
    const Note& n = a.notes[(size_t)i];
    const int len = n.endFrame - n.startFrame;
    EditSet flat;
    flat.notes.resize(a.notes.size());
    EditSet t = flat;
    addTilt(t, a, {i}, true, -1.0);
    const auto c0 = editedPitchCurve(a, flat), c1 = editedPitchCurve(a, t);
    double worst = 0;
    for (int k = n.startFrame; k < n.endFrame; ++k) {
        if (a.frames[(size_t)k].f0 <= 0) continue;
        const double want = -1.0 * (k + 0.5 - n.startFrame) / len;
        worst = std::max(worst, std::abs((c1[(size_t)k] - c0[(size_t)k]) - want));
    }
    std::printf("  curve minus untilted curve: worst %.4f st from the straight line\n", worst);
    CHECK(worst < 0.005, "tilt is not a straight line added to the curve (%.4f st)", worst);
    const double dc = editedCenter(a, t, i) - editedCenter(a, flat, i);
    std::printf("  box centre moved %+.3f st (want about -0.5)\n", dc);
    CHECK(std::abs(dc + 0.5) < 0.08, "centre moved %.3f", dc);

    // Rendered: start unchanged, middle -0.5, end about -0.9.
    const std::vector<float> y0 = render(s.audio.data(), s.audio.size(), a, flat);
    const std::vector<float> y1 = render(s.audio.data(), s.audio.size(), a, t);
    auto at = [&](double u0, double u1) {
        const int f0 = n.startFrame + (int)(u0 * len), f1 = n.startFrame + (int)(u1 * len);
        return renderedPitch(y1, sr, f0, f1) - renderedPitch(y0, sr, f0, f1);
    };
    const double dStart = at(0.05, 0.15), dMid = at(0.45, 0.55), dEnd = at(0.85, 0.95);
    std::printf("  rendered: start %+.2f, middle %+.2f, end %+.2f st (want -0.10 / -0.50 / -0.90)\n", dStart, dMid, dEnd);
    CHECK(std::abs(dStart + 0.10) < 0.07 && std::abs(dMid + 0.50) < 0.07 && std::abs(dEnd + 0.90) < 0.07,
          "rendered tilt off");

    // Dragging again continues from the current tilt; the left corner keeps the right edge.
    addTilt(t, a, {i}, true, -0.5);
    addTilt(t, a, {i}, false, 0.25);
    CHECK(t.notes[(size_t)i].tiltR == -1.5 && t.notes[(size_t)i].tiltL == 0.25, "tilt did not accumulate");
    EditSet cons = flat;
    cons.notes[(size_t)i].consonant = true;
    addTilt(cons, a, {i}, true, -1.0);
    CHECK(cons.notes[(size_t)i].tiltR == 0, "a consonant was tilted");

    // Several notes tilt as one: one straight line over the group, its left edge stays.
    const std::set<int> group = {6, 7, 8};
    const int g0 = a.notes[6].startFrame, g1 = a.notes[8].endFrame;
    EditSet gt = flat;
    addTilt(gt, a, group, true, -1.0);
    const auto cg = editedPitchCurve(a, gt);
    double gWorst = 0;
    for (int k = g0; k < g1; ++k) {
        const int ni = a.noteOfFrame[(size_t)k];
        if (a.frames[(size_t)k].f0 <= 0 || !group.count(ni)) continue;
        const double want = -1.0 * (k + 0.5 - g0) / (g1 - g0);
        gWorst = std::max(gWorst, std::abs((cg[(size_t)k] - c0[(size_t)k]) - want));
    }
    std::printf("  group of 3 notes, right end -1 st: worst %.4f st from one straight line; ends %+.2f / %+.2f\n", gWorst,
                gt.notes[6].tiltL, gt.notes[8].tiltR);
    CHECK(gWorst < 0.005, "group tilt is not one straight line (%.4f st)", gWorst);
    CHECK(gt.notes[6].tiltL == 0 && std::abs(gt.notes[8].tiltR + 1.0) < 1e-12, "group ends wrong");
    CHECK(editedCenter(a, gt, 6) > editedCenter(a, gt, 8) - a.notes[8].centerMidi + a.notes[6].centerMidi,
          "the first note of the group should drop less than the last");

    // Roof: dragging the right edge of the middle note: both group ends stay, the apex is that edge.
    EditSet roof = flat;
    addTilt(roof, a, group, true, -1.0, 7);
    const auto cr = editedPitchCurve(a, roof);
    double rWorst = 0;
    const int apexF = a.notes[7].endFrame;
    for (int k = g0; k < g1; ++k) {
        const int ni = a.noteOfFrame[(size_t)k];
        if (a.frames[(size_t)k].f0 <= 0 || !group.count(ni)) continue;
        const double f = k + 0.5;
        const double want = f <= apexF ? -(f - g0) / (apexF - g0) : -(g1 - f) / (double)(g1 - apexF);
        rWorst = std::max(rWorst, std::abs((cr[(size_t)k] - c0[(size_t)k]) - want));
    }
    std::printf("  roof over 3 notes, apex at the middle note's right edge: worst %.4f st\n", rWorst);
    CHECK(rWorst < 0.005, "roof is not two straight lines (%.4f st)", rWorst);
    CHECK(roof.notes[6].tiltL == 0 && roof.notes[8].tiltR == 0 && roof.notes[7].tiltR == -1.0,
          "roof ends or apex wrong");
    // One note: as before, its own edge moves.
    EditSet one = flat, oneOld = flat;
    addTilt(one, a, {i}, true, -1.0, i);
    addTilt(oneOld, a, {i}, true, -1.0);
    CHECK(one.notes[(size_t)i].tiltR == oneOld.notes[(size_t)i].tiltR && one.notes[(size_t)i].tiltL == 0,
          "one note with apex changed");
}

static void testTiltLayout(const Analysis& a)
{
    std::printf("[tilt] split keeps the line, pencil and harmonies follow the tilt\n");
    const int i = 6;
    const Note n = a.notes[(size_t)i];
    Document d;
    conform(d, a);
    d.edits.notes[(size_t)i].tiltR = -1.0;
    const auto before = editedPitchCurve(a, d.edits);
    const int mid = (n.startFrame + n.endFrame) / 2;
    CHECK(splitNote(d, a, i, mid), "split refused");
    const Analysis eff = withLayout(a, d.edits.spans);
    const double u = (double)(mid - n.startFrame) / (n.endFrame - n.startFrame);
    CHECK(std::abs(d.edits.notes[(size_t)i].tiltR + u) < 1e-9 && std::abs(d.edits.notes[(size_t)i + 1].tiltL + u) < 1e-9 &&
              d.edits.notes[(size_t)i + 1].tiltR == -1.0, "split halves do not continue the line");
    const auto after = editedPitchCurve(eff, d.edits);
    double worst = 0;
    for (int k = n.startFrame; k < n.endFrame; ++k)
        if (a.frames[(size_t)k].f0 > 0) worst = std::max(worst, std::abs(after[(size_t)k] - before[(size_t)k]));
    std::printf("  split: curve changed by at most %.4f st\n", worst);
    CHECK(worst < 0.01, "split changed the tilted curve by %.4f st", worst);
    CHECK(joinNotes(d, eff, i), "join refused");
    CHECK(d.edits.notes[(size_t)i].tiltL == 0 && d.edits.notes[(size_t)i].tiltR == -1.0, "join lost the tilt");

    // Pencil drawn on a tilted note stays where drawn, and moves with further tilt.
    EditSet e;
    e.notes.resize(a.notes.size());
    e.notes[(size_t)i].tiltR = -1.0;
    const int f0 = n.startFrame + (n.endFrame - n.startFrame) / 3, f1 = f0 + 20;
    const float target = (float)(n.centerMidi + 0.4);
    drawPitch(e, a, f0, std::vector<float>((size_t)(f1 - f0), target));
    const auto c = editedPitchCurve(a, e);
    const int k = (f0 + f1) / 2;
    CHECK(std::abs(c[(size_t)k] - target) < 1e-3, "drawn pitch on a tilted note: %.3f (want %.3f)", c[(size_t)k], target);
    const double r0 = tiltAt(n, e.notes[(size_t)i], k);
    e.notes[(size_t)i].tiltR = -2.0;
    const double r1 = tiltAt(n, e.notes[(size_t)i], k);
    const auto c2 = editedPitchCurve(a, e);
    CHECK(std::abs((c2[(size_t)k] - c[(size_t)k]) - (r1 - r0)) < 1e-3, "drawing did not follow the tilt");

    // A harmony voice keeps the lead's tilt and lands its centre on the harmony note.
    EditSet lead;
    lead.notes.resize(a.notes.size());
    lead.notes[(size_t)i].tiltR = -1.0;
    const HarmonyVoice v; // diatonic third above
    const EditSet h = harmonyEdits(a, lead, a.key, v);
    const auto want = harmonyNotePitches(a, lead, a.key, v);
    CHECK(std::abs(editedCenter(a, h, i) - want[(size_t)i]) < 1e-9, "harmony centre %.3f, want %.3f",
          editedCenter(a, h, i), want[(size_t)i]);
    CHECK(h.notes[(size_t)i].tiltR == -1.0, "harmony lost the tilt");
}

static void testAutoVibrato(double sr)
{
    std::printf("[auto vibrato] delay, onset, depth, rate, tremolo; short notes and stretched notes\n");
    const test::SynthResult s = test::synthesize({{57, 0.4, 0, false, 0}, {60, 2.0, 0, false, 0}, {64, 0.25, 0, true, 0}}, sr);
    const Analysis a = analyseSynth(s, sr);
    if (a.notes.size() != 3) { CHECK(false, "synthetic take has %zu notes, want 3", a.notes.size()); return; }
    const int i = 1;
    const Note& n = a.notes[(size_t)i];
    const double hopS = (double)a.hop / sr;
    EditSet e;
    e.notes.resize(a.notes.size());
    AutoVibrato v;
    v.depthCents = 30; v.rateHz = 5.5; v.delayMs = 300; v.onsetPeriods = 1.5; v.variation = 0; v.tremoloDb = 2;
    setTrackVibrato(e, v, false);
    const auto curve = editedPitchCurve(a, e);
    // What is left after taking away the singer's line without his own vibrato.
    auto resid = [&](const std::vector<double>& cv, int k) {
        return cv[(size_t)k] - (a.frames[(size_t)k].midi - a.fastDev[(size_t)k] + n.meanFast);
    };
    double pre = 0;
    for (int k = n.startFrame + (int)(0.06 / hopS); k < n.startFrame + (int)(0.28 / hopS); ++k)
        if (a.frames[(size_t)k].f0 > 0) pre = std::max(pre, std::abs(resid(curve, k)));
    const int s0 = n.startFrame + (int)((0.30 + 1.5 / 5.5 + 0.03) / hopS);
    const int s1 = n.endFrame - (int)((1 / 5.5 + 0.03) / hopS);
    double peak = 0, mean = 0; int cnt = 0, cross = 0;
    double prev = resid(curve, s0);
    for (int k = s0; k < s1; ++k) {
        if (a.frames[(size_t)k].f0 <= 0) continue;
        const double r = resid(curve, k);
        peak = std::max(peak, std::abs(r));
        mean += r; ++cnt;
        if ((r > 0) != (prev > 0)) ++cross;
        prev = r;
    }
    mean /= std::max(1, cnt);
    const double rate = cross / (2.0 * (s1 - s0) * hopS);
    std::printf("  before the delay %.4f st; steady: peak %.3f st (want 0.30), mean %+.3f, rate %.2f Hz (want 5.5)\n",
                pre, peak, mean, rate);
    CHECK(pre < 0.005, "vibrato before the delay (%.4f st)", pre);
    CHECK(std::abs(peak - 0.30) < 0.03, "depth %.3f st", peak);
    CHECK(std::abs(mean) < 0.03, "vibrato moved the centre (%.3f st)", mean);
    CHECK(std::abs(rate - 5.5) < 0.4, "rate %.2f Hz", rate);
    CHECK(std::abs(resid(curve, n.endFrame - 1)) < 0.05, "no fade-out at the note end (%.3f)", resid(curve, n.endFrame - 1));

    const Controls c = buildControls(a, e);
    double trem = 0;
    for (int k = s0; k < s1; ++k) trem = std::max(trem, (double)c.gainDb[(size_t)k]);
    std::printf("  tremolo peak %.2f dB (want 2)\n", trem);
    CHECK(std::abs(trem - 2.0) < 0.2, "tremolo %.2f dB", trem);

    // A note shorter than the delay keeps its own vibrato and gets none.
    EditSet none;
    none.notes.resize(a.notes.size());
    const auto plain = editedPitchCurve(a, none);
    const Note& sh = a.notes[2];
    double shortDiff = 0;
    for (int k = sh.startFrame; k < sh.endFrame; ++k) shortDiff = std::max(shortDiff, std::abs(curve[(size_t)k] - plain[(size_t)k]));
    CHECK(shortDiff < 1e-6, "short note changed by %.4f st", shortDiff);

    // Stretching the note keeps the vibrato rate (it is laid on the output timeline).
    EditSet st = e;
    st.notes[(size_t)i].stretch = 1.5;
    const auto cs = editedPitchCurve(a, st);
    const TimeMap map = TimeMap::build(a, st);
    const double t0 = map.inToOut(n.startFrame * (double)a.hop) / sr, t1 = map.inToOut(n.endFrame * (double)a.hop) / sr;
    int crossS = 0; double first = -1, last = -1; double pv = 0; bool have = false;
    for (int k = n.startFrame; k < n.endFrame; ++k) {
        if (a.frames[(size_t)k].f0 <= 0) continue;
        const double t = map.inToOut(k * (double)a.hop) / sr;
        if (t - t0 < 0.60 || t1 - t < 0.25) continue;
        const double r = resid(cs, k);
        if (have && (r > 0) != (pv > 0)) { ++crossS; if (first < 0) first = t; last = t; }
        pv = r; have = true;
    }
    const double rateS = last > first ? (crossS - 1) / (2.0 * (last - first)) : 0;
    std::printf("  stretched x1.5: rate %.2f Hz (want 5.5)\n", rateS);
    CHECK(std::abs(rateS - 5.5) < 0.4, "stretched note vibrato rate %.2f Hz", rateS);

    // Variation is deterministic per track seed and differs between tracks.
    EditSet w1 = e;
    w1.autoVibrato.variation = 60;
    EditSet w2 = w1;
    w2.vibratoSeed = 7;
    const auto a1 = editedPitchCurve(a, w1), a1b = editedPitchCurve(a, w1), a2 = editedPitchCurve(a, w2);
    double same = 0, diff = 0;
    for (size_t k = 0; k < a1.size(); ++k) { same = std::max(same, std::abs(a1[k] - a1b[k])); diff = std::max(diff, std::abs(a1[k] - a2[k])); }
    CHECK(same == 0, "same seed, different result");
    CHECK(diff > 0.05, "different seeds, same vibrato");

    // Note settings: own values win; "all selected" resets them to the track.
    EditSet own = e;
    AutoVibrato off = v;
    off.depthCents = 0; off.tremoloDb = 0;
    setNoteVibrato(own, {i}, off);
    const auto co = editedPitchCurve(a, own);
    double ownDiff = 0;
    for (int k = n.startFrame; k < n.endFrame; ++k) ownDiff = std::max(ownDiff, std::abs(co[(size_t)k] - plain[(size_t)k]));
    CHECK(ownDiff < 1e-6, "note with its own 'off' still vibrates (%.4f)", ownDiff);
    setTrackVibrato(own, v, true);
    CHECK(!own.notes[(size_t)i].ownVibrato, "all-selected did not reset the note");
}

static void testDocumentV4(const Analysis& a)
{
    std::printf("[document v4] tilt and auto vibrato round trip; documents without them stay v3\n");
    Document d;
    conform(d, a);
    const std::string plain = serialize(d);
    CHECK(plain.rfind("glina-document 3\n", 0) == 0, "document without new features is not v3");
    d.edits.notes[2].tiltL = 0.25;
    d.edits.notes[2].tiltR = -0.75;
    AutoVibrato v;
    v.depthCents = 35; v.rateHz = 6.1; v.delayMs = 0; v.onsetPeriods = 2; v.variation = 40; v.tremoloDb = 1.5;
    setTrackVibrato(d.edits, v, false);
    d.edits.vibratoSeed = 12345;
    AutoVibrato own = v;
    own.depthCents = 10;
    setNoteVibrato(d.edits, {4}, own);
    const std::string text = serialize(d);
    CHECK(text.rfind("glina-document 4\n", 0) == 0, "new features not written as v4");
    Document r;
    CHECK(deserialize(text, r), "v4 parse failed");
    CHECK(serialize(r) == text, "v4 round trip differs");
    CHECK(r.edits.notes[2].tiltR == -0.75 && r.edits.autoVibrato == v && r.edits.vibratoSeed == 12345 &&
              r.edits.notes[4].ownVibrato && r.edits.notes[4].vibrato == own, "v4 fields lost");
    Document p;
    CHECK(deserialize(plain, p) && !p.edits.autoVibrato.active() && p.edits.notes[2].tiltR == 0, "v3 loads with features off");
}

// The worst overlap of any note with any earlier one (independent of the library's checks).
static double worstOverlap(const std::vector<NoteTimes>& t)
{
    double w = 0, latest = -1e9;
    for (const NoteTimes& n : t) {
        w = std::max(w, latest - n.outStart);
        latest = std::max(latest, n.outEnd);
    }
    return w;
}

static void testTrackViewCut()
{
    std::printf("[track view] a note cut at an item's edge keeps its place in time\n");
    const double sr = 48000;
    std::vector<test::SynthNote> melB = test::demoMelody();
    for (auto& n : melB) n.midi += 2;
    const test::SynthResult sa = test::synthesize(test::demoMelody(), sr);
    const test::SynthResult sb = test::synthesize(melB, sr, 30, 5.2, 777);
    auto effA = std::make_shared<const Analysis>(analyze(sa.audio.data(), sa.audio.size(), sr));
    auto effB = std::make_shared<const Analysis>(analyze(sb.audio.data(), sb.audio.size(), sr));
    const double hopSec = (double)effA->hop / sr;
    std::vector<TrackPiece> pieces(2);
    pieces[0].eff = effA;
    pieces[0].winEnd = (double)effA->numSamples / sr;
    pieces[1].eff = effB;
    pieces[1].winStart = 0.05;
    pieces[1].winEnd = (double)effB->numSamples / sr;
    pieces[1].placeSec = pieces[0].winEnd + 0.3;
    conform(pieces[0].doc, *effA);
    conform(pieces[1].doc, *effB);
    TrackView v;
    if (!buildTrackView(pieces, 0, v) || v.analysis == nullptr) { CHECK(false, "view not built"); return; }
    const Analysis& va = *v.analysis;
    const size_t nA = effA->notes.size();
    // One stretched view note running from A's last note into B's first one.
    Document d = v.doc;
    d.edits.spans[nA - 1].end = d.edits.spans[nA].end;
    d.edits.spans.erase(d.edits.spans.begin() + (long)nA);
    d.edits.notes.erase(d.edits.notes.begin() + (long)nA);
    d.edits.notes[nA - 1].stretch = 1.2;
    const auto docs = splitTrackView(v, d, pieces);
    if (docs.size() != 2) { CHECK(false, "split back"); return; }
    Document db = docs[1];
    const Analysis effB2 = withLayout(*effB, db.edits.spans);
    conform(db, effB2);
    const NoteTimes tb = noteTimes(effB2, db.edits)[0];
    const int off = v.maps[1].frameOffset;
    const double vIn0 = va.notes[nA - 1].startSec;
    const double cutIn = tb.inStart + off * hopSec;           // the cut, in view time
    const double expected = vIn0 + (cutIn - vIn0) * 1.2;      // where the view note's output had reached there
    const double got = tb.outStart + off * hopSec;
    std::printf("  part in item B starts at %.4f s, the note had reached %.4f s there\n", got, expected);
    CHECK(std::abs(got - expected) < 1e-6, "the part in item B continues the note (%.6f vs %.6f)", got, expected);
}

static void testSeparations(const Analysis& a)
{
    std::printf("[separations] soft: the neighbour follows; hard: it stays; saved with the project\n");
    const int n = (int)a.notes.size();
    EditSet e0;
    e0.notes.resize((size_t)n);
    const auto t = noteTimes(a, e0);
    int touch = -1, gap = -1; // a touching pair (b, b+1) and a pair with a gap, both away from the ends
    for (int b = 1; b + 2 < n; ++b) {
        const double g = t[(size_t)b + 1].outStart - t[(size_t)b].outEnd;
        if (touch < 0 && std::abs(g) < 0.005) touch = b;
        if (gap < 0 && g > 0.03 && g < 0.5) gap = b;
    }
    std::printf("  touching pair at %d, pair with a gap at %d\n", touch, gap);
    if (touch < 0 || gap < 0) { CHECK(false, "demo melody has both kinds of pairs"); return; }

    CHECK(softSeparation(e0, a, touch), "touching notes: soft by default");
    CHECK(!softSeparation(e0, a, gap), "notes with a gap: hard by default");

    { // soft (default): moving the right note drags the left note's end
        EditSet e = e0;
        const double applied = moveNotes(e, a, {touch + 1}, 0.02);
        const auto u = noteTimes(a, e);
        CHECK(applied > 0.019 && std::abs(u[(size_t)touch].outEnd - (t[(size_t)touch].outEnd + 0.02)) < 1e-9,
              "soft: the left neighbour's end follows");
    }
    { // hard: the left neighbour stays, a gap opens
        EditSet e = e0;
        toggleSeparation(e, a, touch);
        CHECK(!softSeparation(e, a, touch), "toggled to hard");
        const double applied = moveNotes(e, a, {touch + 1}, 0.02);
        const auto u = noteTimes(a, e);
        CHECK(applied > 0.019 && std::abs(u[(size_t)touch].outEnd - t[(size_t)touch].outEnd) < 1e-9,
              "hard: the left neighbour stays put");
        EditSet e2 = e0;
        toggleSeparation(e2, a, touch);
        const double into = moveNotes(e2, a, {touch + 1}, -0.02);
        CHECK(std::abs(into) < 1e-9, "hard: a move into the touching neighbour is stopped (%.4f)", into);
        // edge drag across a hard separation moves only the dragged edge
        EditSet e3 = e0;
        toggleSeparation(e3, a, touch);
        moveBoundary(e3, a, touch, -0.02, {}, false);
        const auto v = noteTimes(a, e3);
        CHECK(std::abs(v[(size_t)touch + 1].outStart - t[(size_t)touch + 1].outStart) < 1e-9, "hard: edge drag leaves the neighbour");
    }
    { // soft across a gap: the neighbour follows and the gap is kept
        EditSet e = e0;
        toggleSeparation(e, a, gap);
        CHECK(softSeparation(e, a, gap), "toggled to soft");
        moveNotes(e, a, {gap + 1}, 0.01);
        const auto u = noteTimes(a, e);
        const double g0 = t[(size_t)gap + 1].outStart - t[(size_t)gap].outEnd;
        const double g1 = u[(size_t)gap + 1].outStart - u[(size_t)gap].outEnd;
        CHECK(std::abs(g1 - g0) < 1e-9, "soft across a gap: the gap is kept (%.4f vs %.4f)", g1, g0);
    }
    { // saved and read back; reset keeps it
        Document d;
        d.edits = e0;
        toggleSeparation(d.edits, a, touch);
        toggleSeparation(d.edits, a, gap);
        Document r;
        CHECK(deserialize(serialize(d), r), "reads back");
        CHECK(r.edits.notes.size() == d.edits.notes.size() && !softSeparation(r.edits, a, touch) && softSeparation(r.edits, a, gap),
              "separations survive save and load");
        Document plain;
        plain.edits = e0;
        CHECK(serialize(plain).find("seps") == std::string::npos, "nothing extra is written without separations");
        resetNoteEdits(r.edits, a, {touch, touch + 1, gap, gap + 1});
        CHECK(!softSeparation(r.edits, a, touch) && softSeparation(r.edits, a, gap), "reset keeps separations");
    }
}

static void testMoveCut(const Analysis& a)
{
    std::printf("[cut] moving a cut re-assigns audio between notes; the sound stays in time\n");
    const int n = (int)a.notes.size();
    const auto spans = spansOf(a);
    int b = -1; // two notes that touch in the source
    for (int i = 1; i + 2 < n; ++i)
        if (spans[(size_t)i].end == spans[(size_t)i + 1].start && spans[(size_t)i].end - spans[(size_t)i].start > 30 &&
            spans[(size_t)i + 1].end - spans[(size_t)i + 1].start > 30) { b = i; break; }
    if (b < 0) { CHECK(false, "demo melody has two long notes touching in the source"); return; }
    const double hopSec = (double)a.hop / a.sampleRate;

    // Untouched notes: the cut moves 10 frames left; both notes stay unedited in time.
    {
        Document d;
        conform(d, a);
        const auto t0 = noteTimes(a, d.edits);
        const double target = t0[(size_t)b].outEnd - 10 * hopSec;
        CHECK(moveCut(d, a, b, false, target), "cut moved");
        CHECK(d.edits.spans[(size_t)b].end == spans[(size_t)b].end - 10 && d.edits.spans[(size_t)b + 1].start == spans[(size_t)b].end - 10,
              "the shared cut is 10 frames earlier (%d)", d.edits.spans[(size_t)b].end);
        const Analysis eff = withLayout(a, d.edits.spans);
        conform(d, eff);
        bool still = true;
        for (const auto& t : noteTimes(eff, d.edits)) still = still && std::abs(t.outStart - t.inStart) < 1e-9 && std::abs(t.outEnd - t.inEnd) < 1e-9;
        CHECK(still, "no note is moved or stretched by moving a cut");
    }
    // A stretched pair: the audio at the new cut plays where it did; the pair's outer ends stay.
    {
        Document d;
        conform(d, a);
        const auto t00 = noteTimes(a, d.edits);
        setNoteDuration(d.edits, a, b, (t00[(size_t)b].inEnd - t00[(size_t)b].inStart) * 1.3); // the right note gives way (soft)
        const auto t0 = noteTimes(a, d.edits);
        const double target = t0[(size_t)b].outEnd + 12 * hopSec; // into the right note
        CHECK(moveCut(d, a, b, false, target), "cut moved into the right note");
        const int f = d.edits.spans[(size_t)b].end;
        const Analysis eff = withLayout(a, d.edits.spans);
        conform(d, eff);
        const auto t1 = noteTimes(eff, d.edits);
        // where the right note played source frame f before
        const NoteTimes& R0 = t0[(size_t)b + 1];
        const double secF = eff.notes[(size_t)b + 1].startSec;
        const double expected = R0.outStart + (secF - R0.inStart) * (R0.outEnd - R0.outStart) / (R0.inEnd - R0.inStart);
        std::printf("  cut to frame %d: notes meet at %.5f s, that audio played at %.5f s before\n", f, t1[(size_t)b].outEnd, expected);
        CHECK(std::abs(t1[(size_t)b].outEnd - expected) < 1e-6 && std::abs(t1[(size_t)b + 1].outStart - expected) < 1e-6,
              "the audio at the new cut plays where it did");
        CHECK(std::abs(t1[(size_t)b].outStart - t0[(size_t)b].outStart) < 1e-9 && std::abs(t1[(size_t)b + 1].outEnd - t0[(size_t)b + 1].outEnd) < 1e-9,
              "the pair's outer ends stay");
        CHECK(worstOverlap(t1) < 1e-9, "no overlap");
    }
    // Limits: a cut dragged far stops so that each note keeps ~20 ms.
    {
        Document d;
        conform(d, a);
        CHECK(moveCut(d, a, b, false, 0.0), "cut dragged to the far left");
        CHECK(d.edits.spans[(size_t)b].end - d.edits.spans[(size_t)b].start == 4, "the left note keeps 4 frames (%d)",
              d.edits.spans[(size_t)b].end - d.edits.spans[(size_t)b].start);
    }
    // Pulled apart (hard separation): the dragged edge follows the pointer, the gap stays.
    {
        Document d;
        conform(d, a);
        toggleSeparation(d.edits, a, b);
        moveBoundary(d.edits, a, b, -0.1, {}, false); // the left note ends 0.1 s earlier
        const auto t0 = noteTimes(a, d.edits);
        const double g0 = t0[(size_t)b + 1].outStart - t0[(size_t)b].outEnd;
        CHECK(g0 > 0.09, "a gap is open (%.4f)", g0);
        const double target = t0[(size_t)b + 1].outStart - 5 * hopSec; // the right note's start, 5 frames left
        CHECK(moveCut(d, a, b + 1, true, target), "cut moved");
        CHECK(d.edits.spans[(size_t)b + 1].start == spans[(size_t)b].end - 5, "the cut is 5 frames earlier (%d)",
              d.edits.spans[(size_t)b + 1].start - spans[(size_t)b].end);
        const Analysis eff = withLayout(a, d.edits.spans);
        conform(d, eff);
        const auto t1 = noteTimes(eff, d.edits);
        const double g1 = t1[(size_t)b + 1].outStart - t1[(size_t)b].outEnd;
        std::printf("  apart: gap %.4f s before, %.4f s after; right note starts %.4f s (pointer %.4f s)\n", g0, g1,
                    t1[(size_t)b + 1].outStart, target);
        CHECK(std::abs(t1[(size_t)b + 1].outStart - target) < 1e-6, "the dragged edge follows the pointer");
        CHECK(g1 > 0.5 * g0, "the gap stays open");
        CHECK(std::abs(t1[(size_t)b + 1].outEnd - t0[(size_t)b + 1].outEnd) < 1e-9 && std::abs(t1[(size_t)b].outStart - t0[(size_t)b].outStart) < 1e-9,
              "the outer ends stay");
    }
}

static void testSilentGaps(const test::SynthResult& s, const Analysis& a, double sr)
{
    std::printf("[gaps] notes pulled apart: the gap is silent; a pause keeps its length before the later note\n");
    const int n = (int)a.notes.size();
    EditSet e0;
    e0.notes.resize((size_t)n);
    const auto t = noteTimes(a, e0);
    int touch = -1, gap = -1;
    for (int b = 1; b + 2 < n; ++b) {
        const double g = t[(size_t)b + 1].outStart - t[(size_t)b].outEnd;
        const double dl = t[(size_t)b].outEnd - t[(size_t)b].outStart, dr = t[(size_t)b + 1].outEnd - t[(size_t)b + 1].outStart;
        if (touch < 0 && std::abs(g) < 0.005 && dl > 0.3 && dr > 0.15) touch = b;
        if (gap < 0 && g > 0.03 && g < 0.5 && dr > 0.15) gap = b;
    }
    std::printf("  touching pair at %d, pair with a pause at %d\n", touch, gap);
    if (touch < 0 || gap < 0) { CHECK(false, "demo melody has both kinds of pairs"); return; }
    const double fade = kSilenceFadeSec * sr;
    const std::vector<float> y0 = render(s.audio.data(), s.audio.size(), a, e0);

    { // soft: the neighbour follows, nothing opens
        EditSet e = e0;
        moveNotes(e, a, {touch + 1}, 0.02);
        CHECK(TimeMap::build(a, e).silences().empty(), "soft move: no silence");
    }
    { // hard: the left note's end pulled back 0.2 s; the right note stays
        EditSet e = e0;
        toggleSeparation(e, a, touch);
        const double applied = moveBoundary(e, a, touch, -0.2, {}, false);
        const TimeMap m = TimeMap::build(a, e);
        CHECK(applied < -0.19, "the edge moved (%.4f)", applied);
        CHECK(m.silences().size() == 1, "one silence (%d)", (int)m.silences().size());
        if (m.silences().size() != 1) return;
        const double s0 = m.silences()[0].first, s1 = m.silences()[0].second;
        std::printf("  touching pair: silence %.4f..%.4f s\n", s0 / sr, s1 / sr);
        CHECK(std::abs(s0 - m.noteEndOut((size_t)touch)) < 1e-6 && std::abs(s1 - m.noteStartOut((size_t)touch + 1)) < 1e-6,
              "the silence is the gap between the notes");
        CHECK(std::abs(m.noteStartOut((size_t)touch + 1) / sr - t[(size_t)touch + 1].outStart) < 2.0 / sr, "the right note stays in place");
        CHECK(std::abs((s1 - s0) / sr - 0.2) < 0.003, "the gap is 0.2 s (%.4f)", (s1 - s0) / sr);
        const double joint = a.notes[(size_t)touch].endSec * sr;
        CHECK(std::abs(m.inToOutBefore(joint) - s0) < 1 && std::abs(m.inToOut(joint) - s1) < 1, "the joint maps to both ends of the silence");
        // The line of the right note's first frame sits after the silence, the left note's last before it.
        const int fr = a.notes[(size_t)touch + 1].startFrame;
        CHECK(m.silenceBetween((fr - 1) * (double)a.hop, fr * (double)a.hop), "the pitch line breaks at the silence");
        CHECK(m.inToOut(fr * (double)a.hop) > s1 && m.inToOut((fr - 1) * (double)a.hop) < s0, "frames on either side");
        const std::vector<float> y = render(s.audio.data(), s.audio.size(), a, e);
        CHECK(y.size() == y0.size(), "the clip keeps its length");
        double peak = 0;
        for (long o = (long)std::ceil(s0); o < (long)std::floor(s1) && o < (long)y.size(); ++o) peak = std::max(peak, std::abs((double)y[(size_t)o]));
        double after = 0;
        for (long o = (long)(s1 + fade); o < (long)(s1 + fade + 0.05 * sr) && o < (long)y.size(); ++o) after += (double)y[(size_t)o] * y[(size_t)o];
        after = std::sqrt(after / (0.05 * sr));
        std::printf("  rendered: peak in the gap %.6f, level after it %.4f\n", peak, after);
        CHECK(peak == 0, "the gap is silent");
        CHECK(after > 0.01, "the right note sounds after it");
        CHECK(m.gainAt(0.5 * (s0 + s1), fade) == 0 && m.gainAt(s1 + 2 * fade, fade) == 1 && m.gainAt(s0 - 2 * fade, fade) == 1,
              "A/B follows the same silence");
        const double mid = m.gainAt(s0 - 0.5 * fade, fade);
        CHECK(mid > 0.4 && mid < 0.6, "a short fade before the silence (%.3f)", mid);
    }
    { // a pause between the notes: the right note moved later; the pause keeps its length before it
        EditSet e = e0;
        const double applied = moveNotes(e, a, {gap + 1}, 0.1);
        const TimeMap m = TimeMap::build(a, e);
        const double pause = a.notes[(size_t)gap + 1].startSec - a.notes[(size_t)gap].endSec;
        std::printf("  pause %.4f s, note moved %.4f s, %d silence(s)\n", pause, applied, (int)m.silences().size());
        CHECK(applied > 3 * kMinSilenceSec, "the note moved");
        CHECK(m.silences().size() == 1, "one silence");
        if (m.silences().size() != 1) return;
        const double r = m.noteStartOut((size_t)gap + 1) / sr;
        CHECK(std::abs(m.silences()[0].second / sr - (r - pause)) < 1e-6, "the silence ends where the pause begins");
        CHECK(std::abs((m.silences()[0].second - m.silences()[0].first) / sr - applied) < 1e-3, "the silence is the move");
        CHECK(std::abs(m.outToIn((r - 0.5 * pause) * sr) / sr - (a.notes[(size_t)gap + 1].startSec - 0.5 * pause)) < 1e-6,
              "the pause plays at its own speed just before the note");
    }
    { // grown by less than the shortest silence: stretched as before
        EditSet e = e0;
        moveNotes(e, a, {gap + 1}, 0.5 * kMinSilenceSec);
        CHECK(TimeMap::build(a, e).silences().empty(), "a small widening: no silence");
    }
    { // the last note shortened: the tail keeps its length, silence fills the clip to its end
        EditSet e = e0;
        const double applied = moveBoundary(e, a, n - 1, -0.2, {}, false);
        const TimeMap m = TimeMap::build(a, e);
        CHECK(applied < -0.19 && m.silences().size() == 1, "tail: one silence (%d, %.4f)", (int)m.silences().size(), applied);
        if (m.silences().size() != 1) return;
        const double tail = (double)a.numSamples - a.notes[(size_t)n - 1].endSec * sr;
        CHECK(std::abs(m.silences()[0].first - m.noteEndOut((size_t)n - 1) - tail) < 2, "the tail keeps its length");
        CHECK(std::abs(m.silences()[0].second - m.inToOut((double)a.numSamples)) < 1e-6 &&
                  std::abs(m.inToOut((double)a.numSamples) - (double)a.numSamples) < 1e-6,
              "silence to the clip's end; the clip keeps its length");
    }
}

static void testTimeFaults(const Analysis& a)
{
    std::printf("[time edits] a fault elsewhere does not block; reset, split, join keep the layout sound\n");
    const int n = (int)a.notes.size();
    if (n < 8) { CHECK(false, "need 8 notes"); return; }

    // An overlap left by an earlier build (note 2 pulled back onto note 1).
    EditSet bad;
    bad.notes.resize((size_t)n);
    {
        const auto t = noteTimes(a, bad);
        bad.notes[2].timeOffsetSec = -0.5 * (t[1].outEnd - t[1].outStart);
    }
    const double ov = worstOverlap(noteTimes(a, bad));
    CHECK(ov > 0.01, "setup: an overlap exists (%.4f)", ov);
    {
        EditSet e = bad;
        const double applied = moveNotes(e, a, {6}, 0.01);
        std::printf("  move far from the overlap: applied %.4f of 0.01\n", applied);
        CHECK(std::abs(applied - 0.01) < 1e-9, "a move far from an overlap is not blocked (%.4f)", applied);
        CHECK(std::abs(worstOverlap(noteTimes(a, e)) - ov) < 1e-9, "the old overlap is untouched");
    }
    {
        EditSet e = bad;
        const double deeper = moveNotes(e, a, {2}, -0.01);
        CHECK(deeper == 0, "the overlapping note may not go deeper (%.4f)", deeper);
        const double away = moveNotes(e, a, {2}, 0.01);
        CHECK(std::abs(away - 0.01) < 1e-9, "the overlapping note may move away (%.4f)", away);
        CHECK(worstOverlap(noteTimes(a, e)) < ov - 0.009, "moving away shrinks the overlap");
    }

    // Reset: a moved run goes back as one block, touching neighbours follow.
    {
        EditSet e;
        e.notes.resize((size_t)n);
        const double applied = moveNotes(e, a, {3, 4}, 0.04);
        CHECK(applied > 0.02, "setup: run moved (%.4f)", applied);
        e.notes[3].pitchOffset = 1;
        resetNoteEdits(e, a, {3, 4});
        double worst = 0;
        for (const auto& t : noteTimes(a, e)) worst = std::max({worst, std::abs(t.outStart - t.inStart), std::abs(t.outEnd - t.inEnd)});
        std::printf("  reset of a moved run: every note within %.2g s of its recorded time\n", worst);
        CHECK(worst < 1e-9, "reset brings the run and its touching neighbours back (%.3g)", worst);
        CHECK(e.notes[3].pitchOffset == 0, "reset clears the pitch");
    }
    // Reset of a note whose recorded place is taken now: pitch goes, timing stays.
    {
        EditSet e;
        e.notes.resize((size_t)n);
        std::set<int> tail;
        for (int i = 4; i < n; ++i) tail.insert(i);
        const double applied = moveNotes(e, a, tail, 0.06);
        if (applied > 0.05) {
            auto t = noteTimes(a, e);
            NoteTimes n3 = t[3];
            n3.outEnd = t[4].inStart + 0.03; // note 3 now reaches into note 4's recorded place, not touching it
            setNoteTimes(e.notes[3], n3);
            e.notes[4].pitchOffset = 1;
            const auto before = noteTimes(a, e);
            CHECK(worstOverlap(before) < 1e-9, "setup: sound layout");
            resetNoteEdits(e, a, {4});
            const auto after = noteTimes(a, e);
            CHECK(std::abs(after[4].outStart - before[4].outStart) < 1e-9 && std::abs(after[4].outEnd - before[4].outEnd) < 1e-9,
                  "note 4 keeps its timing: its recorded place is taken");
            CHECK(e.notes[4].pitchOffset == 0, "note 4's pitch is reset anyway");
            CHECK(worstOverlap(after) < 1e-9, "reset makes no overlap");
        } else {
            CHECK(false, "setup: tail moved (%.4f)", applied);
        }
    }

    // Split of a stretched note: the halves stay where the note was.
    {
        Document d;
        conform(d, a);
        const auto t0 = noteTimes(a, d.edits);
        setNoteDuration(d.edits, a, 2, (t0[2].inEnd - t0[2].inStart) * 1.5);
        const auto before = noteTimes(a, d.edits);
        const Note& nn = a.notes[2];
        const int frame = (nn.startFrame + nn.endFrame) / 2;
        CHECK(splitNote(d, a, 2, frame), "split done");
        const Analysis eff = withLayout(a, d.edits.spans);
        conform(d, eff);
        const auto after = noteTimes(eff, d.edits);
        std::printf("  split of a stretched note: halves %.4f-%.4f | %.4f-%.4f, note was %.4f-%.4f\n", after[2].outStart,
                    after[2].outEnd, after[3].outStart, after[3].outEnd, before[2].outStart, before[2].outEnd);
        CHECK(std::abs(after[2].outStart - before[2].outStart) < 1e-9, "left half starts where the note did");
        CHECK(std::abs(after[3].outEnd - before[2].outEnd) < 1e-9, "right half ends where the note did");
        CHECK(std::abs(after[3].outStart - after[2].outEnd) < 1e-9, "the halves touch");
        CHECK(worstOverlap(after) < 1e-9, "split makes no overlap");
    }
    // A note squeezed to its minimal length is not split (its halves would be too short).
    {
        Document d;
        conform(d, a);
        setNoteDuration(d.edits, a, 2, 0.03);
        const Note& nn = a.notes[2];
        CHECK(!splitNote(d, a, 2, (nn.startFrame + nn.endFrame) / 2), "a squeezed note is not split");
    }

    // Join of two notes with different timing: the joined note covers both.
    {
        Document d;
        conform(d, a);
        const auto t0 = noteTimes(a, d.edits);
        moveBoundary(d.edits, a, 3, 0.03, {}, false); // note 3 longer, note 4 shorter (if they touch)
        moveNotes(d.edits, a, {4}, 0.02);
        const auto before = noteTimes(a, d.edits);
        CHECK(joinNotes(d, a, 3), "join done");
        const Analysis eff = withLayout(a, d.edits.spans);
        conform(d, eff);
        const auto after = noteTimes(eff, d.edits);
        std::printf("  join: %.4f-%.4f + %.4f-%.4f -> %.4f-%.4f\n", before[3].outStart, before[3].outEnd, before[4].outStart,
                    before[4].outEnd, after[3].outStart, after[3].outEnd);
        CHECK(std::abs(after[3].outStart - before[3].outStart) < 1e-9, "joined note starts where the left one did");
        CHECK(std::abs(after[3].outEnd - before[4].outEnd) < 1e-9, "joined note ends where the right one did");
        CHECK(worstOverlap(after) < 1e-9, "join makes no overlap");
        (void)t0;
    }
}

static void testTransfer(const Analysis& a)
{
    std::printf("[transfer A] note edits copied with per-parameter share\n");
    if (a.notes.size() < 2) { CHECK(false, "need at least 2 notes"); return; }
    Document src;
    conform(src, a);
    src.edits.notes[1].pitchOffset = 0.7;
    src.edits.notes[1].timeOffsetSec = 0.02;
    src.edits.notes[1].stretch = 1.2;

    Document full;
    conform(full, a);
    CHECK(transferNoteEdits(src, full, a, {1, 1}), "full transfer refused");
    CHECK(std::abs(full.edits.notes[1].pitchOffset - 0.7) < 1e-12, "pitch not copied at share 1");
    CHECK(std::abs(full.edits.notes[1].timeOffsetSec - 0.02) < 1e-12, "time not copied at share 1");
    CHECK(std::abs(full.edits.notes[1].stretch - 1.2) < 1e-12, "stretch not copied at share 1");

    Document half;
    conform(half, a);
    CHECK(transferNoteEdits(src, half, a, {0.5, 0.5}), "half transfer refused");
    CHECK(std::abs(half.edits.notes[1].pitchOffset - 0.35) < 1e-12, "pitch share 0.5 wrong");
    CHECK(std::abs(half.edits.notes[1].stretch - 1.1) < 1e-12, "stretch share 0.5 wrong");

    Document none;
    conform(none, a);
    CHECK(transferNoteEdits(src, none, a, {0, 0}), "zero transfer refused");
    CHECK(none.edits.notes[1].pitchOffset == 0 && none.edits.notes[1].stretch == 1,
          "share 0 changed the target");

    Document other = src;
    other.edits.notes.pop_back();
    Document target;
    conform(target, a);
    CHECK(!transferNoteEdits(other, target, a, {1, 1}), "mismatched note count accepted");
}

// Thread alignment against the synthetic singer's ground truth: target note k must land
// on reference note refOf[k] (-1 = not checked). Prints and returns the worst note-start error.
static double alignError(const test::SynthResult& ref, const test::SynthResult& tgt, double tgtOffset, const Warp& w,
                         const std::vector<int>& refOf, bool* midsInside)
{
    double worst = 0;
    bool inside = true;
    for (size_t k = 0; k < tgt.noteStart.size() && k < refOf.size(); ++k) {
        const int r = refOf[k];
        if (r < 0) continue;
        const double mid = 0.5 * (tgt.noteStart[k] + tgt.noteEnd[k]) + tgtOffset;
        const double m = w.map(mid);
        if (m < ref.noteStart[(size_t)r] || m > ref.noteEnd[(size_t)r]) inside = false;
        if (k > 0 && refOf[k - 1] == r) continue; // second half of a split note: no start of its own
        worst = std::max(worst, std::abs(w.map(tgt.noteStart[k] + tgtOffset) - ref.noteStart[(size_t)r]));
    }
    if (midsInside) *midsInside = inside;
    return worst;
}

static void testThreadAlign()
{
    std::printf("[thread] alignment by the pitch line: shift, tempo, octave, other part, split note, pin\n");
    const double sr = 48000;
    auto thread = [](const Analysis& a, double offset) {
        EditSet e;
        e.notes.resize(a.notes.size());
        return threadOf(a, e, [offset](double t) { return t + offset; }, 0.0, (double)a.numSamples / a.sampleRate);
    };
    const test::SynthResult ref = test::synthesize(test::demoMelody(), sr);
    const Analysis ra = analyze(ref.audio.data(), ref.audio.size(), sr);
    const auto refT = thread(ra, 0.0);
    std::vector<int> same;
    for (int k = 0; k < (int)ref.noteStart.size(); ++k) same.push_back(k);
    auto report = [&](const char* name, const test::SynthResult& tgt, double off, const Warp& w,
                      const std::vector<int>& refOf, double tol, const AlignInfo& inf) {
        bool inside = false;
        const double e = alignError(ref, tgt, off, w, refOf, &inside);
        std::printf("  %-34s note starts within %5.1f ms, middles %s, octave %+d, coarse %+.3f s, cost %.3f\n", name,
                    e * 1000, inside ? "on the right notes" : "OFF", inf.octave, inf.coarseSec, inf.meanCost);
        CHECK(!w.empty(), "%s: no alignment", name);
        CHECK(e < tol, "%s: note start off by %.1f ms", name, e * 1000);
        CHECK(inside, "%s: a note middle maps onto another note", name);
    };

    { // the same take, its item placed 80 ms later
        AlignInfo inf;
        const Warp w = alignThreads(thread(ra, 0.08), refT, {}, {}, &inf);
        report("same take, 80 ms later", ref, 0.08, w, same, 0.012, inf);
    }
    // Another performance: 10 % slower, other intonation and vibrato.
    std::vector<test::SynthNote> slow = test::demoMelody();
    for (size_t k = 0; k < slow.size(); ++k) { slow[k].dur *= 1.1; slow[k].detuneCents = (k % 2 ? 1 : -1) * 20.0; }
    const test::SynthResult sl = test::synthesize(slow, sr, 25, 5.1, 777);
    const Analysis sa = analyze(sl.audio.data(), sl.audio.size(), sr);
    Warp slowW;
    {
        AlignInfo inf;
        slowW = alignThreads(thread(sa, 0.0), refT, {}, {}, &inf);
        report("another take, 10 % slower", sl, 0.0, slowW, same, 0.035, inf);
        CHECK(!inf.part, "a unison take taken for another part (median %.2f st)", inf.medianDiff);
    }
    { // an octave lower, 50 ms later
        std::vector<test::SynthNote> low = test::demoMelody();
        for (auto& n : low) n.midi -= 12;
        const test::SynthResult lo = test::synthesize(low, sr, 30, 5.4, 4242);
        const Analysis la = analyze(lo.audio.data(), lo.audio.size(), sr);
        AlignInfo inf;
        const Warp w = alignThreads(thread(la, 0.05), refT, {}, {}, &inf);
        report("an octave lower, 50 ms later", lo, 0.05, w, same, 0.02, inf);
        CHECK(inf.octave == 1, "octave found %+d, want +1", inf.octave);
    }
    { // another part: a fourth above (shape only)
        std::vector<test::SynthNote> part = test::demoMelody();
        for (auto& n : part) n.midi += 5;
        const test::SynthResult pt = test::synthesize(part, sr, 35, 5.8, 99);
        const Analysis pa = analyze(pt.audio.data(), pt.audio.size(), sr);
        AlignOptions o;
        o.unison = false;
        AlignInfo inf;
        const Warp w = alignThreads(thread(pa, 0.0), refT, o, {}, &inf);
        report("another part, a fourth above", pt, 0.0, w, same, 0.035, inf);
        AlignInfo autoInf;
        const Warp wa = alignThreads(thread(pa, 0.0), refT, {}, {}, &autoInf);
        report("  the same, part found by itself", pt, 0.0, wa, same, 0.035, autoInf);
        CHECK(autoInf.part, "another part not recognised (median %.2f st)", autoInf.medianDiff);
    }
    { // the third note sung as two syllables ("s" in the middle)
        std::vector<test::SynthNote> split = test::demoMelody();
        split[2].dur = 0.3;
        split.insert(split.begin() + 3, test::SynthNote{64, 0.3, -32, true, 1});
        const test::SynthResult sp = test::synthesize(split, sr, 35, 5.6, 31337);
        const Analysis spa = analyze(sp.audio.data(), sp.audio.size(), sr);
        std::vector<int> refOf = {0, 1, 2, 2};
        for (int k = 3; k < (int)ref.noteStart.size(); ++k) refOf.push_back(k);
        AlignInfo inf;
        const Warp w = alignThreads(thread(spa, 0.0), refT, {}, {}, &inf);
        report("a note split by an extra syllable", sp, 0.0, w, refOf, 0.04, inf);
    }
    { // a pin: target note 5 starts where the user says
        const Pin p{sl.noteStart[5], ref.noteStart[5] + 0.06};
        const Warp w = alignThreads(thread(sa, 0.0), refT, {}, {p});
        const double e = std::abs(w.map(p.tgt) - p.ref);
        std::printf("  pin: passes %.1f ms from the pinned point\n", e * 1000);
        CHECK(e < 0.012, "pin missed by %.1f ms", e * 1000);
    }
}

static void testThreadFix()
{
    std::printf("[thread] wrong notes: whole semitones toward the reference, own cents kept, edits left alone\n");
    const double sr = 48000;
    auto thread = [](const Analysis& a, const EditSet& e) {
        return threadOf(a, e, nullptr, 0.0, (double)a.numSamples / a.sampleRate);
    };
    const test::SynthResult ref = test::synthesize(test::demoMelody(), sr);
    const Analysis ra = analyze(ref.audio.data(), ref.audio.size(), sr);
    EditSet re;
    re.notes.resize(ra.notes.size());
    const auto refT = thread(ra, re);

    // Another take: note 3 a fourth too high, note 6 1.75 st low, the rest within ~30 cents.
    std::vector<test::SynthNote> mel = test::demoMelody();
    for (size_t k = 0; k < mel.size(); ++k) mel[k].detuneCents += (k % 2 ? -12.0 : 14.0);
    mel[3].midi += 5;
    mel[6].midi -= 2;
    mel[6].detuneCents = -15 + 25; // 57 - 0.15 sung by the reference; 55 + 0.10 here: 1.75 st low
    auto run = [&](const std::vector<test::SynthNote>& m, double octaveShift, const char* name, const AlignOptions& o,
                   EditSet* edits, std::vector<NoteFix>* outFixes, const Analysis** outA) {
        std::vector<test::SynthNote> mm = m;
        for (auto& n : mm) n.midi += octaveShift;
        static std::vector<std::unique_ptr<Analysis>> keep;
        const test::SynthResult s = test::synthesize(mm, sr, 28, 5.3, 2024);
        keep.push_back(std::make_unique<Analysis>(analyze(s.audio.data(), s.audio.size(), sr)));
        const Analysis& a = *keep.back();
        EditSet e;
        e.notes.resize(a.notes.size());
        if (edits) { *edits = e; }
        AlignInfo inf;
        const Warp w = alignThreads(thread(a, e), refT, o, {}, &inf);
        std::vector<NoteFix> f = planPitchFix(a, edits ? *edits : e, refT, w, inf);
        std::printf("  %-30s", name);
        for (const auto& x : f) std::printf(" %+d", x.semis);
        std::printf("   (octave %+d%s)\n", inf.octave, inf.part ? ", another part" : "");
        if (outFixes) *outFixes = f;
        if (outA) *outA = &a;
        return f;
    };
    auto only = [](const std::vector<NoteFix>& f, std::map<int, int> want) {
        for (const auto& x : f) {
            const int w = want.count(x.note) ? want[x.note] : 0;
            if (x.semis != w) return false;
        }
        return !f.empty();
    };

    const Analysis* a = nullptr;
    std::vector<NoteFix> fixes;
    run(mel, 0, "two wrong notes", {}, nullptr, &fixes, &a);
    CHECK(only(fixes, {{3, -5}, {6, 2}}), "want -5 on note 3 and +2 on note 6 only");
    if (a != nullptr && fixes.size() == a->notes.size()) {
        EditSet e;
        e.notes.resize(a->notes.size());
        applyPitchFix(e, fixes);
        const double c3 = editedCenter(*a, e, 3), c6 = editedCenter(*a, e, 6);
        const double r3 = 62 + 0.12, r6 = 57 - 0.15; // what the reference sings
        std::printf("    after: note 3 %+.0f cents from the reference, note 6 %+.0f (own intonation kept)\n",
                    (c3 - r3) * 100, (c6 - r6) * 100);
        CHECK(std::abs(c3 - r3) < 0.5 && std::abs(c6 - r6) < 0.5, "fixed notes not on the reference's notes");
        CHECK(std::abs((c6 - r6) - 0.25) < 0.1, "note 6 lost its own cents (%.2f)", c6 - r6);
    }
    {
        std::vector<NoteFix> f = run(mel, -12, "the same an octave lower", {}, nullptr, nullptr, nullptr);
        CHECK(only(f, {{3, -5}, {6, 2}}), "octave lower: want -5 on note 3 and +2 on note 6 only");
    }
    {
        std::vector<test::SynthNote> part = test::demoMelody();
        std::vector<NoteFix> f = run(part, 5, "another part (fourth above)", {}, nullptr, nullptr, nullptr);
        CHECK(only(f, {}), "another part must not be pitch-fixed");
    }
    {
        // A note the user already moved stays as the user left it.
        const test::SynthResult s = test::synthesize(mel, sr, 28, 5.3, 2024);
        const Analysis ea = analyze(s.audio.data(), s.audio.size(), sr);
        EditSet e;
        e.notes.resize(ea.notes.size());
        e.notes[3].pitchOffset = 0.3;
        AlignInfo inf;
        const Warp w = alignThreads(thread(ea, e), refT, {}, {}, &inf);
        const auto f = planPitchFix(ea, e, refT, w, inf);
        CHECK(f.size() == ea.notes.size() && f[3].edited && f[3].semis == 0 && f[6].semis == 2,
              "an edited note was planned for a fix, or the other wrong note was missed");
    }
    {
        // One note of ours where the reference sings two (64 then 61): the reference is not
        // steady over it, so it must not be moved as a whole.
        std::vector<test::SynthNote> two = test::demoMelody();
        two[2].dur = 0.3;
        two.insert(two.begin() + 3, test::SynthNote{61, 0.3, 0, false, 1});
        const test::SynthResult rs = test::synthesize(two, sr);
        const Analysis r2 = analyze(rs.audio.data(), rs.audio.size(), sr);
        EditSet r2e;
        r2e.notes.resize(r2.notes.size());
        const auto ref2 = thread(r2, r2e);
        const test::SynthResult ts = test::synthesize(test::demoMelody(), sr, 28, 5.3, 2024);
        const Analysis ta = analyze(ts.audio.data(), ts.audio.size(), sr);
        EditSet te;
        te.notes.resize(ta.notes.size());
        // The two takes start together, so the true map is the identity (DTW may instead
        // squeeze our long note into their first one, which is also a fair reading).
        Warp w;
        w.tgt = {0.0, 10.0};
        w.ref = {0.0, 10.0};
        const auto f = planPitchFix(ta, te, ref2, w, AlignInfo{});
        bool moved = false;
        for (const auto& x : f) moved = moved || x.semis != 0;
        std::printf("  one note over two of the reference: note 2 %s, nothing moved: %s\n",
                    f.size() > 2 && f[2].unsteady ? "flagged unsteady" : "NOT flagged", moved ? "no" : "yes");
        CHECK(f.size() > 2 && f[2].unsteady && !moved, "a note spanning two reference notes was moved or not flagged");
    }
}


// Track view: two items of one track (the second off the frame grid, trimmed) as one clip.
// Pins between a clip and its reference: kept sorted, never crossing a neighbour, saved with the document.
static void testPins()
{
    std::printf("[pins] add / move keep the order; round trip\n");
    std::vector<Pin> p;
    CHECK(addPin(p, {1.0, 2.0}), "first pin");
    CHECK(addPin(p, {3.0, 4.0}), "later pin");
    CHECK(addPin(p, {2.0, 3.0}), "pin between");
    CHECK(p.size() == 3 && p[1].tgt == 2.0, "sorted insert");
    CHECK(!addPin(p, {2.0005, 3.5}), "pin on a pin accepted");
    CHECK(!addPin(p, {2.5, 2.5}), "pin crossing its neighbour accepted");
    CHECK(movePin(p, 1, true, 99.0) && p[1].ref < p[2].ref, "ref end passes the next pin");
    CHECK(p[1].ref < p[2].ref - 5e-4, "ref end not clamped to the next pin");
    CHECK(movePin(p, 1, false, -5.0) && p[1].tgt > p[0].tgt, "target end passes the previous pin");
    CHECK(!movePin(p, 9, true, 1.0), "missing pin moved");

    Document d;
    d.pins = p;
    d.pinRef = "Track 2 \"lead\"";
    Document r;
    CHECK(deserialize(serialize(d), r), "pins do not load");
    CHECK(r.pins.size() == 3 && r.pins[2].ref == p[2].ref, "pins not kept");
    CHECK(r.pinRef == d.pinRef, "reference name not kept: '%s'", r.pinRef.c_str());
    CHECK(serialize(r) == serialize(d), "pins round trip differs");
}

static void testTrackView()
{
    std::printf("[track view] items of a track as one clip: notes, round trip, edits back to their item\n");
    const double sr = 48000;
    std::vector<test::SynthNote> melB = test::demoMelody();
    for (auto& n : melB) n.midi += 2;
    const test::SynthResult sa = test::synthesize(test::demoMelody(), sr);
    const test::SynthResult sb = test::synthesize(melB, sr, 30, 5.2, 777);
    auto effA = std::make_shared<const Analysis>(analyze(sa.audio.data(), sa.audio.size(), sr));
    auto effB = std::make_shared<const Analysis>(analyze(sb.audio.data(), sb.audio.size(), sr));
    const double lenA = (double)effA->numSamples / sr, lenB = (double)effB->numSamples / sr;
    const double hopSec = (double)effA->hop / sr;

    std::vector<TrackPiece> pieces(2);
    pieces[0].eff = effA;
    pieces[0].winStart = 0;
    pieces[0].winEnd = lenA;
    pieces[0].placeSec = 0;
    pieces[1].eff = effB;
    pieces[1].winStart = 0.05;              // trimmed start (inside the leading silence)
    pieces[1].winEnd = lenB;
    pieces[1].placeSec = lenA + 0.7013 - 0.05; // window starts 0.7013 s after A ends: off the frame grid
    pieces[0].doc.edits.breathGainDb = -6;  // per-item setting, not touched in the view
    conform(pieces[0].doc, *effA);
    conform(pieces[1].doc, *effB);
    pieces[1].doc.edits.notes[2].pitchOffset = 0.5; // an edit made before, in the item itself

    TrackView v;
    CHECK(buildTrackView(pieces, 0, v), "view not built");
    if (v.analysis == nullptr) return;
    const Analysis& va = *v.analysis;
    const size_t nA = effA->notes.size(), nB = effB->notes.size();
    std::printf("  notes: A %zu + B %zu -> view %zu\n", nA, nB, va.notes.size());
    CHECK(va.notes.size() == nA + nB, "every note of both items in the view");
    CHECK(v.doc.edits.notes.size() == va.notes.size() && v.doc.edits.spans.size() == va.notes.size(),
          "view document aligned with its notes");
    double worst = 0;
    for (size_t j = 0; j < nB && nA + j < va.notes.size(); ++j)
        worst = std::max(worst, std::abs(va.notes[nA + j].startSec - (effB->notes[j].startSec + pieces[1].placeSec)));
    std::printf("  item B drawn at most %.2f ms from its exact place (half a frame: %.2f ms)\n", worst * 1000, hopSec * 500);
    CHECK(worst <= hopSec * 0.5 + 1e-9, "item B within half a frame of its place");
    for (size_t j = 1; j < va.notes.size(); ++j)
        CHECK(va.notes[j].startFrame >= va.notes[j - 1].endFrame, "view notes do not overlap (%zu)", j);
    CHECK(std::abs(v.doc.edits.notes[nA + 2].pitchOffset - 0.5) < 1e-12, "item B's own edit shown in the view");
    CHECK(v.doc.edits.breathGainDb == -6, "track settings shown from the primary item");

    // Round trip: nothing edited -> each item's document stays exactly as it was.
    {
        std::vector<bool> lay;
        const auto docs = splitTrackView(v, v.doc, pieces, &lay);
        CHECK(docs.size() == 2, "two documents back");
        bool same = docs.size() == 2 && serialize(docs[0]) == serialize(pieces[0].doc) &&
                    serialize(docs[1]) == serialize(pieces[1].doc);
        CHECK(same, "unedited view gives back the items' documents unchanged");
        CHECK(lay.size() == 2 && !lay[0] && !lay[1], "no layout change reported");
    }

    // A pitch edit on an item-B note goes to item B, at that note.
    {
        Document d = v.doc;
        d.edits.notes[nA + 4].pitchOffset = -1.25;
        const auto docs = splitTrackView(v, d, pieces);
        CHECK(serialize(docs[0]) == serialize(pieces[0].doc), "item A untouched by an edit in item B");
        CHECK(docs[1].edits.notes[4].pitchOffset == -1.25, "edit lands on item B's note 4");
        CHECK(docs[1].edits.notes[2].pitchOffset == 0.5, "item B's earlier edit kept");
    }

    // Time edits keep notes inside their item: A's last note cannot move into the gap or item B.
    {
        EditSet e = v.doc.edits;
        const double applied = moveNotes(e, va, {(int)nA - 1}, 2.0);
        const auto t = noteTimes(va, e);
        std::printf("  A's last note asked +2 s, moved %+.3f s; ends %.3f s, A ends %.3f s\n", applied, t[nA - 1].outEnd,
                    va.segments[0].second);
        CHECK(t[nA - 1].outEnd <= va.segments[0].second + 1e-6, "A's last note stays inside item A");
        EditSet e2 = v.doc.edits;
        const double back = moveNotes(e2, va, {(int)nA}, -2.0);
        const auto t2 = noteTimes(va, e2);
        CHECK(t2[nA].outStart >= va.segments[1].first - 1e-6, "B's first note stays inside item B (moved %.3f)", back);
        // A move inside the item works and comes back to item B as its own time edit.
        EditSet e3 = v.doc.edits;
        const double ok = moveNotes(e3, va, {(int)nA + 3}, 0.02);
        EditSet alone = pieces[1].doc.edits; // the same move in item B on its own
        const double okAlone = moveNotes(alone, *effB, {3}, 0.02);
        std::printf("  note 3 of B asked +20 ms: in the view %+.1f ms, in the item alone %+.1f ms\n", ok * 1000, okAlone * 1000);
        CHECK(ok > 0 && std::abs(ok - okAlone) < 1e-6, "a move inside item B goes as far as in the item alone");
        Document d = v.doc;
        d.edits = e3;
        const auto docs = splitTrackView(v, d, pieces);
        CHECK(std::abs(docs[1].edits.notes[3].timeOffsetSec - e3.notes[nA + 3].timeOffsetSec) < 1e-12,
              "time edit lands on item B's note 3");
        CHECK(serialize(docs[0]) == serialize(pieces[0].doc), "item A untouched by a move in item B");
    }

    // Splitting a note of item B: only item B's layout changes; rebuilt, the view has one more note.
    {
        Document d = v.doc;
        const Note& n = va.notes[nA + 6];
        CHECK(splitNote(d, va, (int)nA + 6, (n.startFrame + n.endFrame) / 2), "split in the view");
        std::vector<bool> lay;
        const auto docs = splitTrackView(v, d, pieces, &lay);
        CHECK(lay.size() == 2 && !lay[0] && lay[1], "layout change reported for item B only");
        CHECK(docs[1].edits.spans.size() == nB + 1 && docs[1].edits.notes.size() == nB + 1, "item B has one more note");
        std::vector<TrackPiece> again = pieces;
        again[1].doc = docs[1];
        again[1].eff = std::make_shared<const Analysis>(withLayout(*effB, docs[1].edits.spans)); // effB is B's base analysis
        TrackView v2;
        CHECK(buildTrackView(again, 0, v2) && v2.analysis->notes.size() == nA + nB + 1, "rebuilt view shows the split");
    }

    // Track-wide setting changed in the view reaches both items; per-item ones stay.
    {
        Document d = v.doc;
        d.keyTonic = 2;
        d.keyMinor = true;
        d.edits.sibilantGainDb = -3;
        const auto docs = splitTrackView(v, d, pieces);
        CHECK(docs[0].keyTonic == 2 && docs[1].keyTonic == 2 && docs[0].keyMinor && docs[1].keyMinor, "key to both items");
        CHECK(docs[0].edits.sibilantGainDb == -3 && docs[1].edits.sibilantGainDb == -3, "sibilant level to both items");
        CHECK(docs[0].edits.breathGainDb == -6 && docs[1].edits.breathGainDb == 0, "breath level stays per item");
    }

    // Pencil drawn in item B's part lands in item B's own frames.
    {
        Document d = v.doc;
        const Note& n = va.notes[nA + 5];
        const int f0 = n.startFrame + 3;
        std::vector<float> midi(8, (float)(n.centerMidi + 1.0));
        drawPitch(d.edits, va, f0, midi);
        const auto docs = splitTrackView(v, d, pieces);
        const auto dense = drawnDense(docs[1].edits, (int)effB->frames.size());
        const int fb = f0 - v.maps[1].frameOffset;
        int drawn = 0;
        for (int k = 0; k < 8; ++k) drawn += std::isnan(dense[(size_t)(fb + k)]) ? 0 : 1;
        CHECK(drawn >= 6, "pencil in item B's frames (%d of 8)", drawn);
        CHECK(docs[0].edits.draw.empty(), "no pencil in item A");
    }

    // Overlapping items: the earlier one ends where the later starts; notes there are left out.
    {
        std::vector<TrackPiece> ov = pieces;
        ov[1].placeSec = lenA * 0.5 - 0.05; // B starts in the middle of A
        TrackView vo;
        CHECK(buildTrackView(ov, 0, vo), "overlapping items still build");
        if (vo.analysis != nullptr) {
            const auto& o = *vo.analysis;
            bool overlap = false;
            for (size_t j = 1; j < o.notes.size(); ++j) overlap = overlap || o.notes[j].startFrame < o.notes[j - 1].endFrame;
            CHECK(!overlap, "no overlapping notes");
            CHECK(vo.maps[0].frameEnd <= vo.maps[1].frameBegin, "item A cut where item B starts");
            CHECK(vo.maps[0].count < (int)nA, "A's notes past the cut are left out (%d of %zu shown)", vo.maps[0].count, nA);
            const auto docs = splitTrackView(vo, vo.doc, ov);
            CHECK(docs.size() == 2 && serialize(docs[0]) == serialize(ov[0].doc), "left-out notes keep item A unchanged");
        }
    }
}

int main()
{
    testSines();
    testSilenceAndNoise();
    testThreadAlign();
    testThreadFix();
    testTrackView();
    testTrackViewCut();
    testPins();
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
        testScaleModes();
        testBoundaryBlocks(s, a, sr);
        testNoteAtFrameZero(s, sr);
        testMoveNote(s, a, sr);
        testAttackKept(a);
        testTransitions(a);
        testPencil(s, a, sr);
        testConsonant(s, a, sr);
        testDocumentV2(a);
        testTilt(s, a, sr);
        testTiltLayout(a);
        testAutoVibrato(sr);
        testDocumentV4(a);
        testTransfer(a);
        testTimeFaults(a);
        testSeparations(a);
        testMoveCut(a);
        testSilentGaps(s, a, sr);
        testMute(s, a, sr);
        testDelete(s, a, sr);
    }
    std::printf(failures ? "\n%d FAILURE(S)\n" : "\nALL PASSED\n", failures);
    return failures ? 1 : 0;
}
