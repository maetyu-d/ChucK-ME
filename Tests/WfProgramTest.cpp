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

        double energy = 0.0;
        for (int block = 0; block < 8; ++block)
        {
            output.clear();
            engine.process (input, output);
            energy += bufferEnergy (output);
        }

        if (energy <= 0.0
            || engine.getRenderExceptionCount() != 0
            || engine.getInternalErrorCount() != 0)
        {
            std::cerr << "state render failed: " << states[i].name << '\n';
            return 4;
        }
    }

    return 0;
}
