// Interface between the note editor and whatever owns the audio: the
// standalone app (Session) or the ARA plugin (one audio modification).
// Everything here runs on the message thread.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "glina/Analysis.h"
#include "glina/Document.h"

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

    // Fired (asynchronously) when analysis/render finished or the clip changed.
    juce::ChangeBroadcaster changes;
};

} // namespace glina::ui
