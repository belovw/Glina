#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace glina::plugin {

GlinaProcessor::GlinaProcessor()
    : juce::AudioProcessor(BusesProperties()
                               .withInput("Input", juce::AudioChannelSet::stereo(), true)
                               .withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
}

GlinaProcessor::~GlinaProcessor() { GlinaDocumentController::unregisterHostLink(&hostLink_); }

void GlinaProcessor::didBindToARA() noexcept
{
    juce::AudioProcessorARAExtension::didBindToARA();
    // Lets the document controller find its REAPER project (for renders kept next to it).
    GlinaDocumentController::registerHostLink(
        juce::ARADocumentControllerSpecialisation::getSpecialisedDocumentController<GlinaDocumentController>(
            ARA::PlugIn::PlugInExtension::getDocumentController()),
        &hostLink_);
}

void GlinaProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    prepareToPlayForARA(sampleRate, samplesPerBlock, getMainBusNumOutputChannels(), getProcessingPrecision());
}

void GlinaProcessor::releaseResources() { releaseResourcesForARA(); }

bool GlinaProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo();
}

void GlinaProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    auto* ph = getPlayHead();
    if (ph != nullptr) {
        if (const auto p = ph->getPosition()) {
            playing.store(p->getIsPlaying(), std::memory_order_relaxed);
            if (const auto t = p->getTimeInSeconds()) playheadSec.store(*t, std::memory_order_relaxed);
        }
    }
    if (!processBlockForARA(buffer, isRealtime(), ph))
        processBlockBypassed(buffer, midi);
}

double GlinaProcessor::getTailLengthSeconds() const
{
    double tail = 0;
    return getTailLengthSecondsForARA(tail) ? tail : 0.0;
}

juce::AudioProcessorEditor* GlinaProcessor::createEditor() { return new GlinaEditor(*this); }

} // namespace glina::plugin

// ---- entry points -----------------------------------------------------------
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new glina::plugin::GlinaProcessor(); }

const ARA::ARAFactory* JUCE_CALLTYPE createARAFactory()
{
    return juce::ARADocumentControllerSpecialisation::createARAFactory<glina::plugin::GlinaDocumentController>();
}
