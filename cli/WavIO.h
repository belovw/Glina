#pragma once

#include <string>
#include <vector>

namespace glina::io {

struct Audio {
    double sampleRate = 48000;
    int channels = 1;
    std::vector<float> interleaved;
    size_t frames() const { return channels ? interleaved.size() / channels : 0; }
    std::vector<float> mono() const;
};

// PCM 16/24/32-bit and 32-bit float WAV (incl. WAVE_FORMAT_EXTENSIBLE).
bool readWav(const std::string& path, Audio& out, std::string& err);
// Writes 24-bit PCM.
bool writeWav(const std::string& path, const std::vector<float>& mono, double sampleRate, std::string& err);

bool writeWavStereo(const std::string& path, const std::vector<float>& left, const std::vector<float>& right,
                    double sampleRate, std::string& err);

} // namespace glina::io
