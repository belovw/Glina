#include "RenderStore.h"

#include <juce_data_structures/juce_data_structures.h>

#include <algorithm>
#include <mutex>
#include <set>

namespace glina::plugin {

// ---- settings --------------------------------------------------------------------
namespace settings {
namespace {
std::mutex settingsMutex;

juce::PropertiesFile::Options options()
{
    juce::PropertiesFile::Options o;
    o.applicationName = "Glina";
    o.filenameSuffix = ".settings";
    o.folderName = "Glina";
    o.osxLibrarySubFolder = "Application Support";
    o.millisecondsBeforeSaving = -1; // saved explicitly (no timer: several plug-in instances share the file)
    return o;
}
} // namespace

bool saveRender()
{
    const std::lock_guard<std::mutex> lk(settingsMutex);
    juce::PropertiesFile p(options());
    return p.getBoolValue("saveRenderInProject", true);
}

void setSaveRender(bool on)
{
    const std::lock_guard<std::mutex> lk(settingsMutex);
    juce::PropertiesFile p(options());
    p.setValue("saveRenderInProject", on);
    p.saveIfNeeded();
}
} // namespace settings

// ---- log -----------------------------------------------------------------------------
juce::File logFile()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("Glina")
        .getChildFile("glina.log");
}

void logLine(const juce::String& text)
{
    static std::mutex logMutex;
    const std::lock_guard<std::mutex> lk(logMutex);
    const auto f = logFile();
    f.getParentDirectory().createDirectory();
    if (f.getSize() > 1024 * 1024) f.moveFileTo(f.withFileExtension(".old.log"));
    const auto t = juce::Time::getCurrentTime();
    const juce::String stamp = t.formatted("%Y-%m-%d %H:%M:%S") + "." + juce::String(t.getMilliseconds()).paddedLeft('0', 3);
    f.appendText("[" + stamp + "] " + text + "\n", false, false, "\r\n");
}

namespace renderstore {
namespace {

constexpr uint64_t kFnvOffset = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

inline uint64_t mixBytes(uint64_t h, const void* p, size_t n)
{
    const auto* b = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= kFnvPrime; }
    return h;
}

int versionOf(const juce::String& fileName)
{
    return fileName.fromLastOccurrenceOf("_r", false, false).upToFirstOccurrenceOf(".", false, false).getIntValue();
}

// "<anything>_<16 hex>_r<digits>.wav" — only such files are ever deleted.
bool isRenderFileName(const juce::String& name)
{
    if (!name.endsWithIgnoreCase(".wav")) return false;
    const juce::String s = name.dropLastCharacters(4);
    const int r = s.lastIndexOf("_r");
    if (r < 17) return false;
    const juce::String ver = s.substring(r + 2);
    if (ver.isEmpty() || !ver.containsOnly("0123456789")) return false;
    const juce::String hex = s.substring(r - 16, r);
    return s[r - 17] == '_' && hex.containsOnly("0123456789abcdefABCDEF");
}

juce::String stem(const juce::String& sourceName)
{
    juce::String s = sourceName.trim();
    if (s.containsChar('.') && s.fromLastOccurrenceOf(".", false, false).length() <= 5)
        s = s.upToLastOccurrenceOf(".", false, false);
    s = juce::File::createLegalFileName(s).substring(0, 48).trim();
    return s.isEmpty() ? juce::String("clip") : s;
}

bool readRender(const juce::File& file, double expectedRate, size_t maxLength, Loaded& out)
{
    auto* in = new juce::FileInputStream(file);
    if (in->failedToOpen()) { delete in; return false; }
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> r(wav.createReaderFor(in, true));
    if (r == nullptr) return false;
    const int channels = (int)r->numChannels;
    const auto length = r->lengthInSamples;
    if (channels < 1 || channels > 2 || length <= 0 || (size_t)length > maxLength) return false;
    if (std::abs(r->sampleRate - expectedRate) > 0.5) return false;

    out.left.assign((size_t)length, 0.f);
    out.right.assign((size_t)length, 0.f);
    const int block = 1 << 20;
    for (juce::int64 pos = 0; pos < length; pos += block) {
        const int n = (int)std::min<juce::int64>(block, length - pos);
        float* dest[2] = { out.left.data() + pos, out.right.data() + pos };
        if (!r->read(dest, channels, pos, n)) return false;
    }
    if (channels == 1) out.right = out.left;
    out.sampleRate = r->sampleRate;
    out.version = versionOf(file.getFileName());
    out.file = file;
    return true;
}

void removeOtherVersions(const juce::File& folder, uint64_t key, const juce::File& keep)
{
    for (const auto& f : folder.findChildFiles(juce::File::findFiles, false, "*_" + keyText(key) + "_r*.wav"))
        if (f != keep && isRenderFileName(f.getFileName())) f.deleteFile();
}

} // namespace

uint64_t hashSamples(const float* x, size_t n, double sampleRate)
{
    uint64_t h = kFnvOffset;
    const uint64_t len = n;
    h = mixBytes(h, &len, sizeof len);
    h = mixBytes(h, &sampleRate, sizeof sampleRate);
    return mixBytes(h, x, n * sizeof(float));
}

uint64_t key(uint64_t sourceHash, const std::string& serializedDocument)
{
    uint64_t h = mixBytes(kFnvOffset, &sourceHash, sizeof sourceHash);
    return mixBytes(h, serializedDocument.data(), serializedDocument.size());
}

juce::String keyText(uint64_t key) { return juce::String::toHexString((juce::int64)key).paddedLeft('0', 16); }

juce::File folderFor(const juce::File& projectFile) { return projectFile.getParentDirectory().getChildFile("Glina"); }

bool find(const std::vector<juce::File>& folders, uint64_t key, double expectedRate, size_t maxLength, Loaded& out)
{
    const juce::String pattern = "*_" + keyText(key) + "_r*.wav";
    for (const auto& folder : folders) {
        if (!folder.isDirectory()) continue;
        auto files = folder.findChildFiles(juce::File::findFiles, false, pattern);
        std::stable_sort(files.begin(), files.end(), [](const juce::File& a, const juce::File& b) {
            return versionOf(a.getFileName()) > versionOf(b.getFileName()); // newest engine first
        });
        for (const auto& f : files)
            if (isRenderFileName(f.getFileName()) && readRender(f, expectedRate, maxLength, out)) return true;
    }
    return false;
}

WriteResult write(const juce::File& folder, const juce::String& sourceName, uint64_t key, int version,
                  double sampleRate, const std::vector<float>& left, const std::vector<float>& right,
                  juce::String& fileName)
{
    // One file per key: reuse the name of a render that is already there (the source
    // may have been renamed since), otherwise derive it from the source name.
    fileName = {};
    const juce::String suffix = "_" + keyText(key) + "_r" + juce::String(version) + ".wav";
    for (const auto& f : folder.findChildFiles(juce::File::findFiles, false, "*" + suffix))
        if (isRenderFileName(f.getFileName()) && f.getSize() > 44) { fileName = f.getFileName(); break; }
    if (fileName.isNotEmpty()) {
        removeOtherVersions(folder, key, folder.getChildFile(fileName));
        return WriteResult::existed;
    }
    fileName = stem(sourceName) + suffix;
    const juce::File target = folder.getChildFile(fileName);
    if (left.empty() || left.size() != right.size() || !folder.createDirectory()) return WriteResult::failed;

    const juce::File part = folder.getChildFile(fileName + ".part");
    part.deleteFile();
    {
        std::unique_ptr<juce::OutputStream> out = std::make_unique<juce::FileOutputStream>(part);
        if (static_cast<juce::FileOutputStream*>(out.get())->failedToOpen()) return WriteResult::failed;
        const bool mono = left == right;
        juce::WavAudioFormat wav;
        auto writer = wav.createWriterFor(out, juce::AudioFormatWriterOptions{}
                                                   .withSampleRate(sampleRate)
                                                   .withNumChannels(mono ? 1 : 2)
                                                   .withBitsPerSample(32)
                                                   .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
        if (writer == nullptr) { out.reset(); part.deleteFile(); return WriteResult::failed; }
        const size_t n = left.size();
        const size_t block = 65536;
        bool ok = true;
        for (size_t pos = 0; pos < n && ok; pos += block) {
            const int len = (int)std::min(block, n - pos);
            const float* chans[2] = { left.data() + pos, right.data() + pos };
            ok = writer->writeFromFloatArrays(chans, mono ? 1 : 2, len);
        }
        writer.reset(); // finishes the header and closes the file
        if (!ok) { part.deleteFile(); return WriteResult::failed; }
    }
    target.deleteFile();
    if (!part.moveFileTo(target)) { part.deleteFile(); return WriteResult::failed; }
    removeOtherVersions(folder, key, target);
    return WriteResult::written;
}

void writeList(const juce::File& folder, const juce::String& projectFileName, const juce::StringArray& files)
{
    if (!folder.createDirectory()) return;
    folder.getChildFile(projectFileName + ".list").replaceWithText(files.joinIntoString("\n") + "\n");
}

int removeUnlisted(const juce::File& folder)
{
    if (!folder.isDirectory()) return 0;
    std::set<juce::String> listed;
    for (const auto& list : folder.findChildFiles(juce::File::findFiles, false, "*.list"))
        for (const auto& line : juce::StringArray::fromLines(list.loadFileAsString()))
            if (line.trim().isNotEmpty()) listed.insert(line.trim().toLowerCase());

    int removed = 0;
    for (const auto& f : folder.findChildFiles(juce::File::findFiles, false, "*.wav"))
        if (isRenderFileName(f.getFileName()) && listed.count(f.getFileName().toLowerCase()) == 0 && f.deleteFile())
            ++removed;
    // Leftovers of an interrupted write (an hour old: another instance may be writing right now).
    const auto hourAgo = juce::Time::getCurrentTime() - juce::RelativeTime::hours(1);
    for (const auto& f : folder.findChildFiles(juce::File::findFiles, false, "*.wav.part"))
        if (isRenderFileName(f.getFileNameWithoutExtension()) && f.getLastModificationTime() < hourAgo) f.deleteFile();
    return removed;
}

} // namespace renderstore
} // namespace glina::plugin
