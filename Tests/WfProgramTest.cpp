#include <WeldChucKEngine.h>

#include "WfChucKPrograms.h"

#include <array>
#include <cmath>
#include <iostream>

namespace
{
double bufferEnergy (const juce::AudioBuffer<float>& buffer)
{
    double energy = 0.0;

    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            const auto value = buffer.getSample (channel, sample);

            if (! std::isfinite (value) || std::abs (value) > 0.981f)
                return -1.0;

            energy += static_cast<double> (value) * static_cast<double> (value);
        }

    return energy;
}

double renderEnergy (EmbeddedChucKEngine& engine,
                     juce::AudioBuffer<float>& input,
                     juce::AudioBuffer<float>& output,
                     int blocks)
{
    double energy = 0.0;

    for (int block = 0; block < blocks; ++block)
    {
        output.clear();
        engine.process (input, output);

        const auto blockEnergy = bufferEnergy (output);
        if (blockEnergy < 0.0)
            return -1.0;

        energy += blockEnergy;
    }

    return energy;
}
}

int main()
{
    juce::ScopedNoDenormals noDenormals;

    constexpr int blockSize = 256;
    auto states = Wf::makeDefaultStates();

    if (states.empty())
    {
        std::cerr << "no wf states\n";
        return 1;
    }

    EmbeddedChucKEngine engine;
    if (! engine.prepare (48000.0, blockSize, 0, 2))
    {
        std::cerr << engine.getLastError() << '\n';
        return 2;
    }

    juce::AudioBuffer<float> input (0, blockSize);
    juce::AudioBuffer<float> output (2, blockSize);
    const auto bindings = Wf::makeWfParameterBindings();

    for (size_t i = 0; i < states.size(); ++i)
    {
        if (! engine.loadProgram (Wf::buildStateProgram (states[i]), bindings))
        {
            std::cerr << "state program failed: " << states[i].name << " / " << engine.getLastError() << '\n';
            return 3;
        }

        static_cast<void> (engine.setParameterValue ("hostMasterGain", 0.2f));
        static_cast<void> (engine.setParameterValue ("hostTempoHz", static_cast<float> (states[i].tempoBpm / 60.0)));
        static_cast<void> (engine.setParameterValue ("hostIntensity", 0.66f));
        static_cast<void> (engine.setParameterValue ("hostBrightness", 0.54f));
        static_cast<void> (engine.setParameterValue ("hostOrbitPhase", static_cast<float> (i) / static_cast<float> (states.size())));

        const auto energy = renderEnergy (engine, input, output, 96);

        if (energy <= 0.0
            || engine.getRenderExceptionCount() != 0
            || engine.getInternalErrorCount() != 0)
        {
            std::cerr << "state render failed: " << states[i].name << '\n';
            return 4;
        }
    }

    auto oneLaneState = states.front();
    oneLaneState.name = "one lane edit check";
    oneLaneState.lanes.resize (1);

    if (! engine.loadProgram (Wf::buildStateProgram (oneLaneState), bindings))
    {
        std::cerr << "one-lane program failed: " << engine.getLastError() << '\n';
        return 5;
    }

    static_cast<void> (engine.setParameterValue ("hostMasterGain", 0.22f));
    static_cast<void> (engine.setParameterValue ("hostTempoHz", static_cast<float> (oneLaneState.tempoBpm / 60.0)));
    static_cast<void> (engine.setParameterValue ("hostIntensity", 0.72f));
    static_cast<void> (engine.setParameterValue ("hostBrightness", 0.52f));
    static_cast<void> (engine.setParameterValue ("hostOrbitPhase", 0.1f));

    const auto oneLaneEnergy = renderEnergy (engine, input, output, 128);
    if (oneLaneEnergy <= 0.0
        || engine.getRenderExceptionCount() != 0
        || engine.getInternalErrorCount() != 0)
    {
        std::cerr << "one-lane render failed: " << oneLaneEnergy << '\n';
        return 6;
    }

    auto eightLaneState = states.front();
    eightLaneState.name = "eight lane edit check";
    const std::array<juce::String, 8> roles { "drum", "bass", "arp", "chord", "air", "snare", "pulse", "arp" };
    while (eightLaneState.lanes.size() < roles.size())
        eightLaneState.lanes.push_back (eightLaneState.lanes[eightLaneState.lanes.size() % states.front().lanes.size()]);

    for (size_t lane = 0; lane < eightLaneState.lanes.size(); ++lane)
    {
        eightLaneState.lanes[lane].name = "edit lane " + juce::String (static_cast<int> (lane + 1));
        eightLaneState.lanes[lane].role = roles[lane];
        eightLaneState.lanes[lane].volume = 0.10f + static_cast<float> (lane) * 0.025f;
    }

    if (! engine.loadProgram (Wf::buildStateProgram (eightLaneState), bindings))
    {
        std::cerr << "eight-lane program failed: " << engine.getLastError() << '\n';
        return 7;
    }

    static_cast<void> (engine.setParameterValue ("hostMasterGain", 0.18f));
    static_cast<void> (engine.setParameterValue ("hostTempoHz", static_cast<float> (eightLaneState.tempoBpm / 60.0)));
    static_cast<void> (engine.setParameterValue ("hostIntensity", 0.68f));
    static_cast<void> (engine.setParameterValue ("hostBrightness", 0.58f));
    static_cast<void> (engine.setParameterValue ("hostOrbitPhase", 0.35f));

    const auto eightLaneEnergy = renderEnergy (engine, input, output, 128);
    if (eightLaneEnergy <= 0.0
        || engine.getRenderExceptionCount() != 0
        || engine.getInternalErrorCount() != 0)
    {
        std::cerr << "eight-lane render failed: " << eightLaneEnergy << '\n';
        return 8;
    }

    auto mutedState = states.front();
    for (auto& lane : mutedState.lanes)
        lane.muted = true;

    if (! engine.loadProgram (Wf::buildStateProgram (mutedState), bindings))
    {
        std::cerr << "muted program failed: " << engine.getLastError() << '\n';
        return 9;
    }

    static_cast<void> (engine.setParameterValue ("hostMasterGain", 0.4f));
    static_cast<void> (engine.setParameterValue ("hostTempoHz", static_cast<float> (mutedState.tempoBpm / 60.0)));
    static_cast<void> (engine.setParameterValue ("hostIntensity", 1.0f));
    static_cast<void> (engine.setParameterValue ("hostBrightness", 1.0f));
    static_cast<void> (engine.setParameterValue ("hostOrbitPhase", 0.5f));

    const auto mutedEnergy = renderEnergy (engine, input, output, 128);
    if (mutedEnergy < 0.0 || mutedEnergy > 0.001
        || engine.getRenderExceptionCount() != 0
        || engine.getInternalErrorCount() != 0)
    {
        std::cerr << "muted render leaked or failed: " << mutedEnergy << '\n';
        return 10;
    }

    auto soloState = states.front();
    soloState.lanes[1].solo = true;

    if (! engine.loadProgram (Wf::buildStateProgram (soloState), bindings))
    {
        std::cerr << "solo program failed: " << engine.getLastError() << '\n';
        return 11;
    }

    static_cast<void> (engine.setParameterValue ("hostMasterGain", 0.22f));
    static_cast<void> (engine.setParameterValue ("hostTempoHz", static_cast<float> (soloState.tempoBpm / 60.0)));
    static_cast<void> (engine.setParameterValue ("hostIntensity", 0.70f));
    static_cast<void> (engine.setParameterValue ("hostBrightness", 0.54f));
    static_cast<void> (engine.setParameterValue ("hostOrbitPhase", 0.25f));

    const auto soloEnergy = renderEnergy (engine, input, output, 128);
    if (soloEnergy <= 0.0
        || engine.getRenderExceptionCount() != 0
        || engine.getInternalErrorCount() != 0)
    {
        std::cerr << "solo render failed: " << soloEnergy << '\n';
        return 12;
    }

    auto drumState = states.front();
    for (size_t lane = 1; lane < drumState.lanes.size(); ++lane)
        drumState.lanes[lane].volume = 0.0f;

    if (! engine.loadProgram (Wf::buildStateProgram (drumState), bindings))
    {
        std::cerr << "drum-only program failed: " << engine.getLastError() << '\n';
        return 13;
    }

    static_cast<void> (engine.setParameterValue ("hostMasterGain", 0.28f));
    static_cast<void> (engine.setParameterValue ("hostTempoHz", static_cast<float> (drumState.tempoBpm / 60.0)));
    static_cast<void> (engine.setParameterValue ("hostIntensity", 0.78f));
    static_cast<void> (engine.setParameterValue ("hostBrightness", 0.56f));
    static_cast<void> (engine.setParameterValue ("hostOrbitPhase", 0.2f));

    const auto drumEnergy = renderEnergy (engine, input, output, 160);
    if (drumEnergy < 12.0
        || engine.getRenderExceptionCount() != 0
        || engine.getInternalErrorCount() != 0)
    {
        std::cerr << "drum render too quiet or unstable: " << drumEnergy << '\n';
        return 14;
    }

    auto snareState = drumState;
    snareState.lanes[0].name = "snare check";
    snareState.lanes[0].role = "snare";

    if (! engine.loadProgram (Wf::buildStateProgram (snareState), bindings))
    {
        std::cerr << "snare-only program failed: " << engine.getLastError() << '\n';
        return 15;
    }

    static_cast<void> (engine.setParameterValue ("hostMasterGain", 0.28f));
    static_cast<void> (engine.setParameterValue ("hostTempoHz", static_cast<float> (snareState.tempoBpm / 60.0)));
    static_cast<void> (engine.setParameterValue ("hostIntensity", 0.78f));
    static_cast<void> (engine.setParameterValue ("hostBrightness", 0.64f));
    static_cast<void> (engine.setParameterValue ("hostOrbitPhase", 0.4f));

    const auto snareEnergy = renderEnergy (engine, input, output, 160);
    if (snareEnergy < 2.4
        || engine.getRenderExceptionCount() != 0
        || engine.getInternalErrorCount() != 0)
    {
        std::cerr << "snare render too quiet or unstable: " << snareEnergy << '\n';
        return 16;
    }

    auto emptyLaneState = states.front();
    emptyLaneState.name = "empty lane robustness check";
    emptyLaneState.lanes.clear();

    if (! engine.loadProgram (Wf::buildStateProgram (emptyLaneState), bindings))
    {
        std::cerr << "empty-lane program failed: " << engine.getLastError() << '\n';
        return 17;
    }

    static_cast<void> (engine.setParameterValue ("hostMasterGain", 0.4f));
    static_cast<void> (engine.setParameterValue ("hostTempoHz", static_cast<float> (emptyLaneState.tempoBpm / 60.0)));
    static_cast<void> (engine.setParameterValue ("hostIntensity", 1.0f));
    static_cast<void> (engine.setParameterValue ("hostBrightness", 1.0f));
    static_cast<void> (engine.setParameterValue ("hostOrbitPhase", 0.0f));

    const auto emptyLaneEnergy = renderEnergy (engine, input, output, 64);
    if (emptyLaneEnergy < 0.0 || emptyLaneEnergy > 0.001
        || engine.getRenderExceptionCount() != 0
        || engine.getInternalErrorCount() != 0)
    {
        std::cerr << "empty-lane render leaked or failed: " << emptyLaneEnergy << '\n';
        return 18;
    }

    return 0;
}
