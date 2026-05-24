#include <WeldChucKEngine.h>

#include "WfChucKPrograms.h"

#include <juce_audio_formats/juce_audio_formats.h>

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

bool bufferIsSilent (const juce::AudioBuffer<float>& buffer)
{
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            const auto value = buffer.getSample (channel, sample);
            if (! std::isfinite (value) || value != 0.0f)
                return false;
        }

    return true;
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

double bufferEnergy (const juce::AudioSampleBuffer& buffer, int frames)
{
    double energy = 0.0;

    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        for (int sample = 0; sample < frames; ++sample)
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

    {
        auto wavState = states.front();
        EmbeddedChucKEngine wavEngine;
        if (! wavEngine.prepare (48000.0, blockSize, 0, 2))
        {
            std::cerr << "wav render prepare failed: " << wavEngine.getLastError() << '\n';
            return 45;
        }

        if (! wavEngine.loadProgram (Wf::buildStateProgram (wavState), bindings))
        {
            std::cerr << "wav render program failed: " << wavEngine.getLastError() << '\n';
            return 46;
        }

        static_cast<void> (wavEngine.setParameterValue ("hostMasterGain", 0.18f));
        static_cast<void> (wavEngine.setParameterValue ("hostTempoHz", static_cast<float> (wavState.tempoBpm / 60.0)));
        static_cast<void> (wavEngine.setParameterValue ("hostIntensity", 0.58f));
        static_cast<void> (wavEngine.setParameterValue ("hostBrightness", 0.48f));
        static_cast<void> (wavEngine.setParameterValue ("hostOrbitPhase", 0.0f));

        juce::MemoryBlock wavData;
        std::unique_ptr<juce::OutputStream> outputStream (new juce::MemoryOutputStream (wavData, false));
        juce::WavAudioFormat wavFormat;
        auto writer = wavFormat.createWriterFor (outputStream,
                                                 juce::AudioFormatWriterOptions {}
                                                    .withSampleRate (48000.0)
                                                    .withNumChannels (2)
                                                    .withBitsPerSample (24));
        if (writer == nullptr)
        {
            std::cerr << "wav writer create failed\n";
            return 47;
        }

        juce::AudioBuffer<float> wavInput (0, blockSize);
        juce::AudioBuffer<float> wavOutput (2, blockSize);
        double writtenEnergy = 0.0;
        for (int block = 0; block < 128; ++block)
        {
            wavOutput.clear();
            wavEngine.process (wavInput, wavOutput);
            writtenEnergy += bufferEnergy (wavOutput, blockSize);
            if (! writer->writeFromAudioSampleBuffer (wavOutput, 0, blockSize))
            {
                std::cerr << "wav writer write failed\n";
                return 48;
            }
        }

        writer.reset();

        if (writtenEnergy <= 0.0)
        {
            std::cerr << "wav render generated silence before write\n";
            return 49;
        }

        std::unique_ptr<juce::AudioFormatReader> reader (wavFormat.createReaderFor (new juce::MemoryInputStream (wavData.getData(), wavData.getSize(), false),
                                                                                    true));
        if (reader == nullptr)
        {
            std::cerr << "wav reader create failed\n";
            return 53;
        }

        juce::AudioBuffer<float> decoded (2, static_cast<int> (reader->lengthInSamples));
        reader->read (&decoded, 0, decoded.getNumSamples(), 0, true, true);
        const auto decodedEnergy = bufferEnergy (decoded, decoded.getNumSamples());
        if (decodedEnergy <= 0.0)
        {
            std::cerr << "wav writer produced silent file\n";
            return 54;
        }
    }

    auto laneClockState = states.front();
    laneClockState.name = "lane tempo duration check";
    laneClockState.clockBeatsPerBar = 3;
    laneClockState.clockQuarterNotesPerBar = 1.5;
    laneClockState.lanes.resize (2);
    laneClockState.lanes[0].tempoBpm = 132.0f;
    laneClockState.lanes[0].duration = Wf::TrackDurationSpec { 1, 1 };
    laneClockState.lanes[1].tempoBpm = 66.0f;
    laneClockState.lanes[1].duration = Wf::TrackDurationSpec { 0, 2 };

    const auto laneClockProgram = Wf::buildStateProgram (laneClockState);
    if (! laneClockProgram.contains ("laneQuarterNotesPerBar")
        || ! laneClockProgram.contains ("* 0.005")
        || ! laneClockProgram.contains ("laneActive0")
        || ! laneClockProgram.contains ("2.20000 => float laneTempo0"))
    {
        std::cerr << "lane clock program missing expected timing code\n";
        return 50;
    }

    if (! engine.loadProgram (laneClockProgram, bindings))
    {
        std::cerr << "lane clock program failed: " << engine.getLastError() << '\n';
        return 51;
    }

    static_cast<void> (engine.setParameterValue ("hostMasterGain", 0.24f));
    static_cast<void> (engine.setParameterValue ("hostTempoHz", static_cast<float> (laneClockState.tempoBpm / 60.0)));
    static_cast<void> (engine.setParameterValue ("hostIntensity", 0.68f));
    static_cast<void> (engine.setParameterValue ("hostBrightness", 0.52f));
    static_cast<void> (engine.setParameterValue ("hostOrbitPhase", 0.2f));

    const auto laneClockEnergy = renderEnergy (engine, input, output, 160);
    if (laneClockEnergy <= 0.0
        || engine.getRenderExceptionCount() != 0
        || engine.getInternalErrorCount() != 0)
    {
        std::cerr << "lane clock render failed: " << laneClockEnergy << '\n';
        return 52;
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

    auto customLaneState = states.front();
    customLaneState.name = "custom lane code check";
    customLaneState.lanes.resize (1);
    customLaneState.lanes[0].customDeclarationCode = "SinOsc lane0 => Gain customGain0 => master;\n0.0 => customGain0.gain;";
    customLaneState.lanes[0].customControlCode = "220.0 + (bright * 180.0) => lane0.freq;\n0.08 * (0.4 + intensity) => customGain0.gain;";

    if (! engine.loadProgram (Wf::buildStateProgram (customLaneState), bindings))
    {
        std::cerr << "custom lane program failed: " << engine.getLastError() << '\n';
        return 13;
    }

    static_cast<void> (engine.setParameterValue ("hostMasterGain", 0.30f));
    static_cast<void> (engine.setParameterValue ("hostTempoHz", static_cast<float> (customLaneState.tempoBpm / 60.0)));
    static_cast<void> (engine.setParameterValue ("hostIntensity", 0.70f));
    static_cast<void> (engine.setParameterValue ("hostBrightness", 0.54f));
    static_cast<void> (engine.setParameterValue ("hostOrbitPhase", 0.25f));

    const auto customLaneEnergy = renderEnergy (engine, input, output, 64);
    if (customLaneEnergy <= 0.0
        || engine.getRenderExceptionCount() != 0
        || engine.getInternalErrorCount() != 0)
    {
        std::cerr << "custom lane render failed: " << customLaneEnergy << '\n';
        return 14;
    }

    auto invalidCustomLaneState = customLaneState;
    invalidCustomLaneState.lanes[0].customControlCode = "not valid chuck code";

    if (engine.loadProgram (Wf::buildStateProgram (invalidCustomLaneState), bindings))
    {
        std::cerr << "invalid custom lane program unexpectedly loaded\n";
        return 15;
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

    output.clear();
    engine.process (input, output);
    const auto firstDrumBlockEnergy = bufferEnergy (output);
    if (firstDrumBlockEnergy <= 0.001
        || engine.getRenderExceptionCount() != 0
        || engine.getInternalErrorCount() != 0)
    {
        std::cerr << "drum first block missed downbeat: " << firstDrumBlockEnergy << '\n';
        return 16;
    }

    const auto drumEnergy = renderEnergy (engine, input, output, 160);
    if (drumEnergy < 12.0
        || engine.getRenderExceptionCount() != 0
        || engine.getInternalErrorCount() != 0)
    {
        std::cerr << "drum render too quiet or unstable: " << drumEnergy << '\n';
        return 17;
    }

    auto snareState = drumState;
    snareState.lanes[0].name = "snare check";
    snareState.lanes[0].role = "snare";

    if (! engine.loadProgram (Wf::buildStateProgram (snareState), bindings))
    {
        std::cerr << "snare-only program failed: " << engine.getLastError() << '\n';
        return 18;
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
        return 19;
    }

    auto emptyLaneState = states.front();
    emptyLaneState.name = "empty lane robustness check";
    emptyLaneState.lanes.clear();

    if (! engine.loadProgram (Wf::buildStateProgram (emptyLaneState), bindings))
    {
        std::cerr << "empty-lane program failed: " << engine.getLastError() << '\n';
        return 20;
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
        return 21;
    }

    {
        const std::array<double, 4> sampleRates { 44100.0, 48000.0, 88200.0, 96000.0 };
        const std::array<int, 6> blockSizes { 64, 127, 256, 511, 1024, 2048 };
        auto accumulatedMatrixEnergy = 0.0;

        for (auto sampleRate : sampleRates)
        {
            for (auto matrixBlockSize : blockSizes)
            {
                EmbeddedChucKEngine matrixEngine;
                if (! matrixEngine.prepare (sampleRate, matrixBlockSize, 0, 2))
                {
                    std::cerr << "matrix prepare failed: " << sampleRate << " / " << matrixBlockSize
                              << " / " << matrixEngine.getLastError() << '\n';
                    return 22;
                }

                auto matrixState = states.front();
                matrixState.tempoBpm = 97.0;
                if (! matrixEngine.loadProgram (Wf::buildStateProgram (matrixState), bindings))
                {
                    std::cerr << "matrix program failed: " << sampleRate << " / " << matrixBlockSize
                              << " / " << matrixEngine.getLastError() << '\n';
                    return 23;
                }

                static_cast<void> (matrixEngine.setParameterValue ("hostMasterGain", 0.22f));
                static_cast<void> (matrixEngine.setParameterValue ("hostTempoHz", static_cast<float> (matrixState.tempoBpm / 60.0)));
                static_cast<void> (matrixEngine.setParameterValue ("hostIntensity", 0.62f));
                static_cast<void> (matrixEngine.setParameterValue ("hostBrightness", 0.56f));
                static_cast<void> (matrixEngine.setParameterValue ("hostOrbitPhase", 0.33f));

                juce::AudioBuffer<float> matrixInput (0, matrixBlockSize);
                juce::AudioBuffer<float> matrixOutput (2, matrixBlockSize);
                const auto blocksToRender = juce::jmax (8,
                                                        static_cast<int> (std::ceil ((sampleRate * 0.30)
                                                                                     / static_cast<double> (matrixBlockSize))));
                const auto matrixEnergy = renderEnergy (matrixEngine, matrixInput, matrixOutput, blocksToRender);
                const auto renderedFrames = static_cast<double> (blocksToRender * matrixBlockSize);
                const auto energyDensity = matrixEnergy / juce::jmax (1.0, renderedFrames * 2.0);

                if (matrixEnergy <= 0.0
                    || energyDensity <= 0.0000001
                    || energyDensity > 0.25
                    || matrixEngine.getRenderExceptionCount() != 0
                    || matrixEngine.getInternalErrorCount() != 0
                    || matrixEngine.getSilentProcessCount() != 0)
                {
                    std::cerr << "sample-rate/block render matrix failed: " << sampleRate
                              << " / " << matrixBlockSize
                              << " energy=" << matrixEnergy
                              << " density=" << energyDensity << '\n';
                    return 24;
                }

                accumulatedMatrixEnergy += matrixEnergy;
                matrixEngine.release();
                matrixOutput.clear();
                matrixOutput.setSample (0, 0, 1.0f);
                matrixEngine.process (matrixInput, matrixOutput);

                if (! bufferIsSilent (matrixOutput))
                {
                    std::cerr << "released matrix engine emitted audio\n";
                    return 25;
                }
            }
        }

        if (accumulatedMatrixEnergy <= 0.0)
        {
            std::cerr << "sample-rate/block matrix rendered silence\n";
            return 26;
        }
    }

    return 0;
}
