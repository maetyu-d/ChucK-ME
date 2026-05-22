#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <WeldChucKEngine.h>

#include "WfChucKPrograms.h"

#include <atomic>
#include <array>
#include <cmath>
#include <limits>
#include <random>
#include <regex>
#include <string>

namespace
{
constexpr int maxHostChannels = 16;
constexpr int fallbackBlockSize = 4096;
constexpr int engineOutputChannels = 2;
constexpr int maxTopLevelStates = 16;
constexpr int populatedTopLevelStates = 2;

juce::Colour ink() { return juce::Colour (0xffeef3ee); }
juce::Colour mutedInk() { return juce::Colour (0xff9da8a2); }
juce::Colour panel() { return juce::Colour (0xff161b19); }
juce::Colour panelSoft() { return juce::Colour (0xff202622); }
juce::Colour green() { return juce::Colour (0xff7bd88f); }
juce::Colour amber() { return juce::Colour (0xffd7a84f); }
juce::Colour coral() { return juce::Colour (0xffdd7c6f); }
juce::Colour blue() { return juce::Colour (0xff72a7d9); }

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
    button.setColour (juce::TextButton::buttonColourId, panelSoft());
    button.setColour (juce::TextButton::buttonOnColourId, colour.withAlpha (0.22f));
    button.setColour (juce::TextButton::textColourOffId, ink());
    button.setColour (juce::TextButton::textColourOnId, ink());
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
    slider.setColour (juce::Slider::backgroundColourId, panelSoft());
    slider.setColour (juce::Slider::textBoxTextColourId, ink());
    slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
}

void styleComboBox (juce::ComboBox& comboBox)
{
    comboBox.setColour (juce::ComboBox::backgroundColourId, panelSoft());
    comboBox.setColour (juce::ComboBox::textColourId, ink());
    comboBox.setColour (juce::ComboBox::outlineColourId, mutedInk().withAlpha (0.26f));
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

        if (! incoming.engine.loadProgram (Wf::buildStateProgram (state), Wf::makeWfParameterBindings()))
            return false;

        refreshWfParameterIndexes (incoming);
        if (! incoming.indexesReady)
            return false;

        applyControlsToSlot (incoming,
                             masterGain,
                             tempoHz,
                             intensity,
                             brightness,
                             orbitPhase);

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

    juce::AudioBuffer<float> emptyInput;
    std::array<EngineSlot, 2> slots;
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
    std::function<void (int)> onStateSelected;

    void setState (const std::vector<Wf::StateSpec>* statesToUse, int selectedIndexToUse, float phaseToUse, bool runningToUse)
    {
        states = statesToUse;
        selectedIndex = selectedIndexToUse;
        phase = phaseToUse;
        running = runningToUse;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff101412));

        auto area = getLocalBounds().toFloat().reduced (24.0f);
        const auto radius = juce::jmin (area.getWidth(), area.getHeight()) * 0.36f;
        const auto centre = area.getCentre();

        g.setColour (panelSoft());
        g.fillEllipse (centre.x - radius, centre.y - radius, radius * 2.0f, radius * 2.0f);

        g.setColour (mutedInk().withAlpha (0.26f));
        g.drawEllipse (centre.x - radius, centre.y - radius, radius * 2.0f, radius * 2.0f, 1.2f);

        if (states == nullptr || states->empty())
            return;

        const auto count = static_cast<int> (states->size());
        nodeCentres.clear();
        nodeCentres.reserve (static_cast<size_t> (count));

        for (int i = 0; i < count; ++i)
        {
            const auto angle = juce::MathConstants<float>::twoPi * (static_cast<float> (i) / static_cast<float> (count)) - juce::MathConstants<float>::halfPi;
            const juce::Point<float> point { centre.x + std::cos (angle) * radius,
                                             centre.y + std::sin (angle) * radius };
            nodeCentres.push_back (point);
        }

        for (int i = 0; i < count; ++i)
        {
            const auto next = (i + 1) % count;
            g.setColour (mutedInk().withAlpha (0.20f));
            g.drawLine ({ nodeCentres[static_cast<size_t> (i)], nodeCentres[static_cast<size_t> (next)] }, 1.4f);
        }

        for (int i = 0; i < count; ++i)
        {
            const auto selected = i == selectedIndex;
            const auto point = nodeCentres[static_cast<size_t> (i)];
            const auto colour = selected ? green() : blue().withAlpha (0.72f);
            const auto size = selected ? 62.0f : 48.0f;

            g.setColour (colour.withAlpha (selected ? 0.28f : 0.16f));
            g.fillEllipse (point.x - size * 0.5f, point.y - size * 0.5f, size, size);
            g.setColour (colour);
            g.drawEllipse (point.x - size * 0.5f, point.y - size * 0.5f, size, size, selected ? 2.0f : 1.2f);

            g.setColour (ink());
            g.setFont (selected ? 15.0f : 13.0f);
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
            g.fillEllipse (marker.x - 5.0f, marker.y - 5.0f, 10.0f, 10.0f);
        }
    }

    void mouseDown (const juce::MouseEvent& event) override
    {
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
    const std::vector<Wf::StateSpec>* states = nullptr;
    std::vector<juce::Point<float>> nodeCentres;
    int selectedIndex = 0;
    float phase = 0.0f;
    bool running = false;
};

class MainComponent final : public juce::Component,
                            private juce::Timer
{
public:
    MainComponent()
    {
        states = Wf::makeDefaultStates();

        titleLabel.setText ("wf:: weld", juce::dontSendNotification);
        titleLabel.setFont (juce::FontOptions (24.0f, juce::Font::bold));
        styleLabel (titleLabel);
        addAndMakeVisible (titleLabel);

        statusLabel.setText ("embedded ChucK ready", juce::dontSendNotification);
        styleLabel (statusLabel, 0.72f);
        addAndMakeVisible (statusLabel);

        for (int i = 0; i < maxTopLevelStates; ++i)
        {
            auto& button = stateButtons[static_cast<size_t> (i)];
            setupButton (button,
                         "State " + juce::String (i + 1),
                         i == 0 ? green() : blue(),
                         [this, i] { selectViewedTopLevelState (i); });

            if (! isTopLevelStatePopulated (i))
            {
                styleEmptyStateButton (button);
                button.setEnabled (false);
            }
        }

        setupButton (runScriptButton, "Run", amber(), [this] { runGlobalScript(); });

        globalScriptEditor.setMultiLine (true);
        globalScriptEditor.setReturnKeyStartsNewLine (true);
        globalScriptEditor.setText ("playState(1, 8);\nplayState(2, 4);\nstop();", juce::dontSendNotification);
        globalScriptEditor.setColour (juce::TextEditor::backgroundColourId, panelSoft());
        globalScriptEditor.setColour (juce::TextEditor::textColourId, ink());
        globalScriptEditor.setColour (juce::TextEditor::outlineColourId, mutedInk().withAlpha (0.24f));
        globalScriptEditor.setColour (juce::TextEditor::focusedOutlineColourId, amber().withAlpha (0.72f));
        globalScriptEditor.setColour (juce::TextEditor::highlightColourId, amber().withAlpha (0.24f));
        globalScriptEditor.setFont (juce::FontOptions (14.0f));
        addAndMakeVisible (globalScriptEditor);

        laneCodeHeader.setText ("lane code", juce::dontSendNotification);
        laneCodeHeader.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        styleLabel (laneCodeHeader, 0.82f);
        addAndMakeVisible (laneCodeHeader);

        laneCodeEditor.setMultiLine (true);
        laneCodeEditor.setReturnKeyStartsNewLine (true);
        laneCodeEditor.setReadOnly (true);
        laneCodeEditor.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff101412));
        laneCodeEditor.setColour (juce::TextEditor::textColourId, ink());
        laneCodeEditor.setColour (juce::TextEditor::outlineColourId, mutedInk().withAlpha (0.22f));
        laneCodeEditor.setColour (juce::TextEditor::focusedOutlineColourId, mutedInk().withAlpha (0.34f));
        laneCodeEditor.setColour (juce::TextEditor::highlightColourId, blue().withAlpha (0.24f));
        laneCodeEditor.setFont (juce::FontOptions (13.0f));
        addAndMakeVisible (laneCodeEditor);

        selectedLabel.setFont (juce::FontOptions (18.0f, juce::Font::bold));
        styleLabel (selectedLabel);
        addAndMakeVisible (selectedLabel);

        stateSettingsLabel.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        styleLabel (stateSettingsLabel, 0.82f);
        addAndMakeVisible (stateSettingsLabel);

        styleLabel (stateTempoLabel, 0.76f);
        addAndMakeVisible (stateTempoLabel);

        setupSlider (stateTempoSlider, "State tempo", 0.25, 2.0, 1.0, amber());
        stateTempoSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 62, 22);
        stateTempoSlider.onValueChange = [this]
        {
            if (suppressStateControlCallbacks)
                return;

            topLevelTempoScales[static_cast<size_t> (viewedTopLevelState)] = static_cast<float> (stateTempoSlider.getValue());
            refreshLabels();

            if (viewedTopLevelState == performingTopLevelState)
                applyCurrentAudioControls();
        };

        styleLabel (stateTimeSigLabel, 0.76f);
        addAndMakeVisible (stateTimeSigLabel);

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
        addAndMakeVisible (orbitCanvas);

        setupButton (playButton, "Stop", green(), [this] { toggleRunning(); });
        setupButton (previousButton, "Prev", blue(), [this] { selectState (selectedState - 1); });
        setupButton (nextButton, "Next", blue(), [this] { selectState (selectedState + 1); });
        setupButton (shuffleButton, "Pick", amber(), [this] { pickState(); });

        setupSlider (gainSlider, "Gain", 0.0, 0.8, 0.18, green());
        setupSlider (rateSlider, "Rate", 0.35, 2.25, 1.0, amber());
        setupSlider (intensitySlider, "Density", 0.0, 1.0, 0.58, coral());
        setupSlider (brightnessSlider, "Colour", 0.0, 1.0, 0.48, blue());

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
        setSize (1240, 760);
        startTimerHz (30);
    }

    ~MainComponent() override
    {
        audioDeviceManager.removeAudioCallback (&audioCallback);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff0c100e));

        auto area = getLocalBounds().reduced (16);
        g.setColour (panel());
        g.fillRoundedRectangle (area.toFloat(), 8.0f);

        g.setColour (mutedInk().withAlpha (0.18f));
        g.drawRoundedRectangle (area.toFloat(), 8.0f, 1.0f);

        auto content = area.reduced (18);
        auto top = content.removeFromTop (48);
        auto stateRow = content.removeFromTop (60);
        auto scriptRow = content.removeFromTop (70);
        g.setColour (mutedInk().withAlpha (0.18f));
        g.drawLine (static_cast<float> (content.getX()),
                    static_cast<float> (top.getBottom()),
                    static_cast<float> (content.getRight()),
                    static_cast<float> (top.getBottom()),
                    1.0f);
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

    void resized() override
    {
        auto area = getLocalBounds().reduced (34);
        auto header = area.removeFromTop (44);
        titleLabel.setBounds (header.removeFromLeft (260));
        statusLabel.setBounds (header);

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
        auto controls = area.removeFromBottom (118);
        auto codePane = area.removeFromLeft (280);
        area.removeFromLeft (18);
        auto right = area.removeFromRight (260);
        area.removeFromRight (18);

        laneCodeHeader.setBounds (codePane.removeFromTop (24));
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
        right.removeFromTop (10);
        selectedLabel.setBounds (right.removeFromTop (34));
        laneHeader.setBounds (right.removeFromTop (28));

        for (auto& button : laneButtons)
            button.setBounds (right.removeFromTop (34).reduced (0, 3));

        controls.removeFromTop (8);
        auto transport = controls.removeFromTop (42);
        playButton.setBounds (transport.removeFromLeft (92).reduced (0, 4));
        previousButton.setBounds (transport.removeFromLeft (82).reduced (6, 4));
        nextButton.setBounds (transport.removeFromLeft (82).reduced (6, 4));
        shuffleButton.setBounds (transport.removeFromLeft (82).reduced (6, 4));

        auto sliderRow = controls.removeFromTop (62);
        auto left = sliderRow.removeFromLeft (sliderRow.getWidth() / 2).reduced (0, 2);
        auto rightSliders = sliderRow.reduced (12, 2);
        gainSlider.setBounds (left.removeFromTop (30));
        rateSlider.setBounds (left.removeFromTop (30));
        intensitySlider.setBounds (rightSliders.removeFromTop (30));
        brightnessSlider.setBounds (rightSliders.removeFromTop (30));
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

    void selectViewedTopLevelState (int index)
    {
        const auto nextState = juce::jlimit (0, maxTopLevelStates - 1, index);
        if (! isTopLevelStatePopulated (nextState))
            return;

        if (viewedTopLevelState == nextState)
        {
            syncViewedStateControls();
            refreshLabels();
            return;
        }

        viewedTopLevelState = nextState;
        syncViewedStateControls();
        refreshLabels();
    }

    void setPerformingTopLevelState (int index)
    {
        const auto nextState = juce::jlimit (0, maxTopLevelStates - 1, index);
        if (! isTopLevelStatePopulated (nextState))
            return;

        if (performingTopLevelState == nextState)
        {
            refreshLabels();
            return;
        }

        performingTopLevelState = nextState;
        loadSelectedContentForCurrentState();
        refreshLabels();
    }

    struct GlobalScriptStep
    {
        int stateIndex = 0;
        double bars = 1.0;
    };

    std::vector<GlobalScriptStep> parseGlobalScript() const
    {
        std::vector<GlobalScriptStep> parsed;
        const auto text = globalScriptEditor.getText().toStdString();
        const std::regex playStateRegex ("playState\\s*\\(\\s*(1[0-6]|[1-9])\\s*,\\s*(\\d+(?:\\.\\d+)?)\\s*\\)",
                                         std::regex_constants::icase);

        for (std::sregex_iterator it (text.begin(), text.end(), playStateRegex), end; it != end; ++it)
        {
            const auto stateIndex = std::stoi ((*it)[1].str()) - 1;
            const auto bars = std::stod ((*it)[2].str());

            if (isTopLevelStatePopulated (stateIndex) && bars > 0.0)
                parsed.push_back ({ stateIndex, bars });
        }

        if (! parsed.empty())
            return parsed;

        const std::regex tokenRegex ("state\\s*\\(\\s*(1[0-6]|[1-9])\\s*\\)|(\\d+(?:\\.\\d+)?)\\s*::\\s*bar",
                                     std::regex_constants::icase);
        int pendingStateIndex = -1;

        for (std::sregex_iterator it (text.begin(), text.end(), tokenRegex), end; it != end; ++it)
        {
            if ((*it)[1].matched)
            {
                pendingStateIndex = std::stoi ((*it)[1].str()) - 1;
                continue;
            }

            if ((*it)[2].matched && pendingStateIndex >= 0)
            {
                const auto bars = std::stod ((*it)[2].str());

                if (isTopLevelStatePopulated (pendingStateIndex) && bars > 0.0)
                    parsed.push_back ({ pendingStateIndex, bars });

                pendingStateIndex = -1;
            }
        }

        return parsed;
    }

    void runGlobalScript()
    {
        globalScriptSteps = parseGlobalScript();

        if (globalScriptSteps.empty())
            return;

        scriptStepIndex = 0;
        scriptStepElapsedBars = 0.0;
        scriptShouldStopAtEnd = globalScriptEditor.getText().containsIgnoreCase ("stop");
        scriptRunning = true;
        running = true;
        playButton.setButtonText ("Stop");
        setPerformingTopLevelState (globalScriptSteps.front().stateIndex);
    }

    void selectState (int index)
    {
        if (states.empty())
            return;

        selectedState = (index + static_cast<int> (states.size())) % static_cast<int> (states.size());
        orbitPhase = 0.0f;
        loadSelectedContentForCurrentState();
        refreshLabels();
    }

    void toggleRunning()
    {
        running = ! running;
        playButton.setButtonText (running ? "Stop" : "Play");
        applyCurrentAudioControls();
        refreshLabels();
    }

    void pickState()
    {
        if (states.empty())
            return;

        std::uniform_int_distribution<int> distribution (0, static_cast<int> (states.size()) - 1);
        selectState (distribution (random));
    }

    void selectLane (int index)
    {
        selectedLane = juce::jlimit (0, static_cast<int> (laneButtons.size()) - 1, index);
        refreshLabels();
    }

    void refreshLabels()
    {
        syncViewedStateControls();

        for (int i = 0; i < maxTopLevelStates; ++i)
        {
            auto& button = stateButtons[static_cast<size_t> (i)];
            button.setEnabled (isTopLevelStatePopulated (i));
            button.setToggleState (viewedTopLevelState == i, juce::dontSendNotification);
        }

        const auto& state = states[static_cast<size_t> (selectedState)];
        selectedLabel.setText (state.name + "  " + juce::String (state.tempoBpm, 1) + " bpm", juce::dontSendNotification);

        for (int i = 0; i < static_cast<int> (laneButtons.size()); ++i)
        {
            const auto& lane = state.lanes[static_cast<size_t> (i)];
            auto& button = laneButtons[static_cast<size_t> (i)];
            button.setButtonText (juce::String (i + 1) + "  " + lane.name + " / " + lane.role);
            button.setToggleState (selectedLane == i, juce::dontSendNotification);
        }

        updateLaneCode();
        orbitCanvas.setState (&states, selectedState, orbitPhase, running);
    }

    void updateLaneCode()
    {
        if (states.empty())
            return;

        const auto& state = states[static_cast<size_t> (selectedState)];
        const auto laneIndex = static_cast<size_t> (juce::jlimit (0, static_cast<int> (state.lanes.size()) - 1, selectedLane));
        laneCodeEditor.setText (makeLaneCode (state.lanes[laneIndex]), juce::dontSendNotification);
    }

    static juce::String makeLaneCode (const Wf::LaneSpec& lane)
    {
        juce::String code;
        code << "// " << lane.name << "\n";
        code << "// role: " << lane.role << "\n";
        code << "// baseHz: " << Wf::chuckFloat (lane.baseHz)
             << "  volume: " << Wf::chuckFloat (lane.volume)
             << "  pulseTicks: " << lane.pulseTicks
             << "  openTicks: " << lane.openTicks << "\n\n";
        code << "// generated lane declaration\n";
        Wf::appendLaneDeclaration (code, lane, 0);
        code << "// generated lane control fragment\n";
        code << "// expects host variables: tick, stepPhase, intensity, bright, orbit\n";
        Wf::appendLaneControl (code, lane, 0);
        return code;
    }

    void syncViewedStateControls()
    {
        juce::ScopedValueSetter<bool> guard (suppressStateControlCallbacks, true);
        const auto index = static_cast<size_t> (juce::jlimit (0, maxTopLevelStates - 1, viewedTopLevelState));
        const auto tempoScale = topLevelTempoScales[index];
        const auto numerator = topLevelTimeSigNumerators[index];
        const auto denominator = topLevelTimeSigDenominators[index];

        stateSettingsLabel.setText ("State " + juce::String (viewedTopLevelState + 1) + " settings", juce::dontSendNotification);
        stateTempoLabel.setText ("Tempo  x" + juce::String (tempoScale, 2), juce::dontSendNotification);
        stateTimeSigLabel.setText ("Time signature  " + juce::String (numerator) + "/" + juce::String (denominator), juce::dontSendNotification);
        stateTempoSlider.setValue (tempoScale, juce::dontSendNotification);
        timeSigNumeratorBox.setSelectedId (numerator, juce::dontSendNotification);
        timeSigDenominatorBox.setSelectedId (denominator, juce::dontSendNotification);
    }

    void timerCallback() override
    {
        const auto now = juce::Time::getMillisecondCounterHiRes();
        const auto deltaSeconds = lastTimerMs > 0.0 ? juce::jlimit (0.0, 0.1, (now - lastTimerMs) * 0.001) : 0.0;
        lastTimerMs = now;

        const auto& state = states[static_cast<size_t> (selectedState)];
        const auto rate = static_cast<float> (rateSlider.getValue());
        const auto tempoScale = getTopLevelTempoScale();

        if (running)
        {
            orbitPhase += static_cast<float> (deltaSeconds * (state.tempoBpm / 60.0) * 0.25 * static_cast<double> (rate) * static_cast<double> (tempoScale));
            advanceGlobalScript (deltaSeconds, state.tempoBpm, rate, tempoScale);

            if (orbitPhase >= 1.0f)
            {
                orbitPhase -= std::floor (orbitPhase);
                selectState (selectedState + 1);
            }
        }

        applyCurrentAudioControls();

        statusLabel.setText (juce::String (running ? "running  " : "stopped  ") + audioCallback.diagnostics(), juce::dontSendNotification);
        orbitCanvas.setState (&states, selectedState, orbitPhase, running);
    }

    void advanceGlobalScript (double deltaSeconds, double tempoBpm, float rate, float tempoScale)
    {
        if (! scriptRunning || scriptStepIndex >= globalScriptSteps.size())
            return;

        scriptStepElapsedBars += deltaSeconds * (tempoBpm / 60.0) * static_cast<double> (rate) * static_cast<double> (tempoScale) / getPerformingQuarterNotesPerBar();

        if (scriptStepElapsedBars < globalScriptSteps[scriptStepIndex].bars)
            return;

        ++scriptStepIndex;
        scriptStepElapsedBars = 0.0;

        if (scriptStepIndex >= globalScriptSteps.size())
        {
            scriptRunning = false;

            if (scriptShouldStopAtEnd)
            {
                running = false;
                playButton.setButtonText ("Play");
                applyCurrentAudioControls();
                refreshLabels();
            }

            return;
        }

        setPerformingTopLevelState (globalScriptSteps[scriptStepIndex].stateIndex);
    }

    float getTopLevelTempoScale() const
    {
        return topLevelTempoScales[static_cast<size_t> (juce::jlimit (0, maxTopLevelStates - 1, performingTopLevelState))];
    }

    double getPerformingQuarterNotesPerBar() const
    {
        const auto index = static_cast<size_t> (juce::jlimit (0, maxTopLevelStates - 1, performingTopLevelState));
        const auto numerator = static_cast<double> (topLevelTimeSigNumerators[index]);
        const auto denominator = static_cast<double> (juce::jmax (1, topLevelTimeSigDenominators[index]));
        return numerator * 4.0 / denominator;
    }

    static bool isTopLevelStatePopulated (int index)
    {
        return index >= 0 && index < populatedTopLevelStates;
    }

    float getCurrentMasterGain() const
    {
        return running ? static_cast<float> (gainSlider.getValue()) : 0.0f;
    }

    float getCurrentTempoHz() const
    {
        if (states.empty())
            return 1.0f;

        const auto& state = states[static_cast<size_t> (selectedState)];
        return static_cast<float> ((state.tempoBpm / 60.0)
                                   * rateSlider.getValue()
                                   * static_cast<double> (getTopLevelTempoScale()));
    }

    void loadSelectedContentForCurrentState()
    {
        if (states.empty())
            return;

        static_cast<void> (audioCallback.loadStateWithControls (states[static_cast<size_t> (selectedState)],
                                                                getCurrentMasterGain(),
                                                                getCurrentTempoHz(),
                                                                static_cast<float> (intensitySlider.getValue()),
                                                                static_cast<float> (brightnessSlider.getValue()),
                                                                orbitPhase));
    }

    void applyCurrentAudioControls()
    {
        if (states.empty())
            return;

        audioCallback.setControls (getCurrentMasterGain(),
                                   getCurrentTempoHz(),
                                   static_cast<float> (intensitySlider.getValue()),
                                   static_cast<float> (brightnessSlider.getValue()),
                                   orbitPhase);
    }

    WfAudioCallback audioCallback;
    juce::AudioDeviceManager audioDeviceManager;
    std::vector<Wf::StateSpec> states;
    std::mt19937 random { 0x5eed1234u };

    juce::Label titleLabel;
    juce::Label statusLabel;
    juce::Label stateSettingsLabel;
    juce::Label stateTempoLabel;
    juce::Label stateTimeSigLabel;
    juce::Label laneCodeHeader;
    juce::Label selectedLabel;
    juce::Label laneHeader;
    std::array<juce::TextButton, 5> laneButtons;

    OrbitCanvas orbitCanvas;
    std::array<juce::TextButton, maxTopLevelStates> stateButtons;
    juce::TextButton runScriptButton;
    juce::TextButton playButton;
    juce::TextButton previousButton;
    juce::TextButton nextButton;
    juce::TextButton shuffleButton;
    juce::Slider gainSlider;
    juce::Slider rateSlider;
    juce::Slider intensitySlider;
    juce::Slider brightnessSlider;
    juce::Slider stateTempoSlider;
    juce::ComboBox timeSigNumeratorBox;
    juce::ComboBox timeSigDenominatorBox;
    juce::TextEditor globalScriptEditor;
    juce::TextEditor laneCodeEditor;
    std::array<float, maxTopLevelStates> topLevelTempoScales
    {
        1.0f, 0.5f, 1.0f, 1.0f,
        1.0f, 1.0f, 1.0f, 1.0f,
        1.0f, 1.0f, 1.0f, 1.0f,
        1.0f, 1.0f, 1.0f, 1.0f
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
    int selectedLane = 0;
    std::vector<GlobalScriptStep> globalScriptSteps;
    size_t scriptStepIndex = 0;
    double scriptStepElapsedBars = 0.0;
    bool scriptRunning = false;
    bool scriptShouldStopAtEnd = false;
    bool suppressStateControlCallbacks = false;
    float orbitPhase = 0.0f;
    bool running = true;
    double lastTimerMs = 0.0;
};

class WfApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "wf:: weld"; }
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
