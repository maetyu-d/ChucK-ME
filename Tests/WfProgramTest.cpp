#include <WeldChucKEngine.h>

#include "WfChucKPrograms.h"

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

    if (states.size() != 6)
    {
        std::cerr << "wf demo should have six states, found " << states.size() << '\n';
        return 9;
    }

    for (const auto& state : states)
    {
        if (state.tracks.size() != 5)
        {
            std::cerr << "state should have five tracks: " << state.name << '\n';
            return 10;
        }

        for (const auto& track : state.tracks)
        {
            if (track.lanes.size() != 2)
            {
                std::cerr << "track should have two lanes: " << state.name << " / " << track.name << '\n';
                return 11;
            }
        }
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

    auto drumState = states.front();
    for (size_t track = 0; track < drumState.tracks.size(); ++track)
        for (size_t lane = 0; lane < drumState.tracks[track].lanes.size(); ++lane)
            if (track != 0 || lane != 0)
                drumState.tracks[track].lanes[lane].volume = 0.0f;

    if (! engine.loadProgram (Wf::buildStateProgram (drumState), bindings))
    {
        std::cerr << "drum-only program failed: " << engine.getLastError() << '\n';
        return 5;
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
        return 6;
    }

    auto snareState = drumState;
    snareState.tracks[0].lanes[0].name = "snare check";
    snareState.tracks[0].lanes[0].role = "snare";

    if (! engine.loadProgram (Wf::buildStateProgram (snareState), bindings))
    {
        std::cerr << "snare-only program failed: " << engine.getLastError() << '\n';
        return 7;
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
        return 8;
    }

    return 0;
}
