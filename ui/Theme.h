#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace glina::ui {

inline juce::String T(const char* utf8) { return juce::String::fromUTF8(utf8); }

namespace col {
const juce::Colour bg { 0xff131519 }, panel { 0xff1a1d23 }, panelLine { 0xff2a2e37 },
    laneWhite { 0xff1b1e24 }, laneBlack { 0xff17191e }, laneScale { 0xff1e2229 }, grid { 0xff262a32 },
    keyWhite { 0xffd8dade }, keyBlack { 0xff2a2d33 }, text { 0xffc9ccd3 }, dim { 0xff7d828c },
    pitchOrig { 0x80a0a6b0 }, pitchEdit { 0xfff2f4f8 }, playhead { 0xff4fb3ff },
    inTune { 0xff57c785 }, nearTune { 0xffe8c547 }, offTune { 0xffe8803d }, select { 0xffffffff },
    sibilant { 0xffb36bd6 }, breath { 0xff5c7a99 }, accent { 0xff4fb3ff }, warn { 0xffe8803d };
inline juce::Colour voice(int i)
{
    static const juce::uint32 c[] = {0xff3fd0c9, 0xffb18cff, 0xffff7eb6, 0xff7cc4ff};
    return juce::Colour(c[(size_t)i % 4]);
}
} // namespace col

class GlinaLookAndFeel final : public juce::LookAndFeel_V4 {
public:
    GlinaLookAndFeel()
        : juce::LookAndFeel_V4(juce::LookAndFeel_V4::ColourScheme(
              col::panel, juce::Colour(0xff23272f), juce::Colour(0xff20232a), col::panelLine, col::text,
              juce::Colour(0xff3a7bd5), juce::Colours::white, col::accent, col::text))
    {
        setColour(juce::ResizableWindow::backgroundColourId, col::bg);
        setColour(juce::TextButton::buttonColourId, juce::Colour(0xff252930));
        setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff2f6db5));
        setColour(juce::Slider::thumbColourId, col::accent);
        setColour(juce::Slider::trackColourId, juce::Colour(0xff3a6ea8));
        setColour(juce::Slider::backgroundColourId, juce::Colour(0xff2a2e37));
        setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour(juce::Label::textColourId, col::text);
        setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xff252930));
        setColour(juce::ComboBox::outlineColourId, col::panelLine);
    }
};

} // namespace glina::ui
