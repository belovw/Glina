#include "WavIO.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>

namespace glina::io {

std::vector<float> Audio::mono() const
{
    const size_t nfr = frames();
    std::vector<float> m(nfr);
    for (size_t i = 0; i < nfr; ++i) {
        double s = 0;
        for (int c = 0; c < channels; ++c) s += interleaved[i * channels + c];
        m[i] = (float)(s / channels);
    }
    return m;
}

static uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

bool readWav(const std::string& path, Audio& out, std::string& err)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open " + path; return false; }
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), {});
    if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) || std::memcmp(d.data() + 8, "WAVE", 4)) {
        err = "not a RIFF/WAVE file"; return false;
    }
    int fmt = 0, ch = 0, bits = 0;
    uint32_t sr = 0;
    const uint8_t* data = nullptr;
    size_t dataLen = 0;
    for (size_t p = 12; p + 8 <= d.size();) {
        const uint32_t len = rd32(&d[p + 4]);
        const uint8_t* body = &d[p + 8];
        if (!std::memcmp(&d[p], "fmt ", 4) && len >= 16) {
            fmt = rd16(body); ch = rd16(body + 2); sr = rd32(body + 4); bits = rd16(body + 14);
            if (fmt == 0xFFFE && len >= 26) fmt = rd16(body + 24);
        } else if (!std::memcmp(&d[p], "data", 4)) {
            data = body;
            dataLen = std::min<size_t>(len, d.size() - (p + 8));
        }
        p += 8 + len + (len & 1);
    }
    if (!data || ch <= 0 || sr == 0) { err = "missing fmt/data chunk"; return false; }
    const int bps = bits / 8;
    const size_t n = dataLen / bps;
    out.sampleRate = sr;
    out.channels = ch;
    out.interleaved.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const uint8_t* s = data + i * bps;
        float v = 0;
        if (fmt == 3 && bits == 32) { std::memcpy(&v, s, 4); }
        else if (fmt == 1 && bits == 16) v = (int16_t)rd16(s) / 32768.f;
        else if (fmt == 1 && bits == 24) {
            int32_t x = s[0] | (s[1] << 8) | (s[2] << 16);
            if (x & 0x800000) x |= ~0xFFFFFF;
            v = x / 8388608.f;
        } else if (fmt == 1 && bits == 32) v = (int32_t)rd32(s) / 2147483648.f;
        else { err = "unsupported WAV format (fmt " + std::to_string(fmt) + ", " + std::to_string(bits) + " bit)"; return false; }
        out.interleaved[i] = v;
    }
    return true;
}

bool writeWav(const std::string& path, const std::vector<float>& m, double sampleRate, std::string& err)
{
    std::ofstream f(path, std::ios::binary);
    if (!f) { err = "cannot write " + path; return false; }
    const uint32_t sr = (uint32_t)std::lround(sampleRate);
    const uint32_t dataLen = (uint32_t)(m.size() * 3);
    auto w32 = [&](uint32_t v) { uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)}; f.write((char*)b, 4); };
    auto w16 = [&](uint16_t v) { uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)}; f.write((char*)b, 2); };
    const uint32_t pad = dataLen & 1u; // RIFF chunks are padded to an even size
    f.write("RIFF", 4); w32(36 + dataLen + pad); f.write("WAVE", 4);
    f.write("fmt ", 4); w32(16); w16(1); w16(1); w32(sr); w32(sr * 3); w16(3); w16(24);
    f.write("data", 4); w32(dataLen);
    for (float v : m) {
        const int32_t x = (int32_t)std::lround(std::clamp((double)v, -1.0, 1.0 - 1.0 / 8388608.0) * 8388608.0);
        const uint8_t b[3] = {(uint8_t)x, (uint8_t)(x >> 8), (uint8_t)(x >> 16)};
        f.write((const char*)b, 3);
    }
    if (pad) f.put('\0');
    return (bool)f;
}

bool writeWavStereo(const std::string& path, const std::vector<float>& l, const std::vector<float>& r, double sampleRate,
                    std::string& err)
{
    std::ofstream f(path, std::ios::binary);
    if (!f) { err = "cannot write " + path; return false; }
    const size_t n = std::max(l.size(), r.size());
    const uint32_t sr = (uint32_t)std::lround(sampleRate);
    const uint32_t dataLen = (uint32_t)(n * 6);
    auto w32 = [&](uint32_t v) { uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)}; f.write((char*)b, 4); };
    auto w16 = [&](uint16_t v) { uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)}; f.write((char*)b, 2); };
    f.write("RIFF", 4); w32(36 + dataLen); f.write("WAVE", 4);
    f.write("fmt ", 4); w32(16); w16(1); w16(2); w32(sr); w32(sr * 6); w16(6); w16(24);
    f.write("data", 4); w32(dataLen);
    for (size_t i = 0; i < n; ++i) {
        for (const auto* ch : {&l, &r}) {
            const double v = i < ch->size() ? (*ch)[i] : 0.0;
            const int32_t x = (int32_t)std::lround(std::clamp(v, -1.0, 1.0 - 1.0 / 8388608.0) * 8388608.0);
            const uint8_t b[3] = {(uint8_t)x, (uint8_t)(x >> 8), (uint8_t)(x >> 16)};
            f.write((const char*)b, 3);
        }
    }
    return (bool)f;
}

} // namespace glina::io
