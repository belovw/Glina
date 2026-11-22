#pragma once

#include "GlinaModel.h"

namespace glina::plugin {

class GlinaProcessor final : public juce::AudioProcessor,
                             public juce::AudioProcessorARAExtension,
                             public juce::VST3ClientExtensions {
public:
    GlinaProcessor();
    ~GlinaProcessor() override;

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

    // VST3: REAPER hands over its host extension here (which project this FX is in).
    juce::VST3ClientExtensions* getVST3ClientExtensions() override { return this; }
    void setIHostApplication(Steinberg::FUnknown* hostContext) override { hostLink_.attach(hostContext); }
    // REAPER's edit cursor in project seconds; NaN until REAPER tells us which project we are in.
    double editCursorSec() const { return reaper::cursorSec(hostLink_.project()); }
    bool hasReaperProject() const { return hostLink_.project() != nullptr; }

protected:
    void didBindToARA() noexcept override;

private:
    reaper::HostLink hostLink_;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GlinaProcessor)
};

} // namespace glina::plugin
