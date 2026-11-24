// Which mouse wheel and drag actions the note roll does, by modifier keys.
// The default scheme, plus overrides from text (saved in the settings file), e.g. "w2=zv d6=pan":
//   wN = wheel with modifier mask N, dN = drag on empty roll with mask N.
//   Mask: 1 Alt, 2 Ctrl (Cmd on macOS), 4 Shift; add them: Ctrl+Alt = 3.
//   Wheel actions: v (vertical scroll: pitch), h (horizontal scroll: time),
//                  zv (vertical zoom: pitch range), zh (horizontal zoom: time), none.
//   Drag actions:  pan (move the view), zoom (horizontal: time, vertical: pitch), none.
#pragma once

#include <array>
#include <optional>
#include <sstream>
#include <string>

namespace glina::ui {

enum class WheelAct { None, ScrollV, ScrollH, ZoomV, ZoomH };
enum class DragAct { None, Pan, Zoom };

struct MouseScheme {
    static constexpr int kAlt = 1, kCtrl = 2, kShift = 4;

    std::array<WheelAct, 8> wheel{};
    std::array<DragAct, 8> drag{};

    // The scheme as decided on 23.11: plain wheel scrolls the pitch, Alt+wheel the time.
    static MouseScheme presetA()
    {
        MouseScheme s;
        s.wheel[0] = WheelAct::ScrollV;
        s.wheel[kAlt] = WheelAct::ScrollH;
        s.wheel[kCtrl] = WheelAct::ZoomV;
        s.wheel[kCtrl | kAlt] = WheelAct::ZoomH;
        s.drag[kCtrl | kShift] = DragAct::Pan;
        s.drag[kCtrl | kAlt] = DragAct::Zoom;
        return s;
    }

    // Applies overrides: "w2=zv d6=pan". Later entries win; unknown entries are ignored.
    void apply(const std::string& text)
    {
        std::istringstream in(text);
        std::string item;
        while (in >> item) {
            const size_t eq = item.find('=');
            if (eq != 2) continue; // "w2=...": a letter and one digit, then "="
            const char kind = item[0];
            const int mask = item[1] - '0';
            if (mask < 0 || mask > 7) continue;
            const std::string act = item.substr(eq + 1);
            if (kind == 'w') {
                const auto a = wheelFrom(act);
                if (a) wheel[(size_t)mask] = *a;
            } else if (kind == 'd') {
                const auto a = dragFrom(act);
                if (a) drag[(size_t)mask] = *a;
            }
        }
    }

    static int maskOf(bool alt, bool ctrl, bool shift)
    {
        return (alt ? kAlt : 0) | (ctrl ? kCtrl : 0) | (shift ? kShift : 0);
    }

private:
    static bool is(const std::string& s, const char* w) { return s == w; }
    static std::optional<WheelAct> wheelFrom(const std::string& s)
    {
        if (is(s, "v")) return WheelAct::ScrollV;
        if (is(s, "h")) return WheelAct::ScrollH;
        if (is(s, "zv")) return WheelAct::ZoomV;
        if (is(s, "zh")) return WheelAct::ZoomH;
        if (is(s, "none")) return WheelAct::None;
        return std::nullopt;
    }
    static std::optional<DragAct> dragFrom(const std::string& s)
    {
        if (is(s, "pan")) return DragAct::Pan;
        if (is(s, "zoom")) return DragAct::Zoom;
        if (is(s, "none")) return DragAct::None;
        return std::nullopt;
    }
};

} // namespace glina::ui
