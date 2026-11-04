// Writes the synthetic singer to WAV + ground-truth JSON (for the Python plots).
#include "../cli/WavIO.h"
#include "Synth.h"

#include <cstdio>
#include <fstream>
#include <string>

int main(int argc, char** argv)
{
    if (argc < 3) { std::fprintf(stderr, "usage: glina_synth out.wav truth.json [sampleRate]\n"); return 2; }
    const double sr = argc > 3 ? std::atof(argv[3]) : 48000.0;
    const auto s = glina::test::synthesize(glina::test::demoMelody(), sr);
    std::string err;
    if (!glina::io::writeWav(argv[1], s.audio, sr, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    std::ofstream f(argv[2]);
    f << "{\"sampleRate\":" << sr << ",\"notes\":[";
    for (size_t i = 0; i < s.noteMidi.size(); ++i)
        f << (i ? "," : "") << "{\"start\":" << s.noteStart[i] << ",\"end\":" << s.noteEnd[i] << ",\"midi\":" << s.noteMidi[i] << "}";
    f << "],\"f0\":[";
    const size_t step = (size_t)(sr * 0.005);
    for (size_t i = 0; i < s.trueF0.size(); i += step) f << (i ? "," : "") << s.trueF0[i];
    f << "]}\n";
    return 0;
}
