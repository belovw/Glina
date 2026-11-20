#include "GlinaModel.h"
#include "PlaybackRenderer.h"
#include "RenderStore.h"

#include "../../ui/Theme.h"
#include "glina/Notes.h"

#include <algorithm>
#include <mutex>

namespace glina::plugin {

using ui::T;

namespace {
// Plug-in instances (their REAPER link) bound to each document controller.
std::mutex linksMutex;
std::vector<std::pair<GlinaDocumentController*, reaper::HostLink*>>& hostLinks()
{
    static std::vector<std::pair<GlinaDocumentController*, reaper::HostLink*>> links;
    return links;
}
} // namespace

// ============================================================================
// Diagnostics (glina_diag.txt on the desktop).
void diagLog(const juce::String& line)
{
    static juce::CriticalSection lock;
    static const juce::File file =
        juce::File::getSpecialLocation(juce::File::userDesktopDirectory).getChildFile("glina_diag.txt");
    const auto t = juce::Time::getCurrentTime();
    const juce::ScopedLock sl(lock);
    file.appendText(t.formatted("%Y-%m-%d %H:%M:%S") + "." + juce::String(t.getMilliseconds()).paddedLeft('0', 3) + "  "
                        + line + "\n",
                    false, false, "\r\n");
}

namespace {
juce::String hexOf(const void* p) { return juce::String::toHexString((juce::pointer_sized_int)p); }

// One line about a finished render: what the player will get from this cache.
juce::String describeRender(const RenderCache& c, const Document& doc, double mapRate)
{
    int timeEdits = 0;
    for (const auto& n : doc.edits.notes)
        if (n.timeOffsetSec != 0 || (!n.consonant && n.stretch != 1)) ++timeEdits;
    const auto& in = c.map.inPoints();
    const auto& out = c.map.outPoints();
    auto sec = [mapRate](double samples) { return juce::String(mapRate > 0 ? samples / mapRate : 0.0, 4); };
    juce::String s;
    s << "key=" << renderstore::keyText(c.key) << " fromDisk=" << (int)c.fromDisk << " sampleRate=" << c.sampleRate
      << " cacheLen=" << (int)c.left.size() << " (" << juce::String(c.sampleRate > 0 ? c.left.size() / c.sampleRate : 0.0, 3)
      << " s) audioHash=" << renderstore::keyText(c.diagAudioHash) << " timeEditsInDoc=" << timeEdits
      << " map: identity=" << (int)c.map.isIdentity() << " points=" << (int)in.size();
    for (size_t i = 0; i < std::min<size_t>(2, in.size()); ++i)
        s << " p" << (int)i << "=(in " << sec(in[i]) << ", out " << sec(out[i]) << ")";
    // The first point the edit actually moved (out != in by more than half a sample).
    size_t moved = in.size();
    for (size_t i = 0; i < in.size(); ++i)
        if (std::abs(out[i] - in[i]) > 0.5) { moved = i; break; }
    if (moved < in.size())
        s << " firstMoved=p" << (int)moved << "(in " << sec(in[moved]) << ", out " << sec(out[moved]) << ", shift "
          << juce::String(mapRate > 0 ? (out[moved] - in[moved]) * 1000.0 / mapRate : 0.0, 1) << " ms)";
    else
        s << " firstMoved=none";
    return s;
}
} // namespace

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

std::shared_ptr<const RenderCache> GlinaAudioModification::currentCache() const
{
    const juce::SpinLock::ScopedLockType sl(cacheLock); // the audio thread only try-locks, briefly
    return cache;
}

juce::String GlinaAudioModification::sourceName() const
{
    const char* n = (const char*)getAudioSource()->getName();
    return n != nullptr ? juce::String::fromUTF8(n) : juce::String();
}

bool GlinaAudioModification::tryGetCache(std::shared_ptr<const RenderCache>& out) const
{
    const juce::SpinLock::ScopedTryLockType sl(cacheLock);
    if (!sl.isLocked()) return false;
    out = cache;
    return out != nullptr;
}

// ============================================================================
// ============================================================================
// Saving renders next to the project. REAPER asks for the ARA archive when it saves the
// project, but possibly at other moments too (undo states, copying items). So a full
// store only arms a watcher; the renders are written once the .rpp has really been
// written (it changed, appeared under a new name, or the project stopped being dirty).
struct GlinaDocumentController::PendingSave {
    struct Item {
        uint64_t key = 0;
        juce::String sourceName;
        std::shared_ptr<const RenderCache> cache;
    };
    void* project = nullptr;
    juce::String fileAtStore;  // .rpp at the time of the store (empty: never saved)
    juce::Time mtimeAtStore;   // of fileAtStore
    int dirtyAtStore = -1;
    juce::Time started;
    std::vector<Item> items;
};


class GlinaDocumentController::SaveWatcher final : private juce::Timer {
public:
    explicit SaveWatcher(GlinaDocumentController& dc) : dc_(dc) {}
    ~SaveWatcher() override { stopTimer(); }

    void start(PendingSave p)
    {
        // Several stores in a row (one save, or stores between saves): keep the oldest baseline.
        if (pending_ != nullptr && pending_->project == p.project && pending_->fileAtStore == p.fileAtStore) {
            p.mtimeAtStore = std::min(p.mtimeAtStore, pending_->mtimeAtStore);
            p.dirtyAtStore = std::max(p.dirtyAtStore, pending_->dirtyAtStore);
        }
        pending_ = std::make_unique<PendingSave>(std::move(p));
        startTimer(250);
    }

    // The document is closing: a save just before still gets its renders.
    void finalCheck()
    {
        stopTimer();
        if (pending_ == nullptr) return;
        const juce::File f(pending_->fileAtStore);
        if (pending_->fileAtStore.isNotEmpty() && f.existsAsFile() && f.getLastModificationTime() > pending_->mtimeAtStore) {
            logLine(T("проект записан перед закрытием: ") + f.getFullPathName());
            writeRenders(f, pending_->items);
        }
        pending_.reset();
    }

private:
    static void writeRenders(const juce::File& projectFile, const std::vector<PendingSave::Item>& items)
    {
        const juce::File folder = renderstore::folderFor(projectFile);
        juce::StringArray names;
        int written = 0, existed = 0, failed = 0;
        for (const auto& it : items) {
            juce::String name;
            switch (renderstore::write(folder, it.sourceName, it.key, it.cache->version, it.cache->sampleRate,
                                       it.cache->left, it.cache->right, name)) {
            case renderstore::WriteResult::written: ++written; names.addIfNotAlreadyThere(name); break;
            case renderstore::WriteResult::existed: ++existed; names.addIfNotAlreadyThere(name); break;
            case renderstore::WriteResult::failed:  ++failed; logLine(T("  не удалось записать ") + folder.getChildFile(name).getFullPathName()); break;
            }
        }
        renderstore::writeList(folder, projectFile.getFileName(), names);
        const int removed = renderstore::removeUnlisted(folder);
        juce::String msg;
        msg << T("рендер сохранён в ") << folder.getFullPathName() << T(": записано ") << written << T(", уже были ")
            << existed << T(", удалено старых ") << removed << T(", ошибок ") << failed;
        logLine(msg);
    }

    void timerCallback() override
    {
        auto& p = *pending_;
        const juce::String now = reaper::projectFile(p.project);
        juce::String how;
        if (now.isNotEmpty()) {
            const juce::File f(now);
            if (now != p.fileAtStore && f.existsAsFile())
                how = p.fileAtStore.isEmpty() ? T("первое сохранение проекта")
                                              : T("сохранён под новым именем; в момент записи состояния был ") + p.fileAtStore;
            else if (f.existsAsFile() && f.getLastModificationTime() > p.mtimeAtStore)
                how = T("файл проекта перезаписан");
            else if (p.dirtyAtStore > 0 && reaper::isDirty(p.project) == 0)
                how = T("проект отмечен как сохранённый");
        }
        if (how.isEmpty()) {
            // A never-saved project may be waiting in the "Save as" dialog: give it longer.
            const int limit = p.fileAtStore.isEmpty() ? 300 : 30;
            if ((juce::Time::getCurrentTime() - p.started).inSeconds() > limit) {
                logLine(T("за ") + juce::String(limit) + T(" с файл проекта не изменился — это была не запись проекта, рендер не сохраняю"));
                stopTimer();
                pending_.reset();
            }
            return;
        }
        stopTimer();
        logLine(T("проект записан: ") + now + " (" + how + ")");
        auto items = std::move(p.items);
        pending_.reset();
        dc_.pool.addJob([file = juce::File(now), items = std::move(items)] { writeRenders(file, items); });
    }

    GlinaDocumentController& dc_;
    std::unique_ptr<PendingSave> pending_;
};

GlinaDocumentController::~GlinaDocumentController()
{
    if (saveWatcher != nullptr) saveWatcher->finalCheck();
    saveWatcher.reset();
    pool.removeAllJobs(true, 20000);
    const std::lock_guard<std::mutex> lk(linksMutex);
    auto& links = hostLinks();
    links.erase(std::remove_if(links.begin(), links.end(), [this](const auto& e) { return e.first == this; }), links.end());
}

void GlinaDocumentController::registerHostLink(GlinaDocumentController* dc, reaper::HostLink* link)
{
    const std::lock_guard<std::mutex> lk(linksMutex);
    auto& links = hostLinks();
    links.erase(std::remove_if(links.begin(), links.end(), [link](const auto& e) { return e.second == link; }), links.end());
    if (dc != nullptr) links.emplace_back(dc, link);
}

void GlinaDocumentController::unregisterHostLink(reaper::HostLink* link) { registerHostLink(nullptr, link); }

void* GlinaDocumentController::reaperProject() const
{
    {
        const std::lock_guard<std::mutex> lk(linksMutex);
        for (const auto& [dc, link] : hostLinks())
            if (dc == this)
                if (void* p = link->project()) return p;
    }
    const auto open = reaper::openProjects();
    return open.size() == 1 ? open.front() : nullptr; // no instance knows yet: only safe with one tab
}

std::vector<juce::File> GlinaDocumentController::renderFolders() const
{
    std::vector<juce::File> out;
    auto add = [&out](void* project) {
        const juce::String file = reaper::projectFile(project);
        if (file.isEmpty()) return;
        const juce::File folder = renderstore::folderFor(juce::File(file));
        if (std::find(out.begin(), out.end(), folder) == out.end()) out.push_back(folder);
    };
    add(reaperProject());
    for (void* p : reaper::openProjects()) add(p);
    return out;
}

void GlinaDocumentController::watchSave(const std::vector<const GlinaAudioModification*>& mods)
{
    size_t total = 0;
    if (auto* doc = getDocumentController()->getDocument<juce::ARADocument>())
        for (auto* src : doc->getAudioSources()) total += src->getAudioModifications().size();
    const bool on = settings::saveRender();
    void* project = reaperProject();
    const juce::String file = reaper::projectFile(project);

    juce::String msg;
    msg << T("запись состояния Glina: клипов ") << (int)mods.size() << T(" из ") << (int)total << T(", проект ")
        << (project == nullptr ? T("неизвестен") : file.isEmpty() ? T("ещё не сохранён") : file)
        << T(", изменён: ") << reaper::isDirty(project) << T(", «Сохранять рендер» ") << (on ? T("вкл.") : T("выкл."));
    logLine(msg);
    if (!on || !reaper::available() || mods.empty() || mods.size() != total || project == nullptr) return;

    PendingSave p;
    p.project = project;
    p.fileAtStore = file;
    if (file.isNotEmpty()) p.mtimeAtStore = juce::File(file).getLastModificationTime();
    p.dirtyAtStore = reaper::isDirty(project);
    p.started = juce::Time::getCurrentTime();
    int notReady = 0;
    for (auto* m : mods) {
        const auto& data = *m->getGlinaSource()->data;
        auto cache = m->currentCache();
        if (!data.samplesReady || cache == nullptr || cache->key != renderstore::key(data.contentHash, serialize(m->doc))) {
            ++notReady; // still analysing or rendering: this state is rendered again on open
            continue;
        }
        p.items.push_back({ cache->key, m->sourceName(), std::move(cache) });
    }
    if (notReady > 0) logLine(T("  рендер ещё не готов у клипов: ") + juce::String(notReady));
    if (saveWatcher == nullptr) saveWatcher = std::make_shared<SaveWatcher>(*this);
    saveWatcher->start(std::move(p));
}

void GlinaDocumentController::rerender(GlinaAudioModification* mod)
{
    logLine(T("перерендер клипа «") + mod->sourceName() + T("» текущей версией"));
    mod->forceRender = true;
    ++mod->editVersion;
    startRender(mod);
    modelChanged.sendChangeMessage();
}

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

        data->contentHash = renderstore::hashSamples(mono.data(), mono.size(), data->sampleRate);
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
    const uint64_t key = renderstore::key(data->contentHash, serialize(doc));
    // A render saved with the project is used whatever the option says (it only controls writing).
    std::vector<juce::File> folders;
    if (!mod->forceRender) folders = renderFolders();
    mod->forceRender = false;
    const bool firstAfterLoad = mod->restoredFromArchive;
    mod->restoredFromArchive = false;
    const juce::String name = mod->sourceName();
    diagLog("renderStart: mod=" + hexOf(mod) + " name=\"" + name + "\" editVersion=" + juce::String(version)
            + " key=" + renderstore::keyText(key) + " searchFolders=" + juce::String((int)folders.size()));
    pool.addJob([this, mod, data, doc, eff, version, alive, key, folders, firstAfterLoad, name] {
        auto cache = std::make_shared<RenderCache>();
        renderstore::Loaded saved;
        const size_t maxLength = data->mono.size() + (size_t)(60.0 * data->sampleRate);
        if (renderstore::find(folders, key, data->sampleRate, maxLength, saved)) {
            cache->sampleRate = saved.sampleRate;
            cache->left = std::move(saved.left);
            cache->right = std::move(saved.right);
            cache->version = saved.version;
            cache->fromDisk = true;
            juce::String msg;
            msg << T("«") << name << T("»: рендер из папки проекта ") << saved.file.getFullPathName();
            if (saved.version != kRenderVersion) msg << T(" (прежняя версия движка: r") << saved.version << T(", сейчас r") << kRenderVersion << ")";
            logLine(msg);
        } else {
            if (firstAfterLoad && reaper::available()) {
                juce::String msg;
                msg << T("«") << name << T("»: сохранённого рендера нет (ключ ") << renderstore::keyText(key) << T("), считаю заново; ")
                    << (folders.empty() ? T("папка проекта неизвестна (проект не сохранён?)") : T("искал в:"));
                for (const auto& f : folders) msg << " " << f.getFullPathName() << ";";
                logLine(msg);
            }
            Mix m = renderMix(data->mono.data(), data->mono.size(), *eff, doc);
            cache->sampleRate = data->sampleRate;
            cache->left = std::move(m.left);
            cache->right = std::move(m.right);
        }
        cache->key = key;
        cache->map = TimeMap::build(*eff, doc.edits);
        cache->diagEditVersion = version;
        cache->diagAudioHash = renderstore::hashSamples(cache->left.data(), cache->left.size(), cache->sampleRate);
        const juce::String diag = describeRender(*cache, doc, eff->sampleRate);

        juce::MessageManager::callAsync([this, mod, cache, version, alive, name, diag] {
            if (!*alive) return;
            mod->renderRunning = false;
            mod->setCache(cache);
            mod->renderedVersion = version;
            mod->renderFromDisk = cache->fromDisk;
            mod->renderEngineVersion = cache->version;
            {
                juce::String regions;
                for (auto* region : mod->getPlaybackRegions()) regions << (regions.isEmpty() ? "" : ",") << hexOf(region);
                diagLog("renderDone: mod=" + hexOf(mod) + " name=\"" + name + "\" cache=" + hexOf(cache.get())
                        + " editVersion=" + juce::String(mod->editVersion) + " renderedVersion=" + juce::String(mod->renderedVersion)
                        + " " + diag + " regions=[" + regions + "]");
            }
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
    try {
        watchSave(std::vector<const GlinaAudioModification*>(mods.begin(), mods.end()));
    } catch (...) {
        // Saving renders is a convenience: never let it fail the project save.
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
            m->restoredFromArchive = true;
            rebuildEffective(m);   // no-op until analysis is ready
            ++m->editVersion;
            startRender(m);        // no-op until analysis is ready
        }
    }
    return !in.failed();
}

} // namespace glina::plugin
