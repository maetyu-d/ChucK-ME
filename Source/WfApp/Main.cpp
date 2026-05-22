#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <WeldChucKEngine.h>

#include "WfChucKPrograms.h"

#include <atomic>
#include <cmath>
#include <limits>
#include <random>

namespace
{
constexpr int maxHostChannels = 16;
constexpr int fallbackBlockSize = 4096;
constexpr int engineOutputChannels = 2;

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
        engine.release();
        scratchOutput.setSize (0, 0);
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
            || numSamples > scratchOutput.getNumSamples()
            || numOutputChannels <= 0
            || numOutputChannels > scratchOutput.getNumChannels())
            return;

        juce::ScopedNoDenormals noDenormals;
        scratchOutput.clear (0, numSamples);

        juce::AudioBuffer<float> outputView (scratchOutput.getArrayOfWritePointers(),
                                             numOutputChannels,
                                             numSamples);
        engine.process (emptyInput, outputView);

        for (int channel = 0; channel < numOutputChannels; ++channel)
            if (outputChannelData[channel] != nullptr)
                juce::FloatVectorOperations::copy (outputChannelData[channel],
                                                   scratchOutput.getReadPointer (channel),
                                                   numSamples);
    }

    bool loadState (const Wf::StateSpec& state)
    {
        if (! engine.isReady())
            return false;

        return engine.loadProgramAsync (Wf::buildStateProgram (state), Wf::makeWfParameterBindings());
    }

    void setControls (float masterGain, float tempoHz, float intensity, float brightness, float orbitPhase)
    {
        static_cast<void> (engine.setParameterValue ("hostMasterGain", masterGain));
        static_cast<void> (engine.setParameterValue ("hostTempoHz", tempoHz));
        static_cast<void> (engine.setParameterValue ("hostIntensity", intensity));
        static_cast<void> (engine.setParameterValue ("hostBrightness", brightness));
        static_cast<void> (engine.setParameterValue ("hostOrbitPhase", orbitPhase));
    }

    juce::String diagnostics() const
    {
        return "blocks=" + juce::String (static_cast<juce::int64> (engine.getRenderedBlockCount()))
             + " async=" + juce::String (static_cast<juce::int64> (engine.getAsyncProgramLoadCompletedCount()))
             + " silent=" + juce::String (static_cast<juce::int64> (engine.getSilentProcessCount()));
    }

private:
    bool prepare (double sampleRate, int reportedBlockSize)
    {
        ready.store (false, std::memory_order_release);
        engine.release();

        const auto blockSize = juce::jlimit (64,
                                             EmbeddedChucKEngine::maximumBlockSizeLimit,
                                             juce::jmax (reportedBlockSize, fallbackBlockSize));

        scratchOutput.setSize (maxHostChannels, blockSize, false, false, true);
        scratchOutput.clear();

        const auto prepared = engine.prepare (sampleRate, blockSize, 0, engineOutputChannels);
        ready.store (prepared, std::memory_order_release);
        return prepared;
    }

    EmbeddedChucKEngine engine;
    juce::AudioBuffer<float> emptyInput;
    juce::AudioBuffer<float> scratchOutput;
    std::atomic<bool> ready { false };
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

        selectedLabel.setFont (juce::FontOptions (18.0f, juce::Font::bold));
        styleLabel (selectedLabel);
        addAndMakeVisible (selectedLabel);

        orbitCanvas.onStateSelected = [this] (int index) { selectState (index); };
        addAndMakeVisible (orbitCanvas);

        setupButton (playButton, "Pause", green(), [this] { toggleRunning(); });
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

        for (auto& label : laneLabels)
        {
            addAndMakeVisible (label);
            label.setFont (juce::FontOptions (14.0f));
            styleLabel (label);
        }

        audioDeviceManager.initialiseWithDefaultDevices (0, 2);
        audioDeviceManager.addAudioCallback (&audioCallback);

        selectState (0);
        setSize (980, 640);
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
        g.setColour (mutedInk().withAlpha (0.18f));
        g.drawLine (static_cast<float> (content.getX()),
                    static_cast<float> (top.getBottom()),
                    static_cast<float> (content.getRight()),
                    static_cast<float> (top.getBottom()),
                    1.0f);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (34);
        auto header = area.removeFromTop (44);
        titleLabel.setBounds (header.removeFromLeft (260));
        statusLabel.setBounds (header);

        area.removeFromTop (18);
        auto controls = area.removeFromBottom (118);
        auto right = area.removeFromRight (260);
        area.removeFromRight (18);

        orbitCanvas.setBounds (area);
        selectedLabel.setBounds (right.removeFromTop (34));
        laneHeader.setBounds (right.removeFromTop (28));

        for (auto& label : laneLabels)
            label.setBounds (right.removeFromTop (34).reduced (0, 3));

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

    void selectState (int index)
    {
        if (states.empty())
            return;

        selectedState = (index + static_cast<int> (states.size())) % static_cast<int> (states.size());
        orbitPhase = 0.0f;
        audioCallback.loadState (states[static_cast<size_t> (selectedState)]);
        refreshLabels();
    }

    void toggleRunning()
    {
        running = ! running;
        playButton.setButtonText (running ? "Pause" : "Play");
        refreshLabels();
    }

    void pickState()
    {
        if (states.empty())
            return;

        std::uniform_int_distribution<int> distribution (0, static_cast<int> (states.size()) - 1);
        selectState (distribution (random));
    }

    void refreshLabels()
    {
        const auto& state = states[static_cast<size_t> (selectedState)];
        selectedLabel.setText (state.name + "  " + juce::String (state.tempoBpm, 1) + " bpm", juce::dontSendNotification);

        for (int i = 0; i < static_cast<int> (laneLabels.size()); ++i)
        {
            const auto& lane = state.lanes[static_cast<size_t> (i)];
            laneLabels[static_cast<size_t> (i)].setText (juce::String (i + 1) + "  " + lane.name + " / " + lane.role,
                                                         juce::dontSendNotification);
        }

        orbitCanvas.setState (&states, selectedState, orbitPhase, running);
    }

    void timerCallback() override
    {
        const auto now = juce::Time::getMillisecondCounterHiRes();
        const auto deltaSeconds = lastTimerMs > 0.0 ? juce::jlimit (0.0, 0.1, (now - lastTimerMs) * 0.001) : 0.0;
        lastTimerMs = now;

        const auto& state = states[static_cast<size_t> (selectedState)];
        const auto rate = static_cast<float> (rateSlider.getValue());

        if (running)
        {
            orbitPhase += static_cast<float> (deltaSeconds * (state.tempoBpm / 60.0) * 0.25 * static_cast<double> (rate));

            if (orbitPhase >= 1.0f)
            {
                orbitPhase -= std::floor (orbitPhase);
                selectState (selectedState + 1);
            }
        }

        audioCallback.setControls (static_cast<float> (gainSlider.getValue()),
                                   static_cast<float> ((state.tempoBpm / 60.0) * static_cast<double> (rate)),
                                   static_cast<float> (intensitySlider.getValue()),
                                   static_cast<float> (brightnessSlider.getValue()),
                                   orbitPhase);

        statusLabel.setText (juce::String (running ? "running  " : "paused  ") + audioCallback.diagnostics(), juce::dontSendNotification);
        orbitCanvas.setState (&states, selectedState, orbitPhase, running);
    }

    WfAudioCallback audioCallback;
    juce::AudioDeviceManager audioDeviceManager;
    std::vector<Wf::StateSpec> states;
    std::mt19937 random { 0x5eed1234u };

    juce::Label titleLabel;
    juce::Label statusLabel;
    juce::Label selectedLabel;
    juce::Label laneHeader;
    std::array<juce::Label, 5> laneLabels;

    OrbitCanvas orbitCanvas;
    juce::TextButton playButton;
    juce::TextButton previousButton;
    juce::TextButton nextButton;
    juce::TextButton shuffleButton;
    juce::Slider gainSlider;
    juce::Slider rateSlider;
    juce::Slider intensitySlider;
    juce::Slider brightnessSlider;

    int selectedState = 0;
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
