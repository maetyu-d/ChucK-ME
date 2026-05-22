#pragma once

#include <WeldChucKEngine.h>

#include <array>
#include <cmath>
#include <vector>

namespace Wf
{
struct LaneSpec
{
    juce::String name;
    juce::String role;
    float baseHz = 220.0f;
    float volume = 0.2f;
    int pulseTicks = 96;
    int openTicks = 18;
};

struct StateSpec
{
    juce::String name;
    double tempoBpm = 104.0;
    std::array<LaneSpec, 5> lanes {};
};

inline std::vector<EmbeddedChucKEngine::ParameterBinding> makeWfParameterBindings()
{
    return
    {
        { "hostMasterGain", 0.18f, 0.0f, 0.8f },
        { "hostTempoHz", 1.0f, 0.2f, 6.0f },
        { "hostIntensity", 0.58f, 0.0f, 1.0f },
        { "hostBrightness", 0.48f, 0.0f, 1.0f },
        { "hostOrbitPhase", 0.0f, 0.0f, 1.0f }
    };
}

inline std::vector<StateSpec> makeDefaultStates()
{
    return
    {
        { "Pocket City", 112.0, {{
            { "Tick machine", "pulse", 523.25f, 0.20f, 24, 7 },
            { "Pocket bass", "bass", 65.41f, 0.34f, 48, 18 },
            { "Signal melody", "lead", 261.63f, 0.24f, 36, 13 },
            { "Glass chorus", "chord", 130.81f, 0.20f, 96, 48 },
            { "Static halo", "air", 1046.50f, 0.09f, 144, 72 }
        }}},
        { "Chrome Avenue", 118.0, {{
            { "Metro ticks", "pulse", 587.33f, 0.21f, 22, 6 },
            { "Rubber motor", "bass", 73.42f, 0.35f, 44, 17 },
            { "Window hook", "lead", 293.66f, 0.25f, 33, 12 },
            { "Warm display", "chord", 146.83f, 0.21f, 88, 44 },
            { "Thin skyline", "air", 1174.66f, 0.09f, 132, 64 }
        }}},
        { "Battery Love", 122.0, {{
            { "Relay hats", "pulse", 659.25f, 0.22f, 20, 6 },
            { "Battery bass", "bass", 82.41f, 0.36f, 40, 16 },
            { "Simple song", "lead", 329.63f, 0.27f, 30, 11 },
            { "Major lights", "chord", 164.81f, 0.22f, 80, 40 },
            { "Soft carrier", "air", 1318.51f, 0.08f, 120, 60 }
        }}},
        { "Pocket Choir", 108.0, {{
            { "Tiny clock", "pulse", 493.88f, 0.19f, 26, 8 },
            { "Round bass", "bass", 61.74f, 0.32f, 52, 20 },
            { "Choir lead", "lead", 246.94f, 0.24f, 39, 14 },
            { "Soft buttons", "chord", 123.47f, 0.22f, 104, 52 },
            { "Air tape", "air", 987.77f, 0.08f, 156, 76 }
        }}},
        { "Neon Postcard", 116.0, {{
            { "Card punch", "pulse", 554.37f, 0.21f, 23, 7 },
            { "Postcard bass", "bass", 69.30f, 0.34f, 46, 18 },
            { "Neon reply", "lead", 277.18f, 0.26f, 35, 12 },
            { "Blue chord", "chord", 138.59f, 0.21f, 92, 46 },
            { "Tape star", "air", 1108.73f, 0.08f, 138, 68 }
        }}}
    };
}

inline juce::String chuckFloat (float value)
{
    return juce::String (static_cast<double> (value), 5);
}

inline juce::String chuckInt (int value)
{
    return juce::String (juce::jmax (1, value));
}

inline float transposeHz (float frequency, int semitones)
{
    return frequency * std::pow (2.0f, static_cast<float> (semitones) / 12.0f);
}

inline std::array<int, 8> sequenceForRole (const juce::String& role)
{
    if (role == "bass")
        return { 0, 0, 7, 0, 5, 5, 7, 10 };

    if (role == "chord")
        return { 0, 5, 7, 9, 0, 12, 7, 5 };

    if (role == "pulse")
        return { 24, 24, 31, 24, 28, 24, 31, 36 };

    if (role == "air")
        return { 12, 19, 16, 12, 14, 21, 19, 16 };

    return { 12, 16, 19, 16, 14, 12, 7, 11 };
}

inline void appendSequencePicker (juce::String& program, const LaneSpec& lane, const juce::String& suffix)
{
    const auto pulseTicks = chuckInt (lane.pulseTicks);
    const auto sequence = sequenceForRole (lane.role);

    program << "    (tick / " << pulseTicks << ") % 8 => int laneNote" << suffix << ";\n";
    program << "    " << chuckFloat (lane.baseHz) << " => laneFreq" << suffix << ";\n";

    for (int step = 0; step < static_cast<int> (sequence.size()); ++step)
    {
        program << "    " << (step == 0 ? "if" : "else if") << " (laneNote" << suffix << " == " << step << ")\n";
        program << "        " << chuckFloat (transposeHz (lane.baseHz, sequence[static_cast<size_t> (step)])) << " => laneFreq" << suffix << ";\n";
    }
}

inline void appendLaneDeclaration (juce::String& program, const LaneSpec& lane, int index)
{
    const auto suffix = juce::String (index);

    if (lane.role == "bass")
    {
        program << "SawOsc lane" << suffix << " => LPF filter" << suffix << " => Gain laneGain" << suffix << " => Gain laneSmooth" << suffix << " => master;\n";
        program << "0.64 => filter" << suffix << ".Q;\n";
    }
    else if (lane.role == "chord")
    {
        program << "SinOsc lane" << suffix << "a => Gain laneGain" << suffix << " => Gain laneSmooth" << suffix << " => master;\n";
        program << "SinOsc lane" << suffix << "b => laneGain" << suffix << ";\n";
        program << "SinOsc lane" << suffix << "c => laneGain" << suffix << ";\n";
    }
    else
    {
        program << "TriOsc lane" << suffix << " => Gain laneGain" << suffix << " => Gain laneSmooth" << suffix << " => master;\n";
    }

    program << "1.0 => laneGain" << suffix << ".gain;\n";
    program << "0.0 => laneSmooth" << suffix << ".gain;\n\n";
}

inline void appendLaneControl (juce::String& program, const LaneSpec& lane, int index)
{
    const auto suffix = juce::String (index);
    const auto pulseTicks = chuckInt (lane.pulseTicks);
    const auto openTicks = chuckInt (lane.openTicks);
    const auto volume = chuckFloat (lane.volume);

    program << "    tick % " << pulseTicks << " => int laneStep" << suffix << ";\n";
    appendSequencePicker (program, lane, suffix);

    program << "    if (laneStep" << suffix << " < " << openTicks << ")\n";
    program << "        " << volume << " * (0.52 + intensity * 0.78) => laneTarget" << suffix << ";\n";
    program << "    else\n";
    program << "        " << volume << " * (0.012 + orbit * 0.035) => laneTarget" << suffix << ";\n";
    program << "    laneLevel" << suffix << " + ((laneTarget" << suffix << " - laneLevel" << suffix << ") * 0.16) => laneLevel" << suffix << ";\n";

    if (lane.role == "bass")
    {
        program << "    Math.max(28.0, laneFreq" << suffix << " * (0.50 + orbit * 0.035)) => lane" << suffix << ".freq;\n";
        program << "    260.0 + bright * 1750.0 + intensity * 420.0 => filter" << suffix << ".freq;\n";
    }
    else if (lane.role == "chord")
    {
        program << "    laneFreq" << suffix << " * (0.997 + orbit * 0.006) => lane" << suffix << "a.freq;\n";
        program << "    laneFreq" << suffix << " * 1.25992 => lane" << suffix << "b.freq;\n";
        program << "    laneFreq" << suffix << " * 1.49831 => lane" << suffix << "c.freq;\n";
    }
    else if (lane.role == "air")
    {
        program << "    laneFreq" << suffix << " * (0.72 + bright * 0.24 + orbit * 0.035) => lane" << suffix << ".freq;\n";
    }
    else
    {
        program << "    laneFreq" << suffix << " * (0.98 + bright * 0.05 + orbit * 0.018) => lane" << suffix << ".freq;\n";
    }

    program << "    laneLevel" << suffix << " => laneSmooth" << suffix << ".gain;\n\n";
}

inline juce::String buildStateProgram (const StateSpec& state)
{
    juce::String program;
    program << "Gain master => dac;\n";
    program << "0.0 => master.gain;\n\n";

    for (int i = 0; i < static_cast<int> (state.lanes.size()); ++i)
        appendLaneDeclaration (program, state.lanes[static_cast<size_t> (i)], i);

    program << "0 => int tick;\n";
    program << "0.0 => float laneLevel0;\n";
    program << "0.0 => float laneLevel1;\n";
    program << "0.0 => float laneLevel2;\n";
    program << "0.0 => float laneLevel3;\n";
    program << "0.0 => float laneLevel4;\n\n";
    program << "0.0 => float laneFreq0;\n";
    program << "0.0 => float laneFreq1;\n";
    program << "0.0 => float laneFreq2;\n";
    program << "0.0 => float laneFreq3;\n";
    program << "0.0 => float laneFreq4;\n\n";
    program << "0.0 => float laneTarget0;\n";
    program << "0.0 => float laneTarget1;\n";
    program << "0.0 => float laneTarget2;\n";
    program << "0.0 => float laneTarget3;\n";
    program << "0.0 => float laneTarget4;\n\n";
    program << "0.0 => float smoothedMaster;\n\n";
    program << "while (true)\n";
    program << "{\n";
    program << "    Math.max(0.0, Math.min(hostMasterGain, 0.8)) => float masterLevel;\n";
    program << "    Math.max(0.2, Math.min(hostTempoHz, 6.0)) => float tempo;\n";
    program << "    Math.max(0.0, Math.min(hostIntensity, 1.0)) => float intensity;\n";
    program << "    Math.max(0.0, Math.min(hostBrightness, 1.0)) => float bright;\n";
    program << "    Math.max(0.0, Math.min(hostOrbitPhase, 1.0)) => float orbit;\n";
    program << "    smoothedMaster + ((masterLevel - smoothedMaster) * 0.055) => smoothedMaster;\n";
    program << "    smoothedMaster => master.gain;\n";
    program << "    tick + 1 => tick;\n\n";

    for (int i = 0; i < static_cast<int> (state.lanes.size()); ++i)
        appendLaneControl (program, state.lanes[static_cast<size_t> (i)], i);

    program << "    5::ms => now;\n";
    program << "}\n";
    return program;
}
}
