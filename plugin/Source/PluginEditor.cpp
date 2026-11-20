#include "PluginEditor.h"
#include "RenderStore.h"

#include <ARA_Library/Utilities/ARATimelineConversion.h>

namespace glina::plugin {

using ui::T;

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
    if (region == region_) return;
    if (region_ != nullptr) region_->removeListener(this);
    if (mod_ != nullptr) mod_->removeListener(this);
    region_ = region;
    mod_ = region != nullptr ? static_cast<GlinaAudioModification*>(region->getAudioModification()) : nullptr;
    if (region_ != nullptr) region_->addListener(this);
    if (mod_ != nullptr) mod_->addListener(this);
    changes.sendChangeMessage();
}

const Analysis* PluginClip::analysis() const
{
    if (mod_ == nullptr || !mod_->getGlinaSource()->data->analysisReady) return nullptr;
    return mod_->effective.get();
}

void PluginClip::documentChanged(bool layoutChanged)
{
    if (mod_ != nullptr && dc_ != nullptr) dc_->documentChanged(mod_, layoutChanged);
}

juce::String PluginClip::busyText() const
{
    if (mod_ == nullptr)
        return T("Выберите item с вокалом. В REAPER Glina ставится как FX на сам item (Take FX), а не на трек.");
    if (!mod_->getGlinaSource()->data->analysisReady) return T("Анализ вокала…");
    return {};
}

bool PluginClip::isRendering() const
{
    return mod_ != nullptr && (mod_->renderRunning || mod_->renderedVersion != mod_->editVersion);
}

double PluginClip::playheadSec() const
{
    if (region_ == nullptr) return -1;
    const double song = proc_.playheadSec.load();
    if (locatorSet_) {
        if (!proc_.playing.load() && song == locatorHostSong_) return locatorSec_;
        locatorSet_ = false; // playing, or the host moved its own cursor
    }
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
    const double md = region_->getDurationInAudioModificationTime();
    const double ratio = md > 0 ? region_->getDurationInPlaybackTime() / md : 1.0;
    return region_->getStartInPlaybackTime() + (t - region_->getStartInAudioModificationTime()) * ratio;
}

double PluginClip::fromSong(double song) const
{
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

// ---- other Glina items in the project -------------------------------------------------
std::vector<juce::ARAPlaybackRegion*> PluginClip::glinaRegions() const
{
    std::vector<juce::ARAPlaybackRegion*> out;
    if (dc_ == nullptr) return out;
    auto* doc = dc_->getDocumentController()->getDocument<juce::ARADocument>();
    if (doc == nullptr) return out;
    std::vector<juce::ARARegionSequence*> seqs(doc->getRegionSequences().begin(), doc->getRegionSequences().end());
    std::stable_sort(seqs.begin(), seqs.end(), [](auto* a, auto* b) { return a->getOrderIndex() < b->getOrderIndex(); });
    for (auto* seq : seqs) {
        std::vector<juce::ARAPlaybackRegion*> rs;
        for (auto* r : seq->getPlaybackRegions<juce::ARAPlaybackRegion>())
            if (dynamic_cast<GlinaAudioModification*>(r->getAudioModification()) != nullptr) rs.push_back(r);
        std::stable_sort(rs.begin(), rs.end(), [](auto* a, auto* b) { return a->getStartInPlaybackTime() < b->getStartInPlaybackTime(); });
        out.insert(out.end(), rs.begin(), rs.end());
    }
    return out;
}

std::vector<ui::ClipHost::TrackRef> PluginClip::tracks() const
{
    std::vector<TrackRef> out;
    const auto regions = glinaRegions();
    for (size_t i = 0; i < regions.size(); ++i) {
        auto* r = regions[i];
        auto* seq = r->getRegionSequence();
        TrackRef t;
        t.id = idOf(r);
        const char* sn = seq != nullptr ? (const char*)seq->getName() : nullptr;
        t.name = sn != nullptr && *sn != 0 ? juce::String::fromUTF8(sn)
                                            : ui::T("Трек ") + juce::String(seq != nullptr ? seq->getOrderIndex() + 1 : 0);
        // Several Glina items on one track: tell them apart.
        int onTrack = 0, index = 0;
        for (auto* o : regions)
            if (o->getRegionSequence() == seq) { ++onTrack; if (o == r) index = onTrack; }
        if (onTrack > 1) {
            const char* rn = (const char*)r->getName();
            t.name << " · " << (rn != nullptr && *rn != 0 ? juce::String::fromUTF8(rn) : ui::T("item ") + juce::String(index));
        }
        const ARA::ARAColor* c = seq != nullptr ? (const ARA::ARAColor*)seq->getColor() : nullptr;
        if (const ARA::ARAColor* rc = (const ARA::ARAColor*)r->getColor()) c = rc;
        if (c != nullptr) t.colour = juce::Colour::fromFloatRGBA(c->r, c->g, c->b, 1.f);
        t.current = r == region_;
        out.push_back(t);
    }
    return out;
}

bool PluginClip::ghost(const juce::String& id, Ghost& out) const
{
    if (region_ == nullptr) return false;
    for (auto* r : glinaRegions()) {
        if (idOf(r) != id) continue;
        auto* m = static_cast<GlinaAudioModification*>(r->getAudioModification());
        if (!m->getGlinaSource()->data->analysisReady || m->effective == nullptr) return false;
        const double md = r->getDurationInAudioModificationTime();
        const double ratio = md > 0 ? r->getDurationInPlaybackTime() / md : 1.0;
        const double ms = r->getStartInAudioModificationTime(), ps = r->getStartInPlaybackTime();
        // ghost clip time -> song time -> this clip's time
        auto toLocal = [&](double t) { return fromSong(ps + (t - ms) * ratio); };
        out = ui::buildGhost(*m->effective, m->doc.edits, toLocal, ms, r->getEndInAudioModificationTime());
        return true;
    }
    return false;
}

void PluginClip::setEditedTrack(const juce::String& id)
{
    for (auto* r : glinaRegions())
        if (idOf(r) == id) { setRegion(r); return; }
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

void PluginClip::setPlaying(bool p)
{
    if (auto* pc = playback()) { if (p) pc->requestStartPlayback(); else pc->requestStopPlayback(); }
}

void PluginClip::setPlayheadSec(double t)
{
    auto* pc = playback();
    if (pc == nullptr || region_ == nullptr) return;
    const double md = region_->getDurationInAudioModificationTime();
    const double ratio = md > 0 ? region_->getDurationInPlaybackTime() / md : 1.0;
    const double song = region_->getStartInPlaybackTime() + (t - region_->getStartInAudioModificationTime()) * ratio;
    pc->requestSetPlaybackPosition(std::max(0.0, song));
    locatorSec_ = std::max(0.0, t);
    locatorHostSong_ = proc_.playheadSec.load();
    locatorSet_ = true;
}

// ============================================================================
GlinaEditor::GlinaEditor(GlinaProcessor& p)
    : juce::AudioProcessorEditor(&p), juce::AudioProcessorEditorARAExtension(&p), proc_(p)
{
    addAndMakeVisible(editor_);
    if (auto* ev = getARAEditorView()) {
        dc_ = juce::ARADocumentControllerSpecialisation::getSpecialisedDocumentController<GlinaDocumentController>(
            ev->getDocumentController());
        ev->addListener(this);
        clip_ = std::make_unique<PluginClip>(dc_, p);
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

void GlinaEditor::onNewSelection(const juce::ARAViewSelection& sel)
{
    const auto& regions = sel.getPlaybackRegions<juce::ARAPlaybackRegion>();
    if (!regions.empty() && clip_ != nullptr && regions.front() != clip_->region()) {
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
    if (clip_ != nullptr && !clip_->valid()) {
        pickFallbackRegion();
        if (clip_->valid()) { editor_.setHost(nullptr); editor_.setHost(clip_.get()); }
    }
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
