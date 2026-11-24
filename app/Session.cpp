#include "Session.h"
#include "../ui/Theme.h"

#include "glina/Notes.h"

namespace glina::app {

using ui::T;

namespace {
inline float cubicAt(const float* x, size_t n, double t) noexcept
{
    const long i = (long)std::floor(t);
    const float f = (float)(t - (double)i);
    auto s = [&](long k) { return (k >= 0 && (size_t)k < n) ? x[k] : 0.f; };
    const float y0 = s(i - 1), y1 = s(i), y2 = s(i + 1), y3 = s(i + 2);
    const float a = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
    const float b = y0 - 2.5f * y1 + 2.f * y2 - 0.5f * y3;
    const float c = -0.5f * y0 + 0.5f * y2;
    return ((a * f + b) * f + c) * f + y1;
}

bool writeWav(const juce::File& file, const float* const* channels, int numChannels, size_t numSamples,
              double sampleRate, juce::String& error)
{
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> out = std::make_unique<juce::FileOutputStream>(file);
    if (static_cast<juce::FileOutputStream*>(out.get())->failedToOpen()) {
        error = T("Не удалось создать файл ") + file.getFullPathName();
        return false;
    }
    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor(out, juce::AudioFormatWriterOptions{}
                                               .withSampleRate(sampleRate)
                                               .withNumChannels(numChannels)
                                               .withBitsPerSample(24));
    if (writer == nullptr) { error = T("WAV: не удалось начать запись"); return false; }
    const int block = 65536;
    std::vector<const float*> ptrs((size_t)numChannels);
    for (size_t pos = 0; pos < numSamples; pos += block) {
        const int n = (int)std::min<size_t>(block, numSamples - pos);
        for (int c = 0; c < numChannels; ++c) ptrs[(size_t)c] = channels[c] + pos;
        if (!writer->writeFromFloatArrays(ptrs.data(), numChannels, n)) { error = T("Ошибка записи WAV"); return false; }
    }
    return true;
}
} // namespace

Session::Session() { formats_.registerBasicFormats(); }

Session::~Session()
{
    alive_->store(false);
    pool_.removeAllJobs(true, 20000);
}

double Session::lengthSec() const
{
    if (lastRender_ != nullptr) return (double)lastRender_->left.size() / lastRender_->sampleRate;
    return mono_ != nullptr ? (double)mono_->size() / sampleRate_ : 0.0;
}

void Session::reset()
{
    ++generation_;
    playing_ = false;
    pos_ = 0;
    base_.reset();
    eff_.reset();
    doc_ = Document{};
    history_.clear();
    pendingDocument_.clear();
    editVersion_ = renderedVersion_ = 0;
    renderRunning_ = false;
    lastRender_.reset();
    dirty_ = false;
    projectFile_ = juce::File();
}

bool Session::readMono(const juce::File& file, std::shared_ptr<std::vector<float>>& mono, double& sampleRate,
                       juce::String& error)
{
    std::unique_ptr<juce::AudioFormatReader> reader(formats_.createReaderFor(file));
    if (reader == nullptr) {
        error = T("Не удалось открыть «") + file.getFileName() + T("». Поддерживаются WAV, AIFF, FLAC, OGG, MP3.");
        return false;
    }
    const auto len = reader->lengthInSamples;
    if (len <= 0 || len > (juce::int64)reader->sampleRate * 60 * 30) {
        error = T("Файл пустой или длиннее 30 минут.");
        return false;
    }
    const int ch = (int)std::max(1u, reader->numChannels);
    juce::AudioBuffer<float> buf(ch, (int)len);
    if (!reader->read(&buf, 0, (int)len, 0, true, ch > 1)) { error = T("Ошибка чтения файла."); return false; }
    mono = std::make_shared<std::vector<float>>((size_t)len, 0.f);
    for (int c = 0; c < ch; ++c) {
        const float* p = buf.getReadPointer(c);
        for (juce::int64 i = 0; i < len; ++i) (*mono)[(size_t)i] += p[i] / (float)ch;
    }
    sampleRate = reader->sampleRate;
    return true;
}

bool Session::openAudio(const juce::File& file, juce::String& error)
{
    std::shared_ptr<std::vector<float>> mono;
    double sr = 48000;
    if (!readMono(file, mono, sr, error)) return false;

    reset();
    mono_ = std::move(mono);
    sampleRate_ = sr;
    audioFile_ = file;
    publish(nullptr);
    startAnalysis();
    changes.sendChangeMessage();
    return true;
}

bool Session::openProject(const juce::File& file, juce::String& error)
{
    const juce::String text = file.loadFileAsString();
    if (!text.startsWith("glina-project 1")) { error = T("Это не проект Glina."); return false; }
    const juce::String audioLine = text.fromFirstOccurrenceOf("audio ", false, false).upToFirstOccurrenceOf("\n", false, false).trim();
    juce::File audio(audioLine);
    if (!audio.existsAsFile()) audio = file.getParentDirectory().getChildFile(juce::File(audioLine).getFileName());
    if (!audio.existsAsFile()) { error = T("Не найден аудиофайл проекта: ") + audioLine; return false; }
    const std::string docText = text.fromFirstOccurrenceOf("glina-document", true, false).toStdString();
    Document test;
    if (!deserialize(docText, test)) { error = T("Проект повреждён."); return false; }
    if (!openAudio(audio, error)) return false;
    pendingDocument_ = docText;
    projectFile_ = file;
    return true;
}

bool Session::saveProject(const juce::File& file, juce::String& error)
{
    if (mono_ == nullptr) { error = T("Нечего сохранять."); return false; }
    juce::String text;
    text << "glina-project 1\n";
    text << "audio " << audioFile_.getFullPathName() << "\n";
    text << juce::String::fromUTF8(serialize(doc_).c_str());
    if (!file.replaceWithText(text, false, false, "\n")) { error = T("Не удалось записать ") + file.getFullPathName(); return false; }
    projectFile_ = file;
    dirty_ = false;
    return true;
}

bool Session::exportMix(const juce::File& file, juce::String& error) const
{
    if (lastRender_ == nullptr || !const_cast<Session*>(this)->isIdle()) { error = T("Подождите окончания рендера."); return false; }
    const float* ch[2] = {lastRender_->left.data(), lastRender_->right.data()};
    return writeWav(file, ch, 2, lastRender_->left.size(), lastRender_->sampleRate, error);
}

bool Session::exportStems(const juce::File& folder, juce::String& error) const
{
    if (lastRender_ == nullptr || !const_cast<Session*>(this)->isIdle()) { error = T("Подождите окончания рендера."); return false; }
    folder.createDirectory();
    const juce::String base = audioFile_.getFileNameWithoutExtension();
    const float* lead[1] = {lastRender_->lead.data()};
    if (!writeWav(folder.getChildFile(base + "_lead.wav"), lead, 1, lastRender_->lead.size(), lastRender_->sampleRate, error))
        return false;
    for (size_t j = 0; j < lastRender_->voices.size(); ++j) {
        const float* v[1] = {lastRender_->voices[j].data()};
        if (!writeWav(folder.getChildFile(base + "_harmony" + juce::String((int)j + 1) + ".wav"), v, 1,
                      lastRender_->voices[j].size(), lastRender_->sampleRate, error))
            return false;
    }
    return true;
}

// ---- reference recording ------------------------------------------------------------
bool Session::loadReference(const juce::File& file, juce::String& error)
{
    std::shared_ptr<std::vector<float>> mono;
    double sr = 48000;
    if (!readMono(file, mono, sr, error)) return false;
    refFile_ = file;
    ref_.reset();
    refLoading_ = true;
    const uint64_t gen = ++refGen_;
    auto alive = alive_;
    pool_.addJob([this, mono, sr, gen, alive] {
        auto a = std::make_shared<const Analysis>(analyze(mono->data(), mono->size(), sr));
        juce::MessageManager::callAsync([this, a, gen, alive] {
            if (!*alive || gen != refGen_) return;
            ref_ = a;
            refLoading_ = false;
            changes.sendChangeMessage();
        });
    });
    changes.sendChangeMessage();
    return true;
}

void Session::openReference()
{
    const juce::File start = audioFile_ != juce::File() ? audioFile_.getParentDirectory() : juce::File();
    refChooser_ = std::make_unique<juce::FileChooser>(T("Референс: запись, под которую подгонять"), start,
                                                     "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
    refChooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                             [this](const juce::FileChooser& fc) {
                                 const juce::File f = fc.getResult();
                                 if (f == juce::File()) return;
                                 juce::String err;
                                 if (!loadReference(f, err))
                                     juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                                                                      .withIconType(juce::MessageBoxIconType::WarningIcon)
                                                                      .withTitle("Glina").withMessage(err).withButton("OK", 0),
                                                                  nullptr);
                             });
}

std::vector<ui::ClipHost::TrackRef> Session::tracks() const
{
    std::vector<TrackRef> out;
    if (mono_ != nullptr) {
        TrackRef t;
        t.id = "main";
        t.name = audioFile_.getFileName();
        t.current = true;
        out.push_back(t);
    }
    if (ref_ != nullptr || refLoading_) {
        TrackRef t;
        t.id = "reference";
        t.name = T("Референс: ") + refFile_.getFileName() + (refLoading_ ? T(" (анализ…)") : juce::String());
        t.editable = false;
        out.push_back(t);
    }
    return out;
}

bool Session::ghost(const juce::String& id, Ghost& out) const
{
    if (id != "reference" || ref_ == nullptr) return false;
    // Both recordings start at 0 s: the reference lies on the same timeline as the file.
    const double len = (double)ref_->numSamples / ref_->sampleRate;
    out = ui::buildGhost(*ref_, EditSet{}, [](double t) { return t; }, 0.0, len);
    return true;
}

bool Session::thread(const juce::String& id, std::vector<ThreadFrame>& out) const
{
    if (id != "reference" || ref_ == nullptr) return false;
    const double len = (double)ref_->numSamples / ref_->sampleRate;
    out = threadOf(*ref_, EditSet{}, [](double t) { return t; }, 0.0, len);
    return !out.empty();
}

bool Session::trackKey(const juce::String& id, Key& out) const
{
    if (id != "reference" || ref_ == nullptr) return false;
    out = ref_->key;
    return true;
}

// ---- analysis / rendering ------------------------------------------------------------
void Session::startAnalysis()
{
    busy_ = T("Анализ вокала…");
    auto mono = mono_;
    const double sr = sampleRate_;
    const uint64_t gen = generation_;
    auto alive = alive_;
    pool_.addJob([this, mono, sr, gen, alive] {
        auto a = std::make_shared<Analysis>(analyze(mono->data(), mono->size(), sr));
        juce::MessageManager::callAsync([this, a, gen, alive] {
            if (!*alive || gen != generation_) return;
            base_ = a;
            if (!pendingDocument_.empty()) {
                Document d;
                if (deserialize(pendingDocument_, d)) doc_ = std::move(d);
                pendingDocument_.clear();
            }
            rebuildEffective();
            busy_.clear();
            ++editVersion_;
            startRender();
            changes.sendChangeMessage();
        });
    });
}

void Session::rebuildEffective()
{
    if (base_ == nullptr) return;
    // Discard a layout that does not fit this analysis (e.g. project from another file).
    auto& spans = doc_.edits.spans;
    const int nf = (int)base_->frames.size();
    for (const auto& s : spans)
        if (s.start < 0 || s.end > nf || s.end <= s.start) { spans.clear(); break; }
    eff_ = std::make_shared<const Analysis>(withLayout(*base_, spans));
    conform(doc_, *eff_);
}

void Session::documentChanged(bool layoutChanged)
{
    if (layoutChanged) rebuildEffective();
    if (eff_ != nullptr) conform(doc_, *eff_);
    dirty_ = true;
    ++editVersion_;
    startRender();
}

void Session::startRender()
{
    if (eff_ == nullptr || mono_ == nullptr || renderRunning_ || renderedVersion_ == editVersion_) return;
    renderRunning_ = true;
    auto eff = eff_;
    auto mono = mono_;
    const Document d = doc_;
    const uint64_t version = editVersion_, gen = generation_;
    const double sr = sampleRate_;
    auto alive = alive_;
    pool_.addJob([this, eff, mono, d, version, gen, sr, alive] {
        Mix m = renderMix(mono->data(), mono->size(), *eff, d);
        auto r = std::make_shared<Rendered>();
        r->sampleRate = sr;
        r->left = std::move(m.left);
        r->right = std::move(m.right);
        r->lead = std::move(m.lead);
        r->voices = std::move(m.voices);
        r->map = TimeMap::build(*eff, d.edits);
        juce::MessageManager::callAsync([this, r, version, gen, alive] {
            if (!*alive || gen != generation_) return;
            renderRunning_ = false;
            renderedVersion_ = version;
            lastRender_ = r;
            publish(r);
            changes.sendChangeMessage();
            if (editVersion_ != version) startRender(); // more edits arrived meanwhile
        });
    });
}

void Session::publish(std::shared_ptr<const Rendered> r)
{
    auto p = std::make_shared<Playback>();
    p->mono = mono_;
    p->rendered = std::move(r);
    p->sampleRate = sampleRate_;
    std::shared_ptr<const Playback> old = p;
    {
        const juce::SpinLock::ScopedLockType sl(playLock_);
        std::swap(play_, old);
    }
    // The audio thread may still hold the previous state: release it here, later.
    if (old) retired_.push_back(std::move(old));
    retired_.erase(std::remove_if(retired_.begin(), retired_.end(), [](const auto& x) { return x.use_count() == 1; }),
                   retired_.end());
}

// ---- playback ------------------------------------------------------------------------------
void Session::setPlaying(bool p)
{
    if (p && pos_.load() >= lengthSec() - 0.05) pos_ = 0;
    playing_ = p && mono_ != nullptr;
}

void Session::prepareToPlay(int, double sampleRate) { deviceRate_ = sampleRate > 0 ? sampleRate : 48000; }

void Session::getNextAudioBlock(const juce::AudioSourceChannelInfo& info)
{
    info.clearActiveBufferRegion();
    if (!playing_.load()) return;
    std::shared_ptr<const Playback> p;
    {
        const juce::SpinLock::ScopedTryLockType sl(playLock_);
        if (!sl.isLocked()) return;
        p = play_;
    }
    if (p == nullptr || p->mono == nullptr) return;

    float* L = info.buffer->getWritePointer(0, info.startSample);
    float* R = info.buffer->getNumChannels() > 1 ? info.buffer->getWritePointer(1, info.startSample) : nullptr;
    const bool dry = bypass_.load() || p->rendered == nullptr;
    const std::vector<float>& mono = *p->mono;
    const Rendered* r = p->rendered.get();
    const double end = r != nullptr ? (double)r->left.size() / r->sampleRate : (double)mono.size() / p->sampleRate;
    double t = pos_.load();
    const double dt = 1.0 / deviceRate_;
    for (int i = 0; i < info.numSamples; ++i, t += dt) {
        if (dry) {
            // A/B: the original at the same musical position (through the time map).
            const double inPos = r != nullptr ? r->map.outToIn(t * r->sampleRate) * (p->sampleRate / r->sampleRate)
                                              : t * p->sampleRate;
            const float s = cubicAt(mono.data(), mono.size(), inPos);
            L[i] = s;
            if (R) R[i] = s;
        } else {
            const double s = t * r->sampleRate;
            L[i] = cubicAt(r->left.data(), r->left.size(), s);
            if (R) R[i] = cubicAt(r->right.data(), r->right.size(), s);
            else L[i] = 0.5f * (L[i] + cubicAt(r->right.data(), r->right.size(), s));
        }
    }
    pos_.store(t);
    if (t >= end) playing_.store(false);
}

} // namespace glina::app
