#pragma once

#include <WeldChucKEngine.h>

#include <array>
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
        { "Ember Gate", 101.0, {{
            { "Copper pulse", "pulse", 151.0f, 0.33f, 79, 15 },
            { "Low braid", "bass", 58.0f, 0.31f, 118, 29 },
            { "Small glass", "glass", 407.0f, 0.17f, 53, 10 },
            { "Warm lattice", "chord", 203.0f, 0.18f, 149, 52 },
            { "Edge shimmer", "air", 835.0f, 0.11f, 197, 78 }
        }}},
        { "Vector Rain", 109.0, {{
            { "Pinwheel clock", "pulse", 171.0f, 0.37f, 67, 16 },
            { "Rubber root", "bass", 64.0f, 0.33f, 103, 27 },
            { "Needle figures", "glass", 457.0f, 0.22f, 43, 9 },
            { "Mirror reply", "glass", 683.0f, 0.18f, 89, 15 },
            { "Under colour", "chord", 229.0f, 0.20f, 133, 44 }
        }}},
        { "Tidal Relay", 113.0, {{
            { "Buoy rhythm", "pulse", 188.0f, 0.35f, 63, 18 },
            { "Pressure line", "bass", 71.0f, 0.34f, 94, 26 },
            { "Foam beads", "glass", 503.0f, 0.23f, 37, 8 },
            { "Far answer", "glass", 601.0f, 0.18f, 81, 14 },
            { "Surface chord", "chord", 251.0f, 0.22f, 121, 43 }
        }}},
        { "Bright Fold", 118.0, {{
            { "Gate wheel", "pulse", 211.0f, 0.41f, 59, 19 },
            { "Fold bass", "bass", 84.0f, 0.36f, 87, 24 },
            { "White thread", "glass", 761.0f, 0.24f, 35, 8 },
            { "Counter sparks", "glass", 541.0f, 0.19f, 47, 9 },
            { "Open frame", "chord", 271.0f, 0.23f, 113, 48 }
        }}},
        { "Ash Field", 94.0, {{
            { "Slow hinge", "pulse", 129.0f, 0.24f, 131, 24 },
            { "Buried wire", "bass", 47.0f, 0.29f, 147, 42 },
            { "Black beads", "glass", 647.0f, 0.16f, 55, 8 },
            { "Soft geometry", "chord", 191.0f, 0.20f, 171, 68 },
            { "Top vapour", "air", 991.0f, 0.10f, 211, 92 }
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
    const auto baseHz = chuckFloat (lane.baseHz);
    const auto pulseTicks = chuckInt (lane.pulseTicks);
    const auto openTicks = chuckInt (lane.openTicks);
    const auto volume = chuckFloat (lane.volume);

    program << "    tick % " << pulseTicks << " => int laneStep" << suffix << ";\n";
    program << "    if (laneStep" << suffix << " < " << openTicks << ")\n";
    program << "        " << volume << " * (0.40 + intensity * 0.72) => laneTarget" << suffix << ";\n";
    program << "    else\n";
    program << "        " << volume << " * (0.035 + orbit * 0.08) => laneTarget" << suffix << ";\n";
    program << "    laneLevel" << suffix << " + ((laneTarget" << suffix << " - laneLevel" << suffix << ") * 0.075) => laneLevel" << suffix << ";\n";

    if (lane.role == "bass")
    {
        program << "    Math.max(28.0, " << baseHz << " * (0.55 + orbit * 0.16)) => lane" << suffix << ".freq;\n";
        program << "    160.0 + bright * 1280.0 + intensity * 260.0 => filter" << suffix << ".freq;\n";
    }
    else if (lane.role == "chord")
    {
        program << "    " << baseHz << " * (0.997 + orbit * 0.014) => lane" << suffix << "a.freq;\n";
        program << "    " << baseHz << " * 1.251 => lane" << suffix << "b.freq;\n";
        program << "    " << baseHz << " * 1.498 => lane" << suffix << "c.freq;\n";
    }
    else if (lane.role == "air")
    {
        program << "    " << baseHz << " * (0.85 + bright * 0.45 + orbit * 0.08) => lane" << suffix << ".freq;\n";
    }
    else
    {
        program << "    " << baseHz << " * (0.96 + bright * 0.22 + orbit * 0.06) => lane" << suffix << ".freq;\n";
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
