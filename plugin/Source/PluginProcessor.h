#pragma once

#include "GlinaModel.h"
#ifdef GLINA_HOST_PROBE
#include "ReaperHostProbe.h"
#endif

namespace glina::plugin {

class GlinaProcessor final : public juce::AudioProcessor,
                             public juce::AudioProcessorARAExtension
#ifdef GLINA_HOST_PROBE
    ,
                             public juce::VST3ClientExtensions
#endif
{
public:
    GlinaProcessor();

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;

    const juce::String getName() const override { return "Glina"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override;
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}   // state lives in the ARA document
    void setStateInformation(const void*, int) override {}

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;

    // Song position for the editor's playhead (written by the audio thread).
    std::atomic<double> playheadSec { 0 };
    std::atomic<bool> playing { false };

#ifdef GLINA_HOST_PROBE
    // Test build only: log what the host tells us (Desktop/glina_host_test.txt).
    juce::VST3ClientExtensions* getVST3ClientExtensions() override { return this; }
    void setIHostApplication(Steinberg::FUnknown* hostContext) override { hostProbe.attach(hostContext); }
#endif

private:
#ifdef GLINA_HOST_PROBE
    ReaperHostProbe hostProbe;
#endif
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GlinaProcessor)
};

} // namespace glina::plugin
