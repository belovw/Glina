// Glina standalone: open a vocal take, edit notes, add harmonies, export.
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include "../ui/PitchEditor.h"
#include "Session.h"
#include "glina/Editing.h"

namespace glina::app {

using ui::T;

class MainComponent final : public juce::Component,
                            public juce::FileDragAndDropTarget,
                            private juce::Timer,
                            private juce::ChangeListener {
public:
    MainComponent()
    {
        setLookAndFeel(&lnf_);
        auto button = [this](juce::TextButton& b, const char* text, const char* tip, std::function<void()> fn) {
            b.setButtonText(T(text));
            b.setTooltip(T(tip));
            b.onClick = std::move(fn);
            b.setWantsKeyboardFocus(false);
            addAndMakeVisible(b);
        };
        button(openBtn_, "Открыть", "Открыть аудио или проект (Ctrl+O)", [this] { chooseOpen(); });
        button(saveBtn_, "Сохранить", "Сохранить проект (Ctrl+S)", [this] { save(false); });
        button(exportBtn_, "Экспорт", "Экспорт микса или дорожек (Ctrl+E)", [this] { exportMenu(); });
        button(audioBtn_, "Звук…", "Аудиоустройство", [this] { audioSettings(); });
        title_.setColour(juce::Label::textColourId, ui::col::text);
        time_.setColour(juce::Label::textColourId, ui::col::dim);
        time_.setJustificationType(juce::Justification::centredRight);
        time_.setFont(juce::FontOptions(14.f));
        addAndMakeVisible(title_);
        addAndMakeVisible(time_);
        addAndMakeVisible(editor_);
        editor_.setHost(&session_);
        session_.changes.addChangeListener(this);

        props_.setStorageParameters([] {
            juce::PropertiesFile::Options o;
            o.applicationName = "Glina";
            o.filenameSuffix = ".settings";
            o.folderName = "Glina";
            o.osxLibrarySubFolder = "Application Support";
            return o;
        }());
        auto saved = props_.getUserSettings()->getXmlValue("audioDevice");
        deviceManager_.initialise(0, 2, saved.get(), true);
        player_.setSource(&session_);
        deviceManager_.addAudioCallback(&player_);

        setSize(1360, 820);
        startTimerHz(15);
        updateTitle();
    }

    ~MainComponent() override
    {
        session_.changes.removeChangeListener(this);
        deviceManager_.removeAudioCallback(&player_);
        player_.setSource(nullptr);
        if (auto xml = deviceManager_.createStateXml()) props_.getUserSettings()->setValue("audioDevice", xml.get());
        props_.saveIfNeeded();
        setLookAndFeel(nullptr);
    }

    Session& session() { return session_; }
    ui::PitchEditor& editor() { return editor_; }

    void open(const juce::File& f)
    {
        juce::String err;
        const bool ok = f.hasFileExtension(".glina") ? session_.openProject(f, err) : session_.openAudio(f, err);
        if (!ok) juce::AlertWindow::showAsync(juce::MessageBoxOptions().withIconType(juce::MessageBoxIconType::WarningIcon)
                                                   .withTitle("Glina").withMessage(err).withButton("OK", 0), nullptr);
        else props_.getUserSettings()->setValue("lastFolder", f.getParentDirectory().getFullPathName());
        editor_.setHost(&session_);
        updateTitle();
    }

    bool confirmClose(std::function<void()> then)
    {
        if (!session_.isDirty()) return true;
        juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                                         .withIconType(juce::MessageBoxIconType::QuestionIcon)
                                         .withTitle("Glina")
                                         .withMessage(T("Сохранить изменения в проекте?"))
                                         .withButton(T("Сохранить"), 1)
                                         .withButton(T("Не сохранять"), 2)
                                         .withButton(T("Отмена"), 0),
                                     [this, then](int r) {
                                         if (r == 1) { if (save(false, then)) return; }
                                         else if (r == 2) then();
                                     });
        return false;
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(ui::col::bg);
        g.setColour(ui::col::panel);
        g.fillRect(getLocalBounds().removeFromTop(36));
        g.setColour(ui::col::panelLine);
        g.drawHorizontalLine(35, 0.f, (float)getWidth());
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto bar = r.removeFromTop(36).reduced(6, 5);
        for (auto* b : {&openBtn_, &saveBtn_, &exportBtn_, &audioBtn_}) {
            b->setBounds(bar.removeFromLeft(b == &audioBtn_ ? 64 : 90));
            bar.removeFromLeft(4);
        }
        bar.removeFromLeft(10);
        time_.setBounds(bar.removeFromRight(150));
        title_.setBounds(bar);
        editor_.setBounds(r);
    }

    bool keyPressed(const juce::KeyPress& k) override
    {
        const bool cmd = k.getModifiers().isCommandDown() || k.getModifiers().isCtrlDown();
        const int c = juce::CharacterFunctions::toLowerCase((juce::juce_wchar)k.getKeyCode());
        if (cmd && c == 'o') { chooseOpen(); return true; }
        if (cmd && c == 's') { save(k.getModifiers().isShiftDown()); return true; }
        if (cmd && c == 'e') { exportMenu(); return true; }
        return false;
    }

    bool isInterestedInFileDrag(const juce::StringArray& files) override
    {
        for (auto& f : files)
            if (juce::File(f).hasFileExtension(".wav;.aif;.aiff;.flac;.ogg;.mp3;.glina")) return true;
        return false;
    }
    void filesDropped(const juce::StringArray& files, int, int) override
    {
        const juce::File f(files[0]);
        if (confirmClose([this, f] { open(f); })) open(f);
    }

private:
    juce::File lastFolder()
    {
        const juce::File f(props_.getUserSettings()->getValue("lastFolder"));
        return f.isDirectory() ? f : juce::File::getSpecialLocation(juce::File::userMusicDirectory);
    }

    void chooseOpen()
    {
        auto go = [this] {
            chooser_ = std::make_unique<juce::FileChooser>(T("Открыть вокал или проект"), lastFolder(),
                                                           "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3;*.glina");
            chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                  [this](const juce::FileChooser& fc) {
                                      if (fc.getResult().existsAsFile()) open(fc.getResult());
                                  });
        };
        if (confirmClose(go)) go();
    }

    bool save(bool saveAs, std::function<void()> then = {})
    {
        if (!session_.hasAudio()) return false;
        auto write = [this, then](const juce::File& f) {
            juce::String err;
            if (!session_.saveProject(f.withFileExtension(".glina"), err))
                juce::AlertWindow::showAsync(juce::MessageBoxOptions().withTitle("Glina").withMessage(err).withButton("OK", 0), nullptr);
            else if (then) then();
            updateTitle();
        };
        if (!saveAs && session_.projectFile() != juce::File()) { write(session_.projectFile()); return true; }
        chooser_ = std::make_unique<juce::FileChooser>(T("Сохранить проект"),
                                                       session_.audioFile().withFileExtension(".glina"), "*.glina");
        chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                              [write](const juce::FileChooser& fc) {
                                  if (fc.getResult() != juce::File()) write(fc.getResult());
                              });
        return true;
    }

    void exportMenu()
    {
        if (!session_.hasAudio()) return;
        juce::PopupMenu m;
        m.addItem(1, T("Микс (стерео WAV)…"));
        m.addItem(2, T("Дорожки: основной голос + каждая гармония…"));
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&exportBtn_), [this](int r) {
            if (r == 0) return;
            if (!session_.isIdle()) {
                juce::AlertWindow::showAsync(juce::MessageBoxOptions().withTitle("Glina")
                                                 .withMessage(T("Рендер ещё идёт — попробуйте через секунду.")).withButton("OK", 0), nullptr);
                return;
            }
            const juce::File def = session_.audioFile().getSiblingFile(session_.audioFile().getFileNameWithoutExtension() + "_glina.wav");
            if (r == 1) {
                chooser_ = std::make_unique<juce::FileChooser>(T("Экспорт микса"), def, "*.wav");
                chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                                      [this](const juce::FileChooser& fc) {
                                          if (fc.getResult() == juce::File()) return;
                                          juce::String err;
                                          if (!session_.exportMix(fc.getResult().withFileExtension(".wav"), err))
                                              juce::AlertWindow::showAsync(juce::MessageBoxOptions().withTitle("Glina").withMessage(err).withButton("OK", 0), nullptr);
                                      });
            } else {
                chooser_ = std::make_unique<juce::FileChooser>(T("Папка для дорожек"), session_.audioFile().getParentDirectory());
                chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                                      [this](const juce::FileChooser& fc) {
                                          if (fc.getResult() == juce::File()) return;
                                          juce::String err;
                                          if (!session_.exportStems(fc.getResult(), err))
                                              juce::AlertWindow::showAsync(juce::MessageBoxOptions().withTitle("Glina").withMessage(err).withButton("OK", 0), nullptr);
                                      });
            }
        });
    }

    void audioSettings()
    {
        auto* sel = new juce::AudioDeviceSelectorComponent(deviceManager_, 0, 0, 2, 2, false, false, true, false);
        sel->setSize(520, 380);
        juce::DialogWindow::LaunchOptions o;
        o.content.setOwned(sel);
        o.dialogTitle = T("Аудиоустройство");
        o.dialogBackgroundColour = ui::col::panel;
        o.useNativeTitleBar = true;
        o.resizable = false;
        o.launchAsync();
    }

    static juce::String fmt(double s)
    {
        s = std::max(0.0, s);
        const int m = (int)(s / 60);
        return juce::String(m) + ":" + juce::String(s - m * 60, 1).paddedLeft('0', 4);
    }

    void updateTitle()
    {
        juce::String t = session_.hasAudio() ? session_.audioFile().getFileName() : T("Glina — откройте файл (Ctrl+O) или перетащите его в окно");
        if (session_.projectFile() != juce::File()) t << "   [" << session_.projectFile().getFileName() << "]";
        if (session_.isDirty()) t << " *";
        title_.setText(t, juce::dontSendNotification);
        saveBtn_.setEnabled(session_.hasAudio());
        exportBtn_.setEnabled(session_.hasAudio());
    }

    void timerCallback() override
    {
        time_.setText(session_.hasAudio() ? fmt(std::max(0.0, session_.playheadSec())) + " / " + fmt(session_.lengthSec()) : juce::String(),
                      juce::dontSendNotification);
        if (session_.isDirty() != wasDirty_) { wasDirty_ = session_.isDirty(); updateTitle(); }
    }
    void changeListenerCallback(juce::ChangeBroadcaster*) override { updateTitle(); }

    ui::GlinaLookAndFeel lnf_;
    Session session_;
    ui::PitchEditor editor_;
    juce::AudioDeviceManager deviceManager_;
    juce::AudioSourcePlayer player_;
    juce::ApplicationProperties props_;
    std::unique_ptr<juce::FileChooser> chooser_;
    juce::TextButton openBtn_, saveBtn_, exportBtn_, audioBtn_;
    juce::Label title_, time_;
    bool wasDirty_ = false;
};

// =========================================================================================
class GlinaApplication final : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return "Glina"; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise(const juce::String& commandLine) override
    {
        window_ = std::make_unique<Window>();
        auto args = juce::StringArray::fromTokens(commandLine, true);
        args.trim();
        args.removeEmptyStrings();
        for (auto& a : args) a = a.unquoted();
        for (int i = 0; i < args.size(); ++i) {
            if (args[i] == "--screenshot" && i + 1 < args.size()) { shotFile_ = juce::File(args[++i]); continue; }
            if (args[i] == "--demo-edits") { demo_ = true; continue; }
            if (args[i] == "--selftest") { selftest_ = true; continue; }
            if (args[i] == "--size" && i + 1 < args.size()) {
                const auto wh = juce::StringArray::fromTokens(args[++i], "x", "");
                if (wh.size() == 2) window_->setSize(wh[0].getIntValue(), wh[1].getIntValue());
                continue;
            }
            const juce::File f = juce::File::getCurrentWorkingDirectory().getChildFile(args[i]);
            if (f.existsAsFile()) window_->main().open(f);
        }
        if (shotFile_ != juce::File() || selftest_) startScreenshotMode();
    }

    void shutdown() override { window_.reset(); }

    void systemRequestedQuit() override
    {
        if (window_ == nullptr || window_->main().confirmClose([this] { quit(); })) quit();
    }

    void anotherInstanceStarted(const juce::String&) override {}

private:
    class Window final : public juce::DocumentWindow {
    public:
        Window() : juce::DocumentWindow("Glina", ui::col::bg, juce::DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar(true);
            setContentOwned(new MainComponent(), true);
            setResizable(true, true);
            setResizeLimits(900, 500, 8000, 5000);
            centreWithSize(getWidth(), getHeight());
            setVisible(true);
        }
        MainComponent& main() { return *static_cast<MainComponent*>(getContentComponent()); }
        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
    };

    // Verification mode: wait for analysis + render, optionally apply demo edits, save a PNG, quit.
    void startScreenshotMode()
    {
        struct Poll final : juce::Timer {
            std::function<void()> fn;
            void timerCallback() override { fn(); }
        };
        auto* poll = new Poll();
        poll_.reset(poll);
        poll->fn = [this] {
            auto& m = window_->main();
            ++ticks_;
            if (ticks_ > 600) { quit(); return; } // 60 s safety
            if (!m.session().isIdle()) return;
            if (selftest_) { if (!selfTestStep(m)) return; selftest_ = false; if (shotFile_ == juce::File()) { quit(); return; } }
            if (demo_ && !demoDone_) {
                demoDone_ = true;
                applyDemoEdits(m);
                return;
            }
            if (++settled_ < 5) return;
            auto img = m.createComponentSnapshot(m.getLocalBounds(), true, 2.0f);
            juce::FileOutputStream out(shotFile_);
            if (out.openedOk()) { out.setPosition(0); out.truncate(); juce::PNGImageFormat().writeImageToStream(img, out); }
            quit();
        };
        poll->startTimer(100);
    }

    // End-to-end check of editor actions through the real UI paths. Returns true when finished.
    bool selfTestStep(MainComponent& m)
    {
        Session& s = m.session();
        auto& ed = m.editor();
        auto notes = [&s] { return s.analysis() != nullptr ? (int)s.analysis()->notes.size() : -1; };
        auto key = [&ed](int code, juce::ModifierKeys mods = {}, juce::juce_wchar ch = 0) { ed.keyPressed(juce::KeyPress(code, mods, ch)); };
        auto check = [this](bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failures_; };
        const auto cmd = juce::ModifierKeys::commandModifier;
        switch (step_++) {
            case 0: n0_ = notes(); std::printf("notes: %d\n", n0_); check(n0_ > 4, "analysis found notes"); break;
            case 1: ed.setSelection({1}); key('q', {}, 'q'); break;
            case 2: {
                const Analysis* a = s.analysis();
                const double c = a->notes[1].centerMidi + s.document().edits.notes[1].pitchOffset;
                check(std::abs(c - s.document().target(c, *a)) < 1e-6, "Q snaps the note to the scale");
                key(juce::KeyPress::upKey);
                break;
            }
            case 3: {
                const Analysis* a = s.analysis();
                const double c = a->notes[1].centerMidi + s.document().edits.notes[1].pitchOffset;
                check(std::abs(c - s.document().target(c - 1.0, *a) - 1.0) < 1e-6, "Up moves the note one semitone");
                key('z', cmd, 'z');
                break;
            }
            case 4: {
                const Analysis* a = s.analysis();
                const double c = a->notes[1].centerMidi + s.document().edits.notes[1].pitchOffset;
                check(std::abs(c - s.document().target(c, *a)) < 1e-6, "Ctrl+Z undoes the move");
                // Find two notes close enough to join.
                joinAt_ = -1;
                for (int i = 0; i + 1 < notes(); ++i)
                    if (a->notes[(size_t)i + 1].startFrame - a->notes[(size_t)i].endFrame <= 8) { joinAt_ = i; break; }
                if (joinAt_ < 0) { std::printf("SKIP join (no adjacent notes)\n"); step_ = 8; break; }
                ed.setSelection({joinAt_, joinAt_ + 1});
                key('j', {}, 'j');
                break;
            }
            case 5: check(notes() == n0_ - 1, "J joins two notes"); key('z', cmd, 'z'); break;
            case 6: check(notes() == n0_, "undo restores both notes"); key('z', cmd | juce::ModifierKeys::shiftModifier, 'z'); break;
            case 7: check(notes() == n0_ - 1, "redo joins again"); key('z', cmd, 'z'); break;
            case 8: {
                ed.setSelection({2});
                for (auto* c : ed.getChildren())
                    if (auto* b = dynamic_cast<juce::TextButton*>(c); b != nullptr && b->getButtonText() == T("Разрезать")) b->triggerClick();
                break;
            }
            case 9: check(notes() == n0_ + 1, "split button splits the selected note"); key('z', cmd, 'z'); break;
            case 10: {
                check(notes() == n0_, "undo of split");
                for (auto* c : ed.getChildren())
                    for (auto* cc : c->getChildren())
                        if (auto* b = dynamic_cast<juce::TextButton*>(cc); b != nullptr && b->getButtonText() == T("+ Голос")) b->triggerClick();
                break;
            }
            case 11: {
                check(s.document().voices.size() == 1 && s.document().voices[0].degrees == 2, "+ Голос adds a third above");
                check(!s.isRendering(), "render finished");
                juce::String err;
                const juce::File tmp = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("glina_selftest.glina");
                check(s.saveProject(tmp, err), "project saves");
                Document loaded;
                const juce::String text = tmp.loadFileAsString();
                check(deserialize(text.fromFirstOccurrenceOf("glina-document", true, false).toStdString(), loaded) &&
                          serialize(loaded) == serialize(s.document()), "project reloads identically");
                const juce::File wav = tmp.withFileExtension(".wav");
                check(s.exportMix(wav, err) && wav.getSize() > 1000, "mix exports");
                std::printf(failures_ == 0 ? "SELFTEST OK\n" : "SELFTEST: %d failure(s)\n", failures_);
                std::fflush(stdout);
                return true;
            }
            default: return true;
        }
        std::fflush(stdout);
        return false;
    }

    static void applyDemoEdits(MainComponent& m)
    {
        Session& s = m.session();
        const Analysis* a = s.analysis();
        if (a == nullptr) return;
        Document& d = s.document();
        const Document before = d;
        for (size_t i = 0; i < a->notes.size(); ++i) {
            const double c = a->notes[i].centerMidi;
            d.edits.notes[i].pitchOffset = d.target(c, *a) - c;
            d.edits.notes[i].driftAmount = 0.6;
        }
        if (a->notes.size() > 3) d.edits.notes[3].pitchOffset += 2.0;
        // Pencil: a gentle wave drawn over the middle of note 6.
        if (a->notes.size() > 5) {
            const Note& n = a->notes[5];
            const int f0 = n.startFrame + (n.endFrame - n.startFrame) / 5, f1 = n.endFrame - (n.endFrame - n.startFrame) / 5;
            std::vector<float> wave((size_t)(f1 - f0));
            const double c = d.target(n.centerMidi, *a) - 0.0;
            for (size_t k = 0; k < wave.size(); ++k) wave[k] = (float)(c + 0.35 * std::sin(k * 0.12));
            drawPitch(d.edits, *a, f0, wave);
        }
        // A consonant: passed through unprocessed (drawn grey).
        if (a->notes.size() > 2) setConsonant(d.edits, *a, 2, true);
        d.tempoBpm = 96;
        d.beatsPerBar = 4;
        HarmonyVoice v1; v1.degrees = 2; v1.pan = -0.45; v1.seed = 7919;
        HarmonyVoice v2; v2.degrees = -3; v2.pan = 0.45; v2.seed = 15838; v2.formantShift = -0.4;
        d.voices = {v1, v2};
        s.history().push(before);
        s.documentChanged(false);
        m.editor().setHost(&s);
        m.editor().setSelection({3});
    }

    std::unique_ptr<Window> window_;
    std::unique_ptr<juce::Timer> poll_;
    juce::File shotFile_;
    bool demo_ = false, demoDone_ = false, selftest_ = false;
    int step_ = 0, n0_ = 0, joinAt_ = -1, failures_ = 0;
    int ticks_ = 0, settled_ = 0;
};

} // namespace glina::app

START_JUCE_APPLICATION(glina::app::GlinaApplication)
