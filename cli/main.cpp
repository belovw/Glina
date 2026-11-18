// glina — command-line front end for the engine (testing without a DAW).
//
//   glina analyze <in.wav> [--json out.json]
//   glina render  <in.wav> <out.wav>                      (no edits: null test)
//   glina tune    <in.wav> <out.wav> [--key auto|C|F#m|chromatic] [--pitch 0..1]
//                 [--drift 0..1] [--vibrato x] [--tolerance cents] [--formant st]
//                 [--sib dB] [--breath dB]
//   glina shift   <in.wav> <out.wav> --semitones x [--formant st]
//   glina stretch <in.wav> <out.wav> --factor f
//   glina harmony <in.wav> <out.wav> [--voices +2,-3] [--tune 0..1] [--key ...]   (stereo mix)

#include "WavIO.h"
#include "glina/Analysis.h"
#include "glina/Renderer.h"
#include "glina/Tuning.h"
#include "glina/Document.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>

using namespace glina;

static std::map<std::string, std::string> parseOpts(int argc, char** argv, int from)
{
    std::map<std::string, std::string> o;
    for (int i = from; i < argc; ++i) {
        if (std::strncmp(argv[i], "--", 2) == 0) {
            const std::string k = argv[i] + 2;
            if (i + 1 < argc && std::strncmp(argv[i + 1], "--", 2) != 0) o[k] = argv[++i];
            else o[k] = "1";
        }
    }
    return o;
}

static double optD(const std::map<std::string, std::string>& o, const char* k, double def)
{
    auto it = o.find(k);
    return it == o.end() ? def : std::atof(it->second.c_str());
}

static bool parseKey(const std::string& s, int& tonic, bool& minor)
{
    static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    static const char* flats[] = {"C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"};
    std::string t = s;
    minor = false;
    if (!t.empty() && t.back() == 'm') { minor = true; t.pop_back(); }
    for (int i = 0; i < 12; ++i)
        if (t == names[i] || t == flats[i]) { tonic = i; return true; }
    return false;
}

static std::string noteName(double midi)
{
    static const char* n[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const int m = (int)std::lround(midi);
    const int cents = (int)std::lround((midi - m) * 100);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%s%d %+3d¢", n[(m % 12 + 12) % 12], m / 12 - 1, cents);
    return buf;
}

static void writeJson(const std::string& path, const Analysis& a, const std::vector<double>* edited)
{
    std::ofstream f(path);
    f << "{\n\"sampleRate\": " << a.sampleRate << ",\n\"hop\": " << a.hop
      << ",\n\"key\": \"" << a.key.name() << "\",\n\"frames\": [";
    for (size_t k = 0; k < a.frames.size(); ++k) {
        const Frame& fr = a.frames[k];
        if (k) f << ",";
        f << "[" << fr.f0 << "," << fr.voicedProb << "," << fr.rmsDb << "," << (int)fr.kind;
        if (edited) f << "," << (*edited)[k];
        f << "]";
    }
    f << "],\n\"notes\": [";
    for (size_t i = 0; i < a.notes.size(); ++i) {
        const Note& n = a.notes[i];
        if (i) f << ",";
        f << "{\"start\":" << n.startSec << ",\"end\":" << n.endSec << ",\"center\":" << n.centerMidi
          << ",\"startFrame\":" << n.startFrame << ",\"endFrame\":" << n.endFrame << "}";
    }
    f << "],\n\"marks\": " << a.marks.size() << "\n}\n";
}

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: glina analyze|render|tune|shift|stretch <in.wav> [out.wav] [options]\n");
        return 2;
    }
    const std::string cmd = argv[1];
    const std::string inPath = argv[2];
    const bool needsOut = cmd != "analyze";
    if (needsOut && argc < 4) { std::fprintf(stderr, "missing output file\n"); return 2; }
    const std::string outPath = needsOut ? argv[3] : "";
    const auto opt = parseOpts(argc, argv, needsOut ? 4 : 3);

    io::Audio audio;
    std::string err;
    if (!io::readWav(inPath, audio, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    const std::vector<float> x = audio.mono();

    auto t0 = std::chrono::steady_clock::now();
    const Analysis a = analyze(x.data(), x.size(), audio.sampleRate);
    auto t1 = std::chrono::steady_clock::now();
    const double durSec = x.size() / audio.sampleRate;
    std::fprintf(stderr, "analysis: %.2f s of audio in %.2f s, %zu notes, %zu marks, key %s (%.2f)\n",
                 durSec, std::chrono::duration<double>(t1 - t0).count(), a.notes.size(), a.marks.size(),
                 a.key.name().c_str(), a.key.confidence);

    EditSet edits;
    if (cmd == "analyze") {
        for (size_t i = 0; i < a.notes.size(); ++i) {
            const Note& n = a.notes[i];
            std::printf("%3zu  %7.3f–%7.3f s  %s\n", i, n.startSec, n.endSec, noteName(n.centerMidi).c_str());
        }
        if (opt.count("json")) writeJson(opt.at("json"), a, nullptr);
        return 0;
    } else if (cmd == "render") {
        // no edits
    } else if (cmd == "tune") {
        TuneParams tp;
        tp.pitchAmount = optD(opt, "pitch", 1.0);
        tp.driftAmount = optD(opt, "drift", 0.0);
        tp.vibratoScale = optD(opt, "vibrato", 1.0);
        tp.toleranceCents = optD(opt, "tolerance", 0.0);
        tp.formantShift = optD(opt, "formant", 0.0);
        if (opt.count("key")) {
            const std::string k = opt.at("key");
            if (k == "chromatic") tp.useKey = false;
            else if (k != "auto" && !parseKey(k, tp.tonic, tp.minor)) { std::fprintf(stderr, "bad key %s\n", k.c_str()); return 2; }
        }
        edits = makeAutoTune(a, tp);
        edits.sibilantGainDb = optD(opt, "sib", 0.0);
        edits.breathGainDb = optD(opt, "breath", 0.0);
    } else if (cmd == "shift") {
        edits = makeTranspose(a, optD(opt, "semitones", 0.0), optD(opt, "formant", 0.0));
    } else if (cmd == "stretch") {
        const double fct = optD(opt, "factor", 1.0);
        edits.keepLength = false; // the whole take gets longer/shorter
        edits.notes.resize(a.notes.size());
        for (size_t i = 0; i < a.notes.size(); ++i) {
            edits.notes[i].timeOffsetSec = a.notes[i].startSec * (fct - 1.0);
            edits.notes[i].stretch = fct;
        }
    } else if (cmd == "harmony") {
        Document d;
        conform(d, a);
        if (opt.count("key")) {
            const std::string k = opt.at("key");
            if (k == "chromatic") d.chromatic = true;
            else if (k != "auto" && !parseKey(k, d.keyTonic, d.keyMinor)) { std::fprintf(stderr, "bad key %s\n", k.c_str()); return 2; }
        }
        const double tune = optD(opt, "tune", 1.0);
        for (size_t i = 0; i < a.notes.size(); ++i) {
            const double c = a.notes[i].centerMidi;
            d.edits.notes[i].pitchOffset = (d.target(c, a) - c) * tune;
            d.edits.notes[i].driftAmount = 1.0 - 0.4 * tune;
        }
        std::string list = opt.count("voices") ? opt.at("voices") : "+2,-3";
        double pan = -0.45;
        for (size_t p = 0; p < list.size();) {
            size_t q = list.find(',', p);
            if (q == std::string::npos) q = list.size();
            HarmonyVoice v;
            v.degrees = std::atoi(list.substr(p, q - p).c_str());
            v.pan = pan; pan = -pan;
            v.seed = (uint32_t)(d.voices.size() + 1) * 7919u;
            d.voices.push_back(v);
            p = q + 1;
        }
        const Mix m = renderMix(x.data(), x.size(), a, d);
        auto t2 = std::chrono::steady_clock::now();
        std::fprintf(stderr, "render (lead + %zu voices): %.2f s\n", d.voices.size(), std::chrono::duration<double>(t2 - t1).count());
        if (!io::writeWavStereo(outPath, m.left, m.right, audio.sampleRate, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
        return 0;
    } else {
        std::fprintf(stderr, "unknown command %s\n", cmd.c_str());
        return 2;
    }

    const std::vector<float> y = render(x.data(), x.size(), a, edits);
    auto t2 = std::chrono::steady_clock::now();
    std::fprintf(stderr, "render: %.2f s\n", std::chrono::duration<double>(t2 - t1).count());
    if (!io::writeWav(outPath, y, audio.sampleRate, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    if (opt.count("json")) {
        const std::vector<double> curve = editedPitchCurve(a, edits);
        writeJson(opt.at("json"), a, &curve);
    }
    return 0;
}
