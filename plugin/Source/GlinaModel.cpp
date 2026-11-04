#include "GlinaModel.h"
#include "PlaybackRenderer.h"

#include "glina/Notes.h"

namespace glina::plugin {

// ============================================================================
GlinaAudioModification::GlinaAudioModification(juce::ARAAudioSource* source, ARA::ARAAudioModificationHostRef ref,
                                               const GlinaAudioModification* clone)
    : juce::ARAAudioModification(source, ref, clone)
{
    if (clone != nullptr) {
        doc = clone->doc;
        effective = clone->effective;
        editVersion = 1; // the clone renders its own copy
    }
}

void GlinaAudioModification::setCache(std::shared_ptr<const RenderCache> c)
{
    {
        const juce::SpinLock::ScopedLockType sl(cacheLock);
        std::swap(cache, c);
    }
    // `c` now holds the previous cache. The audio thread may still hold a copy,
    // so park it here; drop parked caches nobody else references any more.
    if (c) retired.push_back(std::move(c));
    retired.erase(std::remove_if(retired.begin(), retired.end(), [](const auto& p) { return p.use_count() == 1; }),
                  retired.end());
}

bool GlinaAudioModification::tryGetCache(std::shared_ptr<const RenderCache>& out) const
{
    const juce::SpinLock::ScopedTryLockType sl(cacheLock);
    if (!sl.isLocked()) return false;
    out = cache;
    return out != nullptr;
}

// ============================================================================
GlinaDocumentController::~GlinaDocumentController() { pool.removeAllJobs(true, 20000); }

juce::ARAAudioSource* GlinaDocumentController::doCreateAudioSource(juce::ARADocument* doc,
                                                                  ARA::ARAAudioSourceHostRef ref) noexcept
{
    return new GlinaAudioSource(doc, ref);
}

juce::ARAAudioModification* GlinaDocumentController::doCreateAudioModification(
    juce::ARAAudioSource* source, ARA::ARAAudioModificationHostRef ref, const juce::ARAAudioModification* clone) noexcept
{
    return new GlinaAudioModification(source, ref, static_cast<const GlinaAudioModification*>(clone));
}

juce::ARAPlaybackRenderer* GlinaDocumentController::doCreatePlaybackRenderer() noexcept
{
    return new GlinaPlaybackRenderer(getDocumentController(), *this);
}

void GlinaDocumentController::didEndEditing(juce::ARADocument* doc)
{
    processLock.exitWrite();
    // Hosts may enable sample access before or during an edit cycle; catch both.
    for (auto* s : doc->getAudioSources<GlinaAudioSource>()) {
        s->releaseReaderIfDone();
        startAnalysisIfPossible(s);
    }
}

void GlinaDocumentController::didEnableAudioSourceSamplesAccess(juce::ARAAudioSource* s, bool enable)
{
    auto* src = static_cast<GlinaAudioSource*>(s);
    src->releaseReaderIfDone();
    if (enable) startAnalysisIfPossible(src);
}

void GlinaDocumentController::startAnalysisIfPossible(GlinaAudioSource* src)
{
    if (src->analysisStarted || !src->isSampleAccessEnabled() || src->getSampleCount() <= 0) return;
    if (src->data->analysisReady) return;
    src->releaseReaderIfDone();
    if (src->reader != nullptr) return; // a previous read is still winding down
    src->analysisStarted = true;

    auto data = src->data;
    data->sampleRate = src->getSampleRate();
    data->readFinished = false;
    src->reader = std::make_unique<juce::ARAAudioSourceReader>(src); // created on the ARA thread
    juce::ARAAudioSourceReader* reader = src->reader.get();
    const auto numSamples = src->getSampleCount();
    const int numChannels = src->getChannelCount();
    // `this` outlives every job: the pool is joined in the destructor. The reader
    // outlives the read loop: the source destructor waits on readMutex.

    src->notifyAnalysisProgressStarted();
    pool.addJob([this, src, data, reader, numSamples, numChannels] {
        std::vector<float> mono((size_t)numSamples, 0.f);
        juce::AudioBuffer<float> buf(std::max(1, numChannels), 65536);
        bool ok = true;
        for (juce::int64 pos = 0; pos < numSamples; pos += buf.getNumSamples()) {
            const int n = (int)std::min<juce::int64>(buf.getNumSamples(), numSamples - pos);
            {
                const std::lock_guard<std::mutex> lk(data->readMutex);
                if (!data->alive) { ok = false; break; }
                if (!reader->read(&buf, 0, n, pos, true, true)) { ok = false; break; }
            }
            for (int c = 0; c < buf.getNumChannels(); ++c) {
                const float* p = buf.getReadPointer(c);
                for (int i = 0; i < n; ++i) mono[(size_t)(pos + i)] += p[i] / (float)buf.getNumChannels();
            }
        }
        data->readFinished = true; // the reader is released by the next ARA callback on the host thread
        if (!ok || !data->alive) {
            juce::MessageManager::callAsync([src, data] {
                if (!data->alive) return;
                src->analysisStarted = false; // retry when access is re-enabled
                src->notifyAnalysisProgressCompleted();
            });
            return;
        }

        data->mono = std::move(mono);
        data->samplesReady = true;
        data->analysis = analyze(data->mono.data(), data->mono.size(), data->sampleRate);
        data->analysisReady = true;

        juce::MessageManager::callAsync([this, src, data] {
            if (!data->alive) return;
            src->notifyAnalysisProgressCompleted();
            analysisFinished(src);
        });
    });
}

void GlinaDocumentController::rebuildEffective(GlinaAudioModification* mod)
{
    const auto& data = *mod->getGlinaSource()->data;
    if (!data.analysisReady) return;
    auto& spans = mod->doc.edits.spans;
    const int nf = (int)data.analysis.frames.size();
    for (const auto& s : spans)
        if (s.start < 0 || s.end > nf || s.end <= s.start) { spans.clear(); break; }
    mod->effective = std::make_shared<const Analysis>(withLayout(data.analysis, spans));
    conform(mod->doc, *mod->effective);
}

void GlinaDocumentController::analysisFinished(GlinaAudioSource* src)
{
    for (auto* mod : src->getAudioModifications<GlinaAudioModification>()) {
        rebuildEffective(mod);
        ++mod->editVersion;
        startRender(mod);
    }
    modelChanged.sendChangeMessage();
}

void GlinaDocumentController::documentChanged(GlinaAudioModification* mod, bool layoutChanged)
{
    if (layoutChanged || mod->effective == nullptr) rebuildEffective(mod);
    if (mod->effective != nullptr) conform(mod->doc, *mod->effective);
    ++mod->editVersion;
    startRender(mod);
    // Let the host know the project needs saving.
    if (auto* uc = getDocumentController()->getHostModelUpdateController()) uc->notifyDocumentDataChanged();
}

void GlinaDocumentController::startRender(GlinaAudioModification* mod)
{
    auto data = mod->getGlinaSource()->data;
    if (!data->analysisReady || mod->effective == nullptr || mod->renderRunning) return;
    mod->renderRunning = true;

    const Document doc = mod->doc;
    auto eff = mod->effective;
    const uint64_t version = mod->editVersion;
    auto alive = mod->alive;
    pool.addJob([this, mod, data, doc, eff, version, alive] {
        Mix m = renderMix(data->mono.data(), data->mono.size(), *eff, doc);
        auto cache = std::make_shared<RenderCache>();
        cache->sampleRate = data->sampleRate;
        cache->left = std::move(m.left);
        cache->right = std::move(m.right);
        cache->map = TimeMap::build(*eff, doc.edits);

        juce::MessageManager::callAsync([this, mod, cache, version, alive] {
            if (!*alive) return;
            mod->renderRunning = false;
            mod->setCache(cache);
            mod->renderedVersion = version;
            mod->notifyContentChanged(juce::ARAContentUpdateScopes::samplesAreAffected(), true);
            for (auto* region : mod->getPlaybackRegions())
                region->notifyContentChanged(juce::ARAContentUpdateScopes::samplesAreAffected(), true);
            modelChanged.sendChangeMessage();
            if (mod->editVersion != version) startRender(mod); // more edits arrived meanwhile
        });
    });
}

// ============================================================================
// Archive v2: per audio modification — persistent ID, then the serialised document.
// (v1 stored a bare edit set; still readable.)
bool GlinaDocumentController::doStoreObjectsToStream(juce::ARAOutputStream& out,
                                                     const juce::ARAStoreObjectsFilter* filter) noexcept
{
    const auto& mods = filter->getAudioModificationsToStore<GlinaAudioModification>();
    out.writeInt(2);
    out.writeInt64((juce::int64)mods.size());
    for (auto* m : mods) {
        out.writeString(m->getPersistentID());
        out.writeString(juce::String::fromUTF8(serialize(m->doc).c_str()));
    }
    return true;
}

bool GlinaDocumentController::doRestoreObjectsFromStream(juce::ARAInputStream& in,
                                                         const juce::ARARestoreObjectsFilter* filter) noexcept
{
    const int version = in.readInt();
    if (version != 1 && version != 2) return false;
    const auto count = in.readInt64();
    for (juce::int64 i = 0; i < count && !in.failed(); ++i) {
        const juce::String id = in.readString();
        Document d;
        if (version == 2) {
            if (!deserialize(in.readString().toStdString(), d)) return false;
        } else {
            EditSet& e = d.edits;
            e.sibilantGainDb = in.readDouble();
            e.breathGainDb = in.readDouble();
            e.globalFormantShift = in.readDouble();
            e.transitionMs = in.readDouble();
            e.edgeTaperMs = in.readDouble();
            const auto nn = in.readInt64();
            if (nn < 0 || nn > 1000000) return false;
            e.notes.resize((size_t)nn);
            for (NoteEdit& n : e.notes) {
                n.pitchOffset = in.readDouble();   n.driftAmount = in.readDouble();
                n.vibratoAmount = in.readDouble(); n.formantShift = in.readDouble();
                n.gainDb = in.readDouble();        n.timeOffsetSec = in.readDouble();
                n.stretch = in.readDouble();
            }
        }
        if (auto* m = filter->getAudioModificationToRestoreStateWithID<GlinaAudioModification>(id.getCharPointer())) {
            m->doc = std::move(d);
            m->history.clear();
            rebuildEffective(m);   // no-op until analysis is ready
            ++m->editVersion;
            startRender(m);        // no-op until analysis is ready
        }
    }
    return !in.failed();
}

} // namespace glina::plugin
