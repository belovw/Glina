// Everything the user edits for one clip (note edits, note layout, key choice,
// harmony voices), its serialisation, and rendering to a stereo mix. Shared by
// the standalone app and the ARA plugin so both behave identically.
#pragma once

#include "glina/Harmony.h"
#include "glina/Types.h"

#include <atomic>
#include <string>
#include <vector>

namespace glina {

struct Document {
    EditSet edits;
    std::vector<HarmonyVoice> voices;
    int keyTonic = -1;       // -1 = use the detected key
    bool keyMinor = false;
    bool chromatic = false;  // correct to semitones instead of the scale
    double leadGainDb = 0;   // lead level in the mix (harmonies have their own)

    Key key(const Analysis& a) const;
    // Correction target for a pitch: nearest scale note (or semitone in chromatic mode).
    double target(double midi, const Analysis& a) const;
    bool operator==(const Document& o) const;
};

// Keep per-note vectors aligned with the (effective) analysis.
void conform(Document& d, const Analysis& eff);

// Split note `note` at analysis frame `frame` (both halves keep the note's edits).
bool splitNote(Document& d, const Analysis& eff, int note, int frame);
// Join note `note` with the next one (must be adjacent or separated by < 40 ms).
bool joinNotes(Document& d, const Analysis& eff, int note);

std::string serialize(const Document& d);
bool deserialize(const std::string& text, Document& d);

struct Mix {
    double sampleRate = 48000;
    std::vector<float> lead;                 // mono, edited lead (lead gain applied)
    std::vector<std::vector<float>> voices;  // mono per enabled harmony voice (gain + delay applied, no pan)
    std::vector<float> left, right;          // stereo mix of everything
    size_t length() const { return left.size(); }
};

// Renders the lead and every enabled harmony voice (in parallel) and mixes them.
Mix renderMix(const float* x, size_t n, const Analysis& eff, const Document& d);

} // namespace glina
