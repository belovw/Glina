#include "glina/Document.h"
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
    if (keyTonic >= 0) { k.tonic = keyTonic % 12; k.minor = keyMinor; }
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
    conform(d, eff);
    if (d.edits.spans.empty()) d.edits.spans = spansOf(eff);
    if (d.edits.spans.size() != eff.notes.size()) return false;
    d.edits.spans[(size_t)i].end = frame;
    d.edits.spans.insert(d.edits.spans.begin() + i + 1, NoteSpan{frame, n.endFrame});
    const NoteEdit copy = d.edits.notes[(size_t)i];
    d.edits.notes.insert(d.edits.notes.begin() + i + 1, copy);
    for (auto& v : d.voices)
        if (!v.noteMask.empty()) v.noteMask.insert(v.noteMask.begin() + i + 1, v.noteMask[(size_t)i]);
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
    d.edits.spans[(size_t)i].end = d.edits.spans[(size_t)i + 1].end;
    d.edits.spans.erase(d.edits.spans.begin() + i + 1);
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
    o << "glina-document 2\n";
    o << "key " << d.keyTonic << ' ' << (int)d.keyMinor << ' ' << (int)d.chromatic << '\n';
    o << "lead " << d.leadGainDb << '\n';
    o << "tempo " << d.tempoBpm << ' ' << d.beatsPerBar << ' ' << d.barOffsetSec << '\n';
    o << "global " << e.sibilantGainDb << ' ' << e.breathGainDb << ' ' << e.globalFormantShift << ' '
      << e.transitionMs << ' ' << e.edgeTaperMs << '\n';
    o << "spans " << e.spans.size();
    for (const auto& s : e.spans) o << ' ' << s.start << ' ' << s.end;
    o << '\n';
    o << "notes " << e.notes.size() << '\n';
    for (const auto& n : e.notes)
        o << "n " << n.pitchOffset << ' ' << n.driftAmount << ' ' << n.vibratoAmount << ' ' << n.formantShift << ' '
          << n.gainDb << ' ' << n.timeOffsetSec << ' ' << n.stretch << ' ' << n.transition << '\n';
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
    o << "end\n";
    return o.str();
}

bool deserialize(const std::string& text, Document& out)
{
    std::istringstream in(text);
    in.imbue(std::locale::classic());
    std::string tag;
    int version = 0;
    if (!(in >> tag >> version) || tag != "glina-document" || version < 1 || version > 2) return false;
    Document d;
    while (in >> tag) {
        if (tag == "end") { out = std::move(d); return true; }
        if (tag == "key") {
            int mn = 0, ch = 0;
            in >> d.keyTonic >> mn >> ch;
            d.keyMinor = mn != 0; d.chromatic = ch != 0;
        } else if (tag == "lead") {
            in >> d.leadGainDb;
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
            d.edits.notes.push_back(n);
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
    const float leadGain = (float)std::pow(10.0, d.leadGainDb / 20.0);
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
    for (size_t i = 0; i < m.lead.size(); ++i) { m.left[i] += m.lead[i]; m.right[i] += m.lead[i]; }
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
