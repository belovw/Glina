#pragma once

// Test-only (built when GLINA_HOST_PROBE=ON): writes to Desktop/glina_host_test.txt what the
// VST3 host tells the plug-in about itself and about the current REAPER project/media folder.
// Does not touch audio processing or the editor.

#include <juce_audio_processors/juce_audio_processors.h>

namespace glina::plugin {

class ReaperHostProbe final : private juce::Timer {
public:
    ReaperHostProbe();
    ~ReaperHostProbe() override;

    // From VST3ClientExtensions::setIHostApplication (message thread).
    void attach(Steinberg::FUnknown* hostContext);

private:
    void timerCallback() override;
    void runProbe(const juce::String& why);

    Steinberg::FUnknown* host = nullptr;   // add-ref'd
    void* reaperHost = nullptr;            // IReaperHostApplication*, add-ref'd
    int instanceId = 0;
    int attachCount = 0;
    int timerStep = 0;
};

} // namespace glina::plugin
