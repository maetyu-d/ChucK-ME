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

struct TrackSpec
{
    juce::String name;
    std::array<LaneSpec, 5> lanes {};
};

struct StateSpec
{
    juce::String name;
    double tempoBpm = 104.0;
    std::array<TrackSpec, 5> tracks {};
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

inline TrackSpec makeTrack (const juce::String& name,
                            const juce::String& stem,
                            float drumHz,
                            float bassHz,
                            float arpHz,
                            float chordHz,
                            float airHz,
                            float energy)
{
    const auto drumVolume = 0.54f + energy * 0.10f;
    const auto bassVolume = 0.30f + energy * 0.07f;
    const auto arpVolume = 0.22f + energy * 0.06f;
    const auto chordVolume = 0.19f + energy * 0.04f;
    const auto airVolume = 0.065f + energy * 0.018f;

    return
    {
        name,
        {{
            { "808 " + stem, "drum", drumHz, drumVolume, 16, 1 },
            { stem + " bass", "bass", bassHz, bassVolume, 2, 1 },
            { stem + " arp", "arp", arpHz, arpVolume, 1, 1 },
            { stem + " chord", "chord", chordHz, chordVolume, 8, 5 },
            { stem + " air", "air", airHz, airVolume, 12, 6 }
        }}
    };
}

inline std::vector<StateSpec> makeDefaultStates()
{
    return
    {
        { "State 1", 88.0, {{
            makeTrack ("Pocket City", "pocket", 45.0f, 65.41f, 261.63f, 130.81f, 1046.50f, 0.40f),
            makeTrack ("Chrome Avenue", "chrome", 46.5f, 73.42f, 293.66f, 146.83f, 1174.66f, 0.45f),
            makeTrack ("Battery Love", "battery", 43.8f, 82.41f, 329.63f, 164.81f, 1318.51f, 0.52f),
            makeTrack ("Pocket Choir", "choir", 44.2f, 61.74f, 246.94f, 123.47f, 987.77f, 0.34f),
            makeTrack ("Neon Postcard", "neon", 45.6f, 69.30f, 277.18f, 138.59f, 1108.73f, 0.46f)
        }}}
    };
}

inline juce::String chuckFloat (float value)
{
    return juce::String (static_cast<double> (value), 5);
}

inline std::vector<LaneSpec> flattenLanes (const StateSpec& state)
{
    std::vector<LaneSpec> lanes;
    lanes.reserve (state.tracks.size() * 5);

    for (const auto& track : state.tracks)
        for (const auto& lane : track.lanes)
            lanes.push_back (lane);

    return lanes;
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
        program << "SqrOsc lane" << suffix << "SnareDrive => Gain lane" << suffix << "SnareDriveGain => master;\n";
        program << "Noise lane" << suffix << "SnareNoise => Gain lane" << suffix << "SnareNoiseGain => master;\n";
        program << "Noise lane" << suffix << "SnareClap => Gain lane" << suffix << "SnareClapGain => master;\n";
        program << "Noise lane" << suffix << "HatNoise => Gain lane" << suffix << "HatGain => master;\n";
        program << "0.0 => lane" << suffix << "KickGain.gain;\n";
        program << "0.0 => lane" << suffix << "SubGain.gain;\n";
        program << "0.0 => lane" << suffix << "ClickGain.gain;\n";
        program << "0.0 => lane" << suffix << "SnareBodyGain.gain;\n";
        program << "0.0 => lane" << suffix << "SnareSnapGain.gain;\n";
        program << "0.0 => lane" << suffix << "SnareDriveGain.gain;\n";
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
        program << "    265.0 + (snareSnap" << suffix << " * 760.0) + (bright * 520.0) => lane" << suffix << "SnareDrive.freq;\n";
        program << "    " << volume << " * (0.96 + intensity * 0.92) * kickLevel" << suffix << " => lane" << suffix << "KickGain.gain;\n";
        program << "    " << volume << " * (0.52 + intensity * 0.42) * kickLevel" << suffix << " => lane" << suffix << "SubGain.gain;\n";
        program << "    " << volume << " * (0.30 + bright * 0.28) * kickClick" << suffix << " => lane" << suffix << "ClickGain.gain;\n";
        program << "    " << volume << " * (0.60 + intensity * 0.38) * snareLevel" << suffix << " => lane" << suffix << "SnareBodyGain.gain;\n";
        program << "    " << volume << " * (0.41 + bright * 0.38) * snareSnap" << suffix << " => lane" << suffix << "SnareSnapGain.gain;\n";
        program << "    " << volume << " * (0.18 + bright * 0.14) * (snareSnap" << suffix << " + (snareClap" << suffix << " * 0.32)) => lane" << suffix << "SnareDriveGain.gain;\n";
        program << "    " << volume << " * (0.46 + bright * 0.34) * snareLevel" << suffix << " => lane" << suffix << "SnareNoiseGain.gain;\n";
        program << "    " << volume << " * (0.22 + bright * 0.19) * snareClap" << suffix << " => lane" << suffix << "SnareClapGain.gain;\n";
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
    const auto lanes = flattenLanes (state);

    program << "Gain master => dac;\n";
    program << "0.0 => master.gain;\n\n";

    for (int i = 0; i < static_cast<int> (lanes.size()); ++i)
        appendLaneDeclaration (program, lanes[static_cast<size_t> (i)], i);

    program << "0 => int tick;\n";
    program << "0.0 => float stepPhase;\n";

    const std::array<juce::String, 9> laneVariables
    {
        "laneLevel",
        "laneFreq",
        "laneTarget",
        "kickLevel",
        "kickClick",
        "snareLevel",
        "snareSnap",
        "snareClap",
        "hatLevel"
    };

    for (const auto& variable : laneVariables)
    {
        for (int i = 0; i < static_cast<int> (lanes.size()); ++i)
            program << "0.0 => float " << variable << i << ";\n";

        program << "\n";
    }

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

    for (int i = 0; i < static_cast<int> (lanes.size()); ++i)
        appendLaneControl (program, lanes[static_cast<size_t> (i)], i);

    program << "    5::ms => now;\n";
    program << "}\n";
    return program;
}
}
