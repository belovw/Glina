#include "PluginEditor.h"
#include "RenderStore.h"
#include "glina/Notes.h"

#include <ARA_Library/Utilities/ARATimelineConversion.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace glina::plugin {

using ui::T;

namespace {
// A track's Glina items, by where they start (defined with the track helpers below).
std::vector<juce::ARAPlaybackRegion*> glinaItemsOf(juce::ARARegionSequence* seq);
} // namespace

// ============================================================================
PluginClip::PluginClip(GlinaDocumentController* dc, GlinaProcessor& proc) : dc_(dc), proc_(proc)
{
    if (dc_ != nullptr) dc_->modelChanged.addChangeListener(this);
}

PluginClip::~PluginClip()
{
    setRegion(nullptr);
    if (dc_ != nullptr) dc_->modelChanged.removeChangeListener(this);
}

void PluginClip::setRegion(juce::ARAPlaybackRegion* region)
{
    auto* mod = region != nullptr ? dynamic_cast<GlinaAudioModification*>(region->getAudioModification()) : nullptr;
    if (region == region_ && mod == mod_) return;
    if (region_ != nullptr) region_->removeListener(this);
    if (mod_ != nullptr) mod_->removeListener(this);
    region_ = region;
    mod_ = mod;
    if (region_ != nullptr) { region_->addListener(this); seq_ = region_->getRegionSequence(); }
    if (mod_ != nullptr) { mod_->addListener(this); lastMod_ = mod_; }
    if (region_ != nullptr) refreshView();
    changes.sendChangeMessage();
}

const Analysis* PluginClip::analysis() const
{
    if (viewOn_) return viewPending_ ? nullptr : view_.analysis.get();
    if (mod_ == nullptr || !mod_->getGlinaSource()->data->analysisReady) return nullptr;
    return mod_->effective.get();
}

Document& PluginClip::document()
{
    if (viewOn_) return view_.doc;
    return mod_ != nullptr ? mod_->doc : dummyDoc_;
}

ui::DocumentHistory& PluginClip::history()
{
    if (viewOn_) return dummyHistory_; // the track view keeps its own steps (below)
    return mod_ != nullptr ? mod_->history : dummyHistory_;
}

void PluginClip::recordEdit(const Document& before)
{
    if (!viewOn_) { ui::ClipHost::recordEdit(before); return; }
    // The takes the edit changed were collected by documentChanged: that is one step.
    if (viewHist_ != nullptr && !openStep_.empty()) viewHist_->push(std::move(openStep_));
    openStep_.clear();
}

bool PluginClip::canUndo()
{
    if (!viewOn_) return ui::ClipHost::canUndo();
    return viewHist_ != nullptr && !viewHist_->undo.empty();
}

bool PluginClip::canRedo()
{
    if (!viewOn_) return ui::ClipHost::canRedo();
    return viewHist_ != nullptr && !viewHist_->redo.empty();
}

bool PluginClip::undoStep(bool redo)
{
    if (!viewOn_) return ui::ClipHost::undoStep(redo);
    if (viewHist_ == nullptr || dc_ == nullptr) return false;
    auto& from = redo ? viewHist_->redo : viewHist_->undo;
    auto& to = redo ? viewHist_->undo : viewHist_->redo;
    if (from.empty()) return false;
    GlinaDocumentController::TrackStep step = std::move(from.back());
    from.pop_back();
    GlinaDocumentController::TrackStep back; // the same takes as they are now: the way back
    int hidden = 0, gone = 0;
    for (auto& part : step) {
        if (part.alive == nullptr || !part.alive->load()) { ++gone; continue; }
        auto* mod = part.mod;
        back.push_back({mod, part.alive, mod->doc});
        const auto& now = mod->doc.edits.spans;
        const auto& was = part.before.edits.spans;
        const bool layout = now.size() != was.size() ||
                            !std::equal(now.begin(), now.end(), was.begin(), [](const NoteSpan& a, const NoteSpan& b) {
                                return a.start == b.start && a.end == b.end;
                            });
        mod->doc = std::move(part.before);
        dc_->documentChanged(mod, layout);
        if (std::none_of(viewItems_.begin(), viewItems_.end(), [&](const ViewItem& it) { return it.mod == mod; })) ++hidden;
    }
    if (!back.empty()) to.push_back(std::move(back));
    const juce::String what = redo ? T("повтор") : T("отмена");
    juce::String note;
    if (hidden > 0) note = what + T(" в тэйке, которого сейчас не видно");
    if (gone > 0) note << (note.isEmpty() ? what + T(": ") : T("; ")) << T("тэйк удалён, его часть пропущена");
    if (note.isNotEmpty()) {
        notice_ = note;
        noticeMs_ = juce::Time::getMillisecondCounter();
        logLine(T("вид трека: ") + note);
    }
    refreshView(true); // the takes changed: show them as they are now
    return true;
}

juce::String PluginClip::noticeText() const
{
    if (notice_.isEmpty() || juce::Time::getMillisecondCounter() - noticeMs_ > 5000) return {};
    return notice_;
}

void PluginClip::documentChanged(bool layoutChanged)
{
    if (dc_ == nullptr) return;
    if (!viewOn_) {
        if (mod_ != nullptr) dc_->documentChanged(mod_, layoutChanged);
        return;
    }
    if (viewPending_ || view_.analysis == nullptr) return;
    // The items must still be those the view was built from (a take deleted meanwhile: rebuild).
    const auto now = viewItemsNow();
    bool same = now.size() == viewItems_.size();
    for (size_t k = 0; same && k < now.size(); ++k)
        same = now[k].region == viewItems_[k].region && now[k].mod == viewItems_[k].mod && now[k].eff == viewItems_[k].eff;
    if (!same) {
        logLine(T("вид трека: айтемы изменились во время правки, правка не записана"));
        refreshView(true);
        return;
    }
    std::vector<bool> layout;
    const auto docs = splitTrackView(view_, view_.doc, viewPieces_, &layout);
    if (docs.size() != viewPieces_.size()) return;
    bool anyLayout = false;
    for (size_t k = 0; k < docs.size(); ++k) {
        if (serialize(docs[k]) == serialize(viewPieces_[k].doc)) continue;
        auto* mod = viewItems_[k].mod;
        // The undo step keeps each take as it was before this edit (first change only).
        if (std::none_of(openStep_.begin(), openStep_.end(), [&](const auto& p) { return p.mod == mod; }))
            openStep_.push_back({mod, mod->alive, mod->doc});
        mod->doc = docs[k];
        dc_->documentChanged(mod, layout[k]); // its own render, as for a single item
        anyLayout = anyLayout || layout[k];
    }
    if (anyLayout) { refreshView(true); return; } // the view's notes changed
    for (size_t k = 0; k < docs.size(); ++k) viewPieces_[k].doc = docs[k];
    view_.built = view_.doc;
    viewItems_ = viewItemsNow(); // no rebuild for our own edit
}


bool PluginClip::summary(ClipHost::Summary& out) const
{
    if (!viewOn_) return false;
    out = {};
    out.t1 = viewEnd_ - viewStart_;
    reaper::TrackInfo track; // the take numbers: REAPER's items at the same places as the regions
    const bool haveTrack = proc_.reaperTrack(track);
    for (const auto& it : viewItems_) {
        const double t0 = it.playStart - viewStart_;
        const double offset = it.playStart - it.modStart - viewStart_; // view time of the analysis' start
        juce::String name;
        const char* nm = it.region->getName();
        if (nm != nullptr) name = juce::String::fromUTF8(nm);
        int take = 0;
        if (haveTrack)
            for (size_t k = 0; k < track.itemStarts.size() && k < track.itemTakes.size(); ++k)
                if (std::abs(track.itemStarts[k] - it.region->getStartInPlaybackTime()) < 0.005) { take = track.itemTakes[k]; break; }
        if (take > 0) name = juce::String(take) + " " + name;
        if (it.eff != nullptr) out.add(name, t0, t0 + (it.modEnd - it.modStart), *it.eff, offset);
    }
    return true;
}

std::vector<PluginClip::ViewItem> PluginClip::viewItemsNow() const
{
    std::vector<ViewItem> out;
    if (seq_ == nullptr) return out;
    auto* doc = arDoc();
    if (doc == nullptr) return out;
    const auto& all = doc->getRegionSequences();
    if (std::find(all.begin(), all.end(), seq_) == all.end()) return out; // track is gone
    for (auto* r : glinaItemsOf(seq_)) {
        ViewItem it;
        it.region = r;
        it.mod = dynamic_cast<GlinaAudioModification*>(r->getAudioModification());
        if (it.mod == nullptr) continue;
        it.eff = it.mod->getGlinaSource()->data->analysisReady ? it.mod->effective.get() : nullptr;
        it.version = it.mod->editVersion;
        it.playStart = r->getStartInPlaybackTime();
        it.modStart = r->getStartInAudioModificationTime();
        it.modEnd = r->getEndInAudioModificationTime();
        out.push_back(it);
    }
    return out;
}

void PluginClip::refreshView(bool force)
{
    auto turnOff = [this] {
        const bool was = viewOn_;
        viewOn_ = viewPending_ = false;
        view_ = TrackView{};
        viewPieces_.clear();
        viewItems_.clear();
        viewSeq_ = nullptr;
        viewPrimary_ = -1;
        viewHist_ = nullptr; // the track's history stays with the controller
        if (was) changes.sendChangeMessage();
    };
    auto refuse = [&](const juce::String& why) {
        turnOff();
        if (why != viewRefused_) logLine(T("вид трека выключен: ") + why);
        viewRefused_ = why;
    };
    const auto items = viewItemsNow();
    if (items.size() < 2) { turnOff(); return; }
    for (const auto& it : items) {
        const double pd = it.region->getDurationInPlaybackTime(), md = it.region->getDurationInAudioModificationTime();
        if (std::abs(pd - md) > 1e-6) { refuse(T("у айтема изменена скорость воспроизведения")); return; }
    }
    int primary = 0;
    for (int k = 0; k < (int)items.size(); ++k)
        if (items[(size_t)k].region == region_) primary = k;
    if (!force && viewOn_ && seq_ == viewSeq_ && primary == viewPrimary_ && items == viewItems_) return; // nothing changed

    const bool newTrack = seq_ != viewSeq_;
    // View time 0 is where the first item starts in the project.
    viewStart_ = items.front().playStart;
    viewEnd_ = viewStart_;
    for (const auto& it : items) viewEnd_ = std::max(viewEnd_, it.playStart + (it.modEnd - it.modStart));
    if (std::any_of(items.begin(), items.end(), [](const ViewItem& it) { return it.eff == nullptr; })) {
        viewOn_ = viewPending_ = true; // "Анализ вокала…" until every item is ready
        view_ = TrackView{};
        viewItems_ = items;
        viewSeq_ = seq_;
        viewPrimary_ = primary;
        changes.sendChangeMessage();
        return;
    }
    std::vector<TrackPiece> pieces;
    for (const auto& it : items) {
        TrackPiece p;
        p.eff = it.mod->effective;
        p.doc = it.mod->doc;
        p.placeSec = it.playStart - it.modStart - viewStart_; // view time of the item's clip time 0
        p.winStart = it.modStart;
        p.winEnd = it.modEnd;
        pieces.push_back(std::move(p));
    }
    TrackView v;
    if (!buildTrackView(pieces, primary, v)) { refuse(T("у айтемов трека разная частота дискретизации")); return; }
    viewHist_ = dc_ != nullptr ? &dc_->trackHistory(seq_) : nullptr; // the track's shared undo steps
    if (newTrack) openStep_.clear();
    view_ = std::move(v);
    viewPieces_ = std::move(pieces);
    viewItems_ = items;
    viewPrimary_ = primary;
    viewSeq_ = seq_;
    viewOn_ = true;
    viewPending_ = false;
    viewRefused_.clear();
    if (newTrack)
        logLine(T("вид трека: айтемов ") + juce::String((int)items.size()) + T(", нот ") +
                juce::String((int)view_.analysis->notes.size()));
    changes.sendChangeMessage();
}

void PluginClip::setBypassed(bool b)
{
    if (viewOn_) {
        for (const auto& it : viewItems_) it.mod->bypass.store(b);
        return;
    }
    if (mod_ != nullptr) mod_->bypass.store(b);
}

juce::String PluginClip::busyText() const
{
    if (mod_ == nullptr)
        return T("Выберите item с вокалом. В REAPER Glina ставится как FX на сам item (Take FX), а не на трек.");
    if (viewOn_ && viewPending_) return T("Анализ вокала…");
    if (!mod_->getGlinaSource()->data->analysisReady) return T("Анализ вокала…");
    return {};
}

bool PluginClip::isRendering() const
{
    auto rendering = [](const GlinaAudioModification* m) {
        return m->renderRunning || m->renderedVersion != m->editVersion;
    };
    if (viewOn_) return std::any_of(viewItems_.begin(), viewItems_.end(), [&](const ViewItem& it) { return rendering(it.mod); });
    return mod_ != nullptr && rendering(mod_);
}

double PluginClip::playheadSec() const
{
    if (region_ == nullptr) return -1;
    const double song = proc_.playheadSec.load();
    if (locatorSet_) {
        if (!proc_.playing.load() && song == locatorHostSong_) return locatorSec_;
        locatorSet_ = false; // playing, or the host moved its own cursor
    }
    if (viewOn_) return song < viewStart_ || song > viewEnd_ ? -1 : song - viewStart_;
    if (song < region_->getStartInPlaybackTime() || song > region_->getEndInPlaybackTime()) return -1;
    const double pd = region_->getDurationInPlaybackTime();
    const double ratio = pd > 0 ? region_->getDurationInAudioModificationTime() / pd : 1.0;
    return region_->getStartInAudioModificationTime() + (song - region_->getStartInPlaybackTime()) * ratio;
}

ARA::PlugIn::HostPlaybackController* PluginClip::playback() const
{
    return dc_ != nullptr ? dc_->getDocumentController()->getHostPlaybackController() : nullptr;
}

bool PluginClip::canControlTransport() const { return region_ != nullptr && playback() != nullptr; }

// ---- tempo map from the DAW (ARA musical context) -------------------------------------
juce::ARAMusicalContext* PluginClip::musicalContext() const
{
    if (region_ == nullptr) return nullptr;
    auto* seq = region_->getRegionSequence();
    return seq != nullptr ? seq->getMusicalContext() : nullptr;
}

double PluginClip::toSong(double t) const
{
    if (viewOn_) return viewStart_ + t;
    const double md = region_->getDurationInAudioModificationTime();
    const double ratio = md > 0 ? region_->getDurationInPlaybackTime() / md : 1.0;
    return region_->getStartInPlaybackTime() + (t - region_->getStartInAudioModificationTime()) * ratio;
}

double PluginClip::fromSong(double song) const
{
    if (viewOn_) return song - viewStart_;
    const double pd = region_->getDurationInPlaybackTime();
    const double ratio = pd > 0 ? region_->getDurationInAudioModificationTime() / pd : 1.0;
    return region_->getStartInAudioModificationTime() + (song - region_->getStartInPlaybackTime()) * ratio;
}

bool PluginClip::hasHostTempo() const
{
    auto* mc = musicalContext();
    if (mc == nullptr) return false;
    const ARA::PlugIn::HostContentReader<ARA::kARAContentTypeTempoEntries> tempo(mc);
    const ARA::PlugIn::HostContentReader<ARA::kARAContentTypeBarSignatures> bars(mc);
    return tempo && bars;
}

std::vector<ui::ClipHost::GridLine> PluginClip::hostGrid(double t0, double t1) const
{
    std::vector<GridLine> out;
    auto* mc = musicalContext();
    if (mc == nullptr || t1 <= t0) return out;
    const ARA::PlugIn::HostContentReader<ARA::kARAContentTypeTempoEntries> tempoReader(mc);
    const ARA::PlugIn::HostContentReader<ARA::kARAContentTypeBarSignatures> barReader(mc);
    if (!tempoReader || !barReader) return out;
    const ARA::TempoConverter<decltype(tempoReader)> tempo(tempoReader);
    const ARA::BarSignaturesConverter<decltype(barReader)> bars(barReader);

    const double beat0 = bars.getBeatForQuarter(tempo.getQuarterForTime(toSong(t0)));
    const double beat1 = bars.getBeatForQuarter(tempo.getQuarterForTime(toSong(t1)));
    if (!(beat1 > beat0) || beat1 - beat0 > 20000) return out;
    for (long b = (long)std::floor(beat0); b <= (long)std::ceil(beat1); ++b) {
        const auto q = bars.getQuarterForBeat((double)b);
        const auto sig = bars.getBarSignatureForQuarter(q);
        GridLine l;
        l.sec = fromSong(tempo.getTimeForQuarter(q));
        l.bar = bars.getBarIndexForQuarter(q) + 1;
        l.beat = (int)std::lround(bars.getBeatDistanceFromBarStartForQuarter(q)) % std::max(1, (int)sig.numerator) + 1;
        out.push_back(l);
    }
    return out;
}

// ---- Glina tracks in the project ----------------------------------------------------------
// A track is an ARA region sequence. Its Glina items are the playback regions in it, and each
// item plays its active take (the audio modification the region points at).
namespace {
std::vector<juce::ARAPlaybackRegion*> glinaItemsOf(juce::ARARegionSequence* seq)
{
    std::vector<juce::ARAPlaybackRegion*> rs;
    for (auto* r : seq->getPlaybackRegions<juce::ARAPlaybackRegion>())
        if (dynamic_cast<GlinaAudioModification*>(r->getAudioModification()) != nullptr) rs.push_back(r);
    std::stable_sort(rs.begin(), rs.end(), [](auto* a, auto* b) { return a->getStartInPlaybackTime() < b->getStartInPlaybackTime(); });
    return rs;
}
} // namespace

juce::ARADocument* PluginClip::arDoc() const
{
    if (dc_ == nullptr) return nullptr;
    return dc_->getDocumentController()->getDocument<juce::ARADocument>();
}

std::vector<juce::ARARegionSequence*> PluginClip::glinaSequences() const
{
    std::vector<juce::ARARegionSequence*> out;
    auto* doc = arDoc();
    if (doc == nullptr) return out;
    out.assign(doc->getRegionSequences().begin(), doc->getRegionSequences().end());
    out.erase(std::remove_if(out.begin(), out.end(), [](auto* s) { return glinaItemsOf(s).empty(); }), out.end());
    std::stable_sort(out.begin(), out.end(), [](auto* a, auto* b) { return a->getOrderIndex() < b->getOrderIndex(); });
    return out;
}

juce::ARARegionSequence* PluginClip::sequenceOfTrack(const reaper::TrackInfo& t) const
{
    juce::ARARegionSequence* best = nullptr;
    int bestScore = 0;
    for (auto* seq : glinaSequences()) {
        int hits = 0;
        for (auto* r : glinaItemsOf(seq))
            for (double s : t.itemStarts)
                if (std::abs(r->getStartInPlaybackTime() - s) < 0.005) { ++hits; break; }
        const char* sn = (const char*)seq->getName();
        const bool sameName = t.name.isNotEmpty() && sn != nullptr && t.name == juce::String::fromUTF8(sn);
        if (hits == 0 && !sameName) continue;
        // Takes recorded at once on several tracks start at the same places: the name, then the
        // track number decide between them.
        const bool sameNumber = t.number > 0 && seq->getOrderIndex() == t.number - 1;
        const int score = hits * 4 + (sameName ? 2 : 0) + (sameNumber ? 1 : 0);
        if (score > bestScore) { bestScore = score; best = seq; }
    }
    return best;
}

juce::ARAPlaybackRegion* PluginClip::itemAt(const std::vector<juce::ARAPlaybackRegion*>& items) const
{
    if (items.empty()) return nullptr;
    const double song = proc_.playheadSec.load();
    for (auto* r : items)
        if (song >= r->getStartInPlaybackTime() && song <= r->getEndInPlaybackTime()) return r;
    return items.front();
}

void PluginClip::followTrack()
{
    if (seq_ != nullptr) {
        if (auto* doc = arDoc()) {
            const auto& all = doc->getRegionSequences();
            if (std::find(all.begin(), all.end(), seq_) == all.end()) seq_ = nullptr; // track is gone
        }
    }
    if (seq_ == nullptr) return;
    // Still the same take of the same item: nothing to do.
    if (region_ != nullptr && mod_ != nullptr && mod_ == region_->getAudioModification()) return;
    // The host replaced the item or switched its take: stay on this track. If the host only
    // recreated the region of the take we were editing, pick that take again; otherwise
    // fall back to the item under the playhead.
    const auto items = glinaItemsOf(seq_);
    juce::ARAPlaybackRegion* pick = nullptr;
    if (lastMod_ != nullptr)
        for (auto* r : items)
            if (r->getAudioModification() == lastMod_) { pick = r; break; }
    setRegion(pick != nullptr ? pick : itemAt(items));
}

std::vector<ui::ClipHost::TrackRef> PluginClip::tracks() const
{
    std::vector<TrackRef> out;
    for (auto* seq : glinaSequences()) {
        TrackRef t;
        t.id = idOf(seq);
        const char* sn = (const char*)seq->getName();
        t.name = sn != nullptr && *sn != 0 ? juce::String::fromUTF8(sn)
                                            : ui::T("Трек ") + juce::String(seq->getOrderIndex() + 1);
        if (const ARA::ARAColor* c = (const ARA::ARAColor*)seq->getColor())
            t.colour = juce::Colour::fromFloatRGBA(c->r, c->g, c->b, 1.f);
        t.current = seq == seq_;
        out.push_back(t);
    }
    return out;
}

bool PluginClip::ghostOfItem(juce::ARAPlaybackRegion* r, Ghost& out) const
{
    auto* m = dynamic_cast<GlinaAudioModification*>(r->getAudioModification());
    if (m == nullptr || !m->getGlinaSource()->data->analysisReady || m->effective == nullptr) return false;
    const double md = r->getDurationInAudioModificationTime();
    const double ratio = md > 0 ? r->getDurationInPlaybackTime() / md : 1.0;
    const double ms = r->getStartInAudioModificationTime(), ps = r->getStartInPlaybackTime();
    // ghost clip time -> song time -> this clip's time
    auto toLocal = [&](double t) { return fromSong(ps + (t - ms) * ratio); };
    out = ui::buildGhost(*m->effective, m->doc.edits, toLocal, ms, r->getEndInAudioModificationTime());
    return true;
}

bool PluginClip::thread(const juce::String& id, std::vector<ThreadFrame>& out) const
{
    if (region_ == nullptr) return false;
    for (auto* seq : glinaSequences()) {
        if (idOf(seq) != id) continue;
        out.clear();
        auto items = glinaItemsOf(seq);
        std::stable_sort(items.begin(), items.end(),
                         [](auto* a, auto* b) { return a->getStartInPlaybackTime() < b->getStartInPlaybackTime(); });
        for (auto* r : items) {
            auto* m = dynamic_cast<GlinaAudioModification*>(r->getAudioModification());
            if (m == nullptr || !m->getGlinaSource()->data->analysisReady || m->effective == nullptr) continue;
            const double md = r->getDurationInAudioModificationTime();
            const double ratio = md > 0 ? r->getDurationInPlaybackTime() / md : 1.0;
            const double ms = r->getStartInAudioModificationTime(), ps = r->getStartInPlaybackTime();
            auto toLocal = [&](double t) { return fromSong(ps + (t - ms) * ratio); };
            ui::appendThread(out, threadOf(*m->effective, m->doc.edits, toLocal, ms, r->getEndInAudioModificationTime()));
        }
        return !out.empty();
    }
    return false;
}

bool PluginClip::trackKey(const juce::String& id, Key& out) const
{
    if (region_ == nullptr) return false;
    for (auto* seq : glinaSequences()) {
        if (idOf(seq) != id) continue;
        auto items = glinaItemsOf(seq);
        std::stable_sort(items.begin(), items.end(),
                         [](auto* a, auto* b) { return a->getStartInPlaybackTime() < b->getStartInPlaybackTime(); });
        for (auto* r : items) {
            auto* m = dynamic_cast<GlinaAudioModification*>(r->getAudioModification());
            if (m == nullptr || !m->getGlinaSource()->data->analysisReady || m->effective == nullptr) continue;
            out = m->doc.key(*m->effective);
            return true;
        }
        return false;
    }
    return false;
}

bool PluginClip::ghost(const juce::String& id, Ghost& out) const
{
    if (region_ == nullptr) return false;
    for (auto* seq : glinaSequences()) {
        if (idOf(seq) != id) continue;
        // All items of the track at once, one after another with a break between them.
        out = Ghost{};
        bool any = false;
        for (auto* r : glinaItemsOf(seq)) {
            Ghost g;
            if (!ghostOfItem(r, g)) continue;
            if (any && !out.curve.empty())
                out.curve.emplace_back(out.curve.back().x, std::numeric_limits<double>::quiet_NaN());
            out.notes.insert(out.notes.end(), g.notes.begin(), g.notes.end());
            out.curve.insert(out.curve.end(), g.curve.begin(), g.curve.end());
            any = true;
        }
        return any;
    }
    return false;
}

void PluginClip::log(const juce::String& line) const
{
    juce::String s(line);
    if (region_ != nullptr)
        s << T(" [айтем: проект ") << juce::String(region_->getStartInPlaybackTime(), 3) << T(" с, от начала звука ")
          << juce::String(region_->getStartInAudioModificationTime(), 3) << T(" с]");
    logLine(s);
}

void PluginClip::setEditedTrack(const juce::String& id)
{
    for (auto* seq : glinaSequences())
        if (idOf(seq) == id) { setRegion(itemAt(glinaItemsOf(seq))); return; }
}

// ---- render kept in the project folder ------------------------------------------------
bool PluginClip::hasRenderOptions() const { return dc_ != nullptr && dc_->canSaveRenders(); }

bool PluginClip::savesRender() const { return settings::saveRender(); }

void PluginClip::setSavesRender(bool on)
{
    settings::setSaveRender(on);
    logLine(juce::String(T("«Сохранять рендер в папке проекта»: ")) + (on ? T("вкл.") : T("выкл.")));
}

juce::String PluginClip::renderOrigin() const
{
    if (mod_ == nullptr || !mod_->renderFromDisk || mod_->renderedVersion != mod_->editVersion) return {};
    return mod_->renderEngineVersion == kRenderVersion ? T("рендер из папки проекта")
                                                       : T("рендер из папки проекта, прежняя версия");
}

bool PluginClip::canRerender() const
{
    return mod_ != nullptr && mod_->renderFromDisk && mod_->renderEngineVersion != kRenderVersion;
}

void PluginClip::rerender()
{
    if (mod_ != nullptr && dc_ != nullptr) dc_->rerender(mod_);
}

namespace {
// The Glina modifications of a track's items, each once (items may share one).
std::vector<GlinaAudioModification*> modsOfTrack(juce::ARARegionSequence* seq)
{
    std::vector<GlinaAudioModification*> out;
    if (seq == nullptr) return out;
    for (auto* r : glinaItemsOf(seq))
        if (auto* m = dynamic_cast<GlinaAudioModification*>(r->getAudioModification()))
            if (std::find(out.begin(), out.end(), m) == out.end()) out.push_back(m);
    return out;
}
} // namespace

std::vector<NoteSpan> PluginClip::automaticSpans() const
{
    std::vector<NoteSpan> out;
    if (!viewOn_) {
        if (mod_ == nullptr || !mod_->getGlinaSource()->data->analysisReady) return out;
        return spansOf(mod_->getGlinaSource()->data->analysis);
    }
    // Track view: each item's automatic notes, placed on the view's frames as the view places them.
    if (viewPending_ || view_.analysis == nullptr || view_.maps.size() != viewItems_.size()) return out;
    for (size_t k = 0; k < viewItems_.size(); ++k) {
        const auto* m = viewItems_[k].mod;
        if (m == nullptr || !m->getGlinaSource()->data->analysisReady) continue;
        const auto& mp = view_.maps[k];
        for (const NoteSpan& s : spansOf(m->getGlinaSource()->data->analysis)) {
            const NoteSpan v{s.start + mp.frameOffset, s.end + mp.frameOffset};
            if (v.start >= mp.frameBegin && v.end <= mp.frameEnd) out.push_back(v);
        }
    }
    std::sort(out.begin(), out.end(), [](const NoteSpan& a, const NoteSpan& b) { return a.start < b.start; });
    return out;
}

int PluginClip::rerenderTrack()
{
    if (dc_ == nullptr) return 0;
    const auto mods = modsOfTrack(seq_);
    logLine(T("пересчитать трек: айтемов ") + juce::String((int)mods.size()));
    for (auto* m : mods) dc_->rerender(m);
    return (int)mods.size();
}

int PluginClip::itemsOnTrack() const { return (int)modsOfTrack(seq_).size(); }

int PluginClip::staleOnTrack() const
{
    int n = 0;
    for (auto* m : modsOfTrack(seq_))
        if (m->renderFromDisk && m->renderEngineVersion != kRenderVersion) ++n;
    return n;
}

void PluginClip::setPlaying(bool p)
{
    if (auto* pc = playback()) { if (p) pc->requestStartPlayback(); else pc->requestStopPlayback(); }
}

void PluginClip::setPlayheadSec(double t)
{
    auto* pc = playback();
    if (pc == nullptr || region_ == nullptr) return;
    pc->requestSetPlaybackPosition(std::max(0.0, toSong(t)));
    locatorSec_ = std::max(0.0, t);
    locatorHostSong_ = proc_.playheadSec.load();
    locatorSet_ = true;
}

// ============================================================================
GlinaEditor::GlinaEditor(GlinaProcessor& p)
    : juce::AudioProcessorEditor(&p), juce::AudioProcessorEditorARAExtension(&p), proc_(p)
{
    openedMs_ = juce::Time::getMillisecondCounter(); // before the listener: it may fire at once
    addAndMakeVisible(editor_);
    if (auto* ev = getARAEditorView()) {
        dc_ = juce::ARADocumentControllerSpecialisation::getSpecialisedDocumentController<GlinaDocumentController>(
            ev->getDocumentController());
        ev->addListener(this);
        clip_ = std::make_unique<PluginClip>(dc_, p);
        pinOwnRegion();
        pickFallbackRegion();
        editor_.setHost(clip_.get());
        startTimerHz(4);
    } else {
        editor_.setVisible(false);
    }
    setResizable(true, false); // ARA hosts embed the editor and resize it
    setResizeLimits(800, 360, 6000, 3000);
    setSize(1280, 660);
}

GlinaEditor::~GlinaEditor()
{
    if (auto* ev = getARAEditorView()) ev->removeListener(this);
    editor_.setHost(nullptr);
}

// The item this instance sits on is the one it edits. The host's selection at open time is
// the last item the user worked on, so it must not replace that item.
// Which item the editor opens on.
// - Take FX: the instance renders its own take, so that take (several regions: the one under the cursor).
// - Track FX: REAPER hands the instance no regions at all. Its track is found through REAPER (the
//   track this FX sits on) among the ARA tracks; the item under the edit cursor opens, or the
//   nearest one. The view is centred on the cursor either way.
// REAPER names the project and the track a moment after the FX is created, so this waits up to 3 s.
bool GlinaEditor::pinOwnRegion()
{
    if (ownPinned_ || clip_ == nullptr) return ownPinned_;
    const bool waited = juce::Time::getMillisecondCounter() - openedMs_ >= 3000;
    if (!proc_.hasReaperProject() && !waited) return false;
    const double cursor = proc_.editCursorSec(); // the transport's playhead is not updated while stopped
    const double song = std::isfinite(cursor) ? cursor : proc_.playheadSec.load();
    auto inside = [song](juce::ARAPlaybackRegion* r) {
        return song >= r->getStartInPlaybackTime() && song <= r->getEndInPlaybackTime();
    };
    std::vector<juce::ARAPlaybackRegion*> regions;
    if (auto* pr = proc_.getPlaybackRenderer<juce::ARAPlaybackRenderer>())
        regions = pr->getPlaybackRegions<juce::ARAPlaybackRegion>();

    juce::ARAPlaybackRegion* pick = nullptr;
    juce::String from;
    if (!regions.empty()) {
        pick = regions.front();
        for (auto* r : regions)
            if (inside(r)) { pick = r; break; }
        from = T("FX на тэйке, регионов ") + juce::String((int)regions.size());
    } else {
        auto giveUp = [&](const juce::String& why) {
            if (!waited) return false; // REAPER may not have told us yet
            ownPinned_ = true;         // stop trying: the editor stays where it is
            clip_->log(T("открыт: ") + why + T(", остаётся первый трек"));
            return false;
        };
        reaper::TrackInfo t;
        if (!proc_.reaperTrack(t)) return giveUp(T("трек FX не известен (нет REAPER?)"));
        auto* seq = clip_->sequenceOfTrack(t);
        if (seq == nullptr)
            return giveUp(T("трек «") + t.name + T("» №") + juce::String(t.number) + T(" не найден среди треков с Glina"));
        const auto items = glinaItemsOf(seq);
        double best = std::numeric_limits<double>::max();
        for (auto* r : items) {
            const double d = inside(r) ? 0.0
                                       : std::min(std::abs(song - r->getStartInPlaybackTime()),
                                                  std::abs(song - r->getEndInPlaybackTime()));
            if (d < best) { best = d; pick = r; }
        }
        if (pick == nullptr) return giveUp(T("на треке «") + t.name + T("» нет айтемов с Glina"));
        from = T("FX на треке «") + t.name + T("» №") + juce::String(t.number) + T(", айтемов ") +
               juce::String((int)items.size()) + (best > 0 ? T(", ближайший к курсору") : T(", под курсором"));
    }
    clip_->setRegion(pick);
    ownPinned_ = true;
    centreOnOpen_ = true; // the view opens at the cursor, even when no item is there
    openSec_ = song;
    clip_->log(T("открыт: ") + from + T(", курсор ") + juce::String(song, 3) + T(" с"));
    return true;
}

void GlinaEditor::onNewSelection(const juce::ARAViewSelection& sel)
{
    // The selection the host reports right after opening can be the item worked on last. Ignore
    // it for a moment; later selections (a click on another item) switch the editor as usual.
    if (juce::Time::getMillisecondCounter() - openedMs_ < 1500) return;
    // Effective regions: a track selected as a whole (no single region picked) counts too.
    const auto regions = sel.getEffectivePlaybackRegions<juce::ARAPlaybackRegion>();
    if (!regions.empty() && clip_ != nullptr && regions.front() != clip_->region()) {
        // Another item of the track already shown as a whole: nothing to switch, keep the view.
        if (clip_->trackView() && regions.front()->getRegionSequence() == clip_->sequence()) {
            clip_->setRegion(regions.front());
            return;
        }
        clip_->setRegion(regions.front());
        editor_.setHost(nullptr); // reset view and selection for the new clip
        editor_.setHost(clip_.get());
    }
}

void GlinaEditor::pickFallbackRegion()
{
    if (clip_ == nullptr || clip_->valid() || dc_ == nullptr) return;
    // Prefer this plug-in instance's own regions (Take FX), then anything in the document.
    if (auto* pr = proc_.getPlaybackRenderer<juce::ARAPlaybackRenderer>())
        for (auto* r : pr->getPlaybackRegions<juce::ARAPlaybackRegion>()) { clip_->setRegion(r); return; }
    auto* doc = dc_->getDocumentController()->getDocument<juce::ARADocument>();
    for (auto* seq : doc->getRegionSequences())
        for (auto* r : seq->getPlaybackRegions<juce::ARAPlaybackRegion>()) { clip_->setRegion(r); return; }
}

void GlinaEditor::timerCallback()
{
    if (clip_ == nullptr) return;
    const auto* before = clip_->region();
    const auto* beforeMod = clip_->modification();
    pinOwnRegion(); // the renderer may not know its item yet when the editor opens
    if (centreOnOpen_ && editor_.centreOnProjectSec(openSec_)) centreOnOpen_ = false;
    clip_->followTrack(); // item or take changed in the host: stay on the same track
    if (!clip_->valid()) pickFallbackRegion();
    clip_->refreshView(); // items added, moved or edited elsewhere
    if (clip_->region() != before || clip_->modification() != beforeMod)
        editor_.rebindHost(clip_.get()); // keep zoom and scroll when the track or take changes
}

void GlinaEditor::paint(juce::Graphics& g)
{
    g.fillAll(ui::col::bg);
    if (getARAEditorView() == nullptr) {
        g.setColour(ui::col::text);
        g.setFont(juce::FontOptions(15.f));
        g.drawFittedText(T("Glina работает только как ARA-плагин.\n"
                           "В REAPER добавьте его как FX на сам item (Take FX), а не на трек."),
                         getLocalBounds().reduced(20), juce::Justification::centred, 3);
    }
}

void GlinaEditor::resized() { editor_.setBounds(getLocalBounds()); }

} // namespace glina::plugin
