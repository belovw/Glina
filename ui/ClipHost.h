// Interface between the note editor and whatever owns the audio: the
// standalone app (Session) or the ARA plugin (one audio modification).
// Everything here runs on the message thread.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "glina/Analysis.h"
#include "glina/Document.h"
#include "glina/Editing.h"
#include "glina/Renderer.h"

#include <cmath>
#include <functional>
#include <limits>

namespace glina::ui {

// Undo/redo of whole-document snapshots (a document is a few KB).
class DocumentHistory {
public:
    void push(const Document& before)
    {
        undo_.push_back(before);
        if (undo_.size() > 300) undo_.erase(undo_.begin());
        redo_.clear();
    }
    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    bool undo(Document& current)
    {
        if (undo_.empty()) return false;
        redo_.push_back(current);
        current = std::move(undo_.back());
        undo_.pop_back();
        return true;
    }
    bool redo(Document& current)
    {
        if (redo_.empty()) return false;
        undo_.push_back(current);
        current = std::move(redo_.back());
        redo_.pop_back();
        return true;
    }
    void clear() { undo_.clear(); redo_.clear(); }

private:
    std::vector<Document> undo_, redo_;
};

class ClipHost {
public:
    virtual ~ClipHost() = default;

    // ---- model -------------------------------------------------------------
    // Analysis with the user's note layout applied; null while analysing.
    virtual const Analysis* analysis() const = 0;
    virtual Document& document() = 0;
    virtual DocumentHistory& history() = 0;
    // Call after changing document(). `layoutChanged`: notes were split/joined
    // (the effective analysis must be rebuilt before analysis() is used again).
    virtual void documentChanged(bool layoutChanged) = 0;

    virtual juce::String clipName() const { return {}; }
    virtual juce::String busyText() const { return {}; }   // e.g. "Анализ…"; empty = idle
    virtual bool isRendering() const { return false; }

    // ---- transport (clip output time, seconds) ---------------------------------
    virtual double playheadSec() const = 0;               // < 0: playhead not inside this clip
    virtual bool isPlaying() const = 0;
    virtual bool canControlTransport() const { return false; }
    virtual void setPlaying(bool) {}
    virtual void setPlayheadSec(double) {}
    virtual bool isBypassed() const { return false; }     // A/B: hear the original
    virtual void setBypassed(bool) {}
    // Space / Home handled by the editor (standalone). A DAW host returns false:
    // the keys are passed on to the DAW so its own transport shortcuts work.
    virtual bool handlesTransportKeys() const { return canControlTransport(); }

    // ---- musical grid ---------------------------------------------------------
    struct GridLine {
        double sec = 0;   // clip output time
        int bar = 1;      // 1-based
        int beat = 1;     // 1-based within the bar
    };
    // Bars/beats between t0 and t1 (clip output time) from the host's tempo map;
    // empty if the host has none (the editor then uses the document's tempo, or seconds).
    virtual std::vector<GridLine> hostGrid(double /*t0*/, double /*t1*/) const { return {}; }
    virtual bool hasHostTempo() const { return false; }

    // ---- other tracks: shown faintly ("ghosts") to line parts up --------------------
    struct TrackRef {
        juce::String id;          // stable while that clip exists
        juce::String name;
        juce::Colour colour;      // transparent = none given by the host
        bool current = false;     // the clip being edited now
        bool editable = true;     // can become the edited clip (setEditedTrack)
    };
    struct GhostNote { double t0 = 0, t1 = 0, midi = 0; };
    struct Ghost {
        std::vector<GhostNote> notes;            // on THIS clip's output timeline
        std::vector<juce::Point<double>> curve;  // (time, MIDI); NaN MIDI = break in the line
    };
    // Every clip the editor could show or edit, the current one included.
    virtual std::vector<TrackRef> tracks() const { return {}; }
    virtual bool ghost(const juce::String& /*id*/, Ghost& /*out*/) const { return false; }
    virtual void setEditedTrack(const juce::String& /*id*/) {}
    // Standalone: load a reference recording to show as a ghost.
    virtual bool canOpenReference() const { return false; }
    virtual void openReference() {}

    // ---- render kept next to the DAW project (plug-in in REAPER) ----------------------
    virtual bool hasRenderOptions() const { return false; }
    virtual bool savesRender() const { return false; }      // global option, default on
    virtual void setSavesRender(bool) {}
    // Where the audio being played came from, e.g. "рендер с диска"; empty = rendered now.
    virtual juce::String renderOrigin() const { return {}; }
    virtual bool canRerender() const { return false; }      // the render is from an older engine
    virtual void rerender() {}

    // Fired (asynchronously) when analysis/render finished or the clip changed.
    juce::ChangeBroadcaster changes;
};

// Notes and edited pitch line of a clip (a, e), placed on the edited clip's
// timeline by `toLocal` (ghost output time -> edited clip output time). Only
// [lo, hi] of the ghost's own output time is used: the part the DAW plays.
inline ClipHost::Ghost buildGhost(const Analysis& a, const EditSet& e,
                                  const std::function<double(double)>& toLocal, double lo, double hi)
{
    ClipHost::Ghost g;
    const auto times = noteTimes(a, e);
    for (size_t i = 0; i < a.notes.size() && i < times.size(); ++i) {
        const double t0 = std::max(lo, times[i].outStart), t1 = std::min(hi, times[i].outEnd);
        if (t1 <= t0) continue;
        const bool shifted = i < e.notes.size() && !e.notes[i].consonant;
        g.notes.push_back({toLocal(t0), toLocal(t1), shifted ? editedCenter(a, e, (int)i) : a.notes[i].centerMidi});
    }
    const auto curve = editedPitchCurve(a, e);
    const TimeMap map = TimeMap::build(a, e);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    bool open = false;
    for (size_t k = 0; k < curve.size() && k < a.frames.size(); ++k) {
        const double t = map.inToOut(a.frameTime((int)k) * a.sampleRate) / a.sampleRate; // TimeMap works in samples
        const bool ok = a.frames[k].f0 > 0 && std::isfinite(curve[k]) && curve[k] > 0 && t >= lo && t <= hi;
        if (!ok) {
            if (open) g.curve.push_back({0.0, nan});
            open = false;
            continue;
        }
        g.curve.push_back({toLocal(t), curve[k]});
        open = true;
    }
    return g;
}

} // namespace glina::ui
