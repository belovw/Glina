#include "glina/Renderer.h"
#include "glina/Editing.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace glina {

namespace {

inline double dbToLin(double db) { return std::pow(10.0, db / 20.0); }

// Raised-cosine smoothing (two moving-average passes) inside [b, e).
void smoothRange(std::vector<float>& v, int b, int e, int half)
{
    if (half <= 0 || e - b < 3) return;
    std::vector<double> tmp(v.begin() + b, v.begin() + e);
    for (int pass = 0; pass < 2; ++pass) {
        const int n = (int)tmp.size();
        std::vector<double> pre(n + 1, 0.0);
        for (int i = 0; i < n; ++i) pre[i + 1] = pre[i] + tmp[i];
        for (int i = 0; i < n; ++i) {
            const int h = std::min({half, i, n - 1 - i});
            tmp[i] = (pre[i + h + 1] - pre[i - h]) / (2 * h + 1);
        }
    }
    for (int i = b; i < e; ++i) v[i] = (float)tmp[i - b];
}

inline double cubicAt(const float* x, size_t n, double t)
{
    const long i = (long)std::floor(t);
    const double f = t - i;
    auto s = [&](long k) -> double { return (k >= 0 && (size_t)k < n) ? x[k] : 0.0; };
    const double y0 = s(i - 1), y1 = s(i), y2 = s(i + 1), y3 = s(i + 2);
    if (f == 0.0) return y1;
    const double a = -0.5 * y0 + 1.5 * y1 - 1.5 * y2 + 0.5 * y3;
    const double b = y0 - 2.5 * y1 + 2.0 * y2 - 0.5 * y3;
    const double c = -0.5 * y0 + 0.5 * y2;
    return ((a * f + b) * f + c) * f + y1;
}

inline float ctrlAt(const std::vector<float>& v, double frame)
{
    if (v.empty()) return 0.f;
    const int nf = (int)v.size();
    const int k0 = std::clamp((int)std::floor(frame), 0, nf - 1);
    const int k1 = std::min(k0 + 1, nf - 1);
    const float w = (float)std::clamp(frame - k0, 0.0, 1.0);
    return v[k0] * (1 - w) + v[k1] * w;
}

const NoteEdit kNoEdit{};

// Deterministic random value in [-1, 1].
double hashRand(uint32_t seed, uint32_t a, uint32_t b)
{
    uint32_t h = seed * 2654435761u ^ (a * 0x9E3779B9u + 0x7F4A7C15u) ^ (b * 0x85EBCA6Bu);
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return (h / 4294967295.0) * 2.0 - 1.0;
}

// Slow smooth wander in [-1, 1]: random knots `step` seconds apart, raised-cosine
// interpolation (no steps, no jitter).
double wander(uint32_t seed, uint32_t note, uint32_t chan, double t, double step)
{
    const double x = std::max(0.0, t) / step;
    const uint32_t i = (uint32_t)x;
    const double f = x - i;
    const double w = 0.5 - 0.5 * std::cos(M_PI * f);
    const uint32_t c = note * 4 + chan;
    return hashRand(seed, c, i) * (1 - w) + hashRand(seed, c, i + 1) * w;
}

} // namespace

// ---------------------------------------------------------------------------
TimeMap TimeMap::build(const Analysis& a, const EditSet& e)
{
    TimeMap m;
    const double sr = a.sampleRate;
    const double minSilence = kMinSilenceSec * sr;
    m.in_.push_back(0);
    m.out_.push_back(0);
    m.noteOut_.reserve(a.notes.size());
    double lastIn = 0, lastOut = 0;
    for (size_t i = 0; i < a.notes.size(); ++i) {
        const NoteEdit& ed = i < e.notes.size() ? e.notes[i] : kNoEdit;
        const double stretch = ed.consonant ? 1.0 : ed.stretch;
        if (ed.timeOffsetSec != 0 || stretch != 1) m.identity_ = false;
        const double startIn = a.notes[i].startSec * sr;
        double ai = std::max(lastIn + 1, startIn);
        // A note keeps its own position unless moved; a stretched or moved
        // neighbour that would overlap pushes it later instead.
        double ao = ai + ed.timeOffsetSec * sr;
        ao = std::max(ao, lastOut + 0.1 * (ai - lastIn));
        // Further from the audio before it than in the recording: the pause (breath, consonant)
        // keeps its length just before the note and the rest is silence. Stretched, the pause
        // (or, between notes that touched, the one sample of their joint) would buzz.
        const double pauseIn = std::max(0.0, startIn - lastIn);
        if (ao - lastOut - pauseIn > minSilence) {
            const double s1 = ao - pauseIn;
            m.silence_.emplace_back(lastOut, s1);
            m.silenceIn_.push_back(lastIn);
            if (pauseIn > 0) {
                m.in_.push_back(lastIn);
                m.out_.push_back(s1);
            } else {
                ai = lastIn; // the notes touched: this one starts where the last one ended
            }
        }
        const double bi = std::max(ai + 1, a.notes[i].endSec * sr);
        double bo = ao + (bi - ai) * std::max(0.05, stretch);
        bo = std::max(bo, ao + 0.1 * (bi - ai));
        m.in_.push_back(ai); m.out_.push_back(ao);
        // Only the steady middle of a note is stretched: the attack and the
        // transition into the next note keep their timing, so transients stay crisp.
        const double din = bi - ai, dout = bo - ao;
        const double att = std::min(0.040 * sr, 0.30 * din), rel = std::min(0.030 * sr, 0.20 * din);
        if (std::abs(dout - din) > 1 && dout - att - rel > 0.15 * (din - att - rel)) {
            m.in_.push_back(ai + att); m.out_.push_back(ao + att);
            m.in_.push_back(bi - rel); m.out_.push_back(bo - rel);
        }
        m.in_.push_back(bi); m.out_.push_back(bo);
        m.noteOut_.emplace_back(ao, bo);
        lastIn = bi; lastOut = bo;
    }
    const double end = std::max((double)a.numSamples, lastIn + 1);
    // The clip keeps its length (in a DAW the item does not grow): the tail after
    // the last note absorbs any change, unless notes were pushed past the end. A tail
    // that would grow keeps its length after the last note and silence fills the rest.
    const double endOut = e.keepLength ? std::max(end, lastOut + 0.1 * (end - lastIn)) : lastOut + (end - lastIn);
    const double tailIn = end - lastIn;
    if (!a.notes.empty() && endOut - lastOut - tailIn > minSilence) {
        m.in_.push_back(end);
        m.out_.push_back(lastOut + tailIn);
        m.silence_.emplace_back(lastOut + tailIn, endOut);
        m.silenceIn_.push_back(end);
    }
    m.in_.push_back(end);
    m.out_.push_back(endOut);
    if (m.identity_) {
        m.in_ = {0, end};
        m.out_ = {0, end};
        m.silence_.clear();
        m.silenceIn_.clear();
    }
    return m;
}

// Where xs holds the same time twice (a silence: input time stands still), interpMap
// gives the later output and interpMapBefore the earlier one.
static double interpMap(const std::vector<double>& xs, const std::vector<double>& ys, double t)
{
    if (xs.size() < 2) return t;
    if (t < xs.front()) return ys.front() + (t - xs.front());
    if (t >= xs.back()) return ys.back() + (t - xs.back());
    const size_t j = std::upper_bound(xs.begin(), xs.end(), t) - xs.begin();
    const double x0 = xs[j - 1], x1 = xs[j], y0 = ys[j - 1], y1 = ys[j];
    return x1 > x0 ? y0 + (y1 - y0) * (t - x0) / (x1 - x0) : y0;
}

static double interpMapBefore(const std::vector<double>& xs, const std::vector<double>& ys, double t)
{
    if (xs.size() < 2) return t;
    if (t <= xs.front()) return ys.front() + (t - xs.front());
    if (t > xs.back()) return ys.back() + (t - xs.back());
    const size_t j = std::lower_bound(xs.begin(), xs.end(), t) - xs.begin(); // first x >= t (j >= 1)
    if (xs[j] == t) return ys[j];
    const double x0 = xs[j - 1], x1 = xs[j], y0 = ys[j - 1], y1 = ys[j];
    return x1 > x0 ? y0 + (y1 - y0) * (t - x0) / (x1 - x0) : y0;
}

double TimeMap::inToOut(double t) const { return identity_ ? t : interpMap(in_, out_, t); }
double TimeMap::inToOutBefore(double t) const { return identity_ ? t : interpMapBefore(in_, out_, t); }
double TimeMap::outToIn(double t) const { return identity_ ? t : interpMap(out_, in_, t); }

bool TimeMap::silenceBetween(double inA, double inB) const
{
    const auto it = std::upper_bound(silenceIn_.begin(), silenceIn_.end(), inA);
    return it != silenceIn_.end() && *it <= inB;
}

namespace {
// Level around one silence [s0, s1): fades out over `fade` before it, in after it.
double silenceEnv(double t, double s0, double s1, double fade)
{
    if (t >= s0 && t < s1) return 0.0;
    if (fade <= 0) return 1.0;
    if (t < s0 && t >= s0 - fade) return 0.5 + 0.5 * std::cos(M_PI * (t - (s0 - fade)) / fade);
    if (t >= s1 && t < s1 + fade) return 0.5 - 0.5 * std::cos(M_PI * (t - s1) / fade);
    return 1.0;
}
} // namespace

double TimeMap::gainAt(double t, double fade) const
{
    if (silence_.empty()) return 1.0;
    // The first silence whose fade-in is not over by t; then every one whose fade-out has begun.
    auto it = std::lower_bound(silence_.begin(), silence_.end(), t,
                               [&](const std::pair<double, double>& s, double x) { return s.second + fade <= x; });
    double g = 1.0;
    for (; it != silence_.end() && it->first - fade <= t; ++it) g *= silenceEnv(t, it->first, it->second, fade);
    return g;
}

// ---------------------------------------------------------------------------
// Auto vibrato of one note, on the OUTPUT timeline (a stretched note keeps its
// vibrato rate): envelope 0 until the delay, S-curve onset over `onsetPeriods`,
// fade-out over the last period. Phase is the integral of the (wandering) rate.
// Adds pitch (semitones, voiced frames) and level (dB). Returns false when the
// note is too short to get any vibrato.
static bool addAutoVibrato(const Analysis& a, const TimeMap& map, const AutoVibrato& v, uint32_t seed, int ni,
                           std::vector<float>* pitch, std::vector<float>* gain)
{
    const Note& nt = a.notes[(size_t)ni];
    const double sr = a.sampleRate;
    const double t0 = map.inToOut(nt.startFrame * (double)a.hop) / sr;
    double t1 = map.inToOut(nt.endFrame * (double)a.hop) / sr;
    // A silence opened at the note's end: the vibrato fades out before it, as at the clip's end.
    if (map.silenceBetween(nt.endSec * sr - 1, nt.endFrame * (double)a.hop))
        t1 = map.inToOutBefore(nt.endSec * sr) / sr + 0.5 * a.hop / sr;
    const double rate = std::clamp(v.rateHz, 0.5, 20.0);
    const double delay = std::max(0.0, v.delayMs / 1000.0);
    if (t1 - t0 <= delay) return false;
    if (!pitch) return true;
    const double onset = std::max(0.0, v.onsetPeriods) / rate;
    const double fadeOut = 1.0 / rate;
    const double var = std::clamp(v.variation / 100.0, 0.0, 1.0);
    const double depth = std::max(0.0, v.depthCents) / 100.0;
    const uint32_t note = (uint32_t)ni;
    double phase = M_PI * (1.0 + hashRand(seed, note, 0xFFFFu)); // own start phase per note and track
    double prevT = t0 + delay;
    const int nf = (int)a.frames.size();
    for (int k = std::max(0, nt.startFrame); k < std::min(nt.endFrame, nf); ++k) {
        const double t = map.inToOut(k * (double)a.hop) / sr;
        const double tn = t - t0;
        if (tn < delay) continue;
        const double r = rate * (1.0 + 0.10 * var * wander(seed, note, 1, tn, 0.7));
        phase += 2.0 * M_PI * r * (t - prevT);
        prevT = t;
        const double on = onset > 0 ? 0.5 - 0.5 * std::cos(M_PI * std::clamp((tn - delay) / onset, 0.0, 1.0)) : 1.0;
        const double off = 0.5 - 0.5 * std::cos(M_PI * std::clamp((t1 - t) / fadeOut, 0.0, 1.0));
        const double env = on * off;
        const double s = std::sin(phase);
        if (a.frames[(size_t)k].f0 > 0)
            (*pitch)[(size_t)k] += (float)(env * depth * (1.0 + 0.30 * var * wander(seed, note, 2, tn, 0.9)) * s);
        (*gain)[(size_t)k] += (float)(env * v.tremoloDb * s);
    }
    return true;
}

Controls buildControls(const Analysis& a, const EditSet& e)
{
    const int nf = (int)a.frames.size();
    const double hopMs = 1000.0 * a.hop / a.sampleRate;
    Controls c;
    c.pitchSemis.assign(nf, 0.f);
    c.formantSemis.assign(nf, 0.f);
    c.gainDb.assign(nf, 0.f);

    std::vector<float> offset(nf, 0.f), shape(nf, 0.f), formant(nf, (float)e.globalFormantShift);
    const double taper = std::max(1.0, e.edgeTaperMs / hopMs);

    // Auto vibrato: which notes get it (long enough, not consonants). Those lose
    // their own vibrato (it is replaced, not added).
    std::vector<char> autoOn(a.notes.size(), 0);
    TimeMap vibMap;
    bool anyVib = false;
    for (size_t i = 0; i < a.notes.size() && !anyVib; ++i)
        anyVib = vibratoOf(e, (int)i).active() && !(i < e.notes.size() && e.notes[i].consonant);
    if (anyVib) {
        vibMap = TimeMap::build(a, e);
        for (size_t i = 0; i < a.notes.size(); ++i) {
            const AutoVibrato& v = vibratoOf(e, (int)i);
            if (!v.active() || (i < e.notes.size() && e.notes[i].consonant)) continue;
            autoOn[i] = addAutoVibrato(a, vibMap, v, e.vibratoSeed, (int)i, nullptr, nullptr) ? 1 : 0;
        }
    }

    for (int k = 0; k < nf; ++k) {
        const Frame& f = a.frames[k];
        const int ni = a.noteOfFrame.empty() ? -1 : a.noteOfFrame[k];
        if (f.kind == FrameKind::Sibilant) c.gainDb[k] = (float)e.sibilantGainDb;
        if (f.kind == FrameKind::Breath) c.gainDb[k] = (float)e.breathGainDb;
        if (ni < 0) {
            if (f.kind == FrameKind::Voiced) c.gainDb[k] = (float)e.unassignedGainDb;
            continue;
        }
        const NoteEdit& ed = ni < (int)e.notes.size() ? e.notes[ni] : kNoEdit;
        const Note& nt = a.notes[ni];
        // Drift/vibrato edits fade in over the note edges so natural glides survive.
        const double edge = std::min(k - nt.startFrame + 0.5, nt.endFrame - k - 0.5);
        const double t = std::clamp(edge / taper, 0.0, 1.0);
        const double w = 0.5 - 0.5 * std::cos(M_PI * t);
        const double drift = 1.0 + (ed.driftAmount - 1.0) * w;
        const bool replaced = autoOn[(size_t)ni] && vibratoOf(e, ni).depthCents > 0;
        const double vib = 1.0 + ((replaced ? 0.0 : ed.vibratoAmount) - 1.0) * w;
        // Scaling drift/vibrato around their own mean keeps the perceived centre where it was.
        if (!ed.consonant) { // consonants are passed through with their own pitch
            offset[k] = (float)(ed.pitchOffset + tiltAt(nt, ed, k));
            shape[k] = (float)((drift - 1.0) * (a.slowDev[k] - nt.meanSlow) + (vib - 1.0) * (a.fastDev[k] - nt.meanFast));
        }
        formant[k] += (float)ed.formantShift;
        c.gainDb[k] += (float)ed.gainDb;
    }

    // Pitch-offset transitions between adjacent notes within a voiced run: a
    // raised-cosine ramp centred on the boundary, as wide as the later note's
    // smoothness asks (0 = instant step ... 1 = long glide), never wider than
    // half of either neighbouring note. Formant changes use a fixed short blend.
    const int half = std::max(1, (int)std::lround(e.transitionMs / hopMs / 4.0));
    const std::vector<float> raw = offset;
    auto noteAt = [&](int k) { return a.noteOfFrame.empty() ? -1 : a.noteOfFrame[(size_t)k]; };
    for (int k = 0; k < nf;) {
        if (a.frames[k].f0 <= 0) { ++k; continue; }
        int en = k;
        while (en < nf && a.frames[en].f0 > 0) ++en;
        for (int b = k + 1; b < en; ++b) {
            if (noteAt(b) == noteAt(b - 1)) continue;
            int segL = b - 1, segR = b;
            while (segL - 1 >= k && noteAt(segL - 1) == noteAt(b - 1)) --segL;
            while (segR + 1 < en && noteAt(segR + 1) == noteAt(b)) ++segR;
            const int r = noteAt(b) >= 0 ? noteAt(b) : noteAt(b - 1);
            const double smooth = (r >= 0 && r < (int)e.notes.size()) ? e.notes[(size_t)r].transition : 0.5;
            const double widthFrames = transitionWidthMs(smooth, e.transitionMs) / hopMs;
            const int h = (int)std::floor(std::min({0.5 * widthFrames, 0.5 * (b - segL), 0.5 * (segR + 1 - b)}));
            if (h <= 0) continue; // hard step
            const float vL = raw[(size_t)b - 1], vR = raw[(size_t)b];
            // Tilted notes: blend between the two lines extended across the boundary
            // (slope 0 without tilt, so untilted notes are unchanged).
            auto slope = [&](int ni) {
                if (ni < 0 || ni >= (int)e.notes.size()) return 0.0;
                const NoteEdit& ed = e.notes[(size_t)ni];
                if (ed.consonant || ed.tiltL == ed.tiltR) return 0.0;
                return (ed.tiltR - ed.tiltL) / std::max(1, a.notes[(size_t)ni].endFrame - a.notes[(size_t)ni].startFrame);
            };
            const double sL = slope(noteAt(b - 1)), sR = slope(noteAt(b));
            for (int j = b - h; j < b + h; ++j) {
                const double t = (j - (b - h) + 0.5) / (2.0 * h);
                const double S = 0.5 - 0.5 * std::cos(M_PI * t);
                if (sL == 0 && sR == 0) { offset[(size_t)j] = (float)(vL + (vR - vL) * S); continue; }
                const double lineL = vL + sL * (j - (b - 1)), lineR = vR + sR * (j - b);
                offset[(size_t)j] = (float)(lineL + (lineR - lineL) * S);
            }
        }
        smoothRange(formant, k, en, half);
        k = en;
    }
    smoothRange(c.gainDb, 0, nf, std::max(1, (int)std::lround(8.0 / hopMs)));

    for (int k = 0; k < nf; ++k) {
        if (a.frames[k].f0 > 0) {
            c.pitchSemis[k] = offset[k] + shape[k];
            c.formantSemis[k] = formant[k];
        }
    }

    // Transition shape between two touching notes. Smoothness 0.5 keeps the sung
    // glide. Away from 0.5 the glide is replaced by a raised-cosine move between
    // the two (edited) note centres: narrower towards 0 (a clean step), wider
    // towards 1 (a long glide). The replacement fades in as smoothness leaves 0.5
    // (fully at 0.25 / 0.75), so the control has no jump around "natural".
    {
        const double natHalf = 40.0 / hopMs; // a sung glide usually sits within ±40 ms
        const double fade = 30.0 / hopMs;
        for (size_t r = 1; r < a.notes.size(); ++r) {
            const Note& L = a.notes[r - 1];
            const Note& Rn = a.notes[r];
            if (Rn.startFrame - L.endFrame > 1) continue; // not touching: no glide between them
            const NoteEdit& eL = r - 1 < e.notes.size() ? e.notes[r - 1] : kNoEdit;
            const NoteEdit& eR = r < e.notes.size() ? e.notes[r] : kNoEdit;
            if (eL.consonant || eR.consonant) continue;
            const double amount = std::min(1.0, std::abs(eR.transition - 0.5) * 4.0);
            if (amount < 1e-6) continue;
            const int b = Rn.startFrame;
            const double limL = 0.5 * (b - L.startFrame), limR = 0.5 * (Rn.endFrame - b);
            const double half = std::min({0.5 * transitionWidthMs(eR.transition, e.transitionMs) / hopMs, limL, limR});
            const double inner = std::max(half, natHalf);
            // From the (tilted) end of the left note to the (tilted) start of the right one.
            const double lvL = L.centerMidi + eL.pitchOffset + tiltAt(L, eL, L.endFrame - 1);
            const double lvR = Rn.centerMidi + eR.pitchOffset + tiltAt(Rn, eR, Rn.startFrame);
            const int k0 = std::max(L.startFrame, b - (int)std::ceil(std::min(inner + fade, limL)));
            const int k1 = std::min(Rn.endFrame, b + (int)std::ceil(std::min(inner + fade, limR)));
            for (int k = k0; k < k1; ++k) {
                if (k < 0 || k >= nf || a.frames[k].f0 <= 0) continue;
                const double x = k + 0.5 - b; // frames from the boundary
                const double S = half < 0.5 ? (x < 0 ? 0.0 : 1.0)
                               : x <= -half ? 0.0 : x >= half ? 1.0 : 0.5 - 0.5 * std::cos(M_PI * (x + half) / (2.0 * half));
                const double ideal = lvL + (lvR - lvL) * S;
                const double outer = std::min(inner + fade, x < 0 ? limL : limR);
                const double in = std::max(0.0, outer - fade);
                const double dist = std::abs(x);
                const double m = dist <= in ? 1.0 : dist >= outer ? 0.0 : 0.5 + 0.5 * std::cos(M_PI * (dist - in) / (outer - in));
                const double now = a.frames[k].midi + c.pitchSemis[k];
                c.pitchSemis[k] = (float)(c.pitchSemis[k] + amount * m * (ideal - now));
            }
        }
    }

    // Auto vibrato rides on the line (after the transitions, under the pencil).
    if (anyVib)
        for (size_t i = 0; i < a.notes.size(); ++i)
            if (autoOn[i]) addAutoVibrato(a, vibMap, vibratoOf(e, (int)i), e.vibratoSeed, (int)i, &c.pitchSemis, &c.gainDb);

    // Pencil: drawn pitch replaces the computed curve, blended in over ~30 ms at
    // the ends of each drawn stretch so it joins the rest without a jump.
    for (const DrawRun& r : e.draw) {
        const int len = (int)r.delta.size();
        const double blend = std::max(1.0, 30.0 / hopMs);
        for (int q = 0; q < len; ++q) {
            const int k = r.start + q;
            if (k < 0 || k >= nf || a.frames[k].f0 <= 0 || std::isnan(r.delta[(size_t)q])) continue;
            const int ni = noteAt(k);
            if (ni >= 0 && ni < (int)e.notes.size() && e.notes[(size_t)ni].consonant) continue;
            const double noteOff = (ni >= 0 && ni < (int)e.notes.size())
                                       ? e.notes[(size_t)ni].pitchOffset + tiltAt(a.notes[(size_t)ni], e.notes[(size_t)ni], k)
                                       : 0.0;
            const double drawn = noteOff + r.delta[(size_t)q];
            const double edgeDist = std::min(q, len - 1 - q) + 1.0;
            const double w = edgeDist >= blend ? 1.0 : 0.5 - 0.5 * std::cos(M_PI * edgeDist / blend);
            c.pitchSemis[k] = (float)((1.0 - w) * c.pitchSemis[k] + w * drawn);
        }
    }
    return c;
}

std::vector<double> editedPitchCurve(const Analysis& a, const EditSet& e)
{
    const Controls c = buildControls(a, e);
    std::vector<double> out(a.frames.size(), 0.0);
    for (size_t k = 0; k < a.frames.size(); ++k)
        if (a.frames[k].f0 > 0) out[k] = a.frames[k].midi + c.pitchSemis[k];
    return out;
}

// ---------------------------------------------------------------------------
std::vector<float> render(const float* x, size_t n, const Analysis& a, const EditSet& e)
{
    const auto& M = a.marks;
    if (M.empty() || n == 0) return std::vector<float>(x, x + n);

    const Controls c = buildControls(a, e);
    const TimeMap map = TimeMap::build(a, e);
    const double outLen = map.inToOut((double)n);
    const size_t outN = (size_t)std::max(0.0, std::ceil(outLen));
    std::vector<double> acc(outN + 4096, 0.0);
    // Frames of deleted notes: their own grains are left out (a neighbour stretched over them still sounds).
    std::vector<char> deletedFrame(a.frames.size(), 0);
    for (size_t i = 0; i < e.notes.size() && i < a.notes.size(); ++i)
        if (e.notes[i].deleted)
            for (int f = std::max(0, a.notes[i].startFrame); f < std::min(a.notes[i].endFrame, (int)deletedFrame.size()); ++f) deletedFrame[(size_t)f] = 1;

    uint32_t rng = 0x9E3779B9u;
    size_t i = 0;
    long lastGrain = -1;
    double ts = map.inToOut(M[0].pos);

    while (ts < outLen) {
        const double tau = map.outToIn(ts);
        while (i + 1 < M.size() && std::abs(M[i + 1].pos - tau) <= std::abs(M[i].pos - tau)) ++i;

        size_t g = i;
        // Time-stretched noise: avoid repeating the very same unvoiced grain (buzz).
        if (!M[g].voiced && (long)g == lastGrain) {
            rng = rng * 1664525u + 1013904223u;
            const size_t alt = (rng >> 16) & 1 ? g + 1 : (g > 0 ? g - 1 : g + 1);
            if (alt < M.size() && !M[alt].voiced) g = alt;
        }
        lastGrain = (long)g;
        const PitchMark& mk = M[g];

        double halfR = g + 1 < M.size() ? M[g + 1].pos - mk.pos : 0.0;
        double halfL = g > 0 ? mk.pos - M[g - 1].pos : halfR;
        if (halfR <= 0) halfR = halfL;
        if (halfL <= 0 || halfR <= 0) { ts += 1.0; continue; }
        const double maxHalf = 0.03 * a.sampleRate;
        halfL = std::min(halfL, maxHalf);
        halfR = std::min(halfR, maxHalf);
        {
            const size_t fr = (size_t)std::max(0.0, mk.pos / a.hop);
            if (fr < deletedFrame.size() && deletedFrame[fr]) { ts += halfR; continue; }
        }

        const double frame = mk.pos / a.hop;
        const bool voiced = mk.voiced;
        const double beta = voiced ? std::pow(2.0, ctrlAt(c.pitchSemis, frame) / 12.0) : 1.0;
        const double gamma = voiced ? std::pow(2.0, ctrlAt(c.formantSemis, frame) / 12.0) : 1.0;
        double gain = dbToLin(ctrlAt(c.gainDb, frame));
        if (voiced) gain *= std::sqrt(gamma / beta); // keep loudness when pulse rate / grain width change

        const long o0 = (long)std::ceil(ts - halfL / gamma);
        const long o1 = (long)std::floor(ts + halfR / gamma);
        for (long o = std::max(0L, o0); o <= o1 && (size_t)o < acc.size(); ++o) {
            const double u = (o - ts) * gamma;
            double w;
            if (u < 0) w = 0.5 - 0.5 * std::cos(M_PI * std::clamp(1.0 + u / halfL, 0.0, 1.0));
            else       w = 0.5 + 0.5 * std::cos(M_PI * std::clamp(u / halfR, 0.0, 1.0));
            if (w <= 0) continue;
            acc[o] += gain * w * cubicAt(x, n, mk.pos + u);
        }

        ts += voiced ? halfR / beta : halfR;
    }

    // Gaps opened between notes: silence, with short fades in the audio on either side.
    const double fade = kSilenceFadeSec * a.sampleRate;
    for (const auto& [s0, s1] : map.silences()) {
        const long b = std::max(0L, (long)std::floor(s0 - fade));
        const long e = std::min((long)acc.size(), (long)std::ceil(s1 + fade));
        for (long o = b; o < e; ++o) acc[(size_t)o] *= silenceEnv((double)o, s0, s1, fade);
    }
    // Muted consonants: the output gap in front of a note (from the end of the note before it).
    for (size_t i = 0; i < e.notes.size() && i < map.noteCount(); ++i) {
        if (!e.notes[i].consMuted) continue;
        const double s0 = i > 0 ? map.noteEndOut(i - 1) : 0.0, s1 = map.noteStartOut(i);
        if (s1 <= s0) continue;
        const long b = std::max(0L, (long)std::floor(s0 - fade));
        const long end = std::min((long)acc.size(), (long)std::ceil(s1 + fade));
        for (long o = b; o < end; ++o) acc[(size_t)o] *= silenceEnv((double)o, s0, s1, fade);
    }
    // Deleted breaths: the Breath frames in the unvoiced sound before a note, each run silent.
    for (size_t i = 0; i < e.notes.size() && i < map.noteCount() && i < a.notes.size(); ++i) {
        if (!e.notes[i].breathDeleted) continue;
        const size_t from = i > 0 ? (size_t)std::max(0, a.notes[i - 1].endFrame) : 0;
        const size_t to = std::min((size_t)std::max(0, a.notes[i].startFrame), a.frames.size());
        for (size_t f = from; f < to;) {
            if (a.frames[f].kind != FrameKind::Breath) { ++f; continue; }
            size_t g = f;
            while (g < to && a.frames[g].kind == FrameKind::Breath) ++g;
            const double s0 = map.inToOut((double)f * a.hop), s1 = map.inToOut((double)g * a.hop);
            const long b = std::max(0L, (long)std::floor(s0 - fade));
            const long end = std::min((long)acc.size(), (long)std::ceil(s1 + fade));
            if (s1 > s0) for (long o = b; o < end; ++o) acc[(size_t)o] *= silenceEnv((double)o, s0, s1, fade);
            f = g;
        }
    }
    // Muted notes: silent over their whole output span (grains of the neighbours that reach
    // into it are removed too), so the mute is complete.
    for (size_t i = 0; i < e.notes.size() && i < map.noteCount(); ++i) {
        if (!e.notes[i].muted) continue;
        const double s0 = map.noteStartOut(i), s1 = map.noteEndOut(i);
        const long b = std::max(0L, (long)std::floor(s0 - fade));
        const long end = std::min((long)acc.size(), (long)std::ceil(s1 + fade));
        for (long o = b; o < end; ++o) acc[(size_t)o] *= silenceEnv((double)o, s0, s1, fade);
    }

    std::vector<float> out(outN);
    for (size_t k = 0; k < outN; ++k) out[k] = (float)acc[k];
    return out;
}

} // namespace glina
