#include <WeldChucKEngine.h>

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

            if (! std::isfinite (value))
                return -1.0;

            energy += static_cast<double> (value) * static_cast<double> (value);
        }

    return energy;
}
}

int main()
{
    juce::ScopedNoDenormals noDenormals;

    EmbeddedChucKEngine engine;
    constexpr int blockSize = 128;

    if (! engine.prepare (48000.0, blockSize, 0, 2))
    {
        std::cerr << engine.getLastError() << '\n';
        return 1;
    }

    if (engine.getParameterIndex ("hostFreq") < 0
        || ! engine.setParameterValue ("hostFreq", 440.0f)
        || ! engine.setParameterValue ("hostGain", 0.10f))
    {
        std::cerr << "parameter access failed\n";
        return 2;
    }

    juce::AudioBuffer<float> input (0, blockSize);
    juce::AudioBuffer<float> output (2, blockSize);
    output.clear();
    engine.process (input, output);

    if (bufferEnergy (output) <= 0.0 || engine.getRenderExceptionCount() != 0)
    {
        std::cerr << "render failed\n";
        return 3;
    }

    return 0;
}
