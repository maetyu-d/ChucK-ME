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
        { "Pocket City", 88.0, {{
            { "808 street", "drum", 45.0f, 0.58f, 16, 1 },
            { "Pocket bass", "bass", 65.41f, 0.34f, 2, 1 },
            { "Signal arp", "arp", 261.63f, 0.24f, 1, 1 },
            { "Glass chorus", "chord", 130.81f, 0.20f, 8, 5 },
            { "Static halo", "air", 1046.50f, 0.07f, 12, 6 }
        }}},
        { "Chrome Avenue", 92.0, {{
            { "808 avenue", "drum", 46.5f, 0.60f, 16, 1 },
            { "Rubber motor", "bass", 73.42f, 0.35f, 2, 1 },
            { "Window arp", "arp", 293.66f, 0.25f, 1, 1 },
            { "Warm display", "chord", 146.83f, 0.21f, 8, 5 },
            { "Thin skyline", "air", 1174.66f, 0.07f, 12, 6 }
        }}},
        { "Battery Love", 96.0, {{
            { "808 relay", "drum", 43.8f, 0.62f, 16, 1 },
            { "Battery bass", "bass", 82.41f, 0.36f, 2, 1 },
            { "Simple arp", "arp", 329.63f, 0.27f, 1, 1 },
            { "Major lights", "chord", 164.81f, 0.22f, 8, 5 },
            { "Soft carrier", "air", 1318.51f, 0.07f, 12, 6 }
        }}},
        { "Pocket Choir", 84.0, {{
            { "808 choir", "drum", 44.2f, 0.56f, 16, 1 },
            { "Round bass", "bass", 61.74f, 0.32f, 2, 1 },
            { "Choir arp", "arp", 246.94f, 0.24f, 1, 1 },
            { "Soft buttons", "chord", 123.47f, 0.22f, 8, 5 },
            { "Air tape", "air", 987.77f, 0.07f, 12, 6 }
        }}},
        { "Neon Postcard", 90.0, {{
            { "808 postcard", "drum", 45.6f, 0.59f, 16, 1 },
            { "Postcard bass", "bass", 69.30f, 0.34f, 2, 1 },
            { "Neon arp", "arp", 277.18f, 0.26f, 1, 1 },
            { "Blue chord", "chord", 138.59f, 0.21f, 8, 5 },
            { "Tape star", "air", 1108.73f, 0.07f, 12, 6 }
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
        return { 0, 0, 7, 0, 0, 5, 7, 10 };

    if (role == "chord")
        return { 0, 0, 5, 5, 7, 7, 9, 12 };

    if (role == "pulse")
        return { 24, 31, 28, 31, 24, 36, 31, 28 };

    if (role == "air")
        return { 12, 19, 24, 19, 16, 21, 24, 28 };

    return { 12, 16, 19, 24, 19, 16, 14, 19 };
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
    else if (lane.role == "drum" || lane.role == "snare")
    {
        program << "SinOsc lane" << suffix << "Kick => Gain lane" << suffix << "KickGain => master;\n";
        program << "SinOsc lane" << suffix << "Sub => Gain lane" << suffix << "SubGain => master;\n";
        program << "TriOsc lane" << suffix << "Click => Gain lane" << suffix << "ClickGain => master;\n";
        program << "TriOsc lane" << suffix << "SnareBody => Gain lane" << suffix << "SnareBodyGain => master;\n";
        program << "SinOsc lane" << suffix << "SnareSnap => Gain lane" << suffix << "SnareSnapGain => master;\n";
        program << "Noise lane" << suffix << "SnareNoise => Gain lane" << suffix << "SnareNoiseGain => master;\n";
        program << "Noise lane" << suffix << "SnareClap => Gain lane" << suffix << "SnareClapGain => master;\n";
        program << "Noise lane" << suffix << "HatNoise => Gain lane" << suffix << "HatGain => master;\n";
        program << "0.0 => lane" << suffix << "KickGain.gain;\n";
        program << "0.0 => lane" << suffix << "SubGain.gain;\n";
        program << "0.0 => lane" << suffix << "ClickGain.gain;\n";
        program << "0.0 => lane" << suffix << "SnareBodyGain.gain;\n";
        program << "0.0 => lane" << suffix << "SnareSnapGain.gain;\n";
        program << "0.0 => lane" << suffix << "SnareNoiseGain.gain;\n";
        program << "0.0 => lane" << suffix << "SnareClapGain.gain;\n";
        program << "0.0 => lane" << suffix << "HatGain.gain;\n\n";
        return;
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

    if (lane.role == "drum" || lane.role == "snare")
    {
        program << "    if (didTick == 1)\n";
        program << "    {\n";
        program << "        tick % 16 => int drumStep" << suffix << ";\n";
        if (lane.role == "drum")
        {
            program << "        if (drumStep" << suffix << " == 0 || drumStep" << suffix << " == 3 || drumStep" << suffix << " == 7 || drumStep" << suffix << " == 8 || drumStep" << suffix << " == 10 || drumStep" << suffix << " == 14)\n";
            program << "        {\n";
            program << "            Math.min(1.0, kickLevel" << suffix << " + 1.0) => kickLevel" << suffix << ";\n";
            program << "            1.0 => kickClick" << suffix << ";\n";
            program << "        }\n";
        }
        program << "        if (drumStep" << suffix << " == 4 || drumStep" << suffix << " == 12)\n";
        program << "        {\n";
        program << "            1.0 => snareLevel" << suffix << ";\n";
        program << "            1.0 => snareSnap" << suffix << ";\n";
        program << "            1.0 => snareClap" << suffix << ";\n";
        program << "        }\n";
        if (lane.role == "drum")
        {
            program << "        if (drumStep" << suffix << " == 5 || drumStep" << suffix << " == 13)\n";
            program << "            Math.max(snareClap" << suffix << ", 0.42) => snareClap" << suffix << ";\n";
            program << "        if (drumStep" << suffix << " == 2 || drumStep" << suffix << " == 6 || drumStep" << suffix << " == 8 || drumStep" << suffix << " == 11 || drumStep" << suffix << " == 14 || drumStep" << suffix << " == 15)\n";
            program << "            1.0 => hatLevel" << suffix << ";\n";
        }
        program << "    }\n";
        program << "    kickLevel" << suffix << " * 0.968 => kickLevel" << suffix << ";\n";
        program << "    kickClick" << suffix << " * 0.36 => kickClick" << suffix << ";\n";
        program << "    snareLevel" << suffix << " * 0.78 => snareLevel" << suffix << ";\n";
        program << "    snareSnap" << suffix << " * 0.32 => snareSnap" << suffix << ";\n";
        program << "    snareClap" << suffix << " * 0.68 => snareClap" << suffix << ";\n";
        program << "    hatLevel" << suffix << " * 0.48 => hatLevel" << suffix << ";\n";
        program << "    " << chuckFloat (lane.baseHz) << " + (kickLevel" << suffix << " * 18.0) + (kickClick" << suffix << " * 122.0) => lane" << suffix << "Kick.freq;\n";
        program << "    Math.max(28.0, " << chuckFloat (lane.baseHz) << " * 0.50) => lane" << suffix << "Sub.freq;\n";
        program << "    920.0 + (bright * 2400.0) => lane" << suffix << "Click.freq;\n";
        program << "    174.0 + (snareLevel" << suffix << " * 98.0) => lane" << suffix << "SnareBody.freq;\n";
        program << "    1650.0 + (bright * 1850.0) => lane" << suffix << "SnareSnap.freq;\n";
        program << "    " << volume << " * (0.96 + intensity * 0.92) * kickLevel" << suffix << " => lane" << suffix << "KickGain.gain;\n";
        program << "    " << volume << " * (0.52 + intensity * 0.42) * kickLevel" << suffix << " => lane" << suffix << "SubGain.gain;\n";
        program << "    " << volume << " * (0.30 + bright * 0.28) * kickClick" << suffix << " => lane" << suffix << "ClickGain.gain;\n";
        program << "    " << volume << " * (0.50 + intensity * 0.32) * snareLevel" << suffix << " => lane" << suffix << "SnareBodyGain.gain;\n";
        program << "    " << volume << " * (0.34 + bright * 0.32) * snareSnap" << suffix << " => lane" << suffix << "SnareSnapGain.gain;\n";
        program << "    " << volume << " * (0.38 + bright * 0.28) * snareLevel" << suffix << " => lane" << suffix << "SnareNoiseGain.gain;\n";
        program << "    " << volume << " * (0.18 + bright * 0.16) * snareClap" << suffix << " => lane" << suffix << "SnareClapGain.gain;\n";
        program << "    " << volume << " * (0.045 + bright * 0.065) * hatLevel" << suffix << " => lane" << suffix << "HatGain.gain;\n\n";
        return;
    }

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
    else if (lane.role == "arp")
    {
        program << "    laneFreq" << suffix << " * (0.99 + bright * 0.035 + orbit * 0.012) => lane" << suffix << ".freq;\n";
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
    program << "0.0 => float stepPhase;\n";
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
    program << "0.0 => float kickLevel0;\n";
    program << "0.0 => float kickLevel1;\n";
    program << "0.0 => float kickLevel2;\n";
    program << "0.0 => float kickLevel3;\n";
    program << "0.0 => float kickLevel4;\n";
    program << "0.0 => float kickClick0;\n";
    program << "0.0 => float kickClick1;\n";
    program << "0.0 => float kickClick2;\n";
    program << "0.0 => float kickClick3;\n";
    program << "0.0 => float kickClick4;\n";
    program << "0.0 => float snareLevel0;\n";
    program << "0.0 => float snareLevel1;\n";
    program << "0.0 => float snareLevel2;\n";
    program << "0.0 => float snareLevel3;\n";
    program << "0.0 => float snareLevel4;\n";
    program << "0.0 => float snareSnap0;\n";
    program << "0.0 => float snareSnap1;\n";
    program << "0.0 => float snareSnap2;\n";
    program << "0.0 => float snareSnap3;\n";
    program << "0.0 => float snareSnap4;\n";
    program << "0.0 => float snareClap0;\n";
    program << "0.0 => float snareClap1;\n";
    program << "0.0 => float snareClap2;\n";
    program << "0.0 => float snareClap3;\n";
    program << "0.0 => float snareClap4;\n";
    program << "0.0 => float hatLevel0;\n";
    program << "0.0 => float hatLevel1;\n";
    program << "0.0 => float hatLevel2;\n";
    program << "0.0 => float hatLevel3;\n";
    program << "0.0 => float hatLevel4;\n\n";
    program << "0.0 => float smoothedMaster;\n\n";
    program << "while (true)\n";
    program << "{\n";
    program << "    0 => int didTick;\n";
    program << "    Math.max(0.0, Math.min(hostMasterGain, 0.8)) => float masterLevel;\n";
    program << "    Math.max(0.2, Math.min(hostTempoHz, 6.0)) => float tempo;\n";
    program << "    Math.max(0.0, Math.min(hostIntensity, 1.0)) => float intensity;\n";
    program << "    Math.max(0.0, Math.min(hostBrightness, 1.0)) => float bright;\n";
    program << "    Math.max(0.0, Math.min(hostOrbitPhase, 1.0)) => float orbit;\n";
    program << "    smoothedMaster + ((masterLevel - smoothedMaster) * 0.055) => smoothedMaster;\n";
    program << "    smoothedMaster => master.gain;\n";
    program << "    stepPhase + (tempo * 0.020) => stepPhase;\n";
    program << "    if (stepPhase >= 1.0)\n";
    program << "    {\n";
    program << "        tick + 1 => tick;\n";
    program << "        stepPhase - 1.0 => stepPhase;\n";
    program << "        1 => didTick;\n";
    program << "    }\n\n";

    for (int i = 0; i < static_cast<int> (state.lanes.size()); ++i)
        appendLaneControl (program, state.lanes[static_cast<size_t> (i)], i);

    program << "    5::ms => now;\n";
    program << "}\n";
    return program;
}
}
