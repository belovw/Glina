// Rendered audio kept next to the DAW project, in <folder of the .rpp>/Glina/.
//
// A file is the render of one (source audio, document) pair. Its name carries the key
// (hash of the source samples + the serialised document) and the render version that
// made it: "<source>_<key>_r<version>.wav", 32-bit float, mono when both sides match.
// A stale file can therefore never be picked up, and a render made by another engine
// version is recognised as such (it is used as is: the project keeps its sound).
//
// Several .rpp files in one folder share the Glina folder (same state = same file).
// Each saved project lists its files in Glina/<project file name>.list; a file is
// deleted only when no list in the folder mentions it.
#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <cstdint>
#include <string>
#include <vector>

namespace glina::plugin {

// ---- settings shared by every Glina instance (user app data/Glina/Glina.settings) ----
namespace settings {
bool saveRender();             // "save the render in the project folder" (default: on)
void setSaveRender(bool on);
} // namespace settings

// ---- diagnostics: user app data/Glina/glina.log (thread-safe) ----------------------
juce::File logFile();
void logLine(const juce::String& text);

namespace renderstore {

uint64_t hashSamples(const float* x, size_t n, double sampleRate);
uint64_t key(uint64_t sourceHash, const std::string& serializedDocument);
juce::String keyText(uint64_t key); // 16 hex digits
juce::File folderFor(const juce::File& projectFile);

struct Loaded {
    double sampleRate = 0;
    std::vector<float> left, right;
    int version = 0;
    juce::File file;
};
// First folder holding a readable render of `key` wins (one made by the current
// version is preferred within a folder). Any thread.
bool find(const std::vector<juce::File>& folders, uint64_t key, double expectedRate, size_t maxLength, Loaded& out);

// Writes the render unless that exact file exists; removes other versions of the same
// key. `fileName` receives the file's name in the folder. Any thread.
enum class WriteResult { written, existed, failed };
WriteResult write(const juce::File& folder, const juce::String& sourceName, uint64_t key, int version,
                  double sampleRate, const std::vector<float>& left, const std::vector<float>& right,
                  juce::String& fileName);

// Records which files a project uses, then deletes the renders no list mentions.
// Returns the number of files deleted.
void writeList(const juce::File& folder, const juce::String& projectFileName, const juce::StringArray& files);
int removeUnlisted(const juce::File& folder);

} // namespace renderstore
} // namespace glina::plugin
