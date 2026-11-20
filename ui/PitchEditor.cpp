#include "PitchEditor.h"

#include "glina/Notes.h"
#include "glina/Editing.h"
#include "glina/PitchDetector.h"
#include "glina/Tuning.h"

#include <cmath>
#include <map>
#include <set>

namespace glina::ui {

namespace {
const char* kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
bool isBlack(int m) { const int pc = ((m % 12) + 12) % 12; return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10; }
juce::String noteName(double midi)
{
    const int m = (int)std::lround(midi);
    const int cents = (int)std::lround((midi - m) * 100);
    juce::String s = juce::String(kNoteNames[((m % 12) + 12) % 12]) + juce::String(m / 12 - 1);
    if (cents != 0) s << (cents > 0 ? " +" : " ") << cents << T("¢");
    return s;
}
bool inScale(int m, const Key& k)
{
    static const int maj[7] = {0, 2, 4, 5, 7, 9, 11}, mnr[7] = {0, 2, 3, 5, 7, 8, 10};
    const int pc = ((m - k.tonic) % 12 + 12) % 12;
    for (int s : (k.minor ? mnr : maj)) if (s == pc) return true;
    return false;
}
void quiet(juce::Component& c) { c.setWantsKeyboardFocus(false); c.setMouseClickGrabsKeyboardFocus(false); }

// Slider that edits the document: one undo step per drag, live preview while dragging.
struct EditSlider {
    juce::Slider slider;
    juce::Label label;
    std::function<void(Document&, double)> apply;
    bool dragging = false, updating = false;
    Document before;
};
} // namespace

// =====================================================================================
// Help overlay
class PitchEditor::Help final : public juce::Component {
public:
    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xe0101216));
        auto r = getLocalBounds().reduced(24, 16);
        g.setColour(col::text);
        g.setFont(juce::FontOptions(14.f));
        static const char* text =
            "МЫШЬ\n"
            "  Тянуть ноту вверх/вниз — высота (по полутонам; с Alt — плавно)\n"
            "  Тянуть ноту вбок — сдвиг во времени (соседи подстраиваются, остальное стоит)\n"
            "  Тянуть край ноты — граница с соседней нотой; выделенные ноты по обе стороны тянутся блоком\n"
            "  Карандаш (P): рисовать высоту; Shift — по полутонам; Alt или правая кнопка — стирать\n"
            "  Alt + клик по ноте — разрезать здесь;  двойной клик — к ладу\n"
            "  Рамка по пустому месту — выделение (Shift — добавить)\n"
            "  Нитка между нотами — тянуть вбок: вправо плавнее, влево резче (Shift — тоньше, двойной клик — как спето)\n"
            "  Клик по линейке — позиция воспроизведения;  правая кнопка — меню ноты\n"
            "  Колесо — прокрутка;  Ctrl+колесо — масштаб;  Shift+колесо — по высоте\n"
            "\n"
            "КЛАВИШИ\n"
            "  ↑ / ↓ — полутон (Shift — 10 центов)      Q — к ладу\n"
            "  D — убрать / вернуть дрейф       V — вибрато 100 → 50 → 0 %\n"
            "  S — разрезать под курсором       J — склеить выделенные\n"
            "  Del — сбросить правки нот        ← / → — соседняя нота (Shift — добавить)\n"
            "  Ctrl+A — все ноты    Esc — снять выделение\n"
            "  Ctrl+Z / Ctrl+Shift+Z — отмена / повтор\n"
            "  Пробел — пуск/стоп (в плагине — транспорт DAW)    B — сравнить с оригиналом (A/B)\n"
            "  C — согласная (не растягивать, высоту не менять)\n"
            "  G — линии сетки 1/4 → 1/8 → 1/16    L — показать локатор\n"
            "  T — треки: ноты других треков поверх этого, переключение редактирования\n"
            "  0 — показать всё    + / − — масштаб    H — гармонии    F1 — справка\n"
            "\n"
            "Без выделения «Применить», Q, D и V действуют на все ноты.";
        g.drawFittedText(T(text), r, juce::Justification::topLeft, 40);
    }
    void mouseDown(const juce::MouseEvent&) override { setVisible(false); }
};

// =====================================================================================
// Piano roll
class PitchEditor::Roll final : public juce::Component {
public:
    explicit Roll(PitchEditor& o) : ed(o) { setWantsKeyboardFocus(true); }

    static constexpr float kKeysW = 46, kRulerH = 20, kStripH = 6;
    juce::Rectangle<float> canvas() const
    {
        return getLocalBounds().toFloat().withTrimmedLeft(kKeysW).withTrimmedTop(kRulerH).withTrimmedBottom(kStripH);
    }
    float semi() const { return canvas().getHeight() / (float)(ed.hi_ - ed.lo_); }
    float xOf(double sec) const { return canvas().getX() + (float)((sec - ed.scroll_) * ed.pps_); }
    double secOf(float x) const { return ed.scroll_ + (x - canvas().getX()) / ed.pps_; }
    float yOf(double m) const { auto c = canvas(); return c.getY() + (float)((ed.hi_ - m) / (ed.hi_ - ed.lo_)) * c.getHeight(); }

    juce::Rectangle<float> noteRect(int i) const
    {
        const Note& n = ed.eff_->notes[(size_t)i];
        const float x0 = xOf(ed.outSec(n.startSec)), x1 = xOf(ed.outSec(n.endSec));
        const float s = semi();
        return {x0, yOf(ed.editedCentre(i)) - s * 0.5f, std::max(2.f, x1 - x0), s};
    }

    int hitNote(juce::Point<float> p) const
    {
        if (ed.eff_ == nullptr || !canvas().contains(p)) return -1;
        int best = -1;
        float bestD = 1e9f;
        for (int i = 0; i < (int)ed.eff_->notes.size(); ++i) {
            const auto r = noteRect(i).expanded(2, 0);
            if (!r.contains(p)) continue;
            const float d = std::abs(r.getCentreY() - p.y);
            if (d < bestD) { bestD = d; best = i; }
        }
        return best;
    }

    enum class Zone { Body, Left, Right };
    Zone zoneOf(int i, juce::Point<float> p) const
    {
        const auto r = noteRect(i);
        const float edge = std::min(7.f, r.getWidth() * 0.25f);
        if (p.x < r.getX() + edge) return Zone::Left;
        if (p.x > r.getRight() - edge) return Zone::Right;
        return Zone::Body;
    }

    // ---- painting ------------------------------------------------------------------
    void paint(juce::Graphics& g) override
    {
        g.fillAll(col::bg);
        const auto c = canvas();
        const float s = semi();
        Document* d = ed.doc();
        const Analysis* A = ed.eff_;
        const Key key = (A && d) ? d->key(*A) : Key{};
        const bool showScale = A && d && !d->chromatic;

        for (int m = (int)std::floor(ed.lo_); m <= (int)std::ceil(ed.hi_); ++m) {
            const float y = yOf(m + 0.5);
            g.setColour(isBlack(m) ? col::laneBlack : col::laneWhite);
            if (showScale && inScale(m, key)) g.setColour(col::laneScale.withMultipliedBrightness(isBlack(m) ? 0.92f : 1.f));
            g.fillRect(c.getX(), y, c.getWidth(), s);
            g.setColour(isBlack(m) ? col::keyBlack : col::keyWhite);
            g.fillRect(0.f, y, kKeysW - 2, s - 1);
            if (((m % 12) + 12) % 12 == 0 || s > 13) {
                g.setColour(isBlack(m) ? col::text : col::bg);
                g.setFont(juce::FontOptions(std::min(11.f, s * 0.8f)));
                g.drawText(juce::String(kNoteNames[((m % 12) + 12) % 12]) + juce::String(m / 12 - 1),
                           juce::Rectangle<float>(2, y, kKeysW - 6, s), juce::Justification::centredRight);
            }
            if (((m % 12) + 12) % 12 == 0) {
                g.setColour(col::grid);
                g.drawHorizontalLine((int)(y + s), c.getX(), c.getRight());
            }
        }

        drawRuler(g);

        if (ed.host_ == nullptr || A == nullptr || d == nullptr) {
            g.setColour(col::dim);
            g.setFont(juce::FontOptions(16.f));
            juce::String msg = ed.host_ == nullptr ? T("Откройте или перетащите сюда аудиофайл с вокалом")
                                                   : ed.host_->busyText();
            if (msg.isEmpty()) msg = T("Анализ…");
            g.drawFittedText(msg, c.toNearestInt(), juce::Justification::centred, 2);
            drawStatus(g);
            return;
        }

        {
            juce::Graphics::ScopedSaveState clip(g);
            g.reduceClipRegion(c.toNearestInt());
            drawGhosts(g);
            drawHarmonies(g, *A);
            drawNotes(g, *A, *d);
            drawCurves(g, *A);
            drawThreads(g, *d);
            if (!marquee.isEmpty()) {
                g.setColour(col::accent.withAlpha(0.12f));
                g.fillRect(marquee);
                g.setColour(col::accent.withAlpha(0.7f));
                g.drawRect(marquee, 1.f);
            }
        }
        drawStrip(g, *A);

        const double ph = ed.host_->playheadSec();
        if (ph >= 0) {
            const float x = xOf(ph);
            if (x >= c.getX() && x <= c.getRight()) {
                g.setColour(col::playhead);
                g.drawVerticalLine((int)x, 0.f, c.getBottom());
                juce::Path tri;
                tri.addTriangle(x - 5, 0, x + 5, 0, x, 7);
                g.fillPath(tri);
            }
        }
        drawStatus(g);
    }

    // Ruler + grid (output time): bars/beats when a tempo is known, otherwise seconds.
    void drawRuler(juce::Graphics& g)
    {
        const auto c = canvas();
        g.setColour(col::panel);
        g.fillRect(0.f, 0.f, (float)getWidth(), kRulerH);
        g.setFont(juce::FontOptions(10.5f));
        const double t0 = secOf(c.getX()), t1 = secOf(c.getRight());
        const auto lines = ed.gridLines(t0, t1);
        if (!lines.empty()) {
            // Spacing in pixels decides what is drawn and labelled.
            double beatPx = 1e9, barPx = 1e9;
            for (size_t k = 1; k < lines.size(); ++k) {
                const double dx = (lines[k].sec - lines[k - 1].sec) * ed.pps_;
                beatPx = std::min(beatPx, dx);
            }
            int beatsPerBar = 1;
            for (const auto& l : lines) beatsPerBar = std::max(beatsPerBar, l.beat);
            barPx = beatPx * beatsPerBar;
            const int labelEvery = std::max(1, (int)std::ceil(44.0 / std::max(1.0, barPx)));
            // Eighths / sixteenths: extra lines between the beats (beat = -1 marks them).
            // Grid = note value as a fraction of a whole note: 1/4 = beats, 1/8 and 1/16 add
            // lines between beats, 1/2 keeps every second beat, 1/1 keeps bars only.
            const int denom = ed.gridDenom_;
            const int perBeat = denom >= 8 ? denom / 4 : 1;
            std::vector<ClipHost::GridLine> all;
            for (size_t k = 0; k < lines.size(); ++k) {
                const auto& L = lines[k];
                const bool keep = denom == 4 || denom >= 8 || (denom == 2 && (L.beat - 1) % 2 == 0) ||
                                  (denom == 1 && L.beat == 1);
                if (keep) all.push_back(L);
                if (k + 1 >= lines.size() || perBeat <= 1 || !keep) continue;
                for (int j = 1; j < perBeat; ++j) {
                    ClipHost::GridLine s = L;
                    s.sec = L.sec + (lines[k + 1].sec - L.sec) * j / perBeat;
                    s.beat = -1;
                    all.push_back(s);
                }
            }
            for (const auto& l : all) {
                const float x = xOf(l.sec);
                if (x < c.getX() || x > c.getRight()) continue;
                if (l.beat < 0) { // subdivision
                    if (beatPx / perBeat < 6) continue;
                    g.setColour(col::grid.withAlpha(0.55f));
                    g.drawVerticalLine((int)x, c.getY(), c.getBottom());
                    continue;
                }
                const bool bar = l.beat == 1;
                if (!bar && beatPx < 7) continue;
                g.setColour(bar ? col::grid.brighter(0.25f) : col::grid.withAlpha(0.55f));
                g.drawVerticalLine((int)x, c.getY(), c.getBottom());
                g.setColour(col::dim);
                g.drawVerticalLine((int)x, bar ? kRulerH - 8.f : kRulerH - 4.f, kRulerH);
                juce::String label;
                if (bar && ((l.bar - 1) % labelEvery == 0)) label = juce::String(l.bar);
                else if (!bar && beatPx >= 34) label = juce::String(l.bar) + "." + juce::String(l.beat);
                if (label.isNotEmpty())
                    g.drawText(label, juce::Rectangle<float>(x + 3, 0, 50, kRulerH - 2), juce::Justification::centredLeft);
            }
            return;
        }
        const double step = ed.pps_ >= 60 ? 1.0 : ed.pps_ >= 15 ? 5.0 : ed.pps_ >= 5 ? 10.0 : 30.0;
        for (double t = std::floor(ed.scroll_ / step) * step; xOf(t) < c.getRight(); t += step) {
            const float x = xOf(t);
            if (x < c.getX()) continue;
            g.setColour(col::grid);
            g.drawVerticalLine((int)x, c.getY(), c.getBottom());
            g.setColour(col::dim);
            g.drawVerticalLine((int)x, kRulerH - 6, kRulerH);
            const int sec = (int)std::lround(t);
            g.drawText(juce::String(sec / 60) + ":" + juce::String(sec % 60).paddedLeft('0', 2),
                       juce::Rectangle<float>(x + 3, 0, 60, kRulerH - 2), juce::Justification::centredLeft);
        }
    }

    void drawStatus(juce::Graphics& g)
    {
        juce::String st;
        if (ed.host_ != nullptr) {
            st = ed.host_->busyText();
            if (st.isEmpty() && ed.eff_ != nullptr && ed.doc() != nullptr) {
                const Key k = ed.doc()->key(*ed.eff_);
                st << (int)ed.eff_->notes.size() << T(" нот · ")
                   << (ed.doc()->chromatic ? T("хроматика") : ed.keyLabel(k.tonic, k.minor));
                if (!ed.selection_.empty()) st << T(" · выделено ") << (int)ed.selection_.size();
                if (ed.host_->isRendering()) st << T(" · рендер…");
                if (ed.host_->isBypassed()) st << T(" · ОРИГИНАЛ (A/B)");
            }
        }
        if (st.isEmpty()) return;
        const juce::FontOptions font(11.5f);
        g.setFont(font);
        const float w = juce::GlyphArrangement::getStringWidth(juce::Font(font), st) + 16.f;
        auto box = getLocalBounds().toFloat().removeFromTop(kRulerH).removeFromRight(w);
        g.setColour(col::panel);
        g.fillRect(box);
        g.setColour(ed.host_ != nullptr && ed.host_->isBypassed() ? col::warn : col::dim);
        g.drawText(st, box.reduced(8, 0), juce::Justification::centredRight);
    }

    float levelOf(const Analysis& A, int f, double gainDb) const
    {
        const double db = A.frames[(size_t)f].rmsDb + gainDb;
        return (float)std::clamp((db - (ed.levelRef_ - 36.0)) / 36.0, 0.16, 1.0);
    }

    void drawNotes(juce::Graphics& g, const Analysis& A, const Document& d)
    {
        const auto c = canvas();
        const float s = semi();
        const double hopSec = (double)A.hop / A.sampleRate;
        for (int i = 0; i < (int)A.notes.size(); ++i) {
            const Note& n = A.notes[(size_t)i];
            const NoteEdit& e = d.edits.notes[(size_t)i];
            const auto box = noteRect(i);
            if (box.getRight() < c.getX() || box.getX() > c.getRight()) continue;
            const double centre = ed.editedCentre(i);
            const double dev = std::abs(centre - d.target(centre, A)) * 100.0;
            const juce::Colour fill = e.consonant ? col::dim : dev < 10 ? col::inTune : dev < 25 ? col::nearTune : col::offTune;
            const bool sel = ed.selection_.count(i) > 0;

            if (std::abs(e.pitchOffset) > 0.01) { // ghost of the original position
                g.setColour(col::dim.withAlpha(0.45f));
                g.drawRoundedRectangle(box.withY(yOf(n.centerMidi) - s * 0.3f).withHeight(s * 0.6f), 3.f, 0.8f);
            }

            // Blob shaped by the note's level, Melodyne-style.
            const float yc = box.getCentreY();
            const int len = n.endFrame - n.startFrame;
            const int stride = std::max(1, (int)(len / std::max(4.f, box.getWidth() / 2.f)));
            std::vector<juce::Point<float>> top;
            top.reserve((size_t)(len / stride + 3));
            top.push_back({box.getX(), yc});
            for (int f = n.startFrame; f < n.endFrame; f += stride) {
                const float x = xOf(ed.outSec(f * hopSec));
                top.push_back({x, s * 0.47f * levelOf(A, f, e.gainDb)});
            }
            top.push_back({box.getRight(), 0.f});
            juce::Path p;
            p.startNewSubPath(top[0].x, yc);
            for (size_t k = 1; k < top.size(); ++k) p.lineTo(top[k].x, yc - top[k].y);
            for (size_t k = top.size() - 1; k >= 1; --k) p.lineTo(top[k].x, yc + top[k].y);
            p.closeSubPath();
            g.setColour(fill.withAlpha(sel ? 0.62f : 0.36f));
            g.fillPath(p);
            g.setColour(sel ? col::select : fill.withAlpha(0.9f));
            g.strokePath(p, juce::PathStrokeType(sel ? 1.6f : 1.f));

            if (e.formantShift != 0 || e.timeOffsetSec != 0 || e.stretch != 1 || e.driftAmount != 1 ||
                e.vibratoAmount != 1) {
                g.setColour(col::text.withAlpha(0.8f));
                g.fillEllipse(box.getX() + 2, box.getY() + 1, 3.5f, 3.5f); // "has edits" dot
            }
            if (sel && box.getWidth() > 46 && s > 11) {
                g.setColour(col::text);
                g.setFont(juce::FontOptions(10.f));
                g.drawText(noteName(centre), box.translated(0, -s * 0.85f).withHeight(s * 0.5f),
                           juce::Justification::centredLeft);
            }
        }
    }

    void drawCurves(juce::Graphics& g, const Analysis& A)
    {
        const auto c = canvas();
        const double hopSec = (double)A.hop / A.sampleRate;
        const int nf = (int)A.frames.size();
        const int k0 = std::clamp((int)(ed.inSec(secOf(c.getX())) / hopSec) - 2, 0, nf);
        const int k1 = std::clamp((int)(ed.inSec(secOf(c.getRight())) / hopSec) + 3, 0, nf);
        auto draw = [&](auto valueAt, juce::Colour colr, float w) {
            juce::Path p;
            bool pen = false;
            for (int k = k0; k < k1; ++k) {
                const double m = valueAt(k);
                if (m <= 0) { pen = false; continue; }
                const juce::Point<float> pt(xOf(ed.outSec(k * hopSec)), yOf(m));
                if (!pen) p.startNewSubPath(pt); else p.lineTo(pt);
                pen = true;
            }
            g.setColour(colr);
            g.strokePath(p, juce::PathStrokeType(w));
        };
        draw([&](int k) { return A.frames[(size_t)k].midi; }, col::pitchOrig, 1.f);
        if (ed.curve_.size() == A.frames.size()) {
            draw([&](int k) { return ed.curve_[(size_t)k]; }, col::pitchEdit, 1.4f);
            // Pencil-drawn stretches on top, in their own colour.
            if (const Document* d = ed.doc(); d != nullptr && !d->edits.draw.empty()) {
                const std::vector<float> dense = drawnDense(d->edits, nf);
                draw([&](int k) { return std::isnan(dense[(size_t)k]) ? 0.0 : ed.curve_[(size_t)k]; }, col::drawn, 2.f);
            }
        }
    }

    void drawHarmonies(juce::Graphics& g, const Analysis& A)
    {
        const float s = semi();
        for (size_t j = 0; j < ed.harm_.size(); ++j) {
            const auto& hp = ed.harm_[j];
            if (hp.size() != A.notes.size()) continue;
            const juce::Colour vc = col::voice((int)j);
            for (size_t i = 0; i < hp.size(); ++i) {
                if (std::isnan(hp[i])) continue;
                const Note& n = A.notes[i];
                const float x0 = xOf(ed.outSec(n.startSec)), x1 = xOf(ed.outSec(n.endSec));
                const juce::Rectangle<float> r(x0, yOf(hp[i]) - s * 0.17f, std::max(2.f, x1 - x0), s * 0.34f);
                g.setColour(vc.withAlpha(0.22f));
                g.fillRoundedRectangle(r, 3.f);
                g.setColour(vc.withAlpha(0.75f));
                g.drawRoundedRectangle(r, 3.f, 1.f);
            }
        }
    }

    void drawStrip(juce::Graphics& g, const Analysis& A)
    {
        const auto c = canvas();
        const float y = c.getBottom();
        g.setColour(col::panel);
        g.fillRect(c.getX(), y, c.getWidth(), kStripH);
        const double hopSec = (double)A.hop / A.sampleRate;
        for (int k = 0; k < (int)A.frames.size();) {
            const FrameKind kind = A.frames[(size_t)k].kind;
            int e = k + 1;
            while (e < (int)A.frames.size() && A.frames[(size_t)e].kind == kind) ++e;
            if (kind == FrameKind::Sibilant || kind == FrameKind::Breath) {
                const float x0 = xOf(ed.outSec((k - 0.5) * hopSec)), x1 = xOf(ed.outSec((e - 0.5) * hopSec));
                if (x1 >= c.getX() && x0 <= c.getRight()) {
                    g.setColour(kind == FrameKind::Sibilant ? col::sibilant : col::breath);
                    g.fillRect(x0, y + 1, std::max(1.f, x1 - x0), kStripH - 2);
                }
            }
            k = e;
        }
    }

    // ---- mouse ---------------------------------------------------------------------
    // Transition thread: between two touching notes, an S-curve from the left note's
    // centre to the right note's centre, as wide as the transition. Dragging it
    // sideways sets the smoothness of the move into the right note (note i).
    struct Thread { float xb = 0, yL = 0, yR = 0, half = 0; };
    bool hasThread(int i) const
    {
        const Document* d = ed.doc();
        if (ed.eff_ == nullptr || d == nullptr || i < 1 || i >= (int)ed.eff_->notes.size()) return false;
        const Note& L = ed.eff_->notes[(size_t)i - 1];
        const Note& R = ed.eff_->notes[(size_t)i];
        if (R.startFrame - L.endFrame > 1) return false; // a gap: no glide to shape
        if ((size_t)i >= d->edits.notes.size() || d->edits.notes[(size_t)i].consonant ||
            d->edits.notes[(size_t)i - 1].consonant) return false;
        const auto bL = noteRect(i - 1), bR = noteRect(i);
        // Also on one line: then the thread is flat and only its handle grabs it.
        return bL.getWidth() > 6 && bR.getWidth() > 6;
    }
    Thread threadOf(int i) const
    {
        const auto bL = noteRect(i - 1), bR = noteRect(i);
        Thread t;
        t.xb = bR.getX();
        t.yL = bL.getCentreY();
        t.yR = bR.getCentreY();
        const auto& ne = ed.doc()->edits;
        const double ms = transitionWidthMs(ne.notes[(size_t)i].transition, ne.transitionMs);
        t.half = (float)std::min<double>(0.5 * ms / 1000.0 * ed.pps_, 0.5 * std::min(bL.getWidth(), bR.getWidth()));
        return t;
    }
    // The handle never sits on a note: between the two notes when there is room
    // (a step of 1.5 semitones or more), otherwise just above the join.
    juce::Point<float> threadHandle(int i) const
    {
        const auto t = threadOf(i);
        if (std::abs(t.yR - t.yL) >= semi() * 1.5f) return {t.xb, 0.5f * (t.yL + t.yR)};
        return {t.xb, std::min(noteRect(i - 1).getY(), noteRect(i).getY()) - 8.f};
    }
    int hitThread(juce::Point<float> p) const
    {
        if (ed.eff_ == nullptr || ed.doc() == nullptr) return -1;
        if (hitNote(p) >= 0) return -1; // notes (body and edges) always win over threads
        const float s = semi();
        for (int i = 1; i < (int)ed.eff_->notes.size(); ++i) {
            if (!hasThread(i)) continue;
            if (threadHandle(i).getDistanceFrom(p) <= 7.f) return i;
            // Between notes on different pitches the sloped thread itself also grabs.
            const auto t = threadOf(i);
            if (std::abs(t.yR - t.yL) < s * 0.5f) continue;
            const float top = std::min(t.yL, t.yR) + s * 0.5f, bottom = std::max(t.yL, t.yR) - s * 0.5f;
            if (bottom > top && p.y > top && p.y < bottom && std::abs(p.x - t.xb) <= std::max(5.f, t.half)) return i;
        }
        return -1;
    }

    // Other tracks, faint: note outlines and their pitch line in the track's colour.
    void drawGhosts(juce::Graphics& g)
    {
        const auto c = canvas();
        const float s = semi();
        for (const auto& [colour, gh] : ed.ghosts_) {
            for (const auto& n : gh.notes) {
                const float x0 = xOf(n.t0), x1 = xOf(n.t1);
                if (x1 < c.getX() || x0 > c.getRight()) continue;
                const juce::Rectangle<float> r(x0, yOf(n.midi) - s * 0.5f, std::max(2.f, x1 - x0), s);
                g.setColour(colour.withAlpha(0.12f));
                g.fillRoundedRectangle(r, 3.f);
                g.setColour(colour.withAlpha(0.55f));
                g.drawRoundedRectangle(r, 3.f, 1.f);
            }
            juce::Path p;
            bool open = false;
            for (const auto& pt : gh.curve) {
                const float x = std::isfinite(pt.y) ? xOf(pt.x) : 0.f;
                if (!std::isfinite(pt.y) || x < c.getX() - 40 || x > c.getRight() + 40) { open = false; continue; }
                const float y = yOf(pt.y);
                if (!open) p.startNewSubPath(x, y); else p.lineTo(x, y);
                open = true;
            }
            g.setColour(colour.withAlpha(0.75f));
            g.strokePath(p, juce::PathStrokeType(1.3f));
        }
    }

    void drawThreads(juce::Graphics& g, const Document& d)
    {
        const auto c = canvas();
        for (int i = 1; i < (int)ed.eff_->notes.size(); ++i) {
            if (!hasThread(i)) continue;
            const auto t = threadOf(i);
            if (t.xb + t.half < c.getX() || t.xb - t.half > c.getRight()) continue;
            const double sm = d.edits.notes[(size_t)i].transition;
            const bool active = i == hoverThread || (gesture == G::Smooth && pressed == i);
            juce::Path p;
            const float x0 = t.xb - std::max(0.5f, t.half), x1 = t.xb + std::max(0.5f, t.half);
            p.startNewSubPath(x0, t.yL);
            const int steps = 16;
            for (int k = 1; k <= steps; ++k) {
                const float u = (float)k / steps;
                const float w = 0.5f - 0.5f * std::cos(juce::MathConstants<float>::pi * u);
                p.lineTo(x0 + (x1 - x0) * u, t.yL + (t.yR - t.yL) * w);
            }
            const juce::Colour colr = active ? col::select : std::abs(sm - 0.5) > 1e-6 ? col::accent : col::dim;
            g.setColour(colr.withAlpha(active ? 1.f : 0.85f));
            g.strokePath(p, juce::PathStrokeType(active ? 2.2f : 1.4f));
            const auto h = threadHandle(i);
            const float joinTop = std::min(noteRect(i - 1).getY(), noteRect(i).getY());
            if (h.y < joinTop) { // handle above the join: a short stem down to it
                g.setColour(colr.withAlpha(0.7f));
                g.drawLine(h.x, h.y + 4.5f, h.x, joinTop, 1.f);
            }
            g.setColour(col::bg);
            g.fillEllipse(h.x - 4.5f, h.y - 4.5f, 9.f, 9.f);
            g.setColour(colr);
            g.drawEllipse(h.x - 4.5f, h.y - 4.5f, 9.f, 9.f, 1.5f);
            if (active) {
                g.setFont(juce::FontOptions(11.f));
                const juce::String txt = T("Переход ") + juce::String(juce::roundToInt(sm * 100)) + " %";
                const auto box = juce::Rectangle<float>(h.x + 8, h.y - 9, 92, 18);
                g.setColour(col::panel.withAlpha(0.92f));
                g.fillRoundedRectangle(box, 3.f);
                g.setColour(col::text);
                g.drawText(txt, box.reduced(4, 0), juce::Justification::centredLeft);
            }
        }
    }

    void mouseMove(const juce::MouseEvent& e) override
    {
        lastMouse = e.position;
        if (ed.pencil_ && canvas().contains(e.position)) { setMouseCursor(juce::MouseCursor::CrosshairCursor); return; }
        const int th = hitThread(e.position);
        if (th != hoverThread) { hoverThread = th; repaint(); }
        if (th >= 0) { setMouseCursor(juce::MouseCursor::DraggingHandCursor); return; } // thread: hand
        const int i = hitNote(e.position);
        if (i < 0) setMouseCursor(e.position.y < kRulerH ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
        else setMouseCursor(zoneOf(i, e.position) == Zone::Body ? juce::MouseCursor::UpDownLeftRightResizeCursor
                                                                   : juce::MouseCursor::LeftRightResizeCursor);
    }

    void mouseExit(const juce::MouseEvent&) override
    {
        if (hoverThread >= 0) { hoverThread = -1; repaint(); }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        grabKeyboardFocus();
        lastMouse = e.position;
        changed = false;
        gesture = G::None;
        if (ed.eff_ == nullptr || ed.doc() == nullptr) return;
        if (e.position.y < kRulerH && e.position.x > kKeysW) {
            gesture = G::Ruler;
            if (ed.host_->canControlTransport()) ed.host_->setPlayheadSec(std::max(0.0, secOf(e.position.x)));
            return;
        }
        const int th = hitThread(e.position);
        if (th >= 0 && !e.mods.isPopupMenu()) {
            // The thread sets the move into note `th` (selected, so the panel shows its value);
            // if `th` was already selected, all selected notes follow.
            pressed = th;
            if (!ed.selection_.count(th)) { ed.selection_ = {th}; ed.refresh(); }
            before = *ed.doc();
            start.clear();
            for (int k : ed.selection_) start[k] = ed.doc()->edits.notes[(size_t)k];
            if (e.getNumberOfClicks() >= 2) { // double click: back to the sung transition
                ed.edit([&](Document& d) {
                    for (const auto& kv : start) d.edits.notes[(size_t)kv.first].transition = 0.5;
                }, false);
                return;
            }
            gesture = G::Smooth;
            return;
        }
        if (ed.pencil_ && canvas().contains(e.position)) {
            gesture = (e.mods.isAltDown() || e.mods.isPopupMenu()) ? G::Erase : G::Draw;
            before = *ed.doc();
            lastDraw = drawPoint(e.position, e.mods.isShiftDown());
            strokeTo(lastDraw);
            return;
        }
        const int i = hitNote(e.position);
        if (e.mods.isPopupMenu()) {
            if (i >= 0 && !ed.selection_.count(i)) ed.selection_ = {i};
            ed.refresh();
            showMenu(i);
            return;
        }
        if (i < 0) {
            gesture = G::Marquee;
            selBefore = e.mods.isShiftDown() || e.mods.isCommandDown() ? ed.selection_ : std::set<int>{};
            if (selBefore.empty()) ed.selection_.clear();
            marquee = {};
            ed.refresh();
            return;
        }
        if (e.mods.isShiftDown() || e.mods.isCommandDown()) {
            if (!ed.selection_.erase(i)) ed.selection_.insert(i);
        } else if (!ed.selection_.count(i)) {
            ed.selection_ = {i};
        }
        pressed = i;
        before = *ed.doc();
        start.clear();
        for (int k : ed.selection_) start[k] = ed.doc()->edits.notes[(size_t)k];
        start[i] = ed.doc()->edits.notes[(size_t)i];
        const Zone z = zoneOf(i, e.position);
        gesture = z == Zone::Left ? G::StretchL : z == Zone::Right ? G::StretchR : G::Pending;
        ed.refresh();
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        lastMouse = e.position;
        Document* d = ed.doc();
        if (d == nullptr || ed.eff_ == nullptr) return;
        const juce::Point<float> off = e.getOffsetFromDragStart().toFloat();
        switch (gesture) {
            case G::Ruler:
                if (ed.host_->canControlTransport()) ed.host_->setPlayheadSec(std::max(0.0, secOf(e.position.x)));
                return;
            case G::Marquee: {
                marquee = juce::Rectangle<float>(e.mouseDownPosition, e.position);
                ed.selection_ = selBefore;
                for (int i = 0; i < (int)ed.eff_->notes.size(); ++i)
                    if (noteRect(i).intersects(marquee)) ed.selection_.insert(i);
                repaint();
                return;
            }
            case G::Pending:
                if (off.getDistanceFromOrigin() < 4) return;
                gesture = std::abs(off.y) >= std::abs(off.x) ? G::Pitch : G::Move;
                break;
            case G::Draw:
            case G::Erase: {
                const auto p = drawPoint(e.position, e.mods.isShiftDown());
                strokeTo(p);
                lastDraw = p;
                return;
            }
            case G::None:
                return;
            case G::Pitch:
            case G::Move:
            case G::StretchL:
            case G::StretchR:
            case G::Smooth:
                break;
        }
        const double dt = off.x / ed.pps_;
        if (gesture == G::Pitch) {
            double delta = -off.y / semi();
            if (!e.mods.isAltDown()) {
                const double c0 = ed.eff_->notes[(size_t)pressed].centerMidi + start[pressed].pitchOffset;
                delta = std::round(c0 + delta) - c0;
            }
            for (auto& [k, s0] : start) d->edits.notes[(size_t)k].pitchOffset = s0.pitchOffset + delta;
        } else if (gesture == G::Smooth) {
            // 0.7 % per pixel (Shift: 0.2 % for fine steps), relative to where the drag started.
            // Right = smoother / longer, left = sharper. 0.5 % per pixel (Shift: 0.15 %).
            const double delta = (e.mods.isShiftDown() ? 0.0015 : 0.005) * off.x;
            for (auto& [k, s0] : start)
                d->edits.notes[(size_t)k].transition = juce::jlimit(0.0, 1.0, s0.transition + delta);
            ed.syncInspector();
        } else if (gesture == G::Move) {
            // The note(s) travel with their consonant; neighbours absorb, the rest stays put.
            d->edits = before.edits;
            std::set<int> moved = ed.selection_;
            moved.insert(pressed);
            moveNotes(d->edits, *ed.eff_, moved, dt);
        } else {
            // An edge is a boundary between two notes (or two blocks of selected notes).
            d->edits = before.edits;
            moveBoundary(d->edits, *ed.eff_, gesture == G::StretchR ? pressed : pressed - 1, dt, ed.selection_,
                         gesture == G::StretchL);
        }
        changed = true;
        ed.refreshCurves();
        repaint();
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        const G g0 = gesture;
        gesture = G::None;
        marquee = {};
        if (g0 == G::Draw || g0 == G::Erase) {
            if (changed) ed.commit(before);
            changed = false;
            return;
        }
        if (g0 == G::Pending && pressed >= 0 && e.mods.isAltDown() && !changed) {
            ed.splitAt(pressed, secOf(e.position.x));
        } else if (changed) {
            ed.commit(before);
        } else {
            ed.refresh();
        }
        pressed = -1;
        changed = false;
    }

    void mouseDoubleClick(const juce::MouseEvent& e) override
    {
        const int i = hitNote(e.position);
        if (i >= 0) {
            ed.selection_ = {i};
            ed.snapToScale();
        } else if (ed.host_ != nullptr && ed.host_->canControlTransport() && canvas().contains(e.position)) {
            ed.host_->setPlayheadSec(std::max(0.0, secOf(e.position.x)));
        }
    }

    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        const float dy = w.deltaY, dx = w.deltaX;
        if (e.mods.isCommandDown() || e.mods.isCtrlDown()) {
            if (e.mods.isShiftDown()) { // vertical zoom
                const double anchor = ed.hi_ - (e.position.y - canvas().getY()) / semi();
                const double span = juce::jlimit(6.0, 60.0, (ed.hi_ - ed.lo_) * std::pow(0.85, dy * 4));
                const double frac = (ed.hi_ - anchor) / (ed.hi_ - ed.lo_);
                ed.hi_ = anchor + frac * span;
                ed.lo_ = ed.hi_ - span;
            } else {
                const double anchor = secOf(e.position.x);
                ed.pps_ = juce::jlimit(2.0, 4000.0, ed.pps_ * std::pow(1.25, dy * 4));
                ed.scroll_ = std::max(0.0, anchor - (e.position.x - canvas().getX()) / ed.pps_);
            }
        } else if (e.mods.isShiftDown()) {
            ed.lo_ += dy * 4;
            ed.hi_ += dy * 4;
        } else {
            const float d = std::abs(dx) > std::abs(dy) ? -dx : dy;
            ed.scroll_ = std::max(0.0, ed.scroll_ - d * 300.0 / ed.pps_);
        }
        repaint();
    }

    void showMenu(int i)
    {
        juce::PopupMenu m;
        const bool any = !ed.selection_.empty();
        m.addItem(1, T("К ладу\tQ"), true);
        m.addItem(2, T("Сбросить правки\tDel"), any);
        m.addItem(3, T("Разрезать здесь\tS"), i >= 0);
        m.addItem(4, T("Склеить выделенные\tJ"), any);
        m.addItem(5, T("Дрейф убрать / вернуть\tD"), true);
        m.addItem(6, T("Вибрато 100 / 50 / 0 %\tV"), true);
        m.addItem(7, T("Стереть карандаш в выделенных"), any && ed.doc() != nullptr && !ed.doc()->edits.draw.empty());
        bool allCons = any && ed.doc() != nullptr;
        if (allCons)
            for (int s : ed.selection_)
                if ((size_t)s >= ed.doc()->edits.notes.size() || !ed.doc()->edits.notes[(size_t)s].consonant) allCons = false;
        m.addItem(8, T("Согласная (не растягивать, высоту не менять)\tC"), any, allCons);
        Document* d = ed.doc();
        if (d != nullptr && !d->voices.empty() && any) {
            m.addSeparator();
            for (int j = 0; j < (int)d->voices.size(); ++j) {
                const juce::String nm = T("Гармония ") + juce::String(j + 1);
                m.addItem(10 + j, nm + T(": петь выделенные"));
                m.addItem(20 + j, nm + T(": не петь выделенные"));
            }
        }
        const float x = lastMouse.x;
        juce::Component::SafePointer<Roll> safe(this);
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition(), [safe, i, x](int r) {
            if (safe == nullptr || r == 0) return;
            PitchEditor& ed = safe->ed;
            if (r == 1) ed.snapToScale();
            else if (r == 2) ed.resetNotes();
            else if (r == 3) ed.splitAt(i, safe->secOf(x));
            else if (r == 4) ed.joinSelected();
            else if (r == 5) ed.toggleDrift();
            else if (r == 6) ed.cycleVibrato();
            else if (r == 7) ed.erasePencilInSelection();
            else if (r == 8) ed.toggleConsonant();
            else if (r >= 10 && r < 30) {
                const int j = r % 10;
                const bool on = r < 20;
                ed.edit([&](Document& d) {
                    if (j >= (int)d.voices.size() || ed.eff_ == nullptr) return;
                    auto& mask = d.voices[(size_t)j].noteMask;
                    if (mask.empty()) mask.assign(ed.eff_->notes.size(), 1);
                    for (int k : ed.selection_) if (k < (int)mask.size()) mask[(size_t)k] = on ? 1 : 0;
                });
            }
        });
    }

    // ---- pencil ---------------------------------------------------------------------
    struct DrawPt { int frame = 0; float midi = 0; };
    DrawPt drawPoint(juce::Point<float> p, bool snap) const
    {
        const Analysis& A = *ed.eff_;
        const double hopSec = (double)A.hop / A.sampleRate;
        DrawPt d;
        d.frame = (int)std::lround(ed.inSec(secOf(p.x)) / hopSec);
        double m = ed.hi_ - (p.y - canvas().getY()) / semi();
        if (snap) m = std::round(m);
        d.midi = (float)m;
        return d;
    }
    // Draw (or erase) a straight segment from the previous point to `p`.
    void strokeTo(DrawPt p)
    {
        Document* d = ed.doc();
        if (d == nullptr || ed.eff_ == nullptr) return;
        const int a = std::min(lastDraw.frame, p.frame), b = std::max(lastDraw.frame, p.frame);
        if (gesture == G::Erase) {
            eraseDrawn(d->edits, a, b + 1);
        } else {
            std::vector<float> v((size_t)(b - a + 1));
            for (int f = a; f <= b; ++f) {
                const double t = b > a ? (double)(f - lastDraw.frame) / (p.frame - lastDraw.frame) : 1.0;
                v[(size_t)(f - a)] = (float)(lastDraw.midi + (p.midi - lastDraw.midi) * std::clamp(t, 0.0, 1.0));
            }
            drawPitch(d->edits, *ed.eff_, a, v);
        }
        changed = true;
        ed.refreshCurves();
        repaint();
    }

    PitchEditor& ed;
    enum class G { None, Pending, Pitch, Move, StretchL, StretchR, Smooth, Marquee, Ruler, Draw, Erase };
    DrawPt lastDraw;
    G gesture = G::None;
    int pressed = -1;
    bool changed = false;
    Document before;
    std::map<int, NoteEdit> start;
    int hoverThread = -1;
    std::set<int> selBefore;
    juce::Rectangle<float> marquee;
    juce::Point<float> lastMouse;
};

// =====================================================================================
// Inspector: values of the selected notes, or clip-wide settings when nothing is selected.
class PitchEditor::Inspector final : public juce::Component {
public:
    explicit Inspector(PitchEditor& o) : ed(o)
    {
        title.setFont(juce::FontOptions(15.f, juce::Font::bold));
        addAndMakeVisible(title);
        info.setColour(juce::Label::textColourId, col::dim);
        addAndMakeVisible(info);

        auto btn = [this](juce::TextButton& b, const char* text, std::function<void()> fn) {
            b.setButtonText(T(text));
            b.onClick = std::move(fn);
            quiet(b);
            addAndMakeVisible(b);
        };
        btn(dn1, "−1 пт", [this] { ed.nudge(-1); });
        btn(dn10, "−10¢", [this] { ed.nudge(-0.1); });
        btn(up10, "+10¢", [this] { ed.nudge(0.1); });
        btn(up1, "+1 пт", [this] { ed.nudge(1); });
        btn(snap, "К ладу", [this] { ed.snapToScale(); });
        btn(reset, "Сбросить", [this] { ed.resetNotes(); });

        auto sel = [this](PitchEditor& e) { return std::vector<int>(e.selection_.begin(), e.selection_.end()); };
        setup(drift, "Дрейф", 0, 200, 1, " %", [this, sel](Document& d, double v) {
            for (int i : sel(ed)) d.edits.notes[(size_t)i].driftAmount = v / 100.0; });
        setup(vib, "Вибрато", 0, 200, 1, " %", [this, sel](Document& d, double v) {
            for (int i : sel(ed)) d.edits.notes[(size_t)i].vibratoAmount = v / 100.0; });
        setup(formant, "Форманта", -12, 12, 0.1, " пт", [this, sel](Document& d, double v) {
            for (int i : sel(ed)) d.edits.notes[(size_t)i].formantShift = v; });
        setup(gain, "Громкость", -30, 12, 0.1, " дБ", [this, sel](Document& d, double v) {
            for (int i : sel(ed)) d.edits.notes[(size_t)i].gainDb = v; });
        // Duration moves the note's end boundary: the next note absorbs it, the rest stays.
        setup(length, "Длительность", 25, 400, 1, " %", [this, sel](Document& d, double v) {
            if (ed.eff_ == nullptr) return;
            for (int i : sel(ed)) {
                const Note& n = ed.eff_->notes[(size_t)i];
                setNoteDuration(d.edits, *ed.eff_, i, (n.endSec - n.startSec) * v / 100.0);
            }
        });
        setup(smooth, "Плавность перехода в ноту", 0, 100, 1, " %", [this, sel](Document& d, double v) {
            for (int i : sel(ed)) d.edits.notes[(size_t)i].transition = v / 100.0; });
        smooth.slider.setTooltip(T("0 — мгновенный скачок высоты, 50 — естественно, 100 — длинное глиссандо"));

        setup(sib, "Шипящие", -30, 12, 0.5, " дБ", [](Document& d, double v) { d.edits.sibilantGainDb = v; });
        setup(breath, "Дыхание", -40, 12, 0.5, " дБ", [](Document& d, double v) { d.edits.breathGainDb = v; });
        setup(gform, "Форманта (всё)", -12, 12, 0.1, " пт", [](Document& d, double v) { d.edits.globalFormantShift = v; });
        setup(smoothAll, "Плавность переходов (все)", 0, 100, 1, " %", [](Document& d, double v) {
            for (auto& n : d.edits.notes) n.transition = v / 100.0; });
        setup(lead, "Основной голос", -40, 12, 0.5, " дБ", [](Document& d, double v) { d.leadGainDb = v; });
        setup(tempo, "Темп (0 — секунды)", 0, 300, 0.01, " BPM", [](Document& d, double v) { d.tempoBpm = v; });
        setup(beats, "Долей в такте", 1, 12, 1, "", [](Document& d, double v) { d.beatsPerBar = (int)v; });
        setup(barStart, "Начало 1-го такта", -4, 30, 0.001, " с", [](Document& d, double v) { d.barOffsetSec = v; });
        tempo.slider.setDoubleClickReturnValue(true, 0.0);
        beats.slider.setDoubleClickReturnValue(true, 4.0);
    }

    void setup(EditSlider& s, const char* name, double lo, double hi, double step, const char* suffix,
               std::function<void(Document&, double)> apply)
    {
        s.apply = std::move(apply);
        s.label.setText(T(name), juce::dontSendNotification);
        s.label.setColour(juce::Label::textColourId, col::dim);
        s.slider.setSliderStyle(juce::Slider::LinearHorizontal);
        s.slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 62, 20);
        s.slider.setRange(lo, hi, step);
        s.slider.setTextValueSuffix(T(suffix));
        s.slider.setDoubleClickReturnValue(true, lo < 0 && hi > 0 ? 0.0 : (lo <= 100 && hi >= 100 ? 100.0 : lo));
        s.slider.setMouseClickGrabsKeyboardFocus(false);
        EditSlider* p = &s;
        s.slider.onDragStart = [this, p] { if (Document* d = ed.doc()) { p->before = *d; p->dragging = true; } };
        s.slider.onValueChange = [this, p] {
            Document* d = ed.doc();
            if (p->updating || d == nullptr || ed.eff_ == nullptr) return;
            if (!p->dragging) p->before = *d;
            p->apply(*d, p->slider.getValue());
            if (p->dragging) { ed.refreshCurves(); ed.roll_->repaint(); }
            else ed.commit(p->before);
        };
        s.slider.onDragEnd = [this, p] {
            if (!p->dragging) return;
            p->dragging = false;
            if (ed.doc() != nullptr) ed.commit(p->before);
        };
        addAndMakeVisible(s.label);
        addAndMakeVisible(s.slider);
    }

    void update()
    {
        Document* d = ed.doc();
        const bool ready = d != nullptr && ed.eff_ != nullptr;
        const bool notes = ready && !ed.selection_.empty();
        for (auto* s : {&drift, &vib, &formant, &gain, &length, &smooth}) { s->slider.setVisible(notes); s->label.setVisible(notes); }
        for (auto* s : {&sib, &breath, &gform, &smoothAll, &lead}) { s->slider.setVisible(ready && !notes); s->label.setVisible(ready && !notes); }
        const bool ownTempo = ready && !notes && ed.host_ != nullptr && !ed.host_->hasHostTempo();
        for (auto* s : {&tempo, &beats, &barStart}) { s->slider.setVisible(ownTempo); s->label.setVisible(ownTempo); }
        for (auto* b : {&dn1, &dn10, &up10, &up1, &snap, &reset}) b->setVisible(notes);
        info.setVisible(ready);
        if (!ready) { title.setText(T("Нет данных"), juce::dontSendNotification); return; }

        auto set = [](EditSlider& s, double v) {
            if (s.dragging) return;
            s.updating = true;
            s.slider.setValue(v, juce::dontSendNotification);
            s.updating = false;
        };
        if (notes) {
            const int first = *ed.selection_.begin();
            const NoteEdit& e = d->edits.notes[(size_t)first];
            title.setText(ed.selection_.size() == 1 ? T("Нота ") + juce::String(first + 1)
                                                    : T("Выделено нот: ") + juce::String((int)ed.selection_.size()),
                          juce::dontSendNotification);
            const double c = ed.editedCentre(first);
            juce::String inf = noteName(c);
            const double cents = (e.pitchOffset) * 100.0;
            if (std::abs(cents) >= 0.5) inf << T("   (сдвиг ") << (cents > 0 ? "+" : "") << juce::roundToInt(cents) << T("¢)");
            info.setText(inf, juce::dontSendNotification);
            set(drift, e.driftAmount * 100);
            set(vib, e.vibratoAmount * 100);
            set(formant, e.formantShift);
            set(gain, e.gainDb);
            set(length, e.stretch * 100);
            set(smooth, e.transition * 100);
        } else {
            title.setText(T("Весь клип"), juce::dontSendNotification);
            info.setText(T("Выделите ноты, чтобы править их"), juce::dontSendNotification);
            set(sib, d->edits.sibilantGainDb);
            set(breath, d->edits.breathGainDb);
            set(gform, d->edits.globalFormantShift);
            set(smoothAll, d->edits.notes.empty() ? 50.0 : d->edits.notes.front().transition * 100);
            set(lead, d->leadGainDb);
            set(tempo, d->tempoBpm);
            set(beats, d->beatsPerBar);
            set(barStart, d->barOffsetSec);
        }
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(col::panel);
        g.setColour(col::panelLine);
        g.drawHorizontalLine(0, 0.f, (float)getWidth());
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced(10, 8);
        title.setBounds(r.removeFromTop(22));
        info.setBounds(r.removeFromTop(20));
        r.removeFromTop(6);
        if (dn1.isVisible()) {
            auto row = r.removeFromTop(26);
            const int w = row.getWidth() / 4;
            dn1.setBounds(row.removeFromLeft(w).reduced(1));
            dn10.setBounds(row.removeFromLeft(w).reduced(1));
            up10.setBounds(row.removeFromLeft(w).reduced(1));
            up1.setBounds(row.reduced(1));
            r.removeFromTop(4);
            row = r.removeFromTop(26);
            snap.setBounds(row.removeFromLeft(row.getWidth() / 2).reduced(1));
            reset.setBounds(row.reduced(1));
            r.removeFromTop(8);
        }
        auto place = [&r](EditSlider& s) {
            if (!s.slider.isVisible()) return;
            s.label.setBounds(r.removeFromTop(18));
            s.slider.setBounds(r.removeFromTop(24));
            r.removeFromTop(4);
        };
        for (auto* s : {&drift, &vib, &formant, &gain, &length, &smooth, &sib, &breath, &gform, &smoothAll, &lead, &tempo,
                        &beats, &barStart})
            place(*s);
    }

    PitchEditor& ed;
    juce::Label title, info;
    juce::TextButton dn1, dn10, up10, up1, snap, reset;
    EditSlider drift, vib, formant, gain, length, smooth, sib, breath, gform, smoothAll, lead, tempo, beats, barStart;
};

// =====================================================================================
// Harmony voices
class PitchEditor::Harmonies final : public juce::Component {
public:
    static constexpr int kHeader = 30, kRow = 30, kMaxVoices = 4;

    struct Row {
        juce::ToggleButton on;
        juce::ComboBox interval;
        EditSlider formant, gain, pan, delay, detune;
        juce::TextButton all, sel, remove;
    };

    explicit Harmonies(PitchEditor& o) : ed(o)
    {
        title.setText(T("Гармонии"), juce::dontSendNotification);
        title.setFont(juce::FontOptions(14.f, juce::Font::bold));
        addAndMakeVisible(title);
        hint.setColour(juce::Label::textColourId, col::dim);
        hint.setText(T("Интервалы по ладу: терция будет большой или малой в зависимости от ноты. "
                       "Цветные полоски на нотах — партии голосов."),
                     juce::dontSendNotification);
        addAndMakeVisible(hint);
        add.setButtonText(T("+ Голос"));
        quiet(add);
        add.onClick = [this] {
            ed.edit([](Document& d) {
                if (d.voices.size() >= (size_t)kMaxVoices) return;
                static const int deg[] = {2, -2, 4, -3};
                static const double pan[] = {-0.45, 0.45, -0.75, 0.75};
                HarmonyVoice v;
                const size_t j = d.voices.size();
                v.degrees = deg[j];
                v.pan = pan[j];
                v.seed = (uint32_t)(j + 1) * 7919u;
                v.formantShift = deg[j] > 0 ? 0.3 : -0.4;
                d.voices.push_back(v);
            });
        };
        addAndMakeVisible(add);
    }

    int preferredHeight() const
    {
        const Document* d = ed.doc();
        return kHeader + kRow * (d != nullptr ? (int)d->voices.size() : 0) + 6;
    }

    void setupSlider(Row& row, EditSlider& s, int j, const char* name, double lo, double hi, double step,
                     const char* suffix, std::function<void(HarmonyVoice&, double)> fn)
    {
        s.apply = [j, fn](Document& d, double v) { if (j < (int)d.voices.size()) fn(d.voices[(size_t)j], v); };
        s.label.setText(T(name), juce::dontSendNotification);
        s.label.setColour(juce::Label::textColourId, col::dim);
        s.label.setJustificationType(juce::Justification::centredRight);
        s.slider.setSliderStyle(juce::Slider::LinearHorizontal);
        s.slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 54, 20);
        s.slider.setRange(lo, hi, step);
        s.slider.setTextValueSuffix(T(suffix));
        s.slider.setMouseClickGrabsKeyboardFocus(false);
        EditSlider* p = &s;
        s.slider.onDragStart = [this, p] { if (Document* d = ed.doc()) { p->before = *d; p->dragging = true; } };
        s.slider.onValueChange = [this, p] {
            Document* d = ed.doc();
            if (p->updating || d == nullptr) return;
            if (!p->dragging) p->before = *d;
            p->apply(*d, p->slider.getValue());
            if (p->dragging) { ed.refreshCurves(); ed.roll_->repaint(); }
            else ed.commit(p->before);
        };
        s.slider.onDragEnd = [this, p] {
            if (!p->dragging) return;
            p->dragging = false;
            if (ed.doc() != nullptr) ed.commit(p->before);
        };
        addAndMakeVisible(s.label);
        addAndMakeVisible(s.slider);
        juce::ignoreUnused(row);
    }

    void buildRows(int count)
    {
        rows.clear();
        for (int j = 0; j < count; ++j) {
            auto row = std::make_unique<Row>();
            Row& r = *row;
            r.on.setColour(juce::ToggleButton::tickColourId, col::voice(j));
            r.on.setButtonText(juce::String(j + 1));
            quiet(r.on);
            r.on.onClick = [this, j, &r] {
                const bool v = r.on.getToggleState();
                ed.edit([j, v](Document& d) { if (j < (int)d.voices.size()) d.voices[(size_t)j].enabled = v; });
            };
            addAndMakeVisible(r.on);
            static const int degs[] = {7, 5, 4, 3, 2, -2, -3, -4, -5, -7};
            for (int k = 0; k < 10; ++k) {
                HarmonyVoice hv;
                hv.degrees = degs[k];
                r.interval.addItem(juce::String::fromUTF8(harmonyIntervalName(hv).c_str()), 100 + degs[k]);
            }
            quiet(r.interval);
            r.interval.onChange = [this, j, &r] {
                const int deg = r.interval.getSelectedId() - 100;
                ed.edit([j, deg](Document& d) { if (j < (int)d.voices.size()) d.voices[(size_t)j].degrees = deg; });
            };
            addAndMakeVisible(r.interval);
            setupSlider(r, r.formant, j, "Форм.", -6, 6, 0.1, "", [](HarmonyVoice& v, double x) { v.formantShift = x; });
            setupSlider(r, r.gain, j, "Гр.", -30, 6, 0.5, "", [](HarmonyVoice& v, double x) { v.gainDb = x; });
            setupSlider(r, r.pan, j, "Пан.", -100, 100, 1, "", [](HarmonyVoice& v, double x) { v.pan = x / 100.0; });
            setupSlider(r, r.delay, j, "Задерж.", 0, 60, 1, "", [](HarmonyVoice& v, double x) { v.delayMs = x; });
            setupSlider(r, r.detune, j, "Разброс", 0, 30, 1, "", [](HarmonyVoice& v, double x) { v.detuneCents = x; });
            r.formant.slider.setTooltip(T("Форманта, полутоны: минус — темнее/крупнее голос"));
            r.gain.slider.setTooltip(T("Громкость, дБ"));
            r.pan.slider.setTooltip(T("Панорама: −100 лево, +100 право"));
            r.delay.slider.setTooltip(T("Задержка, мс — естественное «отставание» дабла"));
            r.detune.slider.setTooltip(T("Случайный разброс интонации, центы"));
            auto button = [this](juce::TextButton& b, const char* text, std::function<void()> fn) {
                b.setButtonText(T(text));
                quiet(b);
                b.onClick = std::move(fn);
                addAndMakeVisible(b);
            };
            button(r.all, "Все ноты", [this, j] {
                ed.edit([j](Document& d) { if (j < (int)d.voices.size()) d.voices[(size_t)j].noteMask.clear(); });
            });
            button(r.sel, "Только выдел.", [this, j] {
                if (ed.eff_ == nullptr || ed.selection_.empty()) return;
                const size_t n = ed.eff_->notes.size();
                ed.edit([this, j, n](Document& d) {
                    if (j >= (int)d.voices.size()) return;
                    auto& m = d.voices[(size_t)j].noteMask;
                    m.assign(n, 0);
                    for (int k : ed.selection_) if (k < (int)n) m[(size_t)k] = 1;
                });
            });
            button(r.remove, "✕", [this, j] {
                // Deferred: removing the voice destroys this very button.
                juce::Component::SafePointer<Harmonies> safe(this);
                juce::MessageManager::callAsync([safe, j] {
                    if (safe == nullptr) return;
                    safe->ed.edit([j](Document& d) { if (j < (int)d.voices.size()) d.voices.erase(d.voices.begin() + j); });
                });
            });
            rows.push_back(std::move(row));
        }
        resized();
    }

    void update()
    {
        Document* d = ed.doc();
        const int n = d != nullptr ? (int)d->voices.size() : 0;
        if (n != (int)rows.size()) buildRows(n);
        add.setEnabled(d != nullptr && n < kMaxVoices);
        for (int j = 0; j < n; ++j) {
            const HarmonyVoice& v = d->voices[(size_t)j];
            Row& r = *rows[(size_t)j];
            r.on.setToggleState(v.enabled, juce::dontSendNotification);
            r.interval.setSelectedId(100 + v.degrees, juce::dontSendNotification);
            auto set = [](EditSlider& s, double x) {
                if (s.dragging) return;
                s.updating = true;
                s.slider.setValue(x, juce::dontSendNotification);
                s.updating = false;
            };
            set(r.formant, v.formantShift);
            set(r.gain, v.gainDb);
            set(r.pan, v.pan * 100);
            set(r.delay, v.delayMs);
            set(r.detune, v.detuneCents);
            r.all.setToggleState(v.noteMask.empty(), juce::dontSendNotification);
            r.all.setColour(juce::TextButton::buttonColourId, v.noteMask.empty() ? juce::Colour(0xff2f6db5) : juce::Colour(0xff252930));
            r.sel.setColour(juce::TextButton::buttonColourId, !v.noteMask.empty() ? juce::Colour(0xff2f6db5) : juce::Colour(0xff252930));
        }
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(col::panel);
        g.setColour(col::panelLine);
        g.drawHorizontalLine(0, 0.f, (float)getWidth());
        for (int j = 0; j < (int)rows.size(); ++j) {
            g.setColour(col::voice(j));
            g.fillRect(0, kHeader + j * kRow + 6, 3, kRow - 10);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced(8, 0);
        auto head = r.removeFromTop(kHeader).reduced(0, 4);
        title.setBounds(head.removeFromLeft(90));
        add.setBounds(head.removeFromLeft(90));
        head.removeFromLeft(10);
        hint.setBounds(head);
        for (auto& rp : rows) {
            Row& row = *rp;
            auto line = r.removeFromTop(kRow).reduced(0, 3);
            row.on.setBounds(line.removeFromLeft(44));
            row.interval.setBounds(line.removeFromLeft(140));
            line.removeFromLeft(6);
            const int sw = juce::jlimit(70, 150, (line.getWidth() - 260) / 5 - 50);
            for (auto* s : {&row.formant, &row.gain, &row.pan, &row.delay, &row.detune}) {
                s->label.setBounds(line.removeFromLeft(54));
                s->slider.setBounds(line.removeFromLeft(sw + 50));
            }
            line.removeFromLeft(6);
            row.remove.setBounds(line.removeFromRight(30));
            row.sel.setBounds(line.removeFromRight(104).reduced(2, 0));
            row.all.setBounds(line.removeFromRight(80).reduced(2, 0));
        }
    }

    PitchEditor& ed;
    juce::Label title, hint;
    juce::TextButton add;
    std::vector<std::unique_ptr<Row>> rows;
};

// =====================================================================================
// ============================================================================
// Other tracks with Glina: show them faintly over this one, or switch editing to one.
class PitchEditor::Tracks final : public juce::Component {
public:
    static constexpr int kHeader = 30, kRow = 26;

    struct Row {
        juce::String id;
        juce::Colour colour;
        juce::Label name;
        juce::ToggleButton show;
        juce::TextButton edit;
        juce::Rectangle<int> swatch;
    };

    struct Body final : public juce::Component {
        const std::vector<std::unique_ptr<Row>>* rows = nullptr;
        void paint(juce::Graphics& g) override
        {
            if (rows == nullptr) return;
            for (const auto& r : *rows) {
                g.setColour(r->colour);
                g.fillRoundedRectangle(r->swatch.toFloat(), 3.f);
            }
        }
    };

    explicit Tracks(PitchEditor& o) : ed(o)
    {
        title.setText(T("Треки"), juce::dontSendNotification);
        title.setFont(juce::FontOptions(14.f, juce::Font::bold));
        addAndMakeVisible(title);
        hint.setColour(juce::Label::textColourId, col::dim);
        addAndMakeVisible(hint);
        view.setScrollBarsShown(true, false);
        view.setViewedComponent(&body, false);
        addAndMakeVisible(view);
        ref.setButtonText(T("Открыть референс…"));
        quiet(ref);
        ref.onClick = [this] { if (ed.host_ != nullptr) ed.host_->openReference(); };
        addChildComponent(ref);
        body.rows = &rows;
    }

    // Up to kShowRows rows show at once; a longer list scrolls.
    static constexpr int kShowRows = 4;
    int preferredHeight() const { return kHeader + kRow * std::min(kShowRows, (int)rows.size()) + 6; }

    // Rebuild the rows when the set of tracks changed (never from a row's own click).
    void update()
    {
        const auto list = ed.host_ != nullptr ? ed.host_->tracks() : std::vector<ClipHost::TrackRef>{};
        juce::String s;
        for (const auto& t : list) s << t.id << '|' << t.name << '|' << (int)t.current << (int)t.editable << '|';
        ref.setVisible(ed.host_ != nullptr && ed.host_->canOpenReference());
        if (s == sig && !rows.empty() == !list.empty()) { syncToggles(); return; }
        sig = s;
        rows.clear();
        for (size_t i = 0; i < list.size(); ++i) {
            const auto& t = list[i];
            auto row = std::make_unique<Row>();
            Row& r = *row;
            r.id = t.id;
            r.colour = ed.trackColour(t, (int)i);
            r.name.setText(t.name, juce::dontSendNotification);
            r.name.setColour(juce::Label::textColourId, col::text);
            r.show.setButtonText(T("показать"));
            r.show.setTooltip(T("Ноты этого трека бледным цветом поверх редактируемого"));
            r.edit.setButtonText(t.current ? T("редактируется") : T("Редактировать"));
            r.edit.setTooltip(T("Переключить окно на этот трек"));
            r.edit.setEnabled(!t.current);
            const juce::String id = t.id;
            Row* rp = &r;
            r.show.onClick = [this, id, rp] {
                if (rp->show.getToggleState()) ed.shownTracks_.insert(id); else ed.shownTracks_.erase(id);
                ed.refreshGhosts();
                ed.roll_->repaint();
            };
            r.edit.onClick = [this, id] { ed.switchTrack(id); };
            quiet(r.show);
            quiet(r.edit);
            body.addAndMakeVisible(r.name);
            body.addAndMakeVisible(r.show);
            body.addChildComponent(r.edit);
            r.edit.setVisible(t.editable);
            rows.push_back(std::move(row));
        }
        syncToggles();
        hint.setText(list.size() > 1
                         ? T("«Показать» — ноты трека бледным цветом поверх ваших. «Редактировать» — переключить окно на трек.")
                     : ref.isVisible()
                         ? T("Откройте референс: его ноты будут видны бледным цветом поверх ваших.")
                         : T("Других item с Glina в проекте нет. Добавьте Glina как Take FX на другие item — они появятся здесь."),
                     juce::dontSendNotification);
        resized();
        repaint();
    }

    void syncToggles()
    {
        const auto list = ed.host_ != nullptr ? ed.host_->tracks() : std::vector<ClipHost::TrackRef>{};
        for (auto& r : rows) {
            bool current = false;
            for (const auto& t : list) if (t.id == r->id) current = t.current;
            r->show.setToggleState(current || ed.shownTracks_.count(r->id) > 0, juce::dontSendNotification);
            r->show.setEnabled(!current);
        }
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(col::panel);
        g.setColour(col::panelLine);
        g.drawHorizontalLine(0, 0.f, (float)getWidth());
        for (const auto& r : rows) {
            g.setColour(r->colour);
            g.fillRoundedRectangle(r->swatch.toFloat(), 3.f);
        }
    }

    void resized() override
    {
        auto head = getLocalBounds().reduced(10, 0).removeFromTop(kHeader);
        title.setBounds(head.removeFromLeft(70));
        if (ref.isVisible()) ref.setBounds(head.removeFromRight(160).reduced(0, 4));
        hint.setBounds(head);
        const auto area = getLocalBounds().withTrimmedTop(kHeader);
        view.setBounds(area);
        body.setSize(std::max(0, area.getWidth() - view.getScrollBarThickness()), kRow * (int)rows.size() + 6);
        auto r = body.getLocalBounds().reduced(10, 0);
        for (auto& row : rows) {
            auto rr = r.removeFromTop(kRow);
            row->swatch = rr.removeFromLeft(14).withSizeKeepingCentre(12, 12);
            rr.removeFromLeft(6);
            row->edit.setBounds(rr.removeFromRight(130).reduced(0, 3));
            rr.removeFromRight(8);
            row->show.setBounds(rr.removeFromRight(100));
            row->name.setBounds(rr);
        }
        body.repaint();
    }

private:
    PitchEditor& ed;
    juce::Label title, hint;
    juce::TextButton ref;
    juce::Viewport view;
    Body body;
    std::vector<std::unique_ptr<Row>> rows;
    juce::String sig;
};

PitchEditor::PitchEditor()
{
    setLookAndFeel(&lnf_);
    setWantsKeyboardFocus(true);
    roll_ = std::make_unique<Roll>(*this);
    inspector_ = std::make_unique<Inspector>(*this);
    harmonies_ = std::make_unique<Harmonies>(*this);
    tracks_ = std::make_unique<Tracks>(*this);
    help_ = std::make_unique<Help>();
    addAndMakeVisible(*roll_);
    addAndMakeVisible(*inspector_);
    addAndMakeVisible(*harmonies_);
    addChildComponent(*tracks_);
    addChildComponent(*help_);

    auto button = [this](juce::TextButton& b, const char* text, const char* tip, std::function<void()> fn) {
        b.setButtonText(T(text));
        b.setTooltip(T(tip));
        b.onClick = std::move(fn);
        quiet(b);
        addAndMakeVisible(b);
    };
    button(playBtn_, "▶", "Пуск / стоп (пробел)", [this] { togglePlay(); });
    button(undoBtn_, "Отмена", "Отменить (Ctrl+Z)", [this] { undo(); });
    button(redoBtn_, "Повтор", "Повторить (Ctrl+Shift+Z)", [this] { redo(); });
    button(applyBtn_, "Применить", "Подтянуть ноты к ладу и убрать часть дрейфа (выделенные или все)", [this] { applyCorrection(); });
    button(splitBtn_, "Разрезать", "Разрезать ноту под курсором (S) или Alt+клик", [this] {
        if (eff_ == nullptr || selection_.size() != 1) return;
        const int i = *selection_.begin();
        const Note& n = eff_->notes[(size_t)i];
        splitAt(i, outSec(0.5 * (n.startSec + n.endSec)));
    });
    button(joinBtn_, "Склеить", "Склеить выделенные соседние ноты (J)", [this] { joinSelected(); });
    button(pencilBtn_, "Карандаш", "Рисовать высоту (P). Shift — по полутонам, Alt или правая кнопка — стирать", [this] {
        setPencil(!pencil_);
    });
    button(abBtn_, "A/B", "Слушать оригинал (B)", [this] { toggleBypass(); });
    button(tracksBtn_, "Треки", "Другие треки с Glina: показать поверх или редактировать (T)", [this] { toggleTracks(); });
    button(harmBtn_, "Гармонии", "Показать/скрыть панель гармоний (H)", [this] {
        showHarmonies_ = !showHarmonies_;
        resized();
        updateToolbar();
    });
    button(fitBtn_, "Всё", "Показать весь клип (0)", [this] { fitView(); });
    button(gridBtn_, "Сетка 1/4", "Линии сетки: 1/4 → 1/8 → 1/16 (G)", [this] { cycleGrid(); });
    button(helpBtn_, "?", "Справка по мыши и клавишам (F1)", [this] { showHelp(!help_->isVisible()); });
    abBtn_.setClickingTogglesState(false);

    keyLbl_.setText(T("Лад"), juce::dontSendNotification);
    keyLbl_.setColour(juce::Label::textColourId, col::dim);
    addAndMakeVisible(keyLbl_);
    quiet(keyBox_);
    keyBox_.onChange = [this] {
        const int id = keyBox_.getSelectedId();
        if (id == 0 || doc() == nullptr) return;
        edit([id](Document& d) {
            d.chromatic = id == 50;
            if (id == 1) d.keyTonic = -1;
            else if (id >= 10 && id < 22) { d.keyTonic = id - 10; d.keyMinor = false; }
            else if (id >= 30 && id < 42) { d.keyTonic = id - 30; d.keyMinor = true; }
        });
    };
    addAndMakeVisible(keyBox_);

    auto corr = [this](juce::Slider& s, juce::Label& l, const char* name, double def, const char* tip) {
        s.setSliderStyle(juce::Slider::LinearHorizontal);
        s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 44, 20);
        s.setRange(0, 100, 1);
        s.setValue(def, juce::dontSendNotification);
        s.setTextValueSuffix("%");
        s.setTooltip(T(tip));
        s.setMouseClickGrabsKeyboardFocus(false);
        l.setText(T(name), juce::dontSendNotification);
        l.setColour(juce::Label::textColourId, col::dim);
        l.setJustificationType(juce::Justification::centredRight);
        addAndMakeVisible(s);
        addAndMakeVisible(l);
    };
    corr(corrPitch_, corrPitchLbl_, "Высота", 100, "Насколько подтянуть центр ноты к ладу");
    corr(corrDrift_, corrDriftLbl_, "Дрейф", 40, "Сколько дрейфа (плавания) внутри ноты убрать; вибрато не трогается");

    startTimerHz(30);
    setSize(1200, 640);
    updateToolbar();
}

PitchEditor::~PitchEditor()
{
    if (host_ != nullptr) host_->changes.removeChangeListener(this);
    setLookAndFeel(nullptr);
}

void PitchEditor::setHost(ClipHost* h)
{
    if (h == host_) { refresh(); return; }
    if (host_ != nullptr) host_->changes.removeChangeListener(this);
    host_ = h;
    if (host_ != nullptr) host_->changes.addChangeListener(this);
    selection_.clear();
    fitted_ = false;
    eff_ = nullptr;
    refresh();
}

juce::String PitchEditor::keyLabel(int tonic, bool minor) const
{
    return juce::String(kNoteNames[((tonic % 12) + 12) % 12]) + (minor ? T(" минор") : T(" мажор"));
}

// ---- model --------------------------------------------------------------------------
double PitchEditor::outSec(double in) const
{
    if (eff_ == nullptr || timeMap_.isIdentity()) return in;
    return timeMap_.inToOut(in * eff_->sampleRate) / eff_->sampleRate;
}

double PitchEditor::inSec(double out) const
{
    if (eff_ == nullptr || timeMap_.isIdentity()) return out;
    return timeMap_.outToIn(out * eff_->sampleRate) / eff_->sampleRate;
}

double PitchEditor::editedCentre(int i) const
{
    const Document* d = doc();
    if (eff_ == nullptr || d == nullptr || i < 0 || i >= (int)eff_->notes.size()) return 0;
    return eff_->notes[(size_t)i].centerMidi + d->edits.notes[(size_t)i].pitchOffset;
}

void PitchEditor::refreshCurves()
{
    Document* d = doc();
    if (eff_ == nullptr || d == nullptr) { curve_.clear(); harm_.clear(); timeMap_ = {}; return; }
    conform(*d, *eff_);
    curve_ = editedPitchCurve(*eff_, d->edits);
    timeMap_ = TimeMap::build(*eff_, d->edits);
    harm_.clear();
    const Key key = d->key(*eff_);
    for (const auto& v : d->voices)
        harm_.push_back(v.enabled ? harmonyNotePitches(*eff_, d->edits, key, v) : std::vector<double>{});
}

void PitchEditor::refresh()
{
    const Analysis* a = host_ != nullptr ? host_->analysis() : nullptr;
    if (a != eff_) {
        eff_ = a;
        if (eff_ != nullptr) {
            std::vector<float> lv;
            lv.reserve(eff_->frames.size());
            for (const auto& f : eff_->frames) lv.push_back(f.rmsDb);
            levelRef_ = levelReference(lv);
        }
    }
    if (eff_ != nullptr) {
        for (auto it = selection_.begin(); it != selection_.end();)
            it = *it >= (int)eff_->notes.size() ? selection_.erase(it) : std::next(it);
        if (!fitted_ && roll_->getWidth() > 0) fitView();
    } else {
        selection_.clear();
    }
    refreshCurves();

    if (Document* d = doc()) {
        int id = 1;
        if (d->chromatic) id = 50;
        else if (d->keyTonic >= 0) id = (d->keyMinor ? 30 : 10) + d->keyTonic;
        keyBox_.clear(juce::dontSendNotification);
        const Key det = eff_ != nullptr ? eff_->key : Key{};
        keyBox_.addItem(T("Авто: ") + (eff_ != nullptr ? keyLabel(det.tonic, det.minor) : T("…")), 1);
        keyBox_.addSectionHeading(T("Мажор"));
        for (int t = 0; t < 12; ++t) keyBox_.addItem(keyLabel(t, false), 10 + t);
        keyBox_.addSectionHeading(T("Минор"));
        for (int t = 0; t < 12; ++t) keyBox_.addItem(keyLabel(t, true), 30 + t);
        keyBox_.addSeparator();
        keyBox_.addItem(T("Хроматика (полутоны)"), 50);
        keyBox_.setSelectedId(id, juce::dontSendNotification);
    }

    const int hh = harmonies_->preferredHeight();
    const int th = tracks_->preferredHeight();
    inspector_->update();
    harmonies_->update();
    tracks_->update();
    refreshGhosts();
    if ((showHarmonies_ && harmonies_->getHeight() != hh) || (showTracks_ && tracks_->preferredHeight() != th)) resized();
    else inspector_->resized();
    updateToolbar();
    roll_->repaint();
}

std::vector<int> PitchEditor::targets() const
{
    std::vector<int> t;
    if (eff_ == nullptr) return t;
    if (!selection_.empty()) return {selection_.begin(), selection_.end()};
    for (int i = 0; i < (int)eff_->notes.size(); ++i) t.push_back(i);
    return t;
}

void PitchEditor::commit(const Document& before, bool layoutChanged)
{
    if (host_ == nullptr) return;
    host_->history().push(before);
    host_->documentChanged(layoutChanged);
    refresh();
}

template <typename Fn>
void PitchEditor::edit(Fn&& fn, bool layoutChanged)
{
    Document* d = doc();
    if (d == nullptr) return;
    const Document before = *d;
    fn(*d);
    if (serialize(before) == serialize(*d)) { refresh(); return; }
    commit(before, layoutChanged);
}

void PitchEditor::undo()
{
    Document* d = doc();
    if (d == nullptr) return;
    const auto spans = d->edits.spans;
    if (!host_->history().undo(*d)) return;
    const bool layout = spans.size() != d->edits.spans.size() ||
                        !std::equal(spans.begin(), spans.end(), d->edits.spans.begin(),
                                    [](const NoteSpan& a, const NoteSpan& b) { return a.start == b.start && a.end == b.end; });
    host_->documentChanged(layout);
    refresh();
}

void PitchEditor::redo()
{
    Document* d = doc();
    if (d == nullptr) return;
    const auto spans = d->edits.spans;
    if (!host_->history().redo(*d)) return;
    const bool layout = spans.size() != d->edits.spans.size() ||
                        !std::equal(spans.begin(), spans.end(), d->edits.spans.begin(),
                                    [](const NoteSpan& a, const NoteSpan& b) { return a.start == b.start && a.end == b.end; });
    host_->documentChanged(layout);
    refresh();
}

void PitchEditor::fitView()
{
    if (eff_ == nullptr || eff_->notes.empty() || roll_->getWidth() <= 0) return;
    double mn = 200, mx = 0;
    for (const auto& n : eff_->notes) { mn = std::min(mn, n.centerMidi); mx = std::max(mx, n.centerMidi); }
    for (const auto& h : harm_)
        for (double p : h) if (!std::isnan(p)) { mn = std::min(mn, p); mx = std::max(mx, p); }
    lo_ = std::floor(mn) - 4;
    hi_ = std::ceil(mx) + 4;
    if (hi_ - lo_ < 14) { const double c = 0.5 * (lo_ + hi_); lo_ = c - 7; hi_ = c + 7; }
    const double first = outSec(eff_->notes.front().startSec), last = outSec(eff_->notes.back().endSec);
    const double span = std::max(1.0, last - first + 1.0);
    pps_ = std::max(2.0, roll_->canvas().getWidth() / span);
    scroll_ = std::max(0.0, first - 0.5);
    fitted_ = true;
    roll_->repaint();
}

// ---- actions -------------------------------------------------------------------------
void PitchEditor::nudge(double semis)
{
    if (selection_.empty()) return;
    edit([&](Document& d) { for (int i : selection_) d.edits.notes[(size_t)i].pitchOffset += semis; });
}

void PitchEditor::snapToScale()
{
    if (eff_ == nullptr) return;
    edit([&](Document& d) {
        for (int i : targets()) {
            const double c = eff_->notes[(size_t)i].centerMidi;
            NoteEdit& e = d.edits.notes[(size_t)i];
            e.pitchOffset = d.target(c + e.pitchOffset, *eff_) - c;
        }
    });
}

void PitchEditor::applyCorrection()
{
    if (eff_ == nullptr) return;
    const double pa = corrPitch_.getValue() / 100.0, da = corrDrift_.getValue() / 100.0;
    edit([&](Document& d) {
        for (int i : targets()) {
            const double c = eff_->notes[(size_t)i].centerMidi;
            NoteEdit& e = d.edits.notes[(size_t)i];
            e.pitchOffset = (d.target(c, *eff_) - c) * pa;
            e.driftAmount = 1.0 - da;
        }
    });
}

void PitchEditor::resetNotes()
{
    if (selection_.empty() || eff_ == nullptr) return;
    edit([&](Document& d) {
        for (int i : selection_) {
            d.edits.notes[(size_t)i] = NoteEdit{};
            eraseDrawn(d.edits, eff_->notes[(size_t)i].startFrame, eff_->notes[(size_t)i].endFrame);
        }
    });
}

void PitchEditor::erasePencilInSelection()
{
    if (selection_.empty() || eff_ == nullptr) return;
    edit([&](Document& d) {
        for (int i : selection_) eraseDrawn(d.edits, eff_->notes[(size_t)i].startFrame, eff_->notes[(size_t)i].endFrame);
    });
}

void PitchEditor::setPencil(bool on)
{
    pencil_ = on;
    pencilBtn_.setToggleState(on, juce::dontSendNotification);
    updateToolbar();
    roll_->repaint();
}

void PitchEditor::syncInspector() { inspector_->update(); }

// ---- self-test hooks -------------------------------------------------------------------
juce::Rectangle<float> PitchEditor::noteRectForTest(int i) const { return roll_->noteRect(i); }

juce::Point<float> PitchEditor::threadHandleForTest(int i) const
{
    return roll_->hasThread(i) ? roll_->threadHandle(i) : juce::Point<float>(-1.f, -1.f);
}

int PitchEditor::hitThreadForTest(juce::Point<float> p) const { return roll_->hitThread(p); }

void PitchEditor::zoomForTest(double pixelsPerSecond, double scrollSec)
{
    pps_ = pixelsPerSecond;
    scroll_ = scrollSec;
    roll_->repaint();
}

void PitchEditor::dragForTest(juce::Point<float> from, juce::Point<float> to)
{
    auto src = juce::Desktop::getInstance().getMainMouseSource();
    const auto t0 = juce::Time::getCurrentTime();
    const auto mods = juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier);
    auto ev = [&](juce::Point<float> p, bool dragged) {
        return juce::MouseEvent(src, p, mods, 1.f, 0.f, 0.f, 0.f, 0.f, roll_.get(), roll_.get(), t0, from, t0, 1, dragged);
    };
    roll_->mouseMove(ev(from, false));
    roll_->mouseDown(ev(from, false));
    for (int k = 1; k <= 10; ++k) roll_->mouseDrag(ev(from + (to - from) * (k / 10.f), true));
    roll_->mouseUp(ev(to, true));
}

juce::Colour PitchEditor::trackColour(const ClipHost::TrackRef& t, int index) const
{
    static const juce::uint32 pal[] = {0xffe9c46a, 0xfff4a6a6, 0xffa8dadc, 0xffcdb4db, 0xffb5e48c, 0xfff7b267};
    juce::Colour c = t.colour.isTransparent() ? juce::Colour(pal[(size_t)index % 6]) : t.colour;
    if (c.getPerceivedBrightness() < 0.5f) c = c.brighter(0.8f); // dark track colours vanish on the dark roll
    return c;
}

void PitchEditor::showTrack(const juce::String& id, bool on)
{
    if (on) shownTracks_.insert(id); else shownTracks_.erase(id);
    tracks_->update();
    refreshGhosts();
    roll_->repaint();
}

void PitchEditor::refreshGhosts()
{
    ghosts_.clear();
    if (host_ == nullptr) return;
    const auto list = host_->tracks();
    for (size_t i = 0; i < list.size(); ++i) {
        const auto& t = list[i];
        if (t.current || shownTracks_.count(t.id) == 0) continue;
        ClipHost::Ghost g;
        if (host_->ghost(t.id, g)) ghosts_.emplace_back(trackColour(t, (int)i), std::move(g));
    }
}

void PitchEditor::pollTracks()
{
    const auto list = host_->tracks();
    juce::String s;
    for (const auto& t : list) s << t.id << '|' << t.name << '|' << (int)t.current << '|';
    if (s == trackSig_) return;
    trackSig_ = s;
    const int th = tracks_->preferredHeight();
    tracks_->update();
    refreshGhosts();
    if (showTracks_ && tracks_->preferredHeight() != th) resized();
    roll_->repaint();
}

void PitchEditor::toggleTracks()
{
    showTracks_ = !showTracks_;
    if (showTracks_) tracks_->update();
    resized();
    updateToolbar();
}

void PitchEditor::switchTrack(const juce::String& id)
{
    // Deferred: the click came from a row of the tracks panel, which the switch rebuilds.
    juce::Component::SafePointer<PitchEditor> self(this);
    juce::MessageManager::callAsync([self, id] {
        if (self == nullptr || self->host_ == nullptr) return;
        ClipHost* h = self->host_;
        for (const auto& t : h->tracks())
            if (t.current) self->shownTracks_.insert(t.id); // keep seeing the track we leave
        self->shownTracks_.erase(id);
        h->setEditedTrack(id);
        self->setHost(nullptr);
        self->setHost(h);
    });
}

void PitchEditor::toggleConsonant()
{
    if (eff_ == nullptr || selection_.empty()) return;
    bool allOn = true;
    for (int i : selection_)
        if (doc() == nullptr || (size_t)i >= doc()->edits.notes.size() || !doc()->edits.notes[(size_t)i].consonant) allOn = false;
    const bool on = !allOn;
    const std::set<int> sel = selection_;
    const Analysis& a = *eff_;
    edit([&](Document& d) {
        for (int i : sel) setConsonant(d.edits, a, i, on);
    }, true);
}

void PitchEditor::cycleGrid()
{
    static const int order[] = {4, 8, 16, 2, 1};
    int k = 0;
    for (int i = 0; i < 5; ++i) if (order[i] == gridDenom_) k = (i + 1) % 5;
    gridDenom_ = order[k];
    gridBtn_.setButtonText(T("Сетка 1/") + juce::String(gridDenom_));
    roll_->repaint();
}

void PitchEditor::showLocator()
{
    if (host_ == nullptr || roll_->getWidth() <= 0) return;
    const double ph = host_->playheadSec();
    if (ph < 0) return;
    const double visible = roll_->canvas().getWidth() / pps_;
    scroll_ = std::max(0.0, ph - visible * 0.5);
    roll_->repaint();
}

std::vector<ClipHost::GridLine> PitchEditor::gridLines(double t0, double t1) const
{
    std::vector<ClipHost::GridLine> out;
    if (host_ == nullptr || t1 <= t0) return out;
    out = host_->hostGrid(t0, t1);
    if (!out.empty() || host_->hasHostTempo()) return out;
    const Document* d = doc();
    if (d == nullptr || d->tempoBpm < 20) return out;
    const double beat = 60.0 / d->tempoBpm;
    const int bpb = std::max(1, d->beatsPerBar);
    const long first = (long)std::floor((t0 - d->barOffsetSec) / beat) - 1;
    const long last = (long)std::ceil((t1 - d->barOffsetSec) / beat) + 1;
    if (last - first > 20000) return out; // zoomed far out: fall back to seconds
    for (long b = first; b <= last; ++b) {
        const long bar = b >= 0 ? b / bpb : -((-b + bpb - 1) / bpb);
        ClipHost::GridLine l;
        l.sec = d->barOffsetSec + (double)b * beat;
        l.bar = (int)bar + 1;
        l.beat = (int)(b - bar * bpb) + 1;
        out.push_back(l);
    }
    return out;
}

void PitchEditor::toggleDrift()
{
    const auto t = targets();
    if (t.empty()) return;
    bool allFlat = true;
    for (int i : t) if (doc()->edits.notes[(size_t)i].driftAmount > 0.5) allFlat = false;
    edit([&](Document& d) { for (int i : t) d.edits.notes[(size_t)i].driftAmount = allFlat ? 1.0 : 0.0; });
}

void PitchEditor::cycleVibrato()
{
    const auto t = targets();
    if (t.empty()) return;
    const double cur = doc()->edits.notes[(size_t)t.front()].vibratoAmount;
    const double next = cur > 0.75 ? 0.5 : cur > 0.25 ? 0.0 : 1.0;
    edit([&](Document& d) { for (int i : t) d.edits.notes[(size_t)i].vibratoAmount = next; });
}

void PitchEditor::splitAt(int note, double outSecond)
{
    Document* d = doc();
    if (d == nullptr || eff_ == nullptr || note < 0 || note >= (int)eff_->notes.size()) return;
    const Document before = *d;
    const int frame = (int)std::lround(inSec(outSecond) * eff_->sampleRate / eff_->hop);
    if (!splitNote(*d, *eff_, note, frame)) return;
    selection_ = {note + 1};
    commit(before, true);
}

void PitchEditor::joinSelected()
{
    Document* d = doc();
    if (d == nullptr || eff_ == nullptr || selection_.empty()) return;
    std::vector<int> sel(selection_.begin(), selection_.end());
    if (sel.size() == 1) sel.push_back(sel[0] + 1); // single note: join with the next one
    const Document before = *d;
    bool any = false;
    for (int k = (int)sel.size() - 2; k >= 0; --k) {
        if (sel[(size_t)k + 1] != sel[(size_t)k] + 1) continue;
        const Analysis* a = host_->analysis();
        if (a == nullptr) break;
        if (joinNotes(*d, *a, sel[(size_t)k])) {
            any = true;
            host_->documentChanged(true); // rebuild the layout before the next join
        }
    }
    if (!any) return;
    selection_ = {sel.front()};
    host_->history().push(before);
    refresh();
}

void PitchEditor::selectAll()
{
    if (eff_ == nullptr) return;
    for (int i = 0; i < (int)eff_->notes.size(); ++i) selection_.insert(i);
    refresh();
}

void PitchEditor::selectStep(int dir, bool extend)
{
    if (eff_ == nullptr || eff_->notes.empty()) return;
    int i;
    if (selection_.empty()) i = dir > 0 ? 0 : (int)eff_->notes.size() - 1;
    else i = juce::jlimit(0, (int)eff_->notes.size() - 1, (dir > 0 ? *selection_.rbegin() : *selection_.begin()) + dir);
    if (!extend) selection_.clear();
    selection_.insert(i);
    // Keep the note visible.
    const double t0 = outSec(eff_->notes[(size_t)i].startSec), t1 = outSec(eff_->notes[(size_t)i].endSec);
    const double visible = roll_->canvas().getWidth() / pps_;
    if (t0 < scroll_ || t1 > scroll_ + visible) scroll_ = std::max(0.0, t0 - 0.2 * visible);
    refresh();
}

void PitchEditor::togglePlay()
{
    if (host_ != nullptr && host_->canControlTransport()) host_->setPlaying(!host_->isPlaying());
    updateToolbar();
}

void PitchEditor::toggleBypass()
{
    if (host_ == nullptr) return;
    host_->setBypassed(!host_->isBypassed());
    updateToolbar();
    roll_->repaint();
}

void PitchEditor::showHelp(bool show)
{
    help_->setVisible(show);
    if (show) help_->toFront(false);
}

void PitchEditor::updateToolbar()
{
    const bool ready = host_ != nullptr && eff_ != nullptr;
    playBtn_.setVisible(host_ != nullptr && host_->handlesTransportKeys());
    pencilBtn_.setEnabled(ready);
    pencilBtn_.setColour(juce::TextButton::buttonColourId, pencil_ ? col::drawn.darker(0.45f) : juce::Colour(0xff252930));
    playBtn_.setButtonText(host_ != nullptr && host_->isPlaying() ? T("■") : T("▶"));
    undoBtn_.setEnabled(ready && host_->history().canUndo());
    redoBtn_.setEnabled(ready && host_->history().canRedo());
    for (auto* c : std::initializer_list<juce::Component*>{&applyBtn_, &joinBtn_, &keyBox_, &corrPitch_, &corrDrift_, &fitBtn_})
        c->setEnabled(ready);
    splitBtn_.setEnabled(ready && selection_.size() == 1);
    abBtn_.setEnabled(host_ != nullptr);
    const bool by = host_ != nullptr && host_->isBypassed();
    abBtn_.setColour(juce::TextButton::buttonColourId, by ? col::warn.darker(0.3f) : juce::Colour(0xff252930));
    harmBtn_.setColour(juce::TextButton::buttonColourId, showHarmonies_ ? juce::Colour(0xff2f6db5) : juce::Colour(0xff252930));
    tracksBtn_.setColour(juce::TextButton::buttonColourId, showTracks_ ? juce::Colour(0xff2f6db5) : juce::Colour(0xff252930));
}

// ---- layout / paint ----------------------------------------------------------------------
void PitchEditor::paint(juce::Graphics& g)
{
    g.fillAll(col::panel);
    g.setColour(col::panelLine);
    g.drawHorizontalLine(39, 0.f, (float)getWidth());
}

void PitchEditor::resized()
{
    auto r = getLocalBounds();
    auto bar = r.removeFromTop(40).reduced(6, 7);
    // Toolbar: when the window is too narrow, the key box and the correction sliders
    // get narrower first, then the slider captions go (the sliders keep tooltips).
    struct Slot { juce::Component* c; int w, gap, minW; };
    std::vector<Slot> slots = {
        {&playBtn_, 34, 8, 34},      {&undoBtn_, 62, 4, 62},      {&redoBtn_, 62, 12, 62},
        {&keyLbl_, 30, 2, 30},       {&keyBox_, 150, 12, 96},
        {&corrPitchLbl_, 50, 2, 50}, {&corrPitch_, 118, 4, 100},
        {&corrDriftLbl_, 46, 2, 46}, {&corrDrift_, 118, 4, 100},
        {&applyBtn_, 86, 12, 86},    {&splitBtn_, 82, 4, 82},     {&joinBtn_, 74, 12, 74},
        {&pencilBtn_, 84, 12, 84},   {&gridBtn_, 84, 4, 84},      {&abBtn_, 44, 4, 44},
        {&harmBtn_, 84, 4, 84},      {&tracksBtn_, 62, 4, 62},    {&fitBtn_, 44, 4, 44},
        {&helpBtn_, 28, 4, 28}};
    int deficit = -bar.getWidth();
    for (const auto& s : slots) if (s.c->isVisible()) deficit += s.w + s.gap;
    for (auto& s : slots) {
        if (deficit <= 0) break;
        if (!s.c->isVisible()) continue;
        const int cut = std::min(deficit, s.w - s.minW);
        s.w -= cut;
        deficit -= cut;
    }
    if (deficit > 0)
        for (auto& s : slots)
            if (s.c == &corrPitchLbl_ || s.c == &corrDriftLbl_) { deficit -= s.w + s.gap; s.w = 0; s.gap = 0; }
    for (const auto& s : slots) {
        if (!s.c->isVisible()) continue;
        s.c->setBounds(bar.removeFromLeft(s.w));
        bar.removeFromLeft(s.gap);
    }

    harmonies_->setVisible(showHarmonies_);
    if (showHarmonies_) harmonies_->setBounds(r.removeFromBottom(harmonies_->preferredHeight()));
    tracks_->setVisible(showTracks_);
    if (showTracks_) tracks_->setBounds(r.removeFromBottom(tracks_->preferredHeight()));
    inspector_->setBounds(r.removeFromRight(236));
    roll_->setBounds(r);
    help_->setBounds(r);
    if (!fitted_) fitView();
}

bool PitchEditor::keyPressed(const juce::KeyPress& k)
{
    const auto mods = k.getModifiers();
    const int code = k.getKeyCode();
    const juce::juce_wchar ch = juce::CharacterFunctions::toLowerCase(k.getTextCharacter());
    const bool cmd = mods.isCommandDown() || mods.isCtrlDown();

    if (code == juce::KeyPress::F1Key || (ch == '?' && !cmd)) { showHelp(!help_->isVisible()); return true; }
    if (code == juce::KeyPress::escapeKey) {
        if (help_->isVisible()) showHelp(false);
        else if (pencil_) setPencil(false);
        else { selection_.clear(); refresh(); }
        return true;
    }
    if (cmd && (code == 'Z' || code == 'z')) { if (mods.isShiftDown()) redo(); else undo(); return true; }
    if (cmd && (code == 'Y' || code == 'y')) { redo(); return true; }
    if (cmd && (code == 'A' || code == 'a')) { selectAll(); return true; }
    if (code == juce::KeyPress::spaceKey) {
        // In a DAW the key goes back to the host, so its own transport shortcut works.
        if (host_ == nullptr || !host_->handlesTransportKeys()) return false;
        togglePlay();
        return true;
    }
    if (cmd) return false;

    if (code == juce::KeyPress::upKey || code == juce::KeyPress::downKey) {
        nudge((mods.isShiftDown() ? 0.1 : 1.0) * (code == juce::KeyPress::upKey ? 1 : -1));
        return true;
    }
    if (code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey) {
        selectStep(code == juce::KeyPress::rightKey ? 1 : -1, mods.isShiftDown());
        return true;
    }
    if (code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey) { resetNotes(); return true; }
    if (code == juce::KeyPress::homeKey) {
        scroll_ = 0;
        roll_->repaint();
        if (host_ == nullptr || !host_->handlesTransportKeys()) return false; // the DAW moves its cursor too
        host_->setPlayheadSec(0);
        return true;
    }
    switch (ch) {
        case 'q': snapToScale(); return true;
        case 'd': toggleDrift(); return true;
        case 'v': cycleVibrato(); return true;
        case 'j': joinSelected(); return true;
        case 'p': setPencil(!pencil_); return true;
        case 'c': toggleConsonant(); return true;
        case 'g': cycleGrid(); return true;
        case 'l': showLocator(); return true;
        case 'b': toggleBypass(); return true;
        case 'h': showHarmonies_ = !showHarmonies_; resized(); updateToolbar(); return true;
        case 't': toggleTracks(); return true;
        case '0': fitView(); return true;
        case '+': case '=': pps_ = std::min(4000.0, pps_ * 1.4); roll_->repaint(); return true;
        case '-': pps_ = std::max(2.0, pps_ / 1.4); roll_->repaint(); return true;
        case 's': {
            const int i = roll_->hitNote(roll_->lastMouse);
            if (i >= 0) splitAt(i, roll_->secOf(roll_->lastMouse.x));
            return true;
        }
        default: break;
    }
    return false;
}

void PitchEditor::timerCallback()
{
    if (host_ == nullptr) return;
    if (++trackPoll_ >= 30) { trackPoll_ = 0; pollTracks(); } // items added/removed in the DAW
    const bool busy = host_->analysis() == nullptr || host_->isRendering() || host_->busyText().isNotEmpty();
    const bool playing = host_->isPlaying();
    if (host_->analysis() != eff_) refresh();
    // The view follows the playhead only while it moves during playback; when stopped,
    // scrolling by hand stays where the user left it.
    const double ph = host_->playheadSec();
    const bool moved = ph != lastPh_;
    lastPh_ = ph;
    if (playing && moved && ph >= 0 && roll_->getWidth() > 0) {
        const double visible = roll_->canvas().getWidth() / pps_;
        if (ph > scroll_ + visible * 0.92 || ph < scroll_) scroll_ = std::max(0.0, ph - visible * 0.08);
    }
    if (moved || playing || busy || busy != wasBusy_) roll_->repaint();
    if (playing != wasPlaying_ || busy != wasBusy_) updateToolbar();
    wasBusy_ = busy;
    wasPlaying_ = playing;
}

void PitchEditor::changeListenerCallback(juce::ChangeBroadcaster*) { refresh(); }

} // namespace glina::ui
