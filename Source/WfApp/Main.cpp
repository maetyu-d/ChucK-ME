#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <WeldChucKEngine.h>

#include "WfChucKPrograms.h"

#include <atomic>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <random>
#include <regex>
#include <string>

namespace
{
constexpr int maxHostChannels = 16;
constexpr int fallbackBlockSize = 4096;
constexpr int engineOutputChannels = 2;
constexpr int maxTopLevelStates = 16;
constexpr int maxStateTracks = 16;
constexpr int maxTrackLanes = 8;
constexpr int maxTrackEffectSlots = 3;
constexpr int maxGraphTransitions = 32;
constexpr float defaultPlaybackRate = 1.0f;
constexpr float defaultIntensity = 0.58f;
constexpr float defaultBrightness = 0.48f;
constexpr auto laneDeclarationMarker = "// wf::declaration";
constexpr auto laneControlMarker = "// wf::control";

juce::Colour ink() { return juce::Colour (0xfff2f4ef); }
juce::Colour mutedInk() { return juce::Colour (0xff9aa29c); }
juce::Colour panel() { return juce::Colour (0xff111512); }
juce::Colour panelSoft() { return juce::Colour (0xff1a201c); }
juce::Colour green() { return juce::Colour (0xff8adf9a); }
juce::Colour amber() { return juce::Colour (0xffd6b15f); }
juce::Colour coral() { return juce::Colour (0xffd27a70); }
juce::Colour blue() { return juce::Colour (0xff8ab6dc); }

void clearOutputs (float* const* outputChannelData, int numOutputChannels, int numSamples)
{
    if (outputChannelData == nullptr || numSamples <= 0)
        return;

    for (int channel = 0; channel < numOutputChannels; ++channel)
        if (outputChannelData[channel] != nullptr)
            juce::FloatVectorOperations::clear (outputChannelData[channel], numSamples);
}

void styleButton (juce::TextButton& button, juce::Colour colour)
{
    button.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff171d19));
    button.setColour (juce::TextButton::buttonOnColourId, colour.withAlpha (0.22f));
    button.setColour (juce::TextButton::textColourOffId, ink().withAlpha (0.88f));
    button.setColour (juce::TextButton::textColourOnId, ink());
    button.setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void styleEmptyStateButton (juce::TextButton& button)
{
    button.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff141815));
    button.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff141815));
    button.setColour (juce::TextButton::textColourOffId, mutedInk().withAlpha (0.32f));
    button.setColour (juce::TextButton::textColourOnId, mutedInk().withAlpha (0.32f));
}

void styleSlider (juce::Slider& slider, juce::Colour colour)
{
    slider.setSliderStyle (juce::Slider::LinearHorizontal);
    slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 56, 22);
    slider.setColour (juce::Slider::trackColourId, colour);
    slider.setColour (juce::Slider::thumbColourId, ink());
    slider.setColour (juce::Slider::backgroundColourId, mutedInk().withAlpha (0.10f));
    slider.setColour (juce::Slider::textBoxTextColourId, ink());
    slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
}

void styleComboBox (juce::ComboBox& comboBox)
{
    comboBox.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff151a17));
    comboBox.setColour (juce::ComboBox::textColourId, ink());
    comboBox.setColour (juce::ComboBox::outlineColourId, mutedInk().withAlpha (0.18f));
    comboBox.setColour (juce::ComboBox::arrowColourId, ink());
}

void styleLabel (juce::Label& label, float alpha = 1.0f)
{
    label.setColour (juce::Label::textColourId, ink().withAlpha (alpha));
    label.setJustificationType (juce::Justification::centredLeft);
}
}

class WfAudioCallback final : public juce::AudioIODeviceCallback
{
public:
    WfAudioCallback()
    {
        juce::addDefaultFormatsToManager (pluginFormatManager);
    }

    void audioDeviceAboutToStart (juce::AudioIODevice* device) override
    {
        const auto sampleRate = device != nullptr ? device->getCurrentSampleRate() : 44100.0;
        const auto reportedBlockSize = device != nullptr ? device->getCurrentBufferSizeSamples() : 0;
        prepare (sampleRate, reportedBlockSize);
    }

    void audioDeviceStopped() override
    {
        ready.store (false, std::memory_order_release);

        for (auto& slot : slots)
        {
            slot.inUse.store (false, std::memory_order_release);
            slot.targetGain.store (0.0f, std::memory_order_release);
            slot.gain = 0.0f;
            releaseEffectChain (slot);
            slot.engine.release();
            slot.output.setSize (0, 0);
            slot.indexesReady = false;
        }
    }

    void audioDeviceIOCallbackWithContext (const float* const*,
                                           int,
                                           float* const* outputChannelData,
                                           int numOutputChannels,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext&) override
    {
        clearOutputs (outputChannelData, numOutputChannels, numSamples);

        if (! ready.load (std::memory_order_acquire)
            || outputChannelData == nullptr
            || numSamples <= 0
            || numSamples > preparedBlockSize.load (std::memory_order_relaxed)
            || numOutputChannels <= 0
            || numOutputChannels > maxHostChannels)
            return;

        juce::ScopedNoDenormals noDenormals;

        for (auto& slot : slots)
        {
            if (! slot.inUse.load (std::memory_order_acquire)
                || ! slot.engine.isReady()
                || numSamples > slot.output.getNumSamples()
                || numOutputChannels > slot.output.getNumChannels())
                continue;

            slot.output.clear (0, numSamples);
            juce::AudioBuffer<float> slotView (slot.output.getArrayOfWritePointers(),
                                               numOutputChannels,
                                               numSamples);
            slot.engine.process (emptyInput, slotView);
            processEffectChain (slot, slotView);

            const auto startGain = slot.gain;
            const auto targetGain = slot.targetGain.load (std::memory_order_acquire);
            const auto step = static_cast<float> (static_cast<double> (numSamples)
                                                  / juce::jmax (1.0, lastSampleRate.load (std::memory_order_relaxed) * tailFadeSeconds));
            const auto endGain = targetGain > startGain
                                   ? juce::jmin (targetGain, startGain + step)
                                   : juce::jmax (targetGain, startGain - step);

            for (int channel = 0; channel < numOutputChannels; ++channel)
            {
                auto* dst = outputChannelData[channel];
                const auto* src = slot.output.getReadPointer (channel);

                if (dst == nullptr || src == nullptr)
                    continue;

                for (int sample = 0; sample < numSamples; ++sample)
                {
                    const auto alpha = numSamples > 1 ? static_cast<float> (sample) / static_cast<float> (numSamples - 1) : 1.0f;
                    dst[sample] += src[sample] * (startGain + (endGain - startGain) * alpha);
                }
            }

            slot.gain = endGain;
            if (targetGain <= 0.0f && endGain <= 0.0001f && slot.index != activeSlot.load (std::memory_order_acquire))
                slot.inUse.store (false, std::memory_order_release);
        }
    }

    bool loadState (const Wf::StateSpec& state)
    {
        return loadStateWithControls (state,
                                      lastMasterGain.load (std::memory_order_relaxed),
                                      lastTempoHz.load (std::memory_order_relaxed),
                                      lastIntensity.load (std::memory_order_relaxed),
                                      lastBrightness.load (std::memory_order_relaxed),
                                      lastOrbitPhase.load (std::memory_order_relaxed));
    }

    bool loadStateWithControls (const Wf::StateSpec& state,
                                float masterGain,
                                float tempoHz,
                                float intensity,
                                float brightness,
                                float orbitPhase)
    {
        if (! ready.load (std::memory_order_acquire))
            return false;

        lastMasterGain.store (masterGain, std::memory_order_relaxed);
        lastTempoHz.store (tempoHz, std::memory_order_relaxed);
        lastIntensity.store (intensity, std::memory_order_relaxed);
        lastBrightness.store (brightness, std::memory_order_relaxed);
        lastOrbitPhase.store (orbitPhase, std::memory_order_relaxed);

        const auto previousActive = activeSlot.load (std::memory_order_acquire);
        const auto nextSlot = previousActive == 0 ? 1 : 0;
        auto& incoming = slots[static_cast<size_t> (nextSlot)];

        incoming.inUse.store (false, std::memory_order_release);
        incoming.targetGain.store (0.0f, std::memory_order_release);
        incoming.gain = 0.0f;
        incoming.indexesReady = false;
        releaseEffectChain (incoming);

        if (! incoming.engine.loadProgram (Wf::buildStateProgram (state), Wf::makeWfParameterBindings()))
            return false;

        refreshWfParameterIndexes (incoming);
        if (! incoming.indexesReady)
            return false;

        loadEffectChain (incoming, state);

        applyControlsToSlot (incoming,
                             masterGain,
                             tempoHz,
                             intensity,
                             brightness,
                             orbitPhase);

        incoming.gain = previousActive < 0 ? 1.0f : 0.0f;
        incoming.inUse.store (true, std::memory_order_release);
        incoming.targetGain.store (1.0f, std::memory_order_release);

        if (previousActive >= 0)
        {
            auto& outgoing = slots[static_cast<size_t> (previousActive)];
            outgoing.targetGain.store (0.0f, std::memory_order_release);
        }

        activeSlot.store (nextSlot, std::memory_order_release);
        return true;
    }

    void setControls (float masterGain, float tempoHz, float intensity, float brightness, float orbitPhase)
    {
        lastMasterGain.store (masterGain, std::memory_order_relaxed);
        lastTempoHz.store (tempoHz, std::memory_order_relaxed);
        lastIntensity.store (intensity, std::memory_order_relaxed);
        lastBrightness.store (brightness, std::memory_order_relaxed);
        lastOrbitPhase.store (orbitPhase, std::memory_order_relaxed);

        const auto active = activeSlot.load (std::memory_order_acquire);
        if (active < 0 || active >= static_cast<int> (slots.size()))
            return;

        auto& slot = slots[static_cast<size_t> (active)];
        if (! slot.inUse.load (std::memory_order_acquire))
            return;

        if (! slot.indexesReady)
            refreshWfParameterIndexes (slot);

        if (slot.indexesReady)
            applyControlsToSlot (slot, masterGain, tempoHz, intensity, brightness, orbitPhase);
    }

    juce::String diagnostics() const
    {
        uint64_t renderedBlocks = 0;
        uint64_t silentBlocks = 0;
        int voices = 0;

        for (const auto& slot : slots)
        {
            renderedBlocks += slot.engine.getRenderedBlockCount();
            silentBlocks += slot.engine.getSilentProcessCount();
            if (slot.inUse.load (std::memory_order_acquire))
                ++voices;
        }

        return "blocks=" + juce::String (static_cast<juce::int64> (renderedBlocks))
             + " voices=" + juce::String (voices)
             + " silent=" + juce::String (static_cast<juce::int64> (silentBlocks))
             + " sr=" + juce::String (lastSampleRate.load (std::memory_order_relaxed), 0)
             + " bs=" + juce::String (lastReportedBlockSize.load (std::memory_order_relaxed));
    }

private:
    struct EngineSlot
    {
        int index = 0;
        EmbeddedChucKEngine engine;
        juce::AudioBuffer<float> output;
        std::array<int, 5> parameterIndexes { -1, -1, -1, -1, -1 };
        bool indexesReady = false;
        float gain = 0.0f;
        std::atomic<float> targetGain { 0.0f };
        std::atomic<bool> inUse { false };
        std::array<std::unique_ptr<juce::AudioPluginInstance>, maxTrackEffectSlots> effects;
        juce::MidiBuffer midi;
    };

    bool prepare (double sampleRate, int reportedBlockSize)
    {
        ready.store (false, std::memory_order_release);

        const auto blockSize = juce::jlimit (64,
                                             EmbeddedChucKEngine::maximumBlockSizeLimit,
                                             juce::jmax (reportedBlockSize * 4, fallbackBlockSize));
        lastSampleRate.store (sampleRate, std::memory_order_relaxed);
        lastReportedBlockSize.store (reportedBlockSize, std::memory_order_relaxed);
        preparedBlockSize.store (blockSize, std::memory_order_relaxed);

        bool prepared = true;
        for (int slotIndex = 0; slotIndex < static_cast<int> (slots.size()); ++slotIndex)
        {
            auto& slot = slots[static_cast<size_t> (slotIndex)];
            slot.index = slotIndex;
            slot.inUse.store (false, std::memory_order_release);
            slot.targetGain.store (0.0f, std::memory_order_release);
            slot.gain = 0.0f;
            slot.indexesReady = false;
            releaseEffectChain (slot);
            slot.engine.release();
            slot.output.setSize (maxHostChannels, blockSize, false, false, true);
            slot.output.clear();
            prepared = slot.engine.prepare (sampleRate, blockSize, 0, engineOutputChannels) && prepared;
        }

        activeSlot.store (-1, std::memory_order_release);
        ready.store (prepared, std::memory_order_release);
        return prepared;
    }

    void refreshWfParameterIndexes (EngineSlot& slot)
    {
        if (slot.indexesReady || slot.engine.getParameterCount() != 5)
            return;

        const std::array<juce::String, 5> names
        {
            "hostMasterGain",
            "hostTempoHz",
            "hostIntensity",
            "hostBrightness",
            "hostOrbitPhase"
        };

        std::array<int, 5> indexes {};
        for (int i = 0; i < static_cast<int> (names.size()); ++i)
        {
            indexes[static_cast<size_t> (i)] = slot.engine.getParameterIndex (names[static_cast<size_t> (i)]);
            if (indexes[static_cast<size_t> (i)] < 0)
                return;
        }

        slot.parameterIndexes = indexes;
        slot.indexesReady = true;
    }

    static void applyControlsToSlot (EngineSlot& slot, float masterGain, float tempoHz, float intensity, float brightness, float orbitPhase)
    {
        static_cast<void> (slot.engine.setParameterValue (slot.parameterIndexes[0], masterGain));
        static_cast<void> (slot.engine.setParameterValue (slot.parameterIndexes[1], tempoHz));
        static_cast<void> (slot.engine.setParameterValue (slot.parameterIndexes[2], intensity));
        static_cast<void> (slot.engine.setParameterValue (slot.parameterIndexes[3], brightness));
        static_cast<void> (slot.engine.setParameterValue (slot.parameterIndexes[4], orbitPhase));
    }

    void loadEffectChain (EngineSlot& slot, const Wf::StateSpec& state)
    {
        const auto sampleRate = lastSampleRate.load (std::memory_order_relaxed);
        const auto blockSize = preparedBlockSize.load (std::memory_order_relaxed);
        if (sampleRate <= 0.0 || blockSize <= 0)
            return;

        for (int effectIndex = 0; effectIndex < maxTrackEffectSlots; ++effectIndex)
        {
            const auto& spec = state.effectSlots[static_cast<size_t> (effectIndex)];
            if (! spec.active || spec.pluginFileOrIdentifier.isEmpty())
                continue;

            if (auto description = findPluginDescription (spec))
            {
                juce::String error;
                auto plugin = pluginFormatManager.createPluginInstance (*description, sampleRate, blockSize, error);
                if (plugin == nullptr)
                    continue;

                plugin->setPlayConfigDetails (engineOutputChannels, engineOutputChannels, sampleRate, blockSize);
                plugin->prepareToPlay (sampleRate, blockSize);
                slot.effects[static_cast<size_t> (effectIndex)] = std::move (plugin);
            }
        }
    }

    std::unique_ptr<juce::PluginDescription> findPluginDescription (const Wf::TrackEffectSlotSpec& spec)
    {
        for (auto* format : pluginFormatManager.getFormats())
        {
            if (format == nullptr)
                continue;

            juce::OwnedArray<juce::PluginDescription> descriptions;
            format->findAllTypesForFile (descriptions, spec.pluginFileOrIdentifier);

            for (auto* description : descriptions)
            {
                if (description == nullptr)
                    continue;

                const auto identifier = description->createIdentifierString();
                if (spec.pluginIdentifier.isEmpty() || identifier == spec.pluginIdentifier)
                    return std::make_unique<juce::PluginDescription> (*description);
            }
        }

        return {};
    }

    void processEffectChain (EngineSlot& slot, juce::AudioBuffer<float>& buffer)
    {
        slot.midi.clear();

        for (auto& effect : slot.effects)
        {
            if (effect == nullptr)
                continue;

            const auto inputs = effect->getTotalNumInputChannels();
            const auto outputs = effect->getTotalNumOutputChannels();
            if (inputs <= 0 || outputs <= 0)
                continue;

            effect->processBlock (buffer, slot.midi);
        }
    }

    static void releaseEffectChain (EngineSlot& slot)
    {
        for (auto& effect : slot.effects)
        {
            if (effect != nullptr)
                effect->releaseResources();

            effect.reset();
        }
    }

    juce::AudioBuffer<float> emptyInput;
    std::array<EngineSlot, 2> slots;
    juce::AudioPluginFormatManager pluginFormatManager;
    static constexpr double tailFadeSeconds = 0.42;
    std::atomic<bool> ready { false };
    std::atomic<int> activeSlot { -1 };
    std::atomic<int> preparedBlockSize { 0 };
    std::atomic<double> lastSampleRate { 0.0 };
    std::atomic<int> lastReportedBlockSize { 0 };
    std::atomic<float> lastMasterGain { 0.18f };
    std::atomic<float> lastTempoHz { 1.0f };
    std::atomic<float> lastIntensity { 0.58f };
    std::atomic<float> lastBrightness { 0.48f };
    std::atomic<float> lastOrbitPhase { 0.0f };
};

class OrbitCanvas final : public juce::Component
{
public:
    OrbitCanvas()
    {
        setWantsKeyboardFocus (true);

        for (int i = 0; i < maxGraphTransitions; ++i)
        {
            auto& editor = transitionProbabilityEditors[static_cast<size_t> (i)];
            editor.setInputRestrictions (4, "0123456789%");
            editor.setTextToShowWhenEmpty ("%", mutedInk().withAlpha (0.36f));
            editor.setJustification (juce::Justification::centred);
            editor.setColour (juce::TextEditor::backgroundColourId, panel().withAlpha (0.86f));
            editor.setColour (juce::TextEditor::textColourId, ink());
            editor.setColour (juce::TextEditor::outlineColourId, mutedInk().withAlpha (0.22f));
            editor.setColour (juce::TextEditor::focusedOutlineColourId, amber().withAlpha (0.68f));
            editor.setColour (juce::TextEditor::highlightColourId, amber().withAlpha (0.24f));
            editor.setFont (juce::FontOptions (12.0f, juce::Font::bold));
            editor.onTextChange = [this, i]
            {
                if (suppressTransitionCallbacks || onTransitionProbabilityChanged == nullptr)
                    return;

                onTransitionProbabilityChanged (i, parseTransitionProbability (transitionProbabilityEditors[static_cast<size_t> (i)].getText()));
            };
            addAndMakeVisible (editor);
            editor.setVisible (false);
        }
    }

    std::function<void (int)> onStateSelected;
    std::function<void (int, std::optional<int>)> onTransitionProbabilityChanged;

    bool isEditingTransitionProbability() const
    {
        for (const auto& editor : transitionProbabilityEditors)
            if (editor.hasKeyboardFocus (true))
                return true;

        return false;
    }

    void setState (const std::vector<Wf::StateSpec>* statesToUse, int selectedIndexToUse, float phaseToUse, bool runningToUse)
    {
        states = statesToUse;
        selectedIndex = selectedIndexToUse;
        phase = phaseToUse;
        running = runningToUse;
        rebuildNodeCentres();
        syncTransitionEditors();

        if (states == nullptr || states->empty())
            nodeCentres.clear();

        repaint();
    }

    void resized() override
    {
        rebuildNodeCentres();
        updateTransitionEditorBounds();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff0b0f0c));

        auto area = getLocalBounds().toFloat().reduced (24.0f);
        const auto radius = juce::jmin (area.getWidth(), area.getHeight()) * 0.36f;
        const auto centre = area.getCentre();

        g.setColour (panelSoft().withAlpha (0.28f));
        g.fillEllipse (centre.x - radius, centre.y - radius, radius * 2.0f, radius * 2.0f);

        g.setColour (mutedInk().withAlpha (0.14f));
        g.drawEllipse (centre.x - radius, centre.y - radius, radius * 2.0f, radius * 2.0f, 1.2f);

        if (states == nullptr || states->empty())
            return;

        const auto count = static_cast<int> (states->size());
        rebuildNodeCentres();

        for (int i = 0; i < count; ++i)
        {
            const auto next = (i + 1) % count;
            g.setColour (mutedInk().withAlpha (0.12f));
            g.drawLine ({ nodeCentres[static_cast<size_t> (i)], nodeCentres[static_cast<size_t> (next)] }, 1.1f);
        }

        for (int i = 0; i < count; ++i)
        {
            const auto selected = i == selectedIndex;
            const auto point = nodeCentres[static_cast<size_t> (i)];
            const auto colour = selected ? green() : blue().withAlpha (0.72f);
            const auto size = selected ? 62.0f : 47.0f;
            const auto laneCount = static_cast<int> ((*states)[static_cast<size_t> (i)].lanes.size());

            g.setColour (colour.withAlpha (selected ? 0.20f : 0.10f));
            g.fillEllipse (point.x - size * 0.5f, point.y - size * 0.5f, size, size);
            g.setColour (colour.withAlpha (selected ? 0.98f : 0.78f));
            g.drawEllipse (point.x - size * 0.5f, point.y - size * 0.5f, size, size, selected ? 2.0f : 1.2f);
            drawLaneDots (g, point, size, laneCount, selected);

            g.setColour (ink());
            g.setFont (juce::FontOptions (selected ? 15.0f : 13.0f, juce::Font::bold));
            g.drawFittedText ((*states)[static_cast<size_t> (i)].name,
                              juce::Rectangle<int> (static_cast<int> (point.x - 58.0f),
                                                    static_cast<int> (point.y - 10.0f),
                                                    116,
                                                    22),
                              juce::Justification::centred,
                              1);
        }

        if (selectedIndex >= 0 && selectedIndex < count)
        {
            const auto base = nodeCentres[static_cast<size_t> (selectedIndex)];
            const auto angle = juce::MathConstants<float>::twoPi * phase - juce::MathConstants<float>::halfPi;
            const juce::Point<float> marker { base.x + std::cos (angle) * 34.0f,
                                              base.y + std::sin (angle) * 34.0f };

            g.setColour (running ? amber() : coral());
            g.fillEllipse (marker.x - 4.0f, marker.y - 4.0f, 8.0f, 8.0f);
        }
    }

    void mouseDown (const juce::MouseEvent& event) override
    {
        grabKeyboardFocus();

        if (nodeCentres.empty() || onStateSelected == nullptr)
            return;

        auto bestIndex = -1;
        auto bestDistance = std::numeric_limits<float>::max();

        for (int i = 0; i < static_cast<int> (nodeCentres.size()); ++i)
        {
            const auto distance = event.position.getDistanceFrom (nodeCentres[static_cast<size_t> (i)]);
            if (distance < bestDistance)
            {
                bestDistance = distance;
                bestIndex = i;
            }
        }

        if (bestIndex >= 0 && bestDistance < 80.0f)
            onStateSelected (bestIndex);
    }

private:
    void rebuildNodeCentres()
    {
        nodeCentres.clear();

        if (states == nullptr || states->empty())
            return;

        auto area = getLocalBounds().toFloat().reduced (24.0f);
        const auto radius = juce::jmin (area.getWidth(), area.getHeight()) * 0.36f;
        const auto centre = area.getCentre();
        const auto count = static_cast<int> (states->size());

        nodeCentres.reserve (static_cast<size_t> (count));

        for (int i = 0; i < count; ++i)
        {
            const auto angle = juce::MathConstants<float>::twoPi * (static_cast<float> (i) / static_cast<float> (count)) - juce::MathConstants<float>::halfPi;
            nodeCentres.push_back ({ centre.x + std::cos (angle) * radius,
                                     centre.y + std::sin (angle) * radius });
        }
    }

    void syncTransitionEditors()
    {
        juce::ScopedValueSetter<bool> guard (suppressTransitionCallbacks, true);
        const auto count = states == nullptr ? 0 : juce::jmin (static_cast<int> (states->size()), maxGraphTransitions);

        for (int i = 0; i < maxGraphTransitions; ++i)
        {
            auto& editor = transitionProbabilityEditors[static_cast<size_t> (i)];
            const auto active = i < count && count > 1;
            const auto hasProbability = active && (*states)[static_cast<size_t> (i)].transitionProbabilityPercent.has_value();
            editor.setVisible (active);
            editor.setEnabled (active);
            styleTransitionProbabilityEditor (editor, hasProbability);

            if (active && ! editor.hasKeyboardFocus (true))
                editor.setText (formatTransitionProbability ((*states)[static_cast<size_t> (i)].transitionProbabilityPercent),
                                juce::dontSendNotification);
        }

        updateTransitionEditorBounds();
    }

    void updateTransitionEditorBounds()
    {
        const auto count = states == nullptr ? 0 : juce::jmin (static_cast<int> (states->size()), maxGraphTransitions);

        for (int i = 0; i < maxGraphTransitions; ++i)
        {
            auto& editor = transitionProbabilityEditors[static_cast<size_t> (i)];
            if (i >= count || count <= 1 || i >= static_cast<int> (nodeCentres.size()))
            {
                editor.setVisible (false);
                continue;
            }

            const auto next = (i + 1) % count;
            if (next >= static_cast<int> (nodeCentres.size()))
            {
                editor.setVisible (false);
                continue;
            }

            const auto from = nodeCentres[static_cast<size_t> (i)];
            const auto to = nodeCentres[static_cast<size_t> (next)];
            const auto midpoint = (from + to) * 0.5f;
            editor.setBounds (juce::Rectangle<int> (40, 18).withCentre ({ static_cast<int> (std::round (midpoint.x)),
                                                                          static_cast<int> (std::round (midpoint.y)) }));
        }
    }

    static void styleTransitionProbabilityEditor (juce::TextEditor& editor, bool hasProbability)
    {
        editor.setColour (juce::TextEditor::backgroundColourId,
                          hasProbability ? panel().withAlpha (0.78f) : juce::Colours::transparentBlack);
        editor.setColour (juce::TextEditor::outlineColourId,
                          hasProbability ? amber().withAlpha (0.28f) : juce::Colours::transparentBlack);
        editor.setColour (juce::TextEditor::textColourId,
                          hasProbability ? ink() : mutedInk().withAlpha (0.30f));
        editor.setTextToShowWhenEmpty ("%", mutedInk().withAlpha (0.18f));
    }

    static std::optional<int> parseTransitionProbability (juce::String text)
    {
        text = text.retainCharacters ("0123456789").trim();
        if (text.isEmpty())
            return {};

        return juce::jlimit (0, 100, text.getIntValue());
    }

    static juce::String formatTransitionProbability (std::optional<int> probability)
    {
        if (! probability.has_value())
            return {};

        return juce::String (*probability) + "%";
    }

    static juce::Colour laneDotColour (int index)
    {
        static const std::array<juce::Colour, 5> colours
        {
            green(),
            amber(),
            blue(),
            coral(),
            juce::Colour (0xffb4d06c)
        };

        return colours[static_cast<size_t> (index % static_cast<int> (colours.size()))];
    }

    static void drawLaneDots (juce::Graphics& g, juce::Point<float> centre, float nodeSize, int laneCount, bool selected)
    {
        if (laneCount <= 0)
            return;

        const auto dotRadius = selected ? 3.8f : 3.0f;
        const auto dotOrbitRadius = nodeSize * 0.5f + dotRadius + 2.0f;

        for (int lane = 0; lane < laneCount; ++lane)
        {
            const auto angle = juce::MathConstants<float>::twoPi * (static_cast<float> (lane) / static_cast<float> (laneCount))
                             - juce::MathConstants<float>::halfPi;
            const juce::Point<float> dotCentre { centre.x + std::cos (angle) * dotOrbitRadius,
                                                 centre.y + std::sin (angle) * dotOrbitRadius };
            const auto colour = laneDotColour (lane);

            g.setColour (juce::Colour (0xff101412).withAlpha (0.92f));
            g.fillEllipse (dotCentre.x - dotRadius - 1.5f,
                           dotCentre.y - dotRadius - 1.5f,
                           (dotRadius + 1.5f) * 2.0f,
                           (dotRadius + 1.5f) * 2.0f);

            g.setColour (colour.withAlpha (selected ? 0.90f : 0.62f));
            g.fillEllipse (dotCentre.x - dotRadius, dotCentre.y - dotRadius, dotRadius * 2.0f, dotRadius * 2.0f);

            g.setColour (ink().withAlpha (selected ? 0.42f : 0.22f));
            g.drawEllipse (dotCentre.x - dotRadius, dotCentre.y - dotRadius, dotRadius * 2.0f, dotRadius * 2.0f, 0.8f);
        }
    }

    const std::vector<Wf::StateSpec>* states = nullptr;
    std::vector<juce::Point<float>> nodeCentres;
    std::array<juce::TextEditor, maxGraphTransitions> transitionProbabilityEditors;
    int selectedIndex = 0;
    float phase = 0.0f;
    bool running = false;
    bool suppressTransitionCallbacks = false;
};

class MixerCanvas final : public juce::Component
{
public:
    std::function<void (int, int, int)> onChannelSelected;
    std::function<void (int, int, int, float)> onLaneVolumeChanged;
    std::function<void (int, int, int, float)> onLanePanChanged;
    std::function<void (int, int, int)> onEffectSlotClicked;

    void setProject (std::array<std::optional<std::vector<Wf::StateSpec>>, maxTopLevelStates>* statesToUse,
                     int viewedStateToUse,
                     int selectedTrackToUse,
                     int selectedLaneToUse,
                     int playingStateToUse,
                     int playingTrackToUse,
                     bool runningToUse)
    {
        states = statesToUse;
        viewedState = viewedStateToUse;
        selectedTrack = selectedTrackToUse;
        selectedLane = selectedLaneToUse;
        playingState = playingStateToUse;
        playingTrack = playingTrackToUse;
        running = runningToUse;
        rebuildChannels();
        repaint();
    }

    int getPreferredWidth() const
    {
        return juce::jmax (1, horizontalPadding * 2 + static_cast<int> (channels.size()) * channelWidth);
    }

    std::optional<juce::Range<int>> getPlayingChannelRange() const
    {
        if (! running)
            return {};

        auto first = -1;
        auto last = -1;

        for (int i = 0; i < static_cast<int> (channels.size()); ++i)
        {
            const auto& channel = channels[static_cast<size_t> (i)];
            if (isPlayingChannel (channel))
            {
                if (first < 0)
                    first = i;

                last = i;
            }
        }

        if (first < 0 || last < 0)
            return {};

        return juce::Range<int> (horizontalPadding + first * channelWidth,
                                 horizontalPadding + (last + 1) * channelWidth - channelGap);
    }

    bool isDraggingFader() const noexcept
    {
        return activeChannel >= 0;
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff0b0f0c));

        if (channels.empty())
        {
            g.setColour (mutedInk().withAlpha (0.55f));
            g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
            g.drawFittedText ("No lanes to mix", getLocalBounds().reduced (24), juce::Justification::centred, 1);
            return;
        }

        auto clip = g.getClipBounds().toFloat();

        for (int i = 0; i < static_cast<int> (channels.size()); ++i)
        {
            auto strip = channelBounds (i);
            if (! strip.intersects (clip))
                continue;

            const auto& channel = channels[static_cast<size_t> (i)];
            const auto selected = channel.stateIndex == viewedState
                               && channel.trackIndex == selectedTrack
                               && channel.laneIndex == selectedLane;
            const auto playing = isPlayingChannel (channel);

            if (i == 0 || channels[static_cast<size_t> (i - 1)].stateIndex != channel.stateIndex)
            {
                g.setColour (mutedInk().withAlpha (0.12f));
                g.drawVerticalLine (static_cast<int> (strip.getX()) - 6,
                                    strip.getY() + 4.0f,
                                    strip.getBottom() - 4.0f);
            }

            g.setColour (playing ? green().withAlpha (selected ? 0.22f : 0.15f)
                                  : (selected ? blue().withAlpha (0.16f) : panelSoft().withAlpha (0.30f)));
            g.fillRoundedRectangle (strip, 3.0f);
            g.setColour ((playing ? green() : (selected ? blue() : mutedInk())).withAlpha (playing ? 0.58f : (selected ? 0.44f : 0.14f)));
            g.drawRoundedRectangle (strip, 3.0f, playing ? 1.5f : (selected ? 1.3f : 1.0f));

            if (playing)
            {
                g.setColour (green().withAlpha (0.90f));
                g.fillRoundedRectangle (strip.reduced (6.0f, 0.0f).removeFromTop (3.0f), 1.5f);
            }

            g.setColour (mutedInk().withAlpha (0.70f));
            g.setFont (juce::FontOptions (10.5f, juce::Font::bold));
            g.drawFittedText ("S" + juce::String (channel.stateIndex + 1) + " T" + juce::String (channel.trackIndex + 1),
                              strip.removeFromTop (18).toNearestInt(),
                              juce::Justification::centred,
                              1);

            auto labelArea = strip.removeFromTop (42).reduced (5.0f, 0.0f);
            g.setColour (ink().withAlpha (0.84f));
            g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            g.drawFittedText (channel.trackName, labelArea.removeFromTop (20).toNearestInt(), juce::Justification::centred, 1);
            g.setColour (mutedInk().withAlpha (0.78f));
            g.drawFittedText (channel.laneName, labelArea.toNearestInt(), juce::Justification::centred, 1);

            auto pan = panBounds (i);
            const auto panNorm = (juce::jlimit (-1.0f, 1.0f, channel.pan) + 1.0f) * 0.5f;
            const auto panX = pan.getX() + pan.getWidth() * panNorm;
            g.setColour (mutedInk().withAlpha (0.13f));
            g.fillRoundedRectangle (pan, 3.0f);
            g.setColour (amber().withAlpha (0.70f));
            g.fillEllipse (panX - 5.0f, pan.getCentreY() - 5.0f, 10.0f, 10.0f);
            g.setColour (mutedInk().withAlpha (0.62f));
            g.setFont (juce::FontOptions (9.5f, juce::Font::bold));
            g.drawFittedText ("pan", pan.translated (0.0f, -14.0f).toNearestInt(), juce::Justification::centred, 1);

            auto fader = faderBounds (i);
            const auto norm = juce::jlimit (0.0f, 1.0f, channel.volume / maxLaneVolume);
            const auto thumbY = fader.getBottom() - fader.getHeight() * norm;

            g.setColour (mutedInk().withAlpha (0.12f));
            g.fillRoundedRectangle (fader.withWidth (6.0f).withCentre ({ fader.getCentreX(), fader.getCentreY() }), 3.0f);
            g.setColour (green().withAlpha (0.72f));
            g.fillRoundedRectangle (juce::Rectangle<float> (6.0f, fader.getBottom() - thumbY).withPosition (fader.getCentreX() - 3.0f, thumbY), 3.0f);
            g.setColour (ink());
            g.fillEllipse (fader.getCentreX() - 7.0f, thumbY - 7.0f, 14.0f, 14.0f);

            g.setColour (ink().withAlpha (0.88f));
            g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            g.drawFittedText (juce::String (channel.volume, 2),
                              juce::Rectangle<int> (static_cast<int> (fader.getX()) - 8,
                                                    getHeight() - 38,
                                                    static_cast<int> (fader.getWidth()) + 16,
                                                    18),
                              juce::Justification::centred,
                              1);

            for (int slotIndex = 0; slotIndex < maxTrackEffectSlots; ++slotIndex)
            {
                const auto slot = effectSlotBounds (i, slotIndex);
                const auto active = channel.effectActive[static_cast<size_t> (slotIndex)];
                const auto hasPlugin = channel.effectNames[static_cast<size_t> (slotIndex)].isNotEmpty();
                g.setColour (active ? amber().withAlpha (0.20f) : panel().withAlpha (0.90f));
                g.fillRoundedRectangle (slot, 3.0f);
                g.setColour ((active ? amber() : mutedInk()).withAlpha (active ? 0.68f : (hasPlugin ? 0.34f : 0.18f)));
                g.drawRoundedRectangle (slot, 3.0f, active ? 1.1f : 0.9f);
                g.setColour ((active ? ink() : mutedInk()).withAlpha (active ? 0.90f : (hasPlugin ? 0.68f : 0.50f)));
                g.setFont (juce::FontOptions (9.5f, juce::Font::bold));
                g.drawFittedText (hasPlugin ? shortEffectName (channel.effectNames[static_cast<size_t> (slotIndex)])
                                             : "FX" + juce::String (slotIndex + 1),
                                  slot.toNearestInt(),
                                  juce::Justification::centred,
                                  1);
            }
        }
    }

    void mouseDown (const juce::MouseEvent& event) override
    {
        activeChannel = findChannelAt (event.position);
        if (activeChannel < 0)
            return;

        selectActiveChannel();

        if (const auto slot = findEffectSlotAt (activeChannel, event.position); slot >= 0)
        {
            if (activeChannel < static_cast<int> (channels.size()) && onEffectSlotClicked != nullptr)
            {
                const auto& channel = channels[static_cast<size_t> (activeChannel)];
                onEffectSlotClicked (channel.stateIndex, channel.trackIndex, slot);
            }

            activeControl = ActiveControl::none;
            activeChannel = -1;
        }
        else if (panBounds (activeChannel).expanded (8.0f, 8.0f).contains (event.position))
        {
            activeControl = ActiveControl::pan;
            setActiveChannelPanFromX (event.position.x);
        }
        else if (faderBounds (activeChannel).expanded (14.0f, 8.0f).contains (event.position))
        {
            activeControl = ActiveControl::volume;
            setActiveChannelVolumeFromY (event.position.y);
        }
        else
        {
            activeControl = ActiveControl::none;
        }
    }

    void mouseDrag (const juce::MouseEvent& event) override
    {
        if (activeChannel >= 0 && activeControl == ActiveControl::volume)
            setActiveChannelVolumeFromY (event.position.y);
        else if (activeChannel >= 0 && activeControl == ActiveControl::pan)
            setActiveChannelPanFromX (event.position.x);
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        activeChannel = -1;
        activeControl = ActiveControl::none;
    }

private:
    enum class ActiveControl
    {
        none,
        volume,
        pan
    };

    struct Channel
    {
        int stateIndex = 0;
        int trackIndex = 0;
        int laneIndex = 0;
        juce::String trackName;
        juce::String laneName;
        float volume = 0.0f;
        float pan = 0.0f;
        std::array<bool, maxTrackEffectSlots> effectActive {};
        std::array<juce::String, maxTrackEffectSlots> effectNames {};
    };

    void rebuildChannels()
    {
        channels.clear();

        if (states == nullptr)
            return;

        for (int stateIndex = 0; stateIndex < maxTopLevelStates; ++stateIndex)
        {
            const auto& stateSlot = (*states)[static_cast<size_t> (stateIndex)];
            if (! stateSlot.has_value())
                continue;

            const auto& tracks = *stateSlot;
            for (int trackIndex = 0; trackIndex < static_cast<int> (tracks.size()); ++trackIndex)
            {
                const auto& track = tracks[static_cast<size_t> (trackIndex)];
                for (int laneIndex = 0; laneIndex < static_cast<int> (track.lanes.size()); ++laneIndex)
                {
                    const auto& lane = track.lanes[static_cast<size_t> (laneIndex)];
                    Channel channel;
                    channel.stateIndex = stateIndex;
                    channel.trackIndex = trackIndex;
                    channel.laneIndex = laneIndex;
                    channel.trackName = track.name;
                    channel.laneName = lane.name;
                    channel.volume = lane.volume;
                    channel.pan = lane.pan;

                    for (int effectIndex = 0; effectIndex < maxTrackEffectSlots; ++effectIndex)
                    {
                        const auto& effect = track.effectSlots[static_cast<size_t> (effectIndex)];
                        channel.effectActive[static_cast<size_t> (effectIndex)] = effect.active && effect.pluginName.isNotEmpty();
                        channel.effectNames[static_cast<size_t> (effectIndex)] = effect.pluginName;
                    }

                    channels.push_back (std::move (channel));
                }
            }
        }
    }

    bool isPlayingChannel (const Channel& channel) const noexcept
    {
        return running
            && channel.stateIndex == playingState
            && channel.trackIndex == playingTrack;
    }

    juce::Rectangle<float> channelBounds (int channelIndex) const
    {
        return { static_cast<float> (horizontalPadding + channelIndex * channelWidth),
                 12.0f,
                 static_cast<float> (channelWidth - channelGap),
                 static_cast<float> (juce::jmax (120, getHeight() - 24)) };
    }

    juce::Rectangle<float> faderBounds (int channelIndex) const
    {
        auto strip = channelBounds (channelIndex).reduced (0.0f, 12.0f);
        strip.removeFromTop (120.0f);
        strip.removeFromBottom (94.0f);
        const auto height = juce::jlimit (40.0f, 150.0f, strip.getHeight());
        return { strip.getCentreX() - 13.0f, strip.getCentreY() - height * 0.5f, 26.0f, height };
    }

    juce::Rectangle<float> panBounds (int channelIndex) const
    {
        auto strip = channelBounds (channelIndex).reduced (8.0f, 0.0f);
        return { strip.getX(), strip.getY() + 88.0f, strip.getWidth(), 6.0f };
    }

    juce::Rectangle<float> effectSlotBounds (int channelIndex, int slotIndex) const
    {
        auto strip = channelBounds (channelIndex).reduced (7.0f, 0.0f);
        const auto y = strip.getBottom() - 64.0f + static_cast<float> (slotIndex) * 20.0f;
        return { strip.getX(), y, strip.getWidth(), 16.0f };
    }

    int findChannelAt (juce::Point<float> point) const
    {
        if (point.x < static_cast<float> (horizontalPadding))
            return -1;

        const auto channelIndex = static_cast<int> ((point.x - static_cast<float> (horizontalPadding)) / static_cast<float> (channelWidth));
        if (channelIndex < 0 || channelIndex >= static_cast<int> (channels.size()))
            return -1;

        return channelBounds (channelIndex).contains (point) ? channelIndex : -1;
    }

    int findEffectSlotAt (int channelIndex, juce::Point<float> point) const
    {
        for (int slotIndex = 0; slotIndex < maxTrackEffectSlots; ++slotIndex)
            if (effectSlotBounds (channelIndex, slotIndex).contains (point))
                return slotIndex;

        return -1;
    }

    Wf::LaneSpec* getLane (const Channel& channel) const
    {
        if (states == nullptr || channel.stateIndex < 0 || channel.stateIndex >= maxTopLevelStates)
            return nullptr;

        auto& stateSlot = (*states)[static_cast<size_t> (channel.stateIndex)];
        if (! stateSlot.has_value())
            return nullptr;

        auto& tracks = *stateSlot;
        if (channel.trackIndex < 0 || channel.trackIndex >= static_cast<int> (tracks.size()))
            return nullptr;

        auto& lanes = tracks[static_cast<size_t> (channel.trackIndex)].lanes;
        if (channel.laneIndex < 0 || channel.laneIndex >= static_cast<int> (lanes.size()))
            return nullptr;

        return &lanes[static_cast<size_t> (channel.laneIndex)];
    }

    void selectActiveChannel()
    {
        if (activeChannel < 0 || activeChannel >= static_cast<int> (channels.size()))
            return;

        const auto& channel = channels[static_cast<size_t> (activeChannel)];
        if (onChannelSelected != nullptr)
            onChannelSelected (channel.stateIndex, channel.trackIndex, channel.laneIndex);
    }

    void setActiveChannelVolumeFromY (float y)
    {
        if (activeChannel < 0 || activeChannel >= static_cast<int> (channels.size()))
            return;

        auto& channel = channels[static_cast<size_t> (activeChannel)];
        auto* lane = getLane (channel);
        if (lane == nullptr)
            return;

        const auto fader = faderBounds (activeChannel);
        const auto norm = 1.0f - juce::jlimit (0.0f, 1.0f, (y - fader.getY()) / juce::jmax (1.0f, fader.getHeight()));
        const auto volume = juce::jlimit (0.0f, maxLaneVolume, norm * maxLaneVolume);
        lane->volume = volume;
        channel.volume = volume;

        if (onLaneVolumeChanged != nullptr)
            onLaneVolumeChanged (channel.stateIndex, channel.trackIndex, channel.laneIndex, volume);

        repaint();
    }

    void setActiveChannelPanFromX (float x)
    {
        if (activeChannel < 0 || activeChannel >= static_cast<int> (channels.size()))
            return;

        auto& channel = channels[static_cast<size_t> (activeChannel)];
        auto* lane = getLane (channel);
        if (lane == nullptr)
            return;

        const auto panArea = panBounds (activeChannel);
        const auto norm = juce::jlimit (0.0f, 1.0f, (x - panArea.getX()) / juce::jmax (1.0f, panArea.getWidth()));
        const auto pan = norm * 2.0f - 1.0f;
        lane->pan = pan;
        channel.pan = pan;

        if (onLanePanChanged != nullptr)
            onLanePanChanged (channel.stateIndex, channel.trackIndex, channel.laneIndex, pan);

        repaint();
    }

    static juce::String shortEffectName (const juce::String& name)
    {
        if (name.isEmpty())
            return "FX";

        return name.substring (0, 5);
    }

    static constexpr int horizontalPadding = 16;
    static constexpr int channelWidth = 86;
    static constexpr int channelGap = 8;
    static constexpr float maxLaneVolume = 0.8f;

    std::array<std::optional<std::vector<Wf::StateSpec>>, maxTopLevelStates>* states = nullptr;
    std::vector<Channel> channels;
    int viewedState = 0;
    int selectedTrack = 0;
    int selectedLane = 0;
    int playingState = 0;
    int playingTrack = 0;
    int activeChannel = -1;
    ActiveControl activeControl = ActiveControl::none;
    bool running = false;
};

class MainComponent final : public juce::Component,
                            private juce::Timer
{
    enum class MainView
    {
        arrangement,
        code,
        mixer
    };

public:
    MainComponent()
    {
        setWantsKeyboardFocus (true);
        juce::addDefaultFormatsToManager (pluginFormatManager);

        topLevelStates[0] = Wf::makeDefaultStates();
        topLevelStates[1] = Wf::makeDefaultStates();

        titleLabel.setText ("ChucK-ME", juce::dontSendNotification);
        titleLabel.setFont (juce::FontOptions (22.0f, juce::Font::bold));
        styleLabel (titleLabel);
        addAndMakeVisible (titleLabel);

        statusLabel.setText ("embedded ChucK ready", juce::dontSendNotification);
        styleLabel (statusLabel, 0.72f);
        addAndMakeVisible (statusLabel);

        volumeLabel.setText ("volume", juce::dontSendNotification);
        volumeLabel.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        styleLabel (volumeLabel, 0.78f);
        addAndMakeVisible (volumeLabel);

        setupSlider (gainSlider, "Volume", 0.0, 0.8, 0.18, green());
        gainSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 56, 22);

        for (int i = 0; i < maxTopLevelStates; ++i)
        {
            auto& button = stateButtons[static_cast<size_t> (i)];
            setupButton (button,
                         "State " + juce::String (i + 1),
                         i == 0 ? green() : blue(),
                         [this, i]
                         {
                             selectViewedTopLevelState (i);
                             grabKeyboardFocus();
                         });
            button.setWantsKeyboardFocus (false);

            if (! isTopLevelStatePopulated (i))
                styleEmptyStateButton (button);
        }

        setupButton (runScriptButton, running ? "Stop" : "Play", amber(), [this] { toggleMainTransport(); });
        setupButton (newStateButton, "New", blue(), [this] { createTopLevelState(); });
        setupButton (duplicateStateButton, "Duplicate", amber(), [this] { duplicateViewedTopLevelState(); });
        setupButton (deleteStateButton, "Delete", coral(), [this] { deleteViewedTopLevelState(); });

        globalScriptEditor.setMultiLine (true);
        globalScriptEditor.setReturnKeyStartsNewLine (true);
        globalScriptEditor.setText ("tempo(88); timeSig(4, 4); playState(1, 8);\ntempo(44); playState(2, 4);\nstop();", juce::dontSendNotification);
        globalScriptEditor.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff111612));
        globalScriptEditor.setColour (juce::TextEditor::textColourId, ink());
        globalScriptEditor.setColour (juce::TextEditor::outlineColourId, mutedInk().withAlpha (0.12f));
        globalScriptEditor.setColour (juce::TextEditor::focusedOutlineColourId, amber().withAlpha (0.42f));
        globalScriptEditor.setColour (juce::TextEditor::highlightColourId, amber().withAlpha (0.24f));
        globalScriptEditor.setFont (juce::FontOptions (14.0f));
        addAndMakeVisible (globalScriptEditor);

        laneCodeHeader.setText ("lane code", juce::dontSendNotification);
        laneCodeHeader.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        styleLabel (laneCodeHeader, 0.82f);
        addAndMakeVisible (laneCodeHeader);

        trackNameLabel.setText ("name", juce::dontSendNotification);
        trackNameLabel.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        styleLabel (trackNameLabel, 0.72f);
        addAndMakeVisible (trackNameLabel);

        setupTextEditor (trackNameEditor, "Track name");
        trackNameEditor.onTextChange = [this] { applyTrackNameEdit(); };

        trackDurationLabel.setText ("duration (Bar.Beat)", juce::dontSendNotification);
        trackDurationLabel.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        styleLabel (trackDurationLabel, 0.72f);
        addAndMakeVisible (trackDurationLabel);

        setupTextEditor (trackDurationEditor, "1.0");
        trackDurationEditor.onTextChange = [this] { applyTrackDurationEdit(); };

        setupTextEditor (laneNameEditor, "Lane name");
        laneNameEditor.onTextChange = [this] { applyLaneNameEdit(); };

        setupButton (muteLaneButton, "Mute", coral(), [this] { toggleSelectedLaneMute(); });
        setupButton (soloLaneButton, "Solo", amber(), [this] { toggleSelectedLaneSolo(); });
        setupButton (duplicateLaneButton, "Duplicate", blue(), [this] { duplicateSelectedLane(); });
        setupButton (deleteLaneButton, "Delete", coral(), [this] { deleteSelectedLane(); });

        laneCodeEditor.setMultiLine (true);
        laneCodeEditor.setReturnKeyStartsNewLine (true);
        laneCodeEditor.setReadOnly (false);
        laneCodeEditor.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff0b0f0c));
        laneCodeEditor.setColour (juce::TextEditor::textColourId, ink());
        laneCodeEditor.setColour (juce::TextEditor::outlineColourId, mutedInk().withAlpha (0.12f));
        laneCodeEditor.setColour (juce::TextEditor::focusedOutlineColourId, mutedInk().withAlpha (0.24f));
        laneCodeEditor.setColour (juce::TextEditor::highlightColourId, blue().withAlpha (0.24f));
        laneCodeEditor.setFont (juce::FontOptions (12.0f));
        laneCodeEditor.onTextChange = [this] { markLaneCodeEdited(); };
        addAndMakeVisible (laneCodeEditor);

        setupButton (laneCodeRunButton, "Run", green(), [this] { applyLaneCodeEdit(); });
        laneCodeRunButton.setEnabled (false);

        stateCodeHeader.setText ("state code", juce::dontSendNotification);
        stateCodeHeader.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        styleLabel (stateCodeHeader, 0.82f);
        addAndMakeVisible (stateCodeHeader);

        selectedLabel.setFont (juce::FontOptions (18.0f, juce::Font::bold));
        styleLabel (selectedLabel);
        addAndMakeVisible (selectedLabel);

        stateSettingsLabel.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        styleLabel (stateSettingsLabel, 0.82f);
        addAndMakeVisible (stateSettingsLabel);

        styleLabel (stateTempoLabel, 0.76f);
        addAndMakeVisible (stateTempoLabel);

        setupSlider (stateTempoSlider, "State tempo", 30.0, 220.0, 88.0, amber());
        stateTempoSlider.setSkewFactorFromMidPoint (100.0);
        stateTempoSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 62, 22);
        stateTempoSlider.onValueChange = [this]
        {
            if (suppressStateControlCallbacks)
                return;

            topLevelTemposBpm[static_cast<size_t> (viewedTopLevelState)] = static_cast<float> (stateTempoSlider.getValue());
            refreshLabels();

            if (viewedTopLevelState == performingTopLevelState)
                applyCurrentAudioControls();
        };

        styleLabel (stateTimeSigLabel, 0.76f);
        addAndMakeVisible (stateTimeSigLabel);

        stateTrackCountLabel.setText ("Tracks", juce::dontSendNotification);
        styleLabel (stateTrackCountLabel, 0.76f);
        addAndMakeVisible (stateTrackCountLabel);

        setupTextEditor (stateTrackCountEditor, "0");
        stateTrackCountEditor.setInputRestrictions (2, "0123456789");
        stateTrackCountEditor.setSelectAllWhenFocused (true);
        stateTrackCountEditor.onTextChange = [this] { applyStateTrackCountEdit(); };

        for (int numerator = 1; numerator <= 16; ++numerator)
            timeSigNumeratorBox.addItem (juce::String (numerator), numerator);

        for (const auto denominator : { 2, 4, 8, 16 })
            timeSigDenominatorBox.addItem (juce::String (denominator), denominator);

        styleComboBox (timeSigNumeratorBox);
        styleComboBox (timeSigDenominatorBox);
        addAndMakeVisible (timeSigNumeratorBox);
        addAndMakeVisible (timeSigDenominatorBox);

        timeSigNumeratorBox.onChange = [this]
        {
            if (suppressStateControlCallbacks)
                return;

            topLevelTimeSigNumerators[static_cast<size_t> (viewedTopLevelState)] = timeSigNumeratorBox.getSelectedId();
            refreshLabels();
        };

        timeSigDenominatorBox.onChange = [this]
        {
            if (suppressStateControlCallbacks)
                return;

            topLevelTimeSigDenominators[static_cast<size_t> (viewedTopLevelState)] = timeSigDenominatorBox.getSelectedId();
            refreshLabels();
        };

        orbitCanvas.onStateSelected = [this] (int index) { selectState (index); };
        orbitCanvas.onTransitionProbabilityChanged = [this] (int index, std::optional<int> probability)
        {
            setTransitionProbability (index, probability);
        };
        addAndMakeVisible (orbitCanvas);

        mixerViewport.setViewedComponent (&mixerCanvas, false);
        mixerViewport.setScrollBarsShown (false, true);
        mixerCanvas.onChannelSelected = [this] (int stateIndex, int trackIndex, int laneIndex)
        {
            selectMixerChannel (stateIndex, trackIndex, laneIndex);
        };
        mixerCanvas.onLaneVolumeChanged = [this] (int stateIndex, int trackIndex, int laneIndex, float volume)
        {
            applyMixerLaneVolumeChange (stateIndex, trackIndex, laneIndex, volume);
        };
        mixerCanvas.onLanePanChanged = [this] (int stateIndex, int trackIndex, int laneIndex, float pan)
        {
            applyMixerLanePanChange (stateIndex, trackIndex, laneIndex, pan);
        };
        mixerCanvas.onEffectSlotClicked = [this] (int stateIndex, int trackIndex, int slotIndex)
        {
            showEffectSlotMenu (stateIndex, trackIndex, slotIndex);
        };
        addAndMakeVisible (mixerViewport);

        setupButton (arrangementButton, "Arrangement", green(), [this] { setMainView (MainView::arrangement); });
        setupButton (codeViewButton, "Code", blue(), [this] { setMainView (MainView::code); });
        setupButton (mixerViewButton, "Mixer", amber(), [this] { setMainView (MainView::mixer); });

        addAndMakeVisible (laneHeader);
        laneHeader.setText ("lanes", juce::dontSendNotification);
        laneHeader.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        styleLabel (laneHeader, 0.78f);

        for (int i = 0; i < static_cast<int> (laneButtons.size()); ++i)
        {
            auto& button = laneButtons[static_cast<size_t> (i)];
            setupButton (button, juce::String (i + 1), blue(), [this, i] { selectLane (i); });
            button.setClickingTogglesState (false);
        }

        audioDeviceManager.initialiseWithDefaultDevices (0, 2);
        audioDeviceManager.addAudioCallback (&audioCallback);

        selectState (0);
        setMainView (MainView::arrangement);
        setSize (1240, 760);
        startTimerHz (30);
    }

    ~MainComponent() override
    {
        audioDeviceManager.removeAudioCallback (&audioCallback);
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        const auto keyCode = key.getKeyCode();
        if ((keyCode == juce::KeyPress::backspaceKey || keyCode == juce::KeyPress::deleteKey)
            && ! isInlineTextEditorFocused())
        {
            deleteViewedTopLevelState();
            return true;
        }

        return false;
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff090c0a));

        auto area = getLocalBounds().reduced (18);
        g.setColour (panel());
        g.fillRoundedRectangle (area.toFloat(), 6.0f);

        g.setColour (mutedInk().withAlpha (0.10f));
        g.drawRoundedRectangle (area.toFloat(), 6.0f, 1.0f);

        auto content = area.reduced (18);
        auto top = content.removeFromTop (48);
        auto navigation = content.removeFromBottom (52);

        g.setColour (mutedInk().withAlpha (0.11f));
        g.drawLine (static_cast<float> (content.getX()),
                    static_cast<float> (top.getBottom()),
                    static_cast<float> (content.getRight()),
                    static_cast<float> (top.getBottom()),
                    1.0f);

        g.setColour (mutedInk().withAlpha (0.08f));
        g.drawLine (static_cast<float> (navigation.getX()),
                    static_cast<float> (navigation.getY()),
                    static_cast<float> (navigation.getRight()),
                    static_cast<float> (navigation.getY()),
                    1.0f);

        if (mainView == MainView::arrangement)
        {
            auto stateRow = content.removeFromTop (60);
            auto scriptRow = content.removeFromTop (70);
            auto body = content;
            body.removeFromTop (12);
            auto leftPane = body.removeFromLeft (280);
            body.removeFromLeft (18);
            auto rightPane = body.removeFromRight (260);

            g.setColour (juce::Colour (0xff0d110e).withAlpha (0.58f));
            g.fillRoundedRectangle (leftPane.toFloat(), 3.0f);
            g.fillRoundedRectangle (rightPane.toFloat(), 3.0f);

            g.setColour (mutedInk().withAlpha (0.055f));
            g.drawRoundedRectangle (leftPane.toFloat(), 3.0f, 1.0f);
            g.drawRoundedRectangle (rightPane.toFloat(), 3.0f, 1.0f);

            g.setColour (mutedInk().withAlpha (0.11f));
            g.drawLine (static_cast<float> (stateRow.getX()),
                        static_cast<float> (stateRow.getBottom()),
                        static_cast<float> (stateRow.getRight()),
                        static_cast<float> (stateRow.getBottom()),
                        1.0f);
            g.drawLine (static_cast<float> (scriptRow.getX()),
                        static_cast<float> (scriptRow.getBottom()),
                        static_cast<float> (scriptRow.getRight()),
                        static_cast<float> (scriptRow.getBottom()),
                        1.0f);
        }
        else if (mainView == MainView::code)
        {
            content.removeFromTop (12);
            auto stateCodePane = content.removeFromTop ((content.getHeight() - 16) / 3);
            content.removeFromTop (16);
            auto laneCodePane = content;

            g.setColour (juce::Colour (0xff0d110e).withAlpha (0.58f));
            g.fillRoundedRectangle (stateCodePane.toFloat(), 3.0f);
            g.fillRoundedRectangle (laneCodePane.toFloat(), 3.0f);

            g.setColour (mutedInk().withAlpha (0.055f));
            g.drawRoundedRectangle (stateCodePane.toFloat(), 3.0f, 1.0f);
            g.drawRoundedRectangle (laneCodePane.toFloat(), 3.0f, 1.0f);
        }
        else
        {
            content.removeFromTop (12);
            g.setColour (juce::Colour (0xff0d110e).withAlpha (0.58f));
            g.fillRoundedRectangle (content.toFloat(), 3.0f);

            g.setColour (mutedInk().withAlpha (0.055f));
            g.drawRoundedRectangle (content.toFloat(), 3.0f, 1.0f);
        }
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (34);
        auto header = area.removeFromTop (44);
        titleLabel.setBounds (header.removeFromLeft (260));
        auto volumeArea = header.removeFromRight (270);
        volumeLabel.setBounds (volumeArea.removeFromLeft (66).reduced (0, 11));
        gainSlider.setBounds (volumeArea.reduced (0, 7));
        statusLabel.setBounds (header);

        auto navigation = area.removeFromBottom (52);
        navigation.removeFromTop (8);
        auto viewButtons = navigation.removeFromTop (42);
        arrangementButton.setBounds (viewButtons.removeFromLeft (126).reduced (0, 4));
        codeViewButton.setBounds (viewButtons.removeFromLeft (86).reduced (6, 4));
        mixerViewButton.setBounds (viewButtons.removeFromLeft (92).reduced (6, 4));

        if (mainView == MainView::code)
        {
            area.removeFromTop (12);
            auto stateCodePane = area.removeFromTop ((area.getHeight() - 16) / 3);
            area.removeFromTop (16);
            auto laneCodePane = area;

            auto stateCodeHeaderRow = stateCodePane.removeFromTop (32);
            runScriptButton.setBounds (stateCodeHeaderRow.removeFromRight (68).reduced (0, 3));
            stateCodeHeader.setBounds (stateCodeHeaderRow.reduced (8, 2));
            globalScriptEditor.setBounds (stateCodePane.reduced (8, 0));

            auto laneCodeHeaderRow = laneCodePane.removeFromTop (32);
            laneCodeRunButton.setBounds (laneCodeHeaderRow.removeFromRight (68).reduced (0, 3));
            laneCodeHeader.setBounds (laneCodeHeaderRow.reduced (8, 2));
            laneCodeEditor.setBounds (laneCodePane.reduced (8, 0));
            return;
        }

        if (mainView == MainView::mixer)
        {
            area.removeFromTop (12);
            mixerViewport.setBounds (area.reduced (8, 0));
            resizeMixerCanvas();
            return;
        }

        auto stateRow = area.removeFromTop (60);
        stateRow.removeFromLeft (10);

        const auto buttonGap = 6;
        auto firstStateRow = stateRow.removeFromTop (30);
        auto secondStateRow = stateRow.removeFromTop (30);
        const auto buttonWidth = juce::jmax (52, (firstStateRow.getWidth() - buttonGap * 7) / 8);

        for (int i = 0; i < maxTopLevelStates; ++i)
        {
            auto& row = i < 8 ? firstStateRow : secondStateRow;
            auto buttonArea = row.removeFromLeft (buttonWidth);
            stateButtons[static_cast<size_t> (i)].setBounds (buttonArea.reduced (0, 3));
            row.removeFromLeft (buttonGap);
        }

        auto scriptRow = area.removeFromTop (70);
        scriptRow.removeFromLeft (10);
        runScriptButton.setBounds (scriptRow.removeFromRight (86).reduced (8, 16));
        globalScriptEditor.setBounds (scriptRow.reduced (0, 8));
        area.removeFromTop (12);
        auto codePane = area.removeFromLeft (280);
        area.removeFromLeft (18);
        auto right = area.removeFromRight (260);
        area.removeFromRight (18);

        auto trackNameRow = codePane.removeFromTop (30);
        trackNameLabel.setBounds (trackNameRow.removeFromLeft (74).reduced (0, 2));
        trackNameRow.removeFromLeft (8);
        trackNameEditor.setBounds (trackNameRow.reduced (0, 2));
        codePane.removeFromTop (6);
        laneNameEditor.setBounds (codePane.removeFromTop (28));
        codePane.removeFromTop (8);
        auto trackEditRow = codePane.removeFromTop (30);
        trackDurationLabel.setBounds (trackEditRow.removeFromLeft (152).reduced (0, 2));
        trackEditRow.removeFromLeft (8);
        trackDurationEditor.setBounds (trackEditRow.removeFromLeft (74).reduced (0, 2));
        codePane.removeFromTop (6);
        auto laneEditRow = codePane.removeFromTop (30);
        muteLaneButton.setBounds (laneEditRow.removeFromLeft (72).reduced (6, 2));
        soloLaneButton.setBounds (laneEditRow.removeFromLeft (72).reduced (6, 2));
        codePane.removeFromTop (4);
        auto laneActionRow = codePane.removeFromTop (30);
        duplicateLaneButton.setBounds (laneActionRow.removeFromLeft (112).reduced (0, 2));
        deleteLaneButton.setBounds (laneActionRow.removeFromLeft (92).reduced (8, 2));
        codePane.removeFromTop (10);
        auto laneCodeHeaderRow = codePane.removeFromTop (28);
        laneCodeRunButton.setBounds (laneCodeHeaderRow.removeFromRight (62).reduced (0, 3));
        laneCodeHeader.setBounds (laneCodeHeaderRow);
        laneCodeEditor.setBounds (codePane);
        orbitCanvas.setBounds (area);
        stateSettingsLabel.setBounds (right.removeFromTop (24));
        stateTempoLabel.setBounds (right.removeFromTop (22));
        stateTempoSlider.setBounds (right.removeFromTop (30));
        stateTimeSigLabel.setBounds (right.removeFromTop (22));
        auto timeSigRow = right.removeFromTop (30);
        timeSigNumeratorBox.setBounds (timeSigRow.removeFromLeft (78));
        timeSigRow.removeFromLeft (8);
        timeSigDenominatorBox.setBounds (timeSigRow.removeFromLeft (78));
        right.removeFromTop (8);
        auto trackCountRow = right.removeFromTop (30);
        stateTrackCountLabel.setBounds (trackCountRow.removeFromLeft (72).reduced (0, 2));
        trackCountRow.removeFromLeft (8);
        stateTrackCountEditor.setBounds (trackCountRow.removeFromLeft (62).reduced (0, 2));
        right.removeFromTop (8);
        auto stateActionRow = right.removeFromTop (34);
        newStateButton.setBounds (stateActionRow.removeFromLeft (66).reduced (0, 3));
        duplicateStateButton.setBounds (stateActionRow.removeFromLeft (112).reduced (6, 3));
        deleteStateButton.setBounds (stateActionRow.removeFromLeft (82).reduced (6, 3));
        right.removeFromTop (10);
        selectedLabel.setBounds (right.removeFromTop (34));
        laneHeader.setBounds (right.removeFromTop (28));

        for (auto& button : laneButtons)
            button.setBounds (right.removeFromTop (25).reduced (0, 2));
    }

private:
    void setupButton (juce::TextButton& button, const juce::String& text, juce::Colour colour, std::function<void()> action)
    {
        button.setButtonText (text);
        styleButton (button, colour);
        button.onClick = std::move (action);
        addAndMakeVisible (button);
    }

    void setupSlider (juce::Slider& slider, const juce::String& name, double minimum, double maximum, double value, juce::Colour colour)
    {
        slider.setName (name);
        slider.setRange (minimum, maximum, 0.001);
        slider.setValue (value, juce::dontSendNotification);
        styleSlider (slider, colour);
        addAndMakeVisible (slider);
    }

    void setupTextEditor (juce::TextEditor& editor, const juce::String& emptyText)
    {
        editor.setTextToShowWhenEmpty (emptyText, mutedInk().withAlpha (0.66f));
        editor.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff151a17));
        editor.setColour (juce::TextEditor::textColourId, ink());
        editor.setColour (juce::TextEditor::outlineColourId, mutedInk().withAlpha (0.12f));
        editor.setColour (juce::TextEditor::focusedOutlineColourId, blue().withAlpha (0.36f));
        editor.setColour (juce::TextEditor::highlightColourId, blue().withAlpha (0.24f));
        editor.setFont (juce::FontOptions (13.0f));
        addAndMakeVisible (editor);
    }

    void setMainView (MainView nextView)
    {
        mainView = nextView;
        syncMixerView();
        syncViewVisibility();
        syncViewButtons();
        resized();
        repaint();
    }

    void syncViewButtons()
    {
        arrangementButton.setToggleState (mainView == MainView::arrangement, juce::dontSendNotification);
        codeViewButton.setToggleState (mainView == MainView::code, juce::dontSendNotification);
        mixerViewButton.setToggleState (mainView == MainView::mixer, juce::dontSendNotification);
    }

    void syncViewVisibility()
    {
        const auto arrangement = mainView == MainView::arrangement;
        const auto code = mainView == MainView::code;
        const auto mixer = mainView == MainView::mixer;

        for (auto& button : stateButtons)
            button.setVisible (arrangement);

        globalScriptEditor.setVisible (arrangement || code);
        runScriptButton.setVisible (arrangement || code);
        trackNameLabel.setVisible (arrangement);
        trackNameEditor.setVisible (arrangement);
        trackDurationLabel.setVisible (arrangement);
        trackDurationEditor.setVisible (arrangement);
        laneNameEditor.setVisible (arrangement);
        muteLaneButton.setVisible (arrangement);
        soloLaneButton.setVisible (arrangement);
        duplicateLaneButton.setVisible (arrangement);
        deleteLaneButton.setVisible (arrangement);
        orbitCanvas.setVisible (arrangement);
        stateSettingsLabel.setVisible (arrangement);
        stateTempoLabel.setVisible (arrangement);
        stateTempoSlider.setVisible (arrangement);
        stateTimeSigLabel.setVisible (arrangement);
        timeSigNumeratorBox.setVisible (arrangement);
        timeSigDenominatorBox.setVisible (arrangement);
        stateTrackCountLabel.setVisible (arrangement);
        stateTrackCountEditor.setVisible (arrangement);
        newStateButton.setVisible (arrangement);
        duplicateStateButton.setVisible (arrangement);
        deleteStateButton.setVisible (arrangement);
        selectedLabel.setVisible (arrangement);
        laneHeader.setVisible (arrangement);

        for (auto& button : laneButtons)
            button.setVisible (arrangement && button.isEnabled());

        laneCodeHeader.setVisible (arrangement || code);
        laneCodeEditor.setVisible (arrangement || code);
        laneCodeRunButton.setVisible (arrangement || code);
        stateCodeHeader.setVisible (code);
        mixerViewport.setVisible (mixer);

        arrangementButton.setVisible (true);
        codeViewButton.setVisible (true);
        mixerViewButton.setVisible (true);
    }

    void syncMixerView()
    {
        mixerCanvas.setProject (&topLevelStates,
                                viewedTopLevelState,
                                selectedState,
                                selectedLane,
                                performingTopLevelState,
                                performingTrackIndex,
                                running);
        resizeMixerCanvas();

        if (mainView == MainView::mixer)
            scrollMixerToPlayingChannels();
    }

    void resizeMixerCanvas()
    {
        const auto width = juce::jmax (mixerViewport.getWidth(), mixerCanvas.getPreferredWidth());
        const auto height = juce::jmax (1, mixerViewport.getHeight());
        mixerCanvas.setSize (width, height);
    }

    void scrollMixerToPlayingChannels()
    {
        if (! running || mixerCanvas.isDraggingFader())
            return;

        const auto playingRange = mixerCanvas.getPlayingChannelRange();
        if (! playingRange.has_value())
            return;

        const auto viewX = mixerViewport.getViewPositionX();
        const auto viewY = mixerViewport.getViewPositionY();
        const auto viewWidth = mixerViewport.getViewWidth();
        const auto padding = 24;
        auto targetX = viewX;

        if (playingRange->getStart() < viewX + padding)
            targetX = juce::jmax (0, playingRange->getStart() - padding);
        else if (playingRange->getEnd() > viewX + viewWidth - padding)
            targetX = juce::jmax (0, playingRange->getEnd() - viewWidth + padding);

        if (targetX != viewX)
            mixerViewport.setViewPosition (targetX, viewY);
    }

    void selectMixerChannel (int stateIndex, int trackIndex, int laneIndex)
    {
        if (! isTopLevelStatePopulated (stateIndex))
            return;

        viewedTopLevelState = juce::jlimit (0, maxTopLevelStates - 1, stateIndex);
        selectedState = juce::jlimit (0, getViewedTrackCount() - 1, trackIndex);

        if (const auto* viewedTracks = getViewedTracks())
        {
            const auto& track = (*viewedTracks)[static_cast<size_t> (selectedState)];
            selectedLane = juce::jlimit (0, juce::jmax (0, static_cast<int> (track.lanes.size()) - 1), laneIndex);
        }

        refreshLabels();
    }

    void applyMixerLaneVolumeChange (int stateIndex, int trackIndex, int laneIndex, float)
    {
        selectMixerChannel (stateIndex, trackIndex, laneIndex);

        if (stateIndex == performingTopLevelState && trackIndex == performingTrackIndex)
            loadSelectedContentForCurrentState();
    }

    void applyMixerLanePanChange (int stateIndex, int trackIndex, int laneIndex, float)
    {
        selectMixerChannel (stateIndex, trackIndex, laneIndex);

        if (stateIndex == performingTopLevelState && trackIndex == performingTrackIndex)
            loadSelectedContentForCurrentState();
    }

    Wf::StateSpec* getTrack (int stateIndex, int trackIndex)
    {
        if (! isTopLevelStatePopulated (stateIndex))
            return nullptr;

        auto& tracks = *topLevelStates[static_cast<size_t> (stateIndex)];
        if (trackIndex < 0 || trackIndex >= static_cast<int> (tracks.size()))
            return nullptr;

        return &tracks[static_cast<size_t> (trackIndex)];
    }

    void showEffectSlotMenu (int stateIndex, int trackIndex, int slotIndex)
    {
        auto* track = getTrack (stateIndex, trackIndex);
        if (track == nullptr || slotIndex < 0 || slotIndex >= maxTrackEffectSlots)
            return;

        auto& slot = track->effectSlots[static_cast<size_t> (slotIndex)];
        juce::PopupMenu menu;

        if (slot.pluginName.isNotEmpty())
        {
            menu.addItem (1, slot.active ? "Bypass " + slot.pluginName : "Activate " + slot.pluginName);
            menu.addItem (2, "Replace...");
            menu.addItem (3, "Clear");
        }
        else
        {
            menu.addItem (2, "Load AU/VST3...");
        }

        menu.showMenuAsync (juce::PopupMenu::Options(),
                            [this, stateIndex, trackIndex, slotIndex] (int result)
                            {
                                if (result == 1)
                                    toggleEffectSlot (stateIndex, trackIndex, slotIndex);
                                else if (result == 2)
                                    chooseEffectPluginForSlot (stateIndex, trackIndex, slotIndex);
                                else if (result == 3)
                                    clearEffectSlot (stateIndex, trackIndex, slotIndex);
                            });
    }

    void toggleEffectSlot (int stateIndex, int trackIndex, int slotIndex)
    {
        if (auto* track = getTrack (stateIndex, trackIndex))
        {
            auto& slot = track->effectSlots[static_cast<size_t> (slotIndex)];
            if (slot.pluginName.isNotEmpty())
            {
                slot.active = ! slot.active;
                refreshAfterEffectSlotChange (stateIndex, trackIndex);
            }
        }
    }

    void clearEffectSlot (int stateIndex, int trackIndex, int slotIndex)
    {
        if (auto* track = getTrack (stateIndex, trackIndex))
        {
            track->effectSlots[static_cast<size_t> (slotIndex)] = {};
            refreshAfterEffectSlotChange (stateIndex, trackIndex);
        }
    }

    void chooseEffectPluginForSlot (int stateIndex, int trackIndex, int slotIndex)
    {
        pluginChooser = std::make_unique<juce::FileChooser> ("Choose an AU or VST3 effect",
                                                             juce::File::getSpecialLocation (juce::File::userHomeDirectory),
                                                             "*.vst3;*.component");
        pluginChooser->launchAsync (juce::FileBrowserComponent::openMode
                                    | juce::FileBrowserComponent::canSelectFiles
                                    | juce::FileBrowserComponent::canSelectDirectories,
                                    [this, stateIndex, trackIndex, slotIndex] (const juce::FileChooser& chooser)
                                    {
                                        const auto file = chooser.getResult();
                                        if (! file.exists())
                                            return;

                                        if (auto description = findPluginDescriptionForFile (file))
                                        {
                                            if (auto* track = getTrack (stateIndex, trackIndex))
                                            {
                                                auto& slot = track->effectSlots[static_cast<size_t> (slotIndex)];
                                                slot.active = true;
                                                slot.pluginName = description->name;
                                                slot.pluginFormatName = description->pluginFormatName;
                                                slot.pluginFileOrIdentifier = file.getFullPathName();
                                                slot.pluginIdentifier = description->createIdentifierString();
                                                refreshAfterEffectSlotChange (stateIndex, trackIndex);
                                            }
                                        }
                                    });
    }

    std::unique_ptr<juce::PluginDescription> findPluginDescriptionForFile (const juce::File& file)
    {
        for (auto* format : pluginFormatManager.getFormats())
        {
            if (format == nullptr)
                continue;

            juce::OwnedArray<juce::PluginDescription> descriptions;
            format->findAllTypesForFile (descriptions, file.getFullPathName());

            if (! descriptions.isEmpty() && descriptions[0] != nullptr)
                return std::make_unique<juce::PluginDescription> (*descriptions[0]);
        }

        return {};
    }

    void refreshAfterEffectSlotChange (int stateIndex, int trackIndex)
    {
        if (stateIndex == performingTopLevelState && trackIndex == performingTrackIndex)
            loadSelectedContentForCurrentState();

        refreshLabels();
    }

    void selectViewedTopLevelState (int index)
    {
        const auto nextState = juce::jlimit (0, maxTopLevelStates - 1, index);

        if (viewedTopLevelState == nextState)
        {
            syncViewedStateControls();
            refreshLabels();
            return;
        }

        viewedTopLevelState = nextState;
        if (isTopLevelStatePopulated (viewedTopLevelState))
            selectedState = juce::jlimit (0, getViewedTrackCount() - 1, selectedState);

        syncViewedStateControls();
        refreshLabels();
    }

    void setPerformingTopLevelState (int index, bool restartFromFirstTrack = false)
    {
        const auto nextState = juce::jlimit (0, maxTopLevelStates - 1, index);
        if (! isTopLevelStatePopulated (nextState))
            return;

        if (performingTopLevelState == nextState && ! restartFromFirstTrack)
        {
            refreshLabels();
            return;
        }

        performingTopLevelState = nextState;
        performingTrackIndex = restartFromFirstTrack
            ? 0
            : juce::jlimit (0, getPerformingTrackCount() - 1, performingTrackIndex);
        trackElapsedBars = 0.0;
        nextBarTransitionCheck = 1.0;
        orbitPhase = 0.0f;

        if (viewedTopLevelState == performingTopLevelState)
            selectedState = performingTrackIndex;

        loadSelectedContentForCurrentState();
        refreshLabels();
    }

    int firstEmptyTopLevelState() const
    {
        for (int i = 0; i < maxTopLevelStates; ++i)
            if (! isTopLevelStatePopulated (i))
                return i;

        return -1;
    }

    int firstPopulatedTopLevelState() const
    {
        for (int i = 0; i < maxTopLevelStates; ++i)
            if (isTopLevelStatePopulated (i))
                return i;

        return -1;
    }

    void createTopLevelState()
    {
        auto target = viewedTopLevelState;

        if (isTopLevelStatePopulated (target))
            target = firstEmptyTopLevelState();

        if (target < 0 || target >= maxTopLevelStates)
            return;

        auto created = Wf::makeDefaultStates();

        for (int i = 0; i < static_cast<int> (created.size()); ++i)
            created[static_cast<size_t> (i)].name = "Track " + juce::String (i + 1);

        topLevelStates[static_cast<size_t> (target)] = std::move (created);
        topLevelTemposBpm[static_cast<size_t> (target)] = 88.0f;
        topLevelTimeSigNumerators[static_cast<size_t> (target)] = 4;
        topLevelTimeSigDenominators[static_cast<size_t> (target)] = 4;
        viewedTopLevelState = target;
        selectedState = 0;
        selectedLane = 0;
        selectViewedTopLevelState (target);
    }

    void duplicateViewedTopLevelState()
    {
        const auto target = firstEmptyTopLevelState();
        if (target < 0 || ! isTopLevelStatePopulated (viewedTopLevelState))
            return;

        const auto source = static_cast<size_t> (viewedTopLevelState);
        topLevelStates[static_cast<size_t> (target)] = topLevelStates[source];
        topLevelTemposBpm[static_cast<size_t> (target)] = topLevelTemposBpm[source];
        topLevelTimeSigNumerators[static_cast<size_t> (target)] = topLevelTimeSigNumerators[source];
        topLevelTimeSigDenominators[static_cast<size_t> (target)] = topLevelTimeSigDenominators[source];
        selectViewedTopLevelState (target);
    }

    void deleteViewedTopLevelState()
    {
        const auto target = viewedTopLevelState;
        if (! isTopLevelStatePopulated (target))
            return;

        const auto deletingPerformingState = target == performingTopLevelState;
        if (deletingPerformingState)
        {
            scriptRunning = false;
            running = false;
            syncTransportButtons();
            applyCurrentAudioControls();
        }

        topLevelStates[static_cast<size_t> (target)].reset();
        topLevelTemposBpm[static_cast<size_t> (target)] = 88.0f;
        topLevelTimeSigNumerators[static_cast<size_t> (target)] = 4;
        topLevelTimeSigDenominators[static_cast<size_t> (target)] = 4;
        selectedState = 0;
        selectedLane = 0;

        if (deletingPerformingState)
        {
            const auto replacement = firstPopulatedTopLevelState();
            performingTopLevelState = replacement >= 0 ? replacement : target;
            performingTrackIndex = 0;
            trackElapsedBars = 0.0;
            nextBarTransitionCheck = 1.0;
            orbitPhase = 0.0f;
        }

        refreshLabels();
        grabKeyboardFocus();
    }

    bool isInlineTextEditorFocused() const
    {
        return globalScriptEditor.hasKeyboardFocus (true)
            || laneCodeEditor.hasKeyboardFocus (true)
            || trackNameEditor.hasKeyboardFocus (true)
            || trackDurationEditor.hasKeyboardFocus (true)
            || laneNameEditor.hasKeyboardFocus (true)
            || stateTrackCountEditor.hasKeyboardFocus (true)
            || orbitCanvas.isEditingTransitionProbability();
    }

    struct GlobalScriptStep
    {
        int stateIndex = 0;
        double bars = 1.0;
        std::optional<float> tempoBpm;
        std::optional<int> timeSigNumerator;
        std::optional<int> timeSigDenominator;
    };

    std::vector<GlobalScriptStep> parseGlobalScript() const
    {
        std::vector<GlobalScriptStep> parsed;
        const auto text = globalScriptEditor.getText().toStdString();
        const std::regex tokenRegex (
            R"((?:playState\s*\(\s*(1[0-6]|[1-9])\s*,\s*(\d+(?:\.\d+)?)\s*(?:,\s*(?:tempo\s*=\s*)?(\d+(?:\.\d+)?))?\s*(?:,\s*(?:(?:timeSig|meter)\s*=\s*)?(\d{1,2})\s*(?:/|,)\s*(2|4|8|16))?\s*\))|(?:state\s*\(\s*(1[0-6]|[1-9])\s*\))|(?:(\d+(?:\.\d+)?)\s*::\s*bar)|(?:(?:tempo|bpm)\s*\(\s*(\d+(?:\.\d+)?)\s*\))|(?:(?:timeSig|meter)\s*\(\s*(\d{1,2})\s*,\s*(2|4|8|16)\s*\)))",
            std::regex_constants::icase);
        int pendingStateIndex = -1;
        std::optional<float> pendingTempoBpm;
        std::optional<int> pendingTimeSigNumerator;
        std::optional<int> pendingTimeSigDenominator;

        auto addStep = [&] (int stateIndex,
                            double bars,
                            std::optional<float> stepTempoBpm,
                            std::optional<int> stepTimeSigNumerator,
                            std::optional<int> stepTimeSigDenominator)
        {
            if (! isTopLevelStatePopulated (stateIndex) || bars <= 0.0)
                return;

            parsed.push_back ({ stateIndex,
                                bars,
                                stepTempoBpm.has_value() ? stepTempoBpm : pendingTempoBpm,
                                stepTimeSigNumerator.has_value() ? stepTimeSigNumerator : pendingTimeSigNumerator,
                                stepTimeSigDenominator.has_value() ? stepTimeSigDenominator : pendingTimeSigDenominator });
        };

        for (std::sregex_iterator it (text.begin(), text.end(), tokenRegex), end; it != end; ++it)
        {
            if ((*it)[1].matched)
            {
                pendingStateIndex = std::stoi ((*it)[1].str()) - 1;
                const auto bars = std::stod ((*it)[2].str());
                const auto stepTempoBpm = (*it)[3].matched
                    ? std::optional<float> (sanitizeScriptTempo (std::stof ((*it)[3].str())))
                    : std::optional<float>();
                const auto stepNumerator = (*it)[4].matched
                    ? std::optional<int> (sanitizeScriptTimeSigNumerator (std::stoi ((*it)[4].str())))
                    : std::optional<int>();
                const auto stepDenominator = (*it)[5].matched
                    ? std::optional<int> (sanitizeScriptTimeSigDenominator (std::stoi ((*it)[5].str())))
                    : std::optional<int>();

                addStep (pendingStateIndex, bars, stepTempoBpm, stepNumerator, stepDenominator);
                pendingStateIndex = -1;
                continue;
            }

            if ((*it)[6].matched)
            {
                pendingStateIndex = std::stoi ((*it)[6].str()) - 1;
                continue;
            }

            if ((*it)[7].matched && pendingStateIndex >= 0)
            {
                const auto bars = std::stod ((*it)[7].str());
                addStep (pendingStateIndex, bars, {}, {}, {});
                pendingStateIndex = -1;
                continue;
            }

            if ((*it)[8].matched)
            {
                pendingTempoBpm = sanitizeScriptTempo (std::stof ((*it)[8].str()));
                continue;
            }

            if ((*it)[9].matched && (*it)[10].matched)
            {
                pendingTimeSigNumerator = sanitizeScriptTimeSigNumerator (std::stoi ((*it)[9].str()));
                pendingTimeSigDenominator = sanitizeScriptTimeSigDenominator (std::stoi ((*it)[10].str()));
            }
        }

        return parsed;
    }

    static float sanitizeScriptTempo (float tempoBpm)
    {
        return juce::jlimit (30.0f, 220.0f, tempoBpm);
    }

    static int sanitizeScriptTimeSigNumerator (int numerator)
    {
        return juce::jlimit (1, 16, numerator);
    }

    static int sanitizeScriptTimeSigDenominator (int denominator)
    {
        if (denominator == 2 || denominator == 4 || denominator == 8 || denominator == 16)
            return denominator;

        return 4;
    }

    void toggleMainTransport()
    {
        if (running)
            stopMainTransport();
        else
            playFromBeginning();
    }

    void playFromBeginning()
    {
        globalScriptSteps = parseGlobalScript();
        scriptStepIndex = 0;
        scriptStepElapsedBars = 0.0;
        scriptShouldStopAtEnd = globalScriptEditor.getText().containsIgnoreCase ("stop");
        running = true;
        syncTransportButtons();

        if (globalScriptSteps.empty())
        {
            scriptRunning = false;
            setPerformingTopLevelState (performingTopLevelState, true);
        }
        else
        {
            scriptRunning = true;
            applyGlobalScriptStep (globalScriptSteps.front());
        }

        applyCurrentAudioControls();
        refreshLabels();
    }

    void stopMainTransport()
    {
        scriptRunning = false;
        running = false;
        syncTransportButtons();
        applyCurrentAudioControls();
        refreshLabels();
    }

    void syncTransportButtons()
    {
        const auto text = running ? "Stop" : "Play";
        runScriptButton.setButtonText (text);
    }

    void selectState (int index)
    {
        const auto* viewedTracks = getViewedTracks();
        if (viewedTracks == nullptr || viewedTracks->empty())
            return;

        selectedState = (index + static_cast<int> (viewedTracks->size())) % static_cast<int> (viewedTracks->size());
        orbitPhase = 0.0f;

        if (viewedTopLevelState == performingTopLevelState)
        {
            performingTrackIndex = selectedState;
            trackElapsedBars = 0.0;
            nextBarTransitionCheck = 1.0;
            loadSelectedContentForCurrentState();
        }

        refreshLabels();
    }

    void pickState()
    {
        const auto* viewedTracks = getViewedTracks();
        if (viewedTracks == nullptr || viewedTracks->empty())
            return;

        std::uniform_int_distribution<int> distribution (0, static_cast<int> (viewedTracks->size()) - 1);
        selectState (distribution (random));
    }

    void setTransitionProbability (int trackIndex, std::optional<int> probability)
    {
        auto* viewedTracks = getViewedTracks();
        if (viewedTracks == nullptr || trackIndex < 0 || trackIndex >= static_cast<int> (viewedTracks->size()))
            return;

        (*viewedTracks)[static_cast<size_t> (trackIndex)].transitionProbabilityPercent = probability;

        if (viewedTopLevelState == performingTopLevelState && trackIndex == performingTrackIndex)
            nextBarTransitionCheck = std::floor (trackElapsedBars) + 1.0;

        refreshLabels();
    }

    void selectLane (int index)
    {
        const auto* viewedTracks = getViewedTracks();
        if (viewedTracks == nullptr || viewedTracks->empty())
            return;

        const auto& track = (*viewedTracks)[static_cast<size_t> (selectedState)];
        if (track.lanes.empty())
            return;

        selectedLane = juce::jlimit (0, static_cast<int> (track.lanes.size()) - 1, index);
        refreshLabels();
    }

    void refreshLabels()
    {
        syncViewedStateControls();

        for (int i = 0; i < maxTopLevelStates; ++i)
        {
            auto& button = stateButtons[static_cast<size_t> (i)];
            if (isTopLevelStatePopulated (i))
                styleButton (button, i == 0 ? green() : blue());
            else
                styleEmptyStateButton (button);

            button.setEnabled (true);
            button.setToggleState (viewedTopLevelState == i, juce::dontSendNotification);
        }

        const auto hasEmptyStateSlot = firstEmptyTopLevelState() >= 0;
        newStateButton.setEnabled (hasEmptyStateSlot);
        duplicateStateButton.setEnabled (hasEmptyStateSlot && isTopLevelStatePopulated (viewedTopLevelState));
        deleteStateButton.setEnabled (isTopLevelStatePopulated (viewedTopLevelState));

        const auto* viewedTracks = getViewedTracks();
        if (viewedTracks == nullptr || viewedTracks->empty())
        {
            selectedLabel.setText ("Empty state", juce::dontSendNotification);

            for (auto& button : laneButtons)
            {
                button.setButtonText ("");
                button.setToggleState (false, juce::dontSendNotification);
                button.setEnabled (false);
                button.setVisible (false);
            }

            syncEditControls (nullptr, nullptr);
            updateLaneCodeHeader ("lane code", mutedInk().withAlpha (0.22f));
            laneCodeRunButton.setEnabled (false);
            setLaneCodeEditorText ("// Click New to create this state.");
            orbitCanvas.setState (nullptr, 0, orbitPhase, running);
            syncMixerView();
            syncViewVisibility();
            syncViewButtons();
            return;
        }

        selectedState = juce::jlimit (0, static_cast<int> (viewedTracks->size()) - 1, selectedState);
        const auto& state = (*viewedTracks)[static_cast<size_t> (selectedState)];
        selectedLabel.setText (state.name, juce::dontSendNotification);
        selectedLane = juce::jlimit (0, juce::jmax (0, static_cast<int> (state.lanes.size()) - 1), selectedLane);

        for (int i = 0; i < static_cast<int> (laneButtons.size()); ++i)
        {
            auto& button = laneButtons[static_cast<size_t> (i)];

            if (i < static_cast<int> (state.lanes.size()))
            {
                const auto& lane = state.lanes[static_cast<size_t> (i)];
                juce::String prefix;
                if (lane.solo)
                    prefix << "S ";
                else if (lane.muted)
                    prefix << "M ";

                button.setButtonText (juce::String (i + 1) + "  " + prefix + lane.name);
                button.setEnabled (true);
                button.setVisible (true);
                button.setToggleState (selectedLane == i, juce::dontSendNotification);
            }
            else
            {
                button.setButtonText ("");
                button.setEnabled (false);
                button.setVisible (false);
                button.setToggleState (false, juce::dontSendNotification);
            }
        }

        syncEditControls (&state, state.lanes.empty() ? nullptr : &state.lanes[static_cast<size_t> (selectedLane)]);
        updateLaneCode();
        orbitCanvas.setState (viewedTracks, selectedState, orbitPhase, running);
        syncMixerView();
        syncViewVisibility();
        syncViewButtons();
    }

    void updateLaneCode()
    {
        const auto* viewedTracks = getViewedTracks();
        if (viewedTracks == nullptr || viewedTracks->empty())
            return;

        const auto& state = (*viewedTracks)[static_cast<size_t> (selectedState)];
        if (state.lanes.empty())
        {
            updateLaneCodeHeader ("lane code", mutedInk().withAlpha (0.22f));
            laneCodeRunButton.setEnabled (false);
            setLaneCodeEditorText ("// This track has no lanes.");
            return;
        }

        const auto laneIndex = static_cast<size_t> (juce::jlimit (0, static_cast<int> (state.lanes.size()) - 1, selectedLane));
        if (laneCodeEditor.hasKeyboardFocus (true)
            && laneCodeDirty
            && laneCodeViewedTopLevelState == viewedTopLevelState
            && laneCodeTrackIndex == selectedState
            && laneCodeLaneIndex == selectedLane)
            return;

        setLaneCodeEditorText (makeLaneCode (state.lanes[laneIndex], selectedLane));
        laneCodeViewedTopLevelState = viewedTopLevelState;
        laneCodeTrackIndex = selectedState;
        laneCodeLaneIndex = selectedLane;
        laneCodeDirty = false;
        laneCodeRunButton.setEnabled (false);
        laneCodeLastValidatedText = laneCodeEditor.getText();
        updateLaneCodeHeader ("lane code", mutedInk().withAlpha (0.22f));
    }

    void syncEditControls (const Wf::StateSpec* track, const Wf::LaneSpec* lane)
    {
        juce::ScopedValueSetter<bool> guard (suppressEditCallbacks, true);

        const auto hasTrack = track != nullptr;
        const auto hasLane = lane != nullptr;
        const auto probabilityControlsDuration = hasTrack && track->transitionProbabilityPercent.has_value();

        trackNameEditor.setEnabled (hasTrack);
        trackDurationEditor.setEnabled (hasTrack && ! probabilityControlsDuration);
        laneNameEditor.setEnabled (hasLane);
        muteLaneButton.setEnabled (hasLane);
        soloLaneButton.setEnabled (hasLane);
        duplicateLaneButton.setEnabled (hasLane && track != nullptr && static_cast<int> (track->lanes.size()) < maxTrackLanes);
        deleteLaneButton.setEnabled (hasLane && track != nullptr && track->lanes.size() > 1);

        if (! trackNameEditor.hasKeyboardFocus (true))
            trackNameEditor.setText (hasTrack ? track->name : juce::String(), juce::dontSendNotification);

        if (! trackDurationEditor.hasKeyboardFocus (true))
        {
            if (hasTrack && track->duration.has_value())
                trackDurationEditor.setText (formatTrackDuration (*track->duration), juce::dontSendNotification);
            else
                trackDurationEditor.setText ({}, juce::dontSendNotification);
        }

        if (! laneNameEditor.hasKeyboardFocus (true))
            laneNameEditor.setText (hasLane ? lane->name : juce::String(), juce::dontSendNotification);

        if (hasLane)
        {
            muteLaneButton.setToggleState (lane->muted, juce::dontSendNotification);
            soloLaneButton.setToggleState (lane->solo, juce::dontSendNotification);
        }
        else
        {
            muteLaneButton.setToggleState (false, juce::dontSendNotification);
            soloLaneButton.setToggleState (false, juce::dontSendNotification);
        }
    }

    void applyTrackNameEdit()
    {
        if (suppressEditCallbacks)
            return;

        if (auto* track = getSelectedViewedTrack())
        {
            track->name = trackNameEditor.getText().trim().isNotEmpty() ? trackNameEditor.getText().trim() : "Track";
            refreshAfterStructureEdit (false);
        }
    }

    void applyTrackDurationEdit()
    {
        if (suppressEditCallbacks)
            return;

        if (auto* track = getSelectedViewedTrack())
        {
            if (track->transitionProbabilityPercent.has_value())
                return;

            const auto parsed = parseTrackDuration (trackDurationEditor.getText());
            if (parsed.has_value())
                track->duration = *parsed;
            else
                track->duration.reset();

            refreshAfterStructureEdit (false);
        }
    }

    static juce::String formatTrackDuration (const Wf::TrackDurationSpec& duration)
    {
        return juce::String (duration.bars) + "." + juce::String (duration.beats);
    }

    static std::optional<Wf::TrackDurationSpec> parseTrackDuration (juce::String text)
    {
        text = text.trim();
        if (text.isEmpty())
            return {};

        const auto dot = text.indexOfChar ('.');
        const auto barsText = dot >= 0 ? text.substring (0, dot).trim() : text;
        const auto beatsText = dot >= 0 ? text.substring (dot + 1).trim() : juce::String();
        const auto bars = barsText.isEmpty() ? 0 : juce::jmax (0, barsText.getIntValue());
        const auto beats = beatsText.isEmpty() ? 0 : juce::jmax (0, beatsText.getIntValue());

        if (bars == 0 && beats == 0)
            return {};

        return Wf::TrackDurationSpec { juce::jmin (128, bars), juce::jmin (1024, beats) };
    }

    void applyLaneNameEdit()
    {
        if (suppressEditCallbacks)
            return;

        if (auto* lane = getSelectedViewedLane())
        {
            lane->name = laneNameEditor.getText().trim().isNotEmpty() ? laneNameEditor.getText().trim() : "Lane";
            refreshAfterStructureEdit (false);
        }
    }

    void toggleSelectedLaneMute()
    {
        if (auto* lane = getSelectedViewedLane())
        {
            lane->muted = ! lane->muted;
            refreshAfterStructureEdit (true);
        }
    }

    void toggleSelectedLaneSolo()
    {
        if (auto* lane = getSelectedViewedLane())
        {
            lane->solo = ! lane->solo;
            refreshAfterStructureEdit (true);
        }
    }

    void duplicateSelectedLane()
    {
        auto* track = getSelectedViewedTrack();
        if (track == nullptr || selectedLane < 0 || selectedLane >= static_cast<int> (track->lanes.size()) || static_cast<int> (track->lanes.size()) >= maxTrackLanes)
            return;

        auto copy = track->lanes[static_cast<size_t> (selectedLane)];
        copy.name = copy.name + " copy";
        track->lanes.insert (track->lanes.begin() + selectedLane + 1, copy);
        ++selectedLane;
        refreshAfterStructureEdit (true);
    }

    void deleteSelectedLane()
    {
        auto* track = getSelectedViewedTrack();
        if (track == nullptr || track->lanes.size() <= 1 || selectedLane < 0 || selectedLane >= static_cast<int> (track->lanes.size()))
            return;

        track->lanes.erase (track->lanes.begin() + selectedLane);
        selectedLane = juce::jlimit (0, static_cast<int> (track->lanes.size()) - 1, selectedLane);
        refreshAfterStructureEdit (true);
    }

    void refreshAfterStructureEdit (bool reloadAudioIfNeeded)
    {
        if (reloadAudioIfNeeded && viewedTopLevelState == performingTopLevelState && selectedState == performingTrackIndex)
            loadSelectedContentForCurrentState();

        refreshLabels();
    }

    void setLaneCodeEditorText (const juce::String& text)
    {
        juce::ScopedValueSetter<bool> guard (suppressLaneCodeCallbacks, true);
        laneCodeEditor.setText (text, juce::dontSendNotification);
    }

    void updateLaneCodeHeader (const juce::String& text, juce::Colour outline)
    {
        laneCodeHeader.setText (text, juce::dontSendNotification);
        laneCodeEditor.setColour (juce::TextEditor::outlineColourId, outline);
    }

    static juce::String makeLaneCode (const Wf::LaneSpec& lane, int laneIndex)
    {
        juce::String code;
        code << "// " << lane.name << "\n";
        code << "// baseHz: " << Wf::chuckFloat (lane.baseHz)
             << "  volume: " << Wf::chuckFloat (lane.volume)
             << "  pulseTicks: " << lane.pulseTicks
             << "  openTicks: " << lane.openTicks << "\n\n";
        code << laneDeclarationMarker << "\n";
        if (lane.customDeclarationCode.has_value())
            code << *lane.customDeclarationCode << "\n";
        else
            Wf::appendLaneDeclaration (code, lane, laneIndex);

        code << laneControlMarker << "\n";
        code << "// expects host variables: tick, stepPhase, intensity, bright, orbit\n";
        if (lane.customControlCode.has_value())
            code << *lane.customControlCode << "\n";
        else
            Wf::appendLaneControl (code, lane, laneIndex);

        return code;
    }

    struct ParsedLaneCode
    {
        juce::String declaration;
        juce::String control;
    };

    static std::optional<ParsedLaneCode> parseLaneCode (juce::String text)
    {
        const auto declarationMarkerText = juce::String (laneDeclarationMarker);
        const auto controlMarkerText = juce::String (laneControlMarker);
        const auto declarationMarkerIndex = text.indexOf (declarationMarkerText);
        const auto controlMarkerIndex = text.indexOf (controlMarkerText);

        if (declarationMarkerIndex < 0 || controlMarkerIndex < 0 || controlMarkerIndex <= declarationMarkerIndex)
            return {};

        const auto declarationMarkerEnd = declarationMarkerIndex + declarationMarkerText.length();
        const auto controlMarkerEnd = controlMarkerIndex + controlMarkerText.length();
        const auto declarationLineEnd = text.substring (declarationMarkerEnd).indexOfChar ('\n');
        const auto controlLineEnd = text.substring (controlMarkerEnd).indexOfChar ('\n');

        if (declarationLineEnd < 0)
            return {};

        const auto declarationStart = declarationMarkerEnd + declarationLineEnd + 1;
        const auto controlStart = controlLineEnd < 0 ? text.length() : controlMarkerEnd + controlLineEnd + 1;
        auto declaration = text.substring (declarationStart, controlMarkerIndex).trim();
        auto control = text.substring (controlStart).trim();

        if (declaration.isEmpty() || control.isEmpty())
            return {};

        return ParsedLaneCode { declaration, control };
    }

    static bool validateLaneProgram (const Wf::StateSpec& candidateTrack, juce::String& error)
    {
        EmbeddedChucKEngine validator;
        if (! validator.prepare (48000.0, 256, 0, 2))
        {
            error = validator.getLastError();
            return false;
        }

        if (! validator.loadProgram (Wf::buildStateProgram (candidateTrack), Wf::makeWfParameterBindings()))
        {
            error = validator.getLastError();
            return false;
        }

        return true;
    }

    void markLaneCodeEdited()
    {
        if (suppressLaneCodeCallbacks)
            return;

        laneCodeDirty = true;
        laneCodeRunButton.setEnabled (true);
        updateLaneCodeHeader ("lane code - edited", amber().withAlpha (0.62f));
    }

    void applyLaneCodeEdit()
    {
        const auto text = laneCodeEditor.getText();
        if (! laneCodeDirty && text == laneCodeLastValidatedText)
            return;

        laneCodeLastValidatedText = text;

        auto* track = getSelectedViewedTrack();
        if (track == nullptr || selectedLane < 0 || selectedLane >= static_cast<int> (track->lanes.size()))
            return;

        const auto parsed = parseLaneCode (text);
        if (! parsed.has_value())
        {
            laneCodeDirty = true;
            laneCodeRunButton.setEnabled (true);
            updateLaneCodeHeader ("lane code - invalid", coral().withAlpha (0.74f));
            return;
        }

        auto candidateTrack = *track;
        auto& candidateLane = candidateTrack.lanes[static_cast<size_t> (selectedLane)];
        candidateLane.customDeclarationCode = parsed->declaration;
        candidateLane.customControlCode = parsed->control;

        juce::String validationError;
        if (! validateLaneProgram (candidateTrack, validationError))
        {
            laneCodeDirty = true;
            laneCodeRunButton.setEnabled (true);
            updateLaneCodeHeader ("lane code - invalid", coral().withAlpha (0.74f));
            return;
        }

        auto& lane = track->lanes[static_cast<size_t> (selectedLane)];
        lane.customDeclarationCode = parsed->declaration;
        lane.customControlCode = parsed->control;
        laneCodeDirty = false;
        laneCodeRunButton.setEnabled (false);
        updateLaneCodeHeader ("lane code - live", green().withAlpha (0.58f));
        syncMixerView();

        if (viewedTopLevelState == performingTopLevelState && selectedState == performingTrackIndex)
            loadSelectedContentForCurrentState();
    }

    void applyStateTrackCountEdit()
    {
        if (suppressStateControlCallbacks)
            return;

        auto* viewedTracks = getViewedTracks();
        if (viewedTracks == nullptr)
            return;

        const auto digits = stateTrackCountEditor.getText().retainCharacters ("0123456789").trim();
        if (digits.isEmpty())
            return;

        const auto requestedCount = digits.getIntValue();
        const auto clampedCount = juce::jlimit (1, maxStateTracks, requestedCount);

        if (clampedCount != requestedCount)
            stateTrackCountEditor.setText (juce::String (clampedCount), juce::dontSendNotification);

        resizeViewedStateTracks (clampedCount);
    }

    void resizeViewedStateTracks (int requestedCount)
    {
        auto* viewedTracks = getViewedTracks();
        if (viewedTracks == nullptr)
            return;

        const auto targetCount = juce::jlimit (1, maxStateTracks, requestedCount);
        const auto previousCount = static_cast<int> (viewedTracks->size());
        if (targetCount == previousCount)
            return;

        if (targetCount > previousCount)
        {
            const auto defaults = Wf::makeDefaultStates();

            for (int trackIndex = previousCount; trackIndex < targetCount; ++trackIndex)
            {
                auto track = defaults.empty()
                    ? Wf::StateSpec { "Track " + juce::String (trackIndex + 1), 88.0, {}, Wf::TrackDurationSpec { 1, 0 }, {} }
                    : defaults[static_cast<size_t> (trackIndex % static_cast<int> (defaults.size()))];

                track.name = "Track " + juce::String (trackIndex + 1);
                track.duration = Wf::TrackDurationSpec { 1, 0 };
                track.transitionProbabilityPercent.reset();
                viewedTracks->push_back (std::move (track));
            }
        }
        else
        {
            viewedTracks->resize (static_cast<size_t> (targetCount));
        }

        selectedState = juce::jlimit (0, targetCount - 1, selectedState);

        const auto editingPerformingState = viewedTopLevelState == performingTopLevelState;
        const auto previousPerformingTrack = performingTrackIndex;

        if (editingPerformingState)
            performingTrackIndex = juce::jlimit (0, targetCount - 1, performingTrackIndex);

        if (editingPerformingState && performingTrackIndex != previousPerformingTrack)
        {
            selectedState = performingTrackIndex;
            trackElapsedBars = 0.0;
            nextBarTransitionCheck = 1.0;
            orbitPhase = 0.0f;
            loadSelectedContentForCurrentState();
        }

        refreshLabels();
    }

    void syncViewedStateControls()
    {
        juce::ScopedValueSetter<bool> guard (suppressStateControlCallbacks, true);
        const auto index = static_cast<size_t> (juce::jlimit (0, maxTopLevelStates - 1, viewedTopLevelState));
        const auto tempoBpm = topLevelTemposBpm[index];
        const auto numerator = topLevelTimeSigNumerators[index];
        const auto denominator = topLevelTimeSigDenominators[index];
        const auto populated = isTopLevelStatePopulated (viewedTopLevelState);
        const auto* viewedTracks = getViewedTracks();
        const auto trackCount = viewedTracks == nullptr ? 0 : static_cast<int> (viewedTracks->size());

        stateSettingsLabel.setText ("State " + juce::String (viewedTopLevelState + 1) + (populated ? " settings" : " empty"), juce::dontSendNotification);
        stateTempoLabel.setText ("Tempo  " + juce::String (tempoBpm, 1) + " bpm", juce::dontSendNotification);
        stateTimeSigLabel.setText ("Time signature  " + juce::String (numerator) + "/" + juce::String (denominator), juce::dontSendNotification);
        stateTrackCountEditor.setEnabled (populated);
        stateTrackCountLabel.setAlpha (populated ? 1.0f : 0.34f);
        if (! stateTrackCountEditor.hasKeyboardFocus (true))
            stateTrackCountEditor.setText (populated ? juce::String (trackCount) : juce::String(), juce::dontSendNotification);

        stateTempoSlider.setValue (tempoBpm, juce::dontSendNotification);
        timeSigNumeratorBox.setSelectedId (numerator, juce::dontSendNotification);
        timeSigDenominatorBox.setSelectedId (denominator, juce::dontSendNotification);
    }

    void timerCallback() override
    {
        const auto now = juce::Time::getMillisecondCounterHiRes();
        const auto deltaSeconds = lastTimerMs > 0.0 ? juce::jlimit (0.0, 0.1, (now - lastTimerMs) * 0.001) : 0.0;
        lastTimerMs = now;

        const auto rate = defaultPlaybackRate;
        const auto tempoBpm = getPerformingTempoBpm();

        if (running)
        {
            trackElapsedBars += deltaSeconds * (tempoBpm / 60.0) * static_cast<double> (rate) / getPerformingQuarterNotesPerBar();
            advanceGlobalScript (deltaSeconds, tempoBpm, rate);

            if (running)
            {
                if (const auto probabilityPercent = getPerformingTrackTransitionProbabilityPercent())
                {
                    auto didTransition = false;

                    while (running && ! didTransition && trackElapsedBars >= nextBarTransitionCheck)
                    {
                        if (shouldTakeProbabilisticTransition (*probabilityPercent))
                        {
                            advancePerformingTrack();
                            didTransition = true;
                        }
                        else
                        {
                            nextBarTransitionCheck += 1.0;
                        }
                    }

                    if (running && ! didTransition)
                        orbitPhase = static_cast<float> (std::fmod (trackElapsedBars, 1.0));
                }
                else if (const auto durationBars = getPerformingTrackDurationBars())
                {
                    orbitPhase = static_cast<float> (juce::jlimit (0.0, 1.0, trackElapsedBars / *durationBars));

                    if (trackElapsedBars >= *durationBars)
                        advancePerformingTrack();
                }
                else
                {
                    orbitPhase = static_cast<float> (std::fmod (trackElapsedBars, 1.0));
                }
            }
        }

        applyCurrentAudioControls();

        statusLabel.setText (juce::String (running ? "running  " : "stopped  ") + audioCallback.diagnostics(), juce::dontSendNotification);
        if (const auto* viewedTracks = getViewedTracks())
            orbitCanvas.setState (viewedTracks, selectedState, orbitPhase, running);
    }

    void advancePerformingTrack()
    {
        const auto* performingTracks = getPerformingTracks();
        if (performingTracks == nullptr || performingTracks->empty())
            return;

        performingTrackIndex = (performingTrackIndex + 1) % static_cast<int> (performingTracks->size());
        trackElapsedBars = 0.0;
        nextBarTransitionCheck = 1.0;
        orbitPhase = 0.0f;

        if (viewedTopLevelState == performingTopLevelState)
            selectedState = performingTrackIndex;

        loadSelectedContentForCurrentState();
        refreshLabels();
    }

    void advanceGlobalScript (double deltaSeconds, double tempoBpm, float rate)
    {
        if (! scriptRunning || scriptStepIndex >= globalScriptSteps.size())
            return;

        scriptStepElapsedBars += deltaSeconds * (tempoBpm / 60.0) * static_cast<double> (rate) / getPerformingQuarterNotesPerBar();

        if (scriptStepElapsedBars < globalScriptSteps[scriptStepIndex].bars)
            return;

        ++scriptStepIndex;
        scriptStepElapsedBars = 0.0;

        if (scriptStepIndex >= globalScriptSteps.size())
        {
            scriptRunning = false;

            if (scriptShouldStopAtEnd)
            {
                stopMainTransport();
            }

            return;
        }

        applyGlobalScriptStep (globalScriptSteps[scriptStepIndex]);
    }

    float getPerformingTempoBpm() const
    {
        if (scriptRunning && scriptStepIndex < globalScriptSteps.size() && globalScriptSteps[scriptStepIndex].tempoBpm.has_value())
            return *globalScriptSteps[scriptStepIndex].tempoBpm;

        return topLevelTemposBpm[static_cast<size_t> (juce::jlimit (0, maxTopLevelStates - 1, performingTopLevelState))];
    }

    double getPerformingQuarterNotesPerBar() const
    {
        const auto index = static_cast<size_t> (juce::jlimit (0, maxTopLevelStates - 1, performingTopLevelState));
        const auto numerator = static_cast<double> (scriptRunning
                                                        && scriptStepIndex < globalScriptSteps.size()
                                                        && globalScriptSteps[scriptStepIndex].timeSigNumerator.has_value()
                                                    ? *globalScriptSteps[scriptStepIndex].timeSigNumerator
                                                    : topLevelTimeSigNumerators[index]);
        const auto denominator = static_cast<double> (juce::jmax (1, scriptRunning
                                                                      && scriptStepIndex < globalScriptSteps.size()
                                                                      && globalScriptSteps[scriptStepIndex].timeSigDenominator.has_value()
                                                                  ? *globalScriptSteps[scriptStepIndex].timeSigDenominator
                                                                  : topLevelTimeSigDenominators[index]));
        return numerator * 4.0 / denominator;
    }

    double getPerformingBeatsPerBar() const
    {
        const auto index = static_cast<size_t> (juce::jlimit (0, maxTopLevelStates - 1, performingTopLevelState));
        return static_cast<double> (scriptRunning
                                        && scriptStepIndex < globalScriptSteps.size()
                                        && globalScriptSteps[scriptStepIndex].timeSigNumerator.has_value()
                                    ? *globalScriptSteps[scriptStepIndex].timeSigNumerator
                                    : topLevelTimeSigNumerators[index]);
    }

    std::optional<double> getPerformingTrackDurationBars() const
    {
        const auto* performingTracks = getPerformingTracks();
        if (performingTracks == nullptr || performingTracks->empty())
            return {};

        const auto index = static_cast<size_t> (juce::jlimit (0, static_cast<int> (performingTracks->size()) - 1, performingTrackIndex));
        if (! (*performingTracks)[index].duration.has_value())
            return {};

        const auto duration = *(*performingTracks)[index].duration;
        const auto beatsPerBar = getPerformingBeatsPerBar();
        const auto totalBars = static_cast<double> (duration.bars)
                             + (static_cast<double> (duration.beats) / beatsPerBar);

        if (totalBars <= 0.0)
            return {};

        return juce::jmax (0.25, totalBars);
    }

    std::optional<int> getPerformingTrackTransitionProbabilityPercent() const
    {
        const auto* performingTracks = getPerformingTracks();
        if (performingTracks == nullptr || performingTracks->empty())
            return {};

        const auto index = static_cast<size_t> (juce::jlimit (0, static_cast<int> (performingTracks->size()) - 1, performingTrackIndex));
        return (*performingTracks)[index].transitionProbabilityPercent;
    }

    bool shouldTakeProbabilisticTransition (int probabilityPercent)
    {
        if (probabilityPercent <= 0)
            return false;

        if (probabilityPercent >= 100)
            return true;

        std::uniform_int_distribution<int> distribution (1, 100);
        return distribution (random) <= probabilityPercent;
    }

    void applyGlobalScriptStep (const GlobalScriptStep& step)
    {
        setPerformingTopLevelState (step.stateIndex, true);
    }

    bool isTopLevelStatePopulated (int index) const
    {
        return index >= 0
            && index < maxTopLevelStates
            && topLevelStates[static_cast<size_t> (index)].has_value()
            && ! topLevelStates[static_cast<size_t> (index)]->empty();
    }

    const std::vector<Wf::StateSpec>* getViewedTracks() const
    {
        if (! isTopLevelStatePopulated (viewedTopLevelState))
            return nullptr;

        return &*topLevelStates[static_cast<size_t> (viewedTopLevelState)];
    }

    std::vector<Wf::StateSpec>* getViewedTracks()
    {
        if (! isTopLevelStatePopulated (viewedTopLevelState))
            return nullptr;

        return &*topLevelStates[static_cast<size_t> (viewedTopLevelState)];
    }

    Wf::StateSpec* getSelectedViewedTrack()
    {
        auto* viewedTracks = getViewedTracks();
        if (viewedTracks == nullptr || viewedTracks->empty())
            return nullptr;

        selectedState = juce::jlimit (0, static_cast<int> (viewedTracks->size()) - 1, selectedState);
        return &(*viewedTracks)[static_cast<size_t> (selectedState)];
    }

    Wf::LaneSpec* getSelectedViewedLane()
    {
        auto* track = getSelectedViewedTrack();
        if (track == nullptr || track->lanes.empty())
            return nullptr;

        selectedLane = juce::jlimit (0, static_cast<int> (track->lanes.size()) - 1, selectedLane);
        return &track->lanes[static_cast<size_t> (selectedLane)];
    }

    const std::vector<Wf::StateSpec>* getPerformingTracks() const
    {
        if (! isTopLevelStatePopulated (performingTopLevelState))
            return nullptr;

        return &*topLevelStates[static_cast<size_t> (performingTopLevelState)];
    }

    int getViewedTrackCount() const
    {
        if (const auto* viewedTracks = getViewedTracks())
            return static_cast<int> (viewedTracks->size());

        return 1;
    }

    int getPerformingTrackCount() const
    {
        if (const auto* performingTracks = getPerformingTracks())
            return static_cast<int> (performingTracks->size());

        return 1;
    }

    float getCurrentMasterGain() const
    {
        return running ? static_cast<float> (gainSlider.getValue()) : 0.0f;
    }

    float getCurrentTempoHz() const
    {
        if (getPerformingTracks() == nullptr)
            return 1.0f;

        return static_cast<float> ((getPerformingTempoBpm() / 60.0) * defaultPlaybackRate);
    }

    void loadSelectedContentForCurrentState()
    {
        const auto* performingTracks = getPerformingTracks();
        if (performingTracks == nullptr || performingTracks->empty())
            return;

        performingTrackIndex = juce::jlimit (0, static_cast<int> (performingTracks->size()) - 1, performingTrackIndex);
        static_cast<void> (audioCallback.loadStateWithControls ((*performingTracks)[static_cast<size_t> (performingTrackIndex)],
                                                                getCurrentMasterGain(),
                                                                getCurrentTempoHz(),
                                                                defaultIntensity,
                                                                defaultBrightness,
                                                                orbitPhase));
    }

    void applyCurrentAudioControls()
    {
        if (getPerformingTracks() == nullptr)
            return;

        audioCallback.setControls (getCurrentMasterGain(),
                                   getCurrentTempoHz(),
                                   defaultIntensity,
                                   defaultBrightness,
                                   orbitPhase);
    }

    WfAudioCallback audioCallback;
    juce::AudioDeviceManager audioDeviceManager;
    juce::AudioPluginFormatManager pluginFormatManager;
    std::unique_ptr<juce::FileChooser> pluginChooser;
    std::array<std::optional<std::vector<Wf::StateSpec>>, maxTopLevelStates> topLevelStates;
    std::mt19937 random { 0x5eed1234u };

    juce::Label titleLabel;
    juce::Label statusLabel;
    juce::Label volumeLabel;
    juce::Label stateSettingsLabel;
    juce::Label stateTempoLabel;
    juce::Label stateTimeSigLabel;
    juce::Label stateTrackCountLabel;
    juce::Label stateCodeHeader;
    juce::Label laneCodeHeader;
    juce::Label trackNameLabel;
    juce::Label trackDurationLabel;
    juce::Label selectedLabel;
    juce::Label laneHeader;
    std::array<juce::TextButton, maxTrackLanes> laneButtons;

    OrbitCanvas orbitCanvas;
    MixerCanvas mixerCanvas;
    juce::Viewport mixerViewport;
    std::array<juce::TextButton, maxTopLevelStates> stateButtons;
    juce::TextButton runScriptButton;
    juce::TextButton newStateButton;
    juce::TextButton duplicateStateButton;
    juce::TextButton deleteStateButton;
    juce::TextButton arrangementButton;
    juce::TextButton codeViewButton;
    juce::TextButton mixerViewButton;
    juce::TextButton muteLaneButton;
    juce::TextButton soloLaneButton;
    juce::TextButton duplicateLaneButton;
    juce::TextButton deleteLaneButton;
    juce::TextButton laneCodeRunButton;
    juce::Slider gainSlider;
    juce::Slider stateTempoSlider;
    juce::ComboBox timeSigNumeratorBox;
    juce::ComboBox timeSigDenominatorBox;
    juce::TextEditor globalScriptEditor;
    juce::TextEditor laneCodeEditor;
    juce::TextEditor trackNameEditor;
    juce::TextEditor trackDurationEditor;
    juce::TextEditor laneNameEditor;
    juce::TextEditor stateTrackCountEditor;
    std::array<float, maxTopLevelStates> topLevelTemposBpm
    {
        88.0f, 44.0f, 88.0f, 88.0f,
        88.0f, 88.0f, 88.0f, 88.0f,
        88.0f, 88.0f, 88.0f, 88.0f,
        88.0f, 88.0f, 88.0f, 88.0f
    };
    std::array<int, maxTopLevelStates> topLevelTimeSigNumerators
    {
        4, 4, 4, 4,
        4, 4, 4, 4,
        4, 4, 4, 4,
        4, 4, 4, 4
    };
    std::array<int, maxTopLevelStates> topLevelTimeSigDenominators
    {
        4, 4, 4, 4,
        4, 4, 4, 4,
        4, 4, 4, 4,
        4, 4, 4, 4
    };

    int viewedTopLevelState = 0;
    int performingTopLevelState = 0;
    int selectedState = 0;
    int performingTrackIndex = 0;
    int selectedLane = 0;
    std::vector<GlobalScriptStep> globalScriptSteps;
    size_t scriptStepIndex = 0;
    double scriptStepElapsedBars = 0.0;
    bool scriptRunning = false;
    bool scriptShouldStopAtEnd = false;
    bool suppressStateControlCallbacks = false;
    bool suppressEditCallbacks = false;
    bool suppressLaneCodeCallbacks = false;
    bool laneCodeDirty = false;
    MainView mainView = MainView::arrangement;
    juce::String laneCodeLastValidatedText;
    int laneCodeViewedTopLevelState = -1;
    int laneCodeTrackIndex = -1;
    int laneCodeLaneIndex = -1;
    float orbitPhase = 0.0f;
    double trackElapsedBars = 0.0;
    double nextBarTransitionCheck = 1.0;
    bool running = false;
    double lastTimerMs = 0.0;
};

class WfApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "ChucK-ME"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise (const juce::String&) override
    {
        mainWindow = std::make_unique<MainWindow> (getApplicationName());
    }

    void shutdown() override
    {
        mainWindow = nullptr;
    }

    void systemRequestedQuit() override
    {
        quit();
    }

private:
    class MainWindow final : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (juce::String name)
            : DocumentWindow (std::move (name), juce::Colour (0xff0c100e), DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent(), true);
            centreWithSize (getWidth(), getHeight());
            setResizable (true, true);
            setVisible (true);
        }

        void closeButtonPressed() override
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }
    };

    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION (WfApplication)
