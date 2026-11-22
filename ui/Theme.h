#pragma once

#include <juce_data_structures/juce_data_structures.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <utility>
#include <vector>

namespace glina::ui {

inline juce::String T(const char* utf8) { return juce::String::fromUTF8(utf8); }

// Colours of the editor. Two palettes, dark (default) and light: setLight() swaps them;
// windows that are already open repaint, and GlinaLookAndFeel::applyTheme() re-reads them.
namespace col {
inline juce::Colour bg, panel, panelLine, laneWhite, laneBlack, laneScale, grid, keyWhite, keyBlack, keyWhiteText,
    keyBlackText, text, dim, pitchOrig, pitchEdit, playhead, inTune, nearTune, offTune, select, sibilant, breath,
    accent, warn, drawn,
    // controls
    button, buttonOver, buttonDown, buttonOn, buttonText, buttonTextOff, sliderTrack, sliderBg, widget, valueText,
    valueOutline,
    // overlays and labels
    overlay, ghost, ghostText, link, lift, labelText, labelBox;
inline bool light = false;

inline void setLight(bool on)
{
    light = on;
    using C = juce::Colour;
    if (!on) {
        bg = C(0xff131519); panel = C(0xff1a1d23); panelLine = C(0xff2a2e37);
        laneWhite = C(0xff16181d); laneBlack = C(0xff121418); laneScale = C(0xff181b21); grid = C(0xff262a32);
        keyWhite = C(0xffd8dade); keyBlack = C(0xff2a2d33); keyWhiteText = C(0xff131519); keyBlackText = C(0xffc9ccd3);
        text = C(0xffc9ccd3); dim = C(0xff7d828c);
        pitchOrig = C(0x80a0a6b0); pitchEdit = C(0xfff2f4f8); playhead = C(0xff4fb3ff);
        inTune = C(0xff57c785); nearTune = C(0xffe8c547); offTune = C(0xffc492e8); select = C(0xffffffff);
        sibilant = C(0xffb36bd6); breath = C(0xff5c7a99); accent = C(0xff4fb3ff); warn = C(0xffe8803d);
        drawn = C(0xffffa53d);
        button = C(0xff252930); buttonOver = C(0xff2f3540); buttonDown = C(0xff3a4150); buttonOn = C(0xff2f6db5);
        buttonText = C(0xffd6dbe3); buttonTextOff = C(0xff5a616d);
        sliderTrack = C(0xff3a6ea8); sliderBg = C(0xff2a2e37); widget = C(0xff23272f);
        valueText = C(0xffffffff); valueOutline = C(0xff8e989b);
        overlay = C(0xe0101216); ghost = C(0xffa7aec0); ghostText = C(0xff7f8799); link = C(0xffffb547);
        lift = C(0xffffffff); labelText = C(0xffffffff); labelBox = C(0x59ffffff);
    } else {
        bg = C(0xfff4f5f7); panel = C(0xffeaecf0); panelLine = C(0xffd3d7de);
        laneWhite = C(0xfff1f2f5); laneBlack = C(0xffe7e9ed); laneScale = C(0xfff9f9fa); grid = C(0xffd6dae0);
        keyWhite = C(0xffffffff); keyBlack = C(0xffc6cbd3); keyWhiteText = C(0xff59606b); keyBlackText = C(0xff3e444e);
        text = C(0xff23272e); dim = C(0xff6b7280);
        pitchOrig = C(0x80808896); pitchEdit = C(0xff1e2229); playhead = C(0xff2f7fe0);
        inTune = C(0xff3daa6a); nearTune = C(0xffd6a21f); offTune = C(0xffa877d8); select = C(0xff1e2229);
        sibilant = C(0xffa35bc8); breath = C(0xff6a87a6); accent = C(0xff2f7fe0); warn = C(0xffd9702a);
        drawn = C(0xffe8890c);
        button = C(0xffe0e3e8); buttonOver = C(0xffd5d9df); buttonDown = C(0xffc9ced6); buttonOn = C(0xff2f7fe0);
        buttonText = C(0xff23272e); buttonTextOff = C(0xffa3a9b3);
        sliderTrack = C(0xff5d9be8); sliderBg = C(0xffd0d5dc); widget = C(0xffffffff);
        valueText = C(0xff23272e); valueOutline = C(0xffc3c8d0);
        overlay = C(0xf2f5f6f8); ghost = C(0xff8b93a3); ghostText = C(0xff7a8291); link = C(0xffe0890a);
        lift = C(0xff000000); labelText = C(0xff1e2229); labelBox = C(0x1a1e2229);
    }
}
inline const bool kThemeReady = (setLight(false), true);

inline juce::Colour voice(int i)
{
    static const juce::uint32 dark[] = {0xff3fd0c9, 0xffb18cff, 0xffff7eb6, 0xff7cc4ff};
    static const juce::uint32 lite[] = {0xff17a39c, 0xff8a63e6, 0xffe2559a, 0xff3a92e0};
    return juce::Colour((light ? lite : dark)[(size_t)i % 4]);
}
} // namespace col

// ---- theme setting, shared by the app and every plug-in instance ---------------------
// Stored in user app data/Glina/Glina.settings (the file the plug-in keeps its render
// option in). Each call reads or writes the file afresh, so instances do not overwrite
// each other's values.
namespace settings {
inline juce::PropertiesFile::Options options()
{
    juce::PropertiesFile::Options o;
    o.applicationName = "Glina";
    o.filenameSuffix = ".settings";
    o.folderName = "Glina";
    o.osxLibrarySubFolder = "Application Support";
    o.millisecondsBeforeSaving = -1;
    return o;
}
inline bool lightTheme()
{
    juce::PropertiesFile p(options());
    return p.getBoolValue("lightTheme", false);
}
inline void setLightTheme(bool on)
{
    juce::PropertiesFile p(options());
    p.setValue("lightTheme", on);
    p.saveIfNeeded();
}
} // namespace settings

namespace col {
inline bool forced = false; // the palette was chosen on the command line (screenshots): ignore the setting

// The palette from the setting (unless forced). Call before building windows.
inline void loadTheme()
{
    if (!forced) setLight(settings::lightTheme());
}

// Every palette slot, for remapping colours that controls copied when they were built.
inline std::vector<juce::Colour*> slots()
{
    // Most used first: a colour shared by two slots (dark text == key text) maps by the first.
    return {&text, &dim, &button, &buttonOn, &buttonText, &bg, &panel, &panelLine, &laneWhite, &laneBlack, &laneScale,
            &grid, &keyWhite, &keyBlack, &keyWhiteText, &keyBlackText, &pitchOrig, &pitchEdit, &playhead, &inTune, &nearTune, &offTune, &select,
            &sibilant, &breath, &accent, &warn, &drawn, &buttonOver, &buttonDown, &buttonTextOff, &sliderTrack, &sliderBg, &widget, &valueText, &valueOutline, &overlay, &ghost, &ghostText,
            &link, &lift, &labelText, &labelBox};
}
// Old -> new colour of the last switch.
inline std::vector<std::pair<juce::Colour, juce::Colour>>& lastRemap()
{
    static std::vector<std::pair<juce::Colour, juce::Colour>> r;
    return r;
}
inline juce::ChangeBroadcaster& themeChanges()
{
    static juce::ChangeBroadcaster b;
    return b;
}

// Switch the palette, remember it, and tell every open window (synchronously).
inline void switchTheme(bool on)
{
    if (on == light) return;
    std::vector<juce::Colour> before;
    for (auto* c : slots()) before.push_back(*c);
    std::vector<juce::Colour> vBefore;
    for (int i = 0; i < 4; ++i) vBefore.push_back(voice(i));
    setLight(on);
    auto& r = lastRemap();
    r.clear();
    const auto now = slots();
    for (size_t i = 0; i < now.size(); ++i) r.push_back({before[i], *now[i]});
    for (int i = 0; i < 4; ++i) r.push_back({vBefore[(size_t)i], voice(i)});
    if (!forced) settings::setLightTheme(on);
    themeChanges().sendSynchronousChangeMessage();
}

// Colours that controls were given explicitly (setColour with a palette colour) follow the switch.
inline void remapColours(juce::Component& c)
{
    static const int ids[] = {juce::Label::textColourId, juce::Label::backgroundColourId, juce::Label::outlineColourId,
                              juce::TextButton::buttonColourId, juce::TextButton::buttonOnColourId,
                              juce::TextButton::textColourOffId, juce::TextButton::textColourOnId,
                              juce::ToggleButton::textColourId, juce::ToggleButton::tickColourId,
                              juce::ToggleButton::tickDisabledColourId, juce::ComboBox::backgroundColourId,
                              juce::ComboBox::textColourId, juce::ComboBox::outlineColourId};
    for (int id : ids) {
        if (!c.isColourSpecified(id)) continue;
        const juce::Colour v = c.findColour(id);
        for (const auto& [from, to] : lastRemap())
            if (v == from) { c.setColour(id, to); break; }
    }
    for (auto* child : c.getChildren()) remapColours(*child);
}

// Calls `fn` after every theme switch while it lives.
class ThemeWatcher final : private juce::ChangeListener {
public:
    explicit ThemeWatcher(std::function<void()> fn) : fn_(std::move(fn)) { themeChanges().addChangeListener(this); }
    ~ThemeWatcher() override { themeChanges().removeChangeListener(this); }

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override { if (fn_) fn_(); }
    std::function<void()> fn_;
};
} // namespace col

class GlinaLookAndFeel final : public juce::LookAndFeel_V4 {
public:
    GlinaLookAndFeel() { applyTheme(); }

    // Re-reads the palette (after col::setLight()).
    void applyTheme()
    {
        setColourScheme(juce::LookAndFeel_V4::ColourScheme(col::panel, col::widget, col::widget, col::panelLine, col::text,
                                                           col::buttonOn, juce::Colours::white, col::accent, col::text));
        setColour(juce::ResizableWindow::backgroundColourId, col::bg);
        setColour(juce::DocumentWindow::textColourId, col::text);
        setColour(juce::TextButton::buttonColourId, col::button);
        setColour(juce::TextButton::buttonOnColourId, col::buttonOn);
        setColour(juce::TextButton::textColourOffId, col::buttonText);
        setColour(juce::TextButton::textColourOnId, juce::Colours::white);
        setColour(juce::Slider::thumbColourId, col::accent);
        setColour(juce::Slider::trackColourId, col::sliderTrack);
        setColour(juce::Slider::backgroundColourId, col::sliderBg);
        setColour(juce::Slider::textBoxOutlineColourId, col::valueOutline);
        setColour(juce::Slider::textBoxTextColourId, col::valueText);
        setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        setColour(juce::Label::textColourId, col::text);
        setColour(juce::ComboBox::backgroundColourId, col::button);
        setColour(juce::ComboBox::textColourId, col::buttonText);
        setColour(juce::ComboBox::arrowColourId, col::buttonText);
        setColour(juce::ComboBox::outlineColourId, col::panelLine);
        setColour(juce::ToggleButton::textColourId, col::text);
        setColour(juce::ToggleButton::tickColourId, col::text);
        setColour(juce::ToggleButton::tickDisabledColourId, col::dim);
    }
};

} // namespace glina::ui
