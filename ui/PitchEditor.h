// The note editor: piano roll with amplitude-shaped note blobs, original and
// edited pitch lines, harmony notes, an inspector for the selected notes and a
// harmony panel. Shared verbatim by the standalone app and the ARA plugin.
#pragma once

#include "ClipHost.h"
#include "Theme.h"

#include "glina/Renderer.h"

#include "MouseScheme.h"

#include <memory>
#include <set>

namespace glina::ui {

class PitchEditor final : public juce::Component, private juce::Timer, private juce::ChangeListener {
public:
    PitchEditor();
    ~PitchEditor() override;

    // The clip to edit (may be null). Call again when the host's clip changes.
    void setHost(ClipHost* host);
    // Scroll so that the given PROJECT time is in the middle of the view (false: view not ready yet).
    bool centreOnProjectSec(double projectSec);
    // Switch the shown clip (track or take) and keep the zoom, scroll and note range.
    void rebindHost(ClipHost* h);
    ClipHost* host() const { return host_; }

    void undo();
    void redo();
    void fitView();
    void setSelection(std::set<int> notes) { selection_ = std::move(notes); refresh(); }
    // Draw another track (ClipHost::tracks() id) faintly over this one.
    void showTrack(const juce::String& id, bool on);

    // ---- self-test hooks (app --selftest): roll coordinates, synthetic mouse gestures ----
    juce::Rectangle<float> noteRectForTest(int note) const;
    juce::Point<float> threadHandleForTest(int note) const; // join before `note`; (-1,-1) = none
    int hitThreadForTest(juce::Point<float> p) const;
    void zoomForTest(double pixelsPerSecond, double scrollSec);
    void setGhostShiftForTest(bool on);
    void pressThemeButtonForTest() { themeBtn_.triggerClick(); }
    void dragForTest(juce::Point<float> from, juce::Point<float> to);

    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;

private:
    class Roll;
    class Inspector;
    class Harmonies;
    class Tracks;
    class Help;
    class Navigator;

    // ---- model helpers ----------------------------------------------------------
    void refresh();        // re-read host model, rebuild curves, update panels
    // Curves only (live preview during gestures). harmonies = false while notes move in time:
    // their pitch does not change, so the harmony pitches stay until the gesture ends (commit refreshes).
    void refreshCurves(bool harmonies = true);
    Document* doc() const { return host_ != nullptr ? &host_->document() : nullptr; }
    std::vector<int> targets() const; // selection, or every note when nothing is selected
    void commit(const Document& before, bool layoutChanged = false);
    template <typename Fn> void edit(Fn&& fn, bool layoutChanged = false);
    double outSec(double inSec) const;
    double inSec(double outSec) const;
    double editedCentre(int note) const;
    std::vector<ClipHost::GridLine> gridLines(double t0, double t1) const;
    juce::String keyLabel(int tonic, bool minor) const;

    // ---- actions --------------------------------------------------------------------
    void nudge(double semis);
    void snapToScale();
    void applyCorrection();
    void resetNotes();
    void toggleDrift();
    void cycleVibrato();
    void toggleAutoVibrato(); // Shift+V: auto vibrato on (last depth, ±30 ¢ the first time) / off
    // Auto vibrato settings into the selected notes, or the track when nothing or everything is
    // selected (everything: the notes' own settings are dropped).
    void writeVibrato(Document& d, const AutoVibrato& v) const;
    void toggleConsonant();
    void syncInspector();
    void layoutInspector(); // size the inspector to its controls inside its scrolling view
    // ---- other tracks ("ghosts") --------------------------------------------------
    void refreshGhosts();
    // Links to the first shown track: the target -> reference time map found by the
    // thread alignment (transfer by the thread, step 1: nothing is changed yet).
    void toggleLinks();
    void refreshLinks();
    float linkRefMidi(double t) const; // reference pitch at time t (0 = none)
    float linkTgtMidi(double t) const; // this track's pitch at time t (0 = none)
    void addLinkPin(double tgtSec, double refSec); // a pin between this track and the reference
    void removePin(int i);
    void pollTracks();
    void switchTrack(const juce::String& id);
    void toggleTracks();
    juce::Colour trackColour(const ClipHost::TrackRef& t, int index) const;
    void cycleGrid();
    void showLocator();
    void splitAt(int note, double outSecond);
    void joinSelected();
    void selectAll();
    void erasePencilInSelection();
    void setPencil(bool on);
    void selectStep(int dir, bool extend);
    void togglePlay();
    void toggleBypass();
    void showHelp(bool);
    void showRenderMenu();
    void updateToolbar();

    void timerCallback() override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override;

    GlinaLookAndFeel lnf_;
    ClipHost* host_ = nullptr;
    const Analysis* eff_ = nullptr;
    std::vector<double> curve_;                // edited pitch per frame (MIDI, 0 = unvoiced)
    TimeMap timeMap_;
    std::vector<std::vector<double>> harm_;    // per voice: pitch per note (NaN = muted)
    float levelRef_ = -20.f;
    std::set<int> selection_;
    double lastVibDepth_ = 30; // depth Shift+V turns auto vibrato back on with
    bool fitted_ = false;
    int toolbarH_ = 40;     // height of the toolbar (it wraps onto more lines when narrow)
    double pps_ = 120, scroll_ = 0, lo_ = 48, hi_ = 72; // view: pixels/second, left edge (s), MIDI range
    bool wasBusy_ = true, wasPlaying_ = false;

    std::unique_ptr<Roll> roll_;
    std::unique_ptr<Navigator> navigator_; // overview strip under the roll: whole track, shown part
    std::unique_ptr<Inspector> inspector_;
    juce::Viewport inspView_;
    std::unique_ptr<Harmonies> harmonies_;
    std::unique_ptr<Tracks> tracks_;
    std::unique_ptr<Help> help_;

    // Square toolbar button drawn with an icon (undo/redo, track stacks).
    struct IconButton final : public juce::Button {
        std::function<void(juce::Graphics&, juce::Rectangle<float>, juce::Colour)> icon;
        bool on = false;
        IconButton() : juce::Button({}) {}
        void paintButton(juce::Graphics& g, bool over, bool down) override;
    };
    juce::TextButton playBtn_, applyBtn_, splitBtn_, joinBtn_, abBtn_, harmBtn_, helpBtn_, fitBtn_;
    IconButton undoBtn_, redoBtn_;
    juce::ComboBox keyBox_, modeBox_;
    juce::Slider corrPitch_, corrDrift_, snapSlider_;
    juce::Label snapLbl_;
    juce::Label keyLbl_, corrLbl_, corrPitchLbl_, corrDriftLbl_, status_;
    bool showHarmonies_ = true;
    bool showTracks_ = false;
    IconButton tracksBtn_;
    IconButton themeBtn_;                         // light / dark theme
    std::unique_ptr<col::ThemeWatcher> themeWatch_;
    void themeChanged();                          // after a switch (this or another window)
    IconButton shiftBtn_;   // visual +3 semitone shift of the ghosts
    bool ghostShift_ = false;                                     // ghosts drawn shifted (visual only)
    bool showLinks_ = false;
    MouseScheme scheme_;                             // wheel and drag actions by modifier keys
    int hoverPin_ = -1;                              // pin end under the pointer (Delete removes it)
    struct Links {
        bool ok = false;
        bool pinsOn = false;               // the document's pins are for this reference
        juce::String name;                 // the reference track
        std::vector<ThreadFrame> tgt, ref; // this clip's thread, the reference's (this clip's timeline)
        Warp warp;
        AlignInfo info;
        std::vector<NoteFix> fixes;        // wrong notes found through the links (step 2)
        int toFix = 0;                     // notes F would move
    };
    Links links_;
    void applyLinkFixes(); // F: move the wrong notes by whole semitones (one undo step)
    std::set<juce::String> shownTracks_;                         // ids of tracks drawn as ghosts
    struct GhostView {
        juce::String id;     // the track (ClipHost::TrackRef id)
        juce::Colour colour;
        ClipHost::Ghost g;
        juce::String name;   // track name, for the label
    };
    // Shown ghosts are moved up by 3 semitones each, relative to the previous one (visual only).
    std::vector<GhostView> ghosts_;
    juce::String editedName_;                                    // name of the track being edited
    juce::String trackSig_;                                      // detects added/removed tracks
    int trackPoll_ = 0;
    bool pencil_ = false;
    juce::TextButton pencilBtn_, gridBtn_;
    juce::TextButton renderBtn_; // plug-in: render kept in the project folder
    int gridDenom_ = 4;        // grid = note value 1/N of a whole note: 4, 8 or 16
    bool triplets_ = false;    // triplet grid: beats split in 3 (6 for 1/16)
    IconButton triBtn_;
    double lastPh_ = -2;       // last playhead seen by the timer (-2 = none)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PitchEditor)
};

} // namespace glina::ui
