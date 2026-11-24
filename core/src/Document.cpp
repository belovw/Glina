#include "glina/Document.h"
#include "glina/Editing.h"
#include "glina/Notes.h"
#include "glina/Renderer.h"
#include "glina/Tuning.h"

#include <algorithm>
#include <cmath>
#include <future>
#include <iomanip>
#include <locale>
#include <sstream>

namespace glina {

Key Document::key(const Analysis& a) const
{
    Key k = a.key;
    if (keyTonic >= 0) k.tonic = keyTonic % 12;
    if (keyMode >= 0) {
        // 0 major, 1 natural minor, 2 melodic, 3 harmonic, 4 pure major, 5 pure minor
        k.just = keyMode >= 4;
        k.minor = keyMode == 1 || keyMode == 2 || keyMode == 3 || keyMode == 5;
        k.minorForm = keyMode == 2 ? 1 : keyMode == 3 ? 2 : 0;
    } else if (keyTonic >= 0) {
        k.minor = keyMinor;
        k.minorForm = 0;
    }
    return k;
}

double Document::target(double midi, const Analysis& a) const
{
    return nearestTarget(midi, key(a), !chromatic);
}

bool Document::operator==(const Document& o) const
{
    return serialize(*this) == serialize(o);
}

void conform(Document& d, const Analysis& eff)
{
    const size_t n = eff.notes.size();
    d.edits.notes.resize(n);
    for (auto& v : d.voices)
        if (!v.noteMask.empty()) v.noteMask.resize(n, 0);
}

bool splitNote(Document& d, const Analysis& eff, int i, int frame)
{
    if (i < 0 || i >= (int)eff.notes.size()) return false;
    const Note& n = eff.notes[(size_t)i];
    if (frame - n.startFrame < 4 || n.endFrame - frame < 4) return false; // halves of at least ~20 ms
    // Timing: the note stays where it is. The left half keeps the note's start and stretch; the
    // right half starts where the left one ends. (With the same time offset, the right half of a
    // stretched note would start elsewhere and overlap its neighbour.) A note squeezed so short
    // that a half would fall under the minimal length is not split.
    const NoteTimes was = noteTimes(eff, d.edits)[(size_t)i];
    const double u = (double)(frame - n.startFrame) / std::max(1, n.endFrame - n.startFrame);
    const double splitSec = was.inStart + (was.inEnd - was.inStart) * u;
    const double splitOut = was.outStart + (was.outEnd - was.outStart) * u;
    const NoteTimes left{was.inStart, splitSec, was.outStart, splitOut}, right{splitSec, was.inEnd, splitOut, was.outEnd};
    if (left.outEnd - left.outStart < minNoteLength(left) - 1e-9 || right.outEnd - right.outStart < minNoteLength(right) - 1e-9)
        return false;
    conform(d, eff);
    if (d.edits.spans.empty()) d.edits.spans = spansOf(eff);
    if (d.edits.spans.size() != eff.notes.size()) return false;
    d.edits.spans[(size_t)i].end = frame;
    d.edits.spans.insert(d.edits.spans.begin() + i + 1, NoteSpan{frame, n.endFrame});
    NoteEdit copy = d.edits.notes[(size_t)i];
    // A tilt is split where it is: each half keeps its piece of the same line.
    const double mid = copy.tiltL + (copy.tiltR - copy.tiltL) * u;
    d.edits.notes[(size_t)i].tiltR = mid;
    copy.tiltL = mid;
    copy.sepBefore = NoteEdit::Sep::Auto; // the halves touch: soft
    setNoteTimes(copy, right);
    d.edits.notes.insert(d.edits.notes.begin() + i + 1, copy);
    for (auto& v : d.voices)
        if (!v.noteMask.empty()) v.noteMask.insert(v.noteMask.begin() + i + 1, v.noteMask[(size_t)i]);
    return true;
}

bool moveCut(Document& d, const Analysis& eff, int note, bool startEdge, double outSec)
{
    const int n = (int)eff.notes.size();
    if (note < 0 || note >= n) return false;
    if (d.edits.notes.size() > eff.notes.size()) return false;
    const std::vector<NoteTimes> t = noteTimes(eff, d.edits);
    std::vector<NoteSpan> spans = d.edits.spans.empty() ? spansOf(eff) : d.edits.spans;
    if (spans.size() != eff.notes.size()) return false;
    const int L = startEdge ? note - 1 : note, R = startEdge ? note : note + 1; // the notes left and right of the cut
    const bool hasL = L >= 0, hasR = R < n;
    // In a track view the audio of one item never moves into another: the cut stays in the note's item.
    const double hopSec = (double)eff.hop / eff.sampleRate;
    auto segOf = [&](int k) {
        const Note& m = eff.notes[(size_t)k];
        const double mid = 0.5 * (m.startSec + m.endSec);
        for (size_t s = 0; s < eff.segments.size(); ++s)
            if (mid >= eff.segments[s].first && mid < eff.segments[s].second) return (int)s;
        return -1;
    };
    const bool shared = hasL && hasR && spans[(size_t)L].end == spans[(size_t)R].start && // they touch in the source
                        segOf(L) == segOf(R);                                                // and in the same item
    constexpr int kMinFrames = 4; // ~20 ms, as for a split

    // Source frame <-> seconds, and output position of a source second under a note's mapping
    // (also past its edges: an edge moved into a gap carries the note's stretch along).
    auto frameSec = [&](int k, int f) {
        const Note& m = eff.notes[(size_t)k];
        return m.startSec + (double)(f - m.startFrame) * (m.endSec - m.startSec) / std::max(1, m.endFrame - m.startFrame);
    };
    auto secFrame = [&](int k, double sec) {
        const Note& m = eff.notes[(size_t)k];
        return m.startFrame + (sec - m.startSec) * std::max(1, m.endFrame - m.startFrame) / std::max(1e-9, m.endSec - m.startSec);
    };
    auto outOf = [&](int k, double sec) {
        const NoteTimes& m = t[(size_t)k];
        return m.outStart + (sec - m.inStart) * (m.outEnd - m.outStart) / std::max(1e-9, m.inEnd - m.inStart);
    };
    auto inOf = [&](int k, double out) {
        const NoteTimes& m = t[(size_t)k];
        return m.inStart + (out - m.outStart) * (m.inEnd - m.inStart) / std::max(1e-9, m.outEnd - m.outStart);
    };

    // Moved into deleted notes next to it, the cut runs through them, up to the next live note:
    // the note takes their audio back (it sounds again, as a part of this note). Deleted notes
    // the cut passes are gone; one it stops inside keeps the rest of its audio, still deleted.
    auto deletedAt = [&](int k) { return k >= 0 && k < n && k < (int)d.edits.notes.size() && d.edits.notes[(size_t)k].deleted; };
    {
        const int old = startEdge ? spans[(size_t)note].start : spans[(size_t)note].end;
        int f = (int)std::lround(secFrame(note, inOf(note, outSec)));
        const int dir = startEdge ? -1 : 1;
        if (deletedAt(note + dir) && (f - old) * dir > 0) {
            int live = note + dir;
            while (deletedAt(live)) live += dir;
            int lo = 0, hi = (int)eff.frames.size();
            if (startEdge) {
                if (live >= 0) lo = spans[(size_t)live].end;
                hi = old - 1;
            } else {
                if (live < n) hi = spans[(size_t)live].start;
                lo = old + 1;
            }
            if (const int sg = segOf(note); sg >= 0) {
                lo = std::max(lo, (int)std::lround(eff.segments[(size_t)sg].first / hopSec));
                hi = std::min(hi, (int)std::lround(eff.segments[(size_t)sg].second / hopSec));
            }
            if (lo > hi) return false;
            f = std::clamp(f, lo, hi);
            // A deleted note left with less than kMinFrames is taken whole.
            for (int k = note + dir; k != live; k += dir) {
                const NoteSpan& sp = spans[(size_t)k];
                if (startEdge ? (f > sp.start && f < sp.end && f - sp.start < kMinFrames) : (f > sp.start && f < sp.end && sp.end - f < kMinFrames))
                    f = startEdge ? sp.start : sp.end;
            }
            if (f == old) return false;
            std::vector<NoteTimes> u = t;
            std::vector<NoteSpan> s2 = spans;
            const double sec = frameSec(note, f);
            if (startEdge) { s2[(size_t)note].start = f; u[(size_t)note].inStart = sec; u[(size_t)note].outStart = outOf(note, sec); }
            else { s2[(size_t)note].end = f; u[(size_t)note].inEnd = sec; u[(size_t)note].outEnd = outOf(note, sec); }
            std::vector<int> gone;
            for (int k = note + dir; k != live; k += dir) {
                NoteSpan& sp = s2[(size_t)k];
                if (startEdge ? sp.start >= f : sp.end <= f) { gone.push_back(k); continue; }
                const bool cut = startEdge ? sp.end > f : sp.start < f;
                if (!cut) continue;
                const double ks = frameSec(k, f);
                if (startEdge) { sp.end = f; u[(size_t)k].inEnd = ks; u[(size_t)k].outEnd = outOf(k, ks); }
                else { sp.start = f; u[(size_t)k].inStart = ks; u[(size_t)k].outStart = outOf(k, ks); }
            }
            if (!noNewLayoutFaults(eff, d.edits, t, u)) return false;
            conform(d, eff);
            d.edits.spans = s2;
            for (int k = 0; k < n; ++k)
                if (k == note || (deletedAt(k) && std::find(gone.begin(), gone.end(), k) == gone.end()))
                    if (u[(size_t)k].inStart != t[(size_t)k].inStart || u[(size_t)k].inEnd != t[(size_t)k].inEnd)
                        setNoteTimes(d.edits.notes[(size_t)k], u[(size_t)k]);
            std::sort(gone.begin(), gone.end());
            for (auto it = gone.rbegin(); it != gone.rend(); ++it) {
                const int k = *it;
                d.edits.spans.erase(d.edits.spans.begin() + k);
                d.edits.notes.erase(d.edits.notes.begin() + k);
                for (auto& v : d.voices)
                    if (!v.noteMask.empty() && k < (int)v.noteMask.size()) v.noteMask.erase(v.noteMask.begin() + k);
            }
            return true;
        }
    }

    // Which note's mapping the new cut falls under: the note that now plays that moment;
    // with no shared cut, the moved note's own (extended past its edge if need be).
    const double cutOut = startEdge ? t[(size_t)note].outStart : t[(size_t)note].outEnd;
    // Notes that touched in the recording but were pulled apart (a hard separation): the dragged
    // edge follows the pointer and each note keeps its own place, so the gap stays.
    const bool apart = shared && std::abs(t[(size_t)R].outStart - t[(size_t)L].outEnd) > 1e-4;
    int owner = note;
    if (shared && !apart) owner = outSec < cutOut ? L : R;
    int f = (int)std::lround(secFrame(owner, inOf(owner, outSec)));
    // Limits: every note keeps kMinFrames; an edge stops at the neighbour's span.
    int lo = 0, hi = (int)eff.frames.size();
    if (hasL) lo = std::max(lo, spans[(size_t)L].start + kMinFrames);
    if (hasR) hi = std::min(hi, spans[(size_t)R].end - kMinFrames);
    if (!shared) {
        if (startEdge && hasL) lo = std::max(lo, spans[(size_t)L].end);
        if (!startEdge && hasR) hi = std::min(hi, spans[(size_t)R].start);
        if (startEdge) hi = std::min(hi, spans[(size_t)note].end - kMinFrames);
        else lo = std::max(lo, spans[(size_t)note].start + kMinFrames);
    }
    if (const int sg = segOf(note); sg >= 0) {
        lo = std::max(lo, (int)std::lround(eff.segments[(size_t)sg].first / hopSec));
        hi = std::min(hi, (int)std::lround(eff.segments[(size_t)sg].second / hopSec));
    }
    if (lo > hi) return false;
    f = std::clamp(f, lo, hi);
    const int old = startEdge ? spans[(size_t)note].start : spans[(size_t)note].end;
    if (f == old) return false;
    // The owner by frame (exact): who holds the audio at the new cut now.
    if (shared && !apart) owner = f < old ? L : R;
    const double sec = frameSec(owner, f);
    const double out = outOf(owner, sec);

    std::vector<NoteTimes> u = t;
    std::vector<NoteSpan> s2 = spans;
    auto setEdge = [&](int k, bool start) {
        const double o = apart ? outOf(k, sec) : out;
        if (start) { s2[(size_t)k].start = f; u[(size_t)k].inStart = sec; u[(size_t)k].outStart = o; }
        else { s2[(size_t)k].end = f; u[(size_t)k].inEnd = sec; u[(size_t)k].outEnd = o; }
    };
    if (shared) { setEdge(L, false); setEdge(R, true); }
    else setEdge(note, startEdge);
    if (!noNewLayoutFaults(eff, t, u)) return false;

    conform(d, eff);
    d.edits.spans = s2;
    for (int k : {L, R})
        if (k >= 0 && k < n) setNoteTimes(d.edits.notes[(size_t)k], u[(size_t)k]);
    return true;
}

bool joinNotes(Document& d, const Analysis& eff, int i)
{
    if (i < 0 || i + 1 >= (int)eff.notes.size()) return false;
    const Note& a = eff.notes[(size_t)i];
    const Note& b = eff.notes[(size_t)i + 1];
    const int maxGap = std::max(1, (int)std::lround(0.040 * eff.sampleRate / eff.hop));
    if (b.startFrame - a.endFrame > maxGap) return false;
    conform(d, eff);
    if (d.edits.spans.empty()) d.edits.spans = spansOf(eff);
    if (d.edits.spans.size() != eff.notes.size()) return false;
    const std::vector<NoteTimes> t = noteTimes(eff, d.edits);
    const NoteTimes& L = t[(size_t)i];
    const NoteTimes& R = t[(size_t)i + 1];
    // Timing: the joined note covers both, from the left note's start to the right note's end.
    // (With the left note's offset and stretch alone it would end elsewhere and overlap.)
    // Two notes squeezed so hard that the joined one would fall under the minimal length (the
    // pause between them in the recording counts too) are not joined.
    const NoteTimes joined{L.inStart, R.inEnd, L.outStart, std::max(L.outEnd, R.outEnd)};
    if (joined.outEnd - joined.outStart < minNoteLength(joined) - 1e-9) return false;
    d.edits.spans[(size_t)i].end = d.edits.spans[(size_t)i + 1].end;
    d.edits.spans.erase(d.edits.spans.begin() + i + 1);
    d.edits.notes[(size_t)i].tiltR = d.edits.notes[(size_t)i + 1].tiltR; // the joined note ends where the right one did
    setNoteTimes(d.edits.notes[(size_t)i], joined);
    d.edits.notes.erase(d.edits.notes.begin() + i + 1);
    for (auto& v : d.voices)
        if (!v.noteMask.empty()) {
            v.noteMask[(size_t)i] = (uint8_t)(v.noteMask[(size_t)i] | v.noteMask[(size_t)i + 1]);
            v.noteMask.erase(v.noteMask.begin() + i + 1);
        }
    return true;
}

// ---- serialisation ------------------------------------------------------------
// Plain text, one record per line, locale-independent. Version 1.
std::string serialize(const Document& d)
{
    std::ostringstream o;
    o.imbue(std::locale::classic());
    o << std::setprecision(10);
    const EditSet& e = d.edits;
    // Version 4 (tilt, auto vibrato) only when they are used: a document without
    // them is written exactly as before, so render keys of existing projects stay.
    bool v4 = !(e.autoVibrato == AutoVibrato{}) || e.vibratoSeed != 1;
    for (const auto& n : e.notes) v4 = v4 || n.tiltL != 0 || n.tiltR != 0 || n.ownVibrato;
    o << (v4 ? "glina-document 4\n" : "glina-document 3\n");
    o << "key " << d.keyTonic << ' ' << (int)d.keyMinor << ' ' << (int)d.chromatic << '\n';
    // Written only when used: a document without them is written exactly as before.
    if (d.keyMode != -1 || d.snapAmount != 1.0) o << "mode " << d.keyMode << ' ' << d.snapAmount << '\n';
    o << "lead " << d.leadGainDb << '\n';
    if (!d.leadEnabled || d.leadPan != 0) o << "leadmix " << (int)d.leadEnabled << ' ' << d.leadPan << '\n';
    o << "tempo " << d.tempoBpm << ' ' << d.beatsPerBar << ' ' << d.barOffsetSec << '\n';
    o << "global " << e.sibilantGainDb << ' ' << e.breathGainDb << ' ' << e.globalFormantShift << ' '
      << e.transitionMs << ' ' << e.edgeTaperMs << '\n';
    o << "spans " << e.spans.size();
    for (const auto& s : e.spans) o << ' ' << s.start << ' ' << s.end;
    o << '\n';
    o << "notes " << e.notes.size() << '\n';
    auto vib = [&](const AutoVibrato& v) {
        o << v.depthCents << ' ' << v.rateHz << ' ' << v.delayMs << ' ' << v.onsetPeriods << ' ' << v.variation << ' '
          << v.tremoloDb;
    };
    for (const auto& n : e.notes) {
        o << "n " << n.pitchOffset << ' ' << n.driftAmount << ' ' << n.vibratoAmount << ' ' << n.formantShift << ' '
          << n.gainDb << ' ' << n.timeOffsetSec << ' ' << n.stretch << ' ' << n.transition << ' ' << (n.consonant ? 1 : 0);
        if (v4) {
            o << ' ' << n.tiltL << ' ' << n.tiltR << ' ' << (n.ownVibrato ? 1 : 0) << ' ';
            vib(n.vibrato);
        }
        o << '\n';
    }
    if (v4) {
        o << "autovib ";
        vib(e.autoVibrato);
        o << ' ' << e.vibratoSeed << '\n';
    }
    o << std::setprecision(6);
    for (const auto& r : e.draw) {
        o << "draw " << r.start << ' ' << r.delta.size();
        for (float v : r.delta) o << ' ' << v;
        o << '\n';
    }
    o << std::setprecision(10);
    for (const auto& v : d.voices) {
        o << "voice " << (int)v.enabled << ' ' << (int)v.diatonic << ' ' << v.degrees << ' ' << v.semitones << ' '
          << v.formantShift << ' ' << v.gainDb << ' ' << v.pan << ' ' << v.delayMs << ' ' << v.detuneCents << ' '
          << v.vibratoAmount << ' ' << v.driftAmount << ' ' << (int)v.consonants << ' ' << v.seed << ' ';
        if (v.noteMask.empty()) o << '-';
        else for (uint8_t m : v.noteMask) o << (m ? '1' : '0');
        o << '\n';
    }
    // Muted consonants (only when any): an older reader skips the record.
    {
        std::vector<size_t> set;
        for (size_t i = 0; i < e.notes.size(); ++i) if (e.notes[i].consMuted) set.push_back(i);
        if (!set.empty()) {
            o << "consmutes " << set.size();
            for (size_t i : set) o << ' ' << i;
            o << '\n';
        }
    }
    // Deleted breaths (only when any): an older reader skips the record.
    {
        std::vector<size_t> set;
        for (size_t i = 0; i < e.notes.size(); ++i) if (e.notes[i].breathDeleted) set.push_back(i);
        if (!set.empty()) {
            o << "breathdels " << set.size();
            for (size_t i : set) o << ' ' << i;
            o << '\n';
        }
    }
    // Deleted notes (only when any): an older reader skips the record.
    {
        std::vector<size_t> set;
        for (size_t i = 0; i < e.notes.size(); ++i) if (e.notes[i].deleted) set.push_back(i);
        if (!set.empty()) {
            o << "deletes " << set.size();
            for (size_t i : set) o << ' ' << i;
            o << '\n';
        }
    }
    // Muted notes (only when any): an older reader skips the record.
    {
        std::vector<size_t> set;
        for (size_t i = 0; i < e.notes.size(); ++i) if (e.notes[i].muted) set.push_back(i);
        if (!set.empty()) {
            o << "mutes " << set.size();
            for (size_t i : set) o << ' ' << i;
            o << '\n';
        }
    }
    // Separations the user set (only when any): records an older reader does not know are skipped.
    {
        std::vector<size_t> set;
        for (size_t i = 0; i < e.notes.size(); ++i)
            if (e.notes[i].sepBefore != NoteEdit::Sep::Auto) set.push_back(i);
        if (!set.empty()) {
            o << "seps " << set.size();
            for (size_t i : set) o << ' ' << i << ' ' << (int)e.notes[i].sepBefore;
            o << '\n';
        }
    }
    // Links to a reference (only when made): records an older reader does not know are skipped.
    if (!d.pins.empty()) {
        o << "pinref " << std::quoted(d.pinRef) << '\n';
        o << "pins " << d.pins.size();
        for (const Pin& p : d.pins) o << ' ' << p.tgt << ' ' << p.ref;
        o << '\n';
    }
    o << "end\n";
    return o.str();
}

bool deserialize(const std::string& text, Document& out)
{
    std::istringstream in(text);
    in.imbue(std::locale::classic());
    std::string tag;
    int version = 0;
    if (!(in >> tag >> version) || tag != "glina-document" || version < 1 || version > 4) return false;
    auto readVib = [&](AutoVibrato& v) {
        in >> v.depthCents >> v.rateHz >> v.delayMs >> v.onsetPeriods >> v.variation >> v.tremoloDb;
    };
    Document d;
    while (in >> tag) {
        if (tag == "end") { out = std::move(d); return true; }
        if (tag == "key") {
            int mn = 0, ch = 0;
            in >> d.keyTonic >> mn >> ch;
            d.keyMinor = mn != 0; d.chromatic = ch != 0;
        } else if (tag == "mode") {
            in >> d.keyMode >> d.snapAmount;
            d.keyMode = std::clamp(d.keyMode, -1, 5);
            d.snapAmount = std::clamp(d.snapAmount, 0.0, 1.0);
        } else if (tag == "lead") {
            in >> d.leadGainDb;
        } else if (tag == "leadmix") {
            int en = 1;
            in >> en >> d.leadPan;
            d.leadEnabled = en != 0;
            d.leadPan = std::clamp(d.leadPan, -1.0, 1.0);
        } else if (tag == "tempo") {
            in >> d.tempoBpm >> d.beatsPerBar >> d.barOffsetSec;
            d.beatsPerBar = std::clamp(d.beatsPerBar, 1, 16);
        } else if (tag == "global") {
            EditSet& e = d.edits;
            in >> e.sibilantGainDb >> e.breathGainDb >> e.globalFormantShift >> e.transitionMs >> e.edgeTaperMs;
        } else if (tag == "spans") {
            size_t c = 0;
            in >> c;
            if (c > 10000000) return false;
            d.edits.spans.resize(c);
            for (auto& s : d.edits.spans) in >> s.start >> s.end;
        } else if (tag == "notes") {
            size_t c = 0;
            in >> c;
            if (c > 10000000) return false;
            d.edits.notes.reserve(c);
        } else if (tag == "n") {
            NoteEdit n;
            in >> n.pitchOffset >> n.driftAmount >> n.vibratoAmount >> n.formantShift >> n.gainDb >> n.timeOffsetSec >>
                n.stretch;
            if (version >= 2) in >> n.transition;
            if (version >= 3) { int c = 0; in >> c; n.consonant = c != 0; }
            if (version >= 4) {
                int own = 0;
                in >> n.tiltL >> n.tiltR >> own;
                n.ownVibrato = own != 0;
                readVib(n.vibrato);
            }
            d.edits.notes.push_back(n);
        } else if (tag == "autovib") {
            readVib(d.edits.autoVibrato);
            in >> d.edits.vibratoSeed;
        } else if (tag == "consmutes") {
            size_t c = 0;
            in >> c;
            if (c > 10000000) return false;
            for (size_t k = 0; k < c; ++k) {
                size_t i = 0;
                in >> i;
                if (i < d.edits.notes.size()) d.edits.notes[i].consMuted = true;
            }
        } else if (tag == "breathdels") {
            size_t c = 0;
            in >> c;
            if (c > 10000000) return false;
            for (size_t k = 0; k < c; ++k) {
                size_t i = 0;
                in >> i;
                if (i < d.edits.notes.size()) d.edits.notes[i].breathDeleted = true;
            }
        } else if (tag == "deletes") {
            size_t c = 0;
            in >> c;
            if (c > 10000000) return false;
            for (size_t k = 0; k < c; ++k) {
                size_t i = 0;
                in >> i;
                if (i < d.edits.notes.size()) d.edits.notes[i].deleted = true;
            }
        } else if (tag == "mutes") {
            size_t c = 0;
            in >> c;
            if (c > 10000000) return false;
            for (size_t k = 0; k < c; ++k) {
                size_t i = 0;
                in >> i;
                if (i < d.edits.notes.size()) d.edits.notes[i].muted = true;
            }
        } else if (tag == "seps") {
            size_t c = 0;
            in >> c;
            if (c > 10000000) return false;
            for (size_t k = 0; k < c; ++k) {
                size_t i = 0;
                int v = 0;
                in >> i >> v;
                if (i < d.edits.notes.size() && v >= 0 && v <= 2) d.edits.notes[i].sepBefore = (NoteEdit::Sep)v;
            }
        } else if (tag == "pinref") {
            in >> std::quoted(d.pinRef);
        } else if (tag == "pins") {
            size_t c = 0;
            in >> c;
            if (c > 1000000) return false;
            d.pins.resize(c);
            for (Pin& p : d.pins) in >> p.tgt >> p.ref;
        } else if (tag == "draw") {
            DrawRun r;
            size_t c = 0;
            in >> r.start >> c;
            if (c > 10000000) return false;
            r.delta.resize(c);
            for (float& v : r.delta) in >> v;
            d.edits.draw.push_back(std::move(r));
        } else if (tag == "voice") {
            HarmonyVoice v;
            int en = 0, dia = 0, cons = 0;
            std::string mask;
            in >> en >> dia >> v.degrees >> v.semitones >> v.formantShift >> v.gainDb >> v.pan >> v.delayMs >>
                v.detuneCents >> v.vibratoAmount >> v.driftAmount >> cons >> v.seed >> mask;
            v.enabled = en != 0; v.diatonic = dia != 0; v.consonants = cons != 0;
            if (mask != "-") for (char c : mask) v.noteMask.push_back(c == '1' ? 1 : 0);
            d.voices.push_back(std::move(v));
        } else {
            std::string rest;
            std::getline(in, rest); // unknown record from a newer version: skip
        }
        if (in.fail()) return false;
    }
    return false;
}

// ---- rendering --------------------------------------------------------------------
Mix renderMix(const float* x, size_t n, const Analysis& eff, const Document& d)
{
    Mix m;
    m.sampleRate = eff.sampleRate;
    const Key key = d.key(eff);

    std::vector<const HarmonyVoice*> active;
    for (const auto& v : d.voices) if (v.enabled) active.push_back(&v);

    std::vector<std::future<std::vector<float>>> jobs;
    for (const HarmonyVoice* v : active)
        jobs.push_back(std::async(std::launch::async, [&, v] {
            return render(x, n, eff, harmonyEdits(eff, d.edits, key, *v));
        }));
    m.lead = render(x, n, eff, d.edits);
    const float leadGain = d.leadEnabled ? (float)std::pow(10.0, d.leadGainDb / 20.0) : 0.f;
    if (leadGain != 1.f) for (float& s : m.lead) s *= leadGain;

    size_t len = m.lead.size();
    std::vector<size_t> delays;
    for (size_t j = 0; j < jobs.size(); ++j) {
        std::vector<float> y = jobs[j].get();
        const HarmonyVoice& v = *active[j];
        const size_t delay = (size_t)std::max(0.0, std::round(v.delayMs * 0.001 * m.sampleRate));
        const float g = (float)std::pow(10.0, v.gainDb / 20.0);
        std::vector<float> z(y.size() + delay, 0.f);
        for (size_t i = 0; i < y.size(); ++i) z[i + delay] = y[i] * g;
        len = std::max(len, z.size());
        m.voices.push_back(std::move(z));
    }

    m.left.assign(len, 0.f);
    m.right.assign(len, 0.f);
    {
        const double p = std::clamp(d.leadPan, -1.0, 1.0);
        const float gl = (float)(p <= 0 ? 1.0 : std::cos(p * M_PI / 2));
        const float gr = (float)(p >= 0 ? 1.0 : std::cos(-p * M_PI / 2));
        for (size_t i = 0; i < m.lead.size(); ++i) { m.left[i] += m.lead[i] * gl; m.right[i] += m.lead[i] * gr; }
    }
    for (size_t j = 0; j < m.voices.size(); ++j) {
        const double p = std::clamp(active[j]->pan, -1.0, 1.0);
        // Balance-style pan: centre = unity on both sides, the far side fades out.
        const float gl = (float)(p <= 0 ? 1.0 : std::cos(p * M_PI / 2));
        const float gr = (float)(p >= 0 ? 1.0 : std::cos(-p * M_PI / 2));
        const auto& z = m.voices[j];
        for (size_t i = 0; i < z.size(); ++i) { m.left[i] += z[i] * gl; m.right[i] += z[i] * gr; }
    }
    return m;
}

} // namespace glina
