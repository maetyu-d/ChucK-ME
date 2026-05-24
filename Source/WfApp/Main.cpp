#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <WeldChucKEngine.h>

#include "WfChucKPrograms.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <random>
#include <regex>
#include <string>
#include <thread>

namespace
{
constexpr int maxHostChannels = 16;
constexpr int fallbackBlockSize = 4096;
constexpr int engineOutputChannels = 2;
constexpr int maxTopLevelStates = 16;
constexpr int maxStateTracks = 16;
constexpr int maxTrackLanes = 8;
constexpr int maxTrackEffectSlots = 3;
constexpr int maxGraphTransitions = 32;
constexpr int arrangementLeftPaneWidth = 292;
constexpr int arrangementRightPaneWidth = 292;
constexpr int arrangementPaneGap = 18;
constexpr int defaultTrackFocusCodePaneWidth = 390;
constexpr int minTrackFocusCodePaneWidth = 260;
constexpr int minTrackFocusCanvasWidth = 380;
constexpr int trackFocusPaneGap = 16;
constexpr int codeViewDividerHeight = 14;
constexpr int minCodeViewPaneHeight = 120;
constexpr float defaultPlaybackRate = 1.0f;
constexpr float defaultIntensity = 0.58f;
constexpr float defaultBrightness = 0.48f;
constexpr auto laneDeclarationMarker = "// wf::declaration";
constexpr auto laneControlMarker = "// wf::control";

juce::String defaultGlobalScriptText()
{
    return "tempo(88); timeSig(4, 4); playState(1, 8);\n"
           "tempo(44); playState(2, 4);\n"
           "stop();";
}

std::array<float, maxTopLevelStates> defaultTopLevelTempos()
{
    return
    {
        88.0f, 44.0f, 88.0f, 88.0f,
        88.0f, 88.0f, 88.0f, 88.0f,
        88.0f, 88.0f, 88.0f, 88.0f,
        88.0f, 88.0f, 88.0f, 88.0f
    };
}

std::array<int, maxTopLevelStates> defaultTopLevelTimeSigNumerators()
{
    return
    {
        4, 4, 4, 4,
        4, 4, 4, 4,
        4, 4, 4, 4,
        4, 4, 4, 4
    };
}

std::array<int, maxTopLevelStates> defaultTopLevelTimeSigDenominators()
{
    return
    {
        4, 4, 4, 4,
        4, 4, 4, 4,
        4, 4, 4, 4,
        4, 4, 4, 4
    };
}

struct GlobalScriptStep
{
    int stateIndex = 0;
    double bars = 1.0;
    std::optional<float> tempoBpm;
    std::optional<int> timeSigNumerator;
    std::optional<int> timeSigDenominator;
};

struct ImportedAudioRegion
{
    double startSeconds = 0.0;
    double sourceStartSeconds = 0.0;
    double lengthSeconds = 0.0;
    double fadeInSeconds = 0.0;
    double fadeOutSeconds = 0.0;
    float gain = 1.0f;
    int fadeInCurve = 1;
    int fadeOutCurve = 1;
};

struct ImportedLaneAudioClip
{
    int stateIndex = 0;
    int trackIndex = 0;
    int laneIndex = 0;
    juce::File file;
    std::shared_ptr<juce::AudioBuffer<float>> audioData;
    std::vector<float> waveformPeaks;
    double audioSampleRate = 48000.0;
    double lengthSeconds = 0.0;
    float mixGain = 1.0f;
    float mixPan = 0.0f;
    std::vector<ImportedAudioRegion> regions;
};

enum class ArrangementAudioTool
{
    pointer,
    scissors,
    fade
};

struct ArrangementAudioSelection
{
    int clipIndex = -1;
    int regionIndex = -1;

    bool isValid() const noexcept
    {
        return clipIndex >= 0 && regionIndex >= 0;
    }

    bool operator== (const ArrangementAudioSelection& other) const noexcept
    {
        return clipIndex == other.clipIndex && regionIndex == other.regionIndex;
    }
};

enum class RegionFadeCurve
{
    linear = 0,
    equalPower = 1,
    slow = 2,
    fast = 3
};

float applyRegionFadeCurve (float value, int curve)
{
    value = juce::jlimit (0.0f, 1.0f, value);

    switch (static_cast<RegionFadeCurve> (juce::jlimit (0, 3, curve)))
    {
        case RegionFadeCurve::linear:     return value;
        case RegionFadeCurve::equalPower: return std::sin (value * juce::MathConstants<float>::halfPi);
        case RegionFadeCurve::slow:       return value * value;
        case RegionFadeCurve::fast:       return std::sqrt (value);
    }

    return value;
}

juce::String regionFadeCurveName (int curve)
{
    switch (static_cast<RegionFadeCurve> (juce::jlimit (0, 3, curve)))
    {
        case RegionFadeCurve::linear:     return "linear";
        case RegionFadeCurve::equalPower: return "equal";
        case RegionFadeCurve::slow:       return "slow";
        case RegionFadeCurve::fast:       return "fast";
    }

    return "linear";
}

double regionEndSeconds (const ImportedAudioRegion& region)
{
    return region.startSeconds + juce::jmax (0.0, region.lengthSeconds);
}

bool regionContainsTime (const ImportedAudioRegion& region, double timeSeconds)
{
    return timeSeconds >= region.startSeconds && timeSeconds < regionEndSeconds (region);
}

void applyAutomaticCrossfades (std::vector<ImportedAudioRegion>& regions)
{
    if (regions.size() < 2)
        return;

    for (size_t i = 1; i < regions.size(); ++i)
    {
        auto& previous = regions[i - 1];
        auto& current = regions[i];
        const auto overlap = regionEndSeconds (previous) - current.startSeconds;
        if (overlap <= 0.000001)
            continue;

        const auto crossfade = juce::jmin (overlap,
                                           juce::jmax (0.0, previous.lengthSeconds),
                                           juce::jmax (0.0, current.lengthSeconds));
        previous.fadeOutSeconds = juce::jmax (previous.fadeOutSeconds, crossfade);
        current.fadeInSeconds = juce::jmax (current.fadeInSeconds, crossfade);
        previous.fadeOutCurve = juce::jlimit (0, 3, previous.fadeOutCurve);
        current.fadeInCurve = juce::jlimit (0, 3, current.fadeInCurve);
    }
}

float importedRegionFadeGain (const ImportedAudioRegion& region, double localSeconds)
{
    localSeconds = juce::jlimit (0.0, juce::jmax (0.0, region.lengthSeconds), localSeconds);
    auto gain = juce::jlimit (0.0f, 2.0f, region.gain);

    if (region.fadeInSeconds > 0.000001)
        gain *= applyRegionFadeCurve (static_cast<float> (localSeconds / region.fadeInSeconds), region.fadeInCurve);

    if (region.fadeOutSeconds > 0.000001)
        gain *= applyRegionFadeCurve (static_cast<float> ((region.lengthSeconds - localSeconds) / region.fadeOutSeconds), region.fadeOutCurve);

    return juce::jlimit (0.0f, 2.0f, gain);
}

double sourceSamplePositionForRegionTime (const ImportedAudioRegion& region,
                                          int64_t timelineSample,
                                          double timelineSampleRate,
                                          double sourceSampleRate)
{
    if (timelineSampleRate <= 0.0 || sourceSampleRate <= 0.0)
        return -1.0;

    const auto timelineSampleInRegion = static_cast<double> (timelineSample)
                                      - region.startSeconds * timelineSampleRate;
    return region.sourceStartSeconds * sourceSampleRate
         + timelineSampleInRegion * (sourceSampleRate / timelineSampleRate);
}

juce::Colour ink() { return juce::Colour (0xfffbfff8); }
juce::Colour mutedInk() { return juce::Colour (0xffb6c1bb); }
juce::Colour panel() { return juce::Colour (0xff111813); }
juce::Colour panelSoft() { return juce::Colour (0xff1d2520); }
juce::Colour green() { return juce::Colour (0xff9cffae); }
juce::Colour amber() { return juce::Colour (0xffffcf70); }
juce::Colour coral() { return juce::Colour (0xffff8e83); }
juce::Colour blue() { return juce::Colour (0xff9bd0ff); }
juce::Colour cyan() { return juce::Colour (0xff80f4e8); }
juce::Colour lime() { return juce::Colour (0xffcfff7c); }
juce::Colour rose() { return juce::Colour (0xffff9ab4); }
juce::Colour violet() { return juce::Colour (0xffc6b0ff); }

juce::Colour accentColourForIndex (int index)
{
    static const std::array<juce::Colour, 8> colours
    {
        green(),
        blue(),
        amber(),
        coral(),
        cyan(),
        lime(),
        rose(),
        violet()
    };

    const auto wrapped = ((index % static_cast<int> (colours.size())) + static_cast<int> (colours.size())) % static_cast<int> (colours.size());
    return colours[static_cast<size_t> (wrapped)];
}

juce::Colour laneAccentForIndex (int index)
{
    return accentColourForIndex (index);
}

juce::Colour stateAccentForIndex (int index)
{
    static const std::array<juce::Colour, 8> colours
    {
        green(),
        blue(),
        cyan(),
        amber(),
        coral(),
        lime(),
        violet(),
        rose()
    };

    const auto wrapped = ((index % static_cast<int> (colours.size())) + static_cast<int> (colours.size())) % static_cast<int> (colours.size());
    return colours[static_cast<size_t> (wrapped)];
}

std::array<float, 2> linearPanGains (float gain, float pan)
{
    pan = juce::jlimit (-1.0f, 1.0f, pan);
    return { gain * (pan <= 0.0f ? 1.0f : 1.0f - pan),
             gain * (pan >= 0.0f ? 1.0f : 1.0f + pan) };
}

float readInterpolatedSample (const juce::AudioBuffer<float>& buffer, int channel, double sourcePosition)
{
    if (buffer.getNumSamples() <= 0 || buffer.getNumChannels() <= 0)
        return 0.0f;

    channel = juce::jlimit (0, buffer.getNumChannels() - 1, channel);
    if (sourcePosition < 0.0 || sourcePosition >= static_cast<double> (buffer.getNumSamples() - 1))
    {
        const auto index = static_cast<int> (std::floor (sourcePosition));
        return index >= 0 && index < buffer.getNumSamples() ? buffer.getSample (channel, index) : 0.0f;
    }

    const auto index = static_cast<int> (std::floor (sourcePosition));
    const auto alpha = static_cast<float> (sourcePosition - static_cast<double> (index));
    const auto a = buffer.getSample (channel, index);
    const auto b = buffer.getSample (channel, index + 1);
    return a + (b - a) * alpha;
}

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
    button.setColour (juce::TextButton::buttonColourId, colour.withAlpha (0.085f));
    button.setColour (juce::TextButton::buttonOnColourId, colour.withAlpha (0.36f));
    button.setColour (juce::TextButton::textColourOffId, ink().withAlpha (0.84f));
    button.setColour (juce::TextButton::textColourOnId, ink());
    button.setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void styleEmptyStateButton (juce::TextButton& button)
{
    button.setColour (juce::TextButton::buttonColourId, panel().withAlpha (0.54f));
    button.setColour (juce::TextButton::buttonOnColourId, panel().withAlpha (0.54f));
    button.setColour (juce::TextButton::textColourOffId, mutedInk().withAlpha (0.25f));
    button.setColour (juce::TextButton::textColourOnId, mutedInk().withAlpha (0.25f));
    button.setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void styleSlider (juce::Slider& slider, juce::Colour colour)
{
    slider.setSliderStyle (juce::Slider::LinearHorizontal);
    slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 56, 22);
    slider.setColour (juce::Slider::trackColourId, colour);
    slider.setColour (juce::Slider::thumbColourId, ink());
    slider.setColour (juce::Slider::backgroundColourId, mutedInk().withAlpha (0.10f));
    slider.setColour (juce::Slider::textBoxTextColourId, ink());
    slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
}

void styleComboBox (juce::ComboBox& comboBox)
{
    comboBox.setColour (juce::ComboBox::backgroundColourId, panelSoft().withAlpha (0.60f));
    comboBox.setColour (juce::ComboBox::textColourId, ink());
    comboBox.setColour (juce::ComboBox::outlineColourId, mutedInk().withAlpha (0.12f));
    comboBox.setColour (juce::ComboBox::arrowColourId, mutedInk().withAlpha (0.78f));
}

void styleLabel (juce::Label& label, float alpha = 1.0f)
{
    label.setColour (juce::Label::textColourId, ink().withAlpha (alpha));
    label.setJustificationType (juce::Justification::centredLeft);
}

juce::String trackDisplayName (const juce::String& name)
{
    return name.toUpperCase();
}

class MinimalLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    MinimalLookAndFeel()
    {
        setColour (juce::CaretComponent::caretColourId, ink());
    }

    void drawButtonBackground (juce::Graphics& g,
                               juce::Button& button,
                               const juce::Colour& backgroundColour,
                               bool shouldDrawButtonAsHighlighted,
                               bool shouldDrawButtonAsDown) override
    {
        auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);
        const auto enabled = button.isEnabled();
        const auto toggled = button.getToggleState();
        const auto accent = button.findColour (juce::TextButton::buttonOnColourId).withAlpha (1.0f);
        auto fill = toggled ? button.findColour (juce::TextButton::buttonOnColourId)
                            : backgroundColour;

        if (shouldDrawButtonAsDown)
            fill = toggled ? fill.brighter (0.10f) : accent.withAlpha (0.17f);
        else if (shouldDrawButtonAsHighlighted)
            fill = toggled ? fill.brighter (0.06f) : accent.withAlpha (0.13f);

        if (! enabled)
            fill = panelSoft().withAlpha (0.46f);

        g.setColour (fill);
        g.fillRoundedRectangle (bounds, 4.0f);

        const auto lineColour = toggled ? accent.withAlpha (enabled ? 0.70f : 0.16f)
                                        : accent.withAlpha (enabled ? 0.26f : 0.08f);
        g.setColour (lineColour);
        g.drawRoundedRectangle (bounds, 4.0f, toggled ? 1.0f : 0.7f);

        if (toggled && enabled)
        {
            g.setColour (accent.withAlpha (0.66f));
            g.fillRoundedRectangle (bounds.reduced (5.0f, 0.0f).removeFromTop (2.0f), 1.0f);
        }
    }

    void drawButtonText (juce::Graphics& g,
                         juce::TextButton& button,
                         bool,
                         bool) override
    {
        const auto enabled = button.isEnabled();
        const auto textColour = button.findColour (button.getToggleState()
                                                       ? juce::TextButton::textColourOnId
                                                       : juce::TextButton::textColourOffId);
        g.setColour (enabled ? textColour : textColour.withAlpha (0.28f));
        g.setFont (juce::FontOptions (juce::jlimit (11.0f, 14.0f, static_cast<float> (button.getHeight()) * 0.38f),
                                      juce::Font::bold));
        g.drawFittedText (button.getButtonText(),
                          button.getLocalBounds().reduced (6, 2),
                          juce::Justification::centred,
                          1);
    }

    void fillTextEditorBackground (juce::Graphics& g,
                                   int width,
                                   int height,
                                   juce::TextEditor& editor) override
    {
        g.setColour (editor.findColour (juce::TextEditor::backgroundColourId));
        g.fillRoundedRectangle (juce::Rectangle<float> (0.5f, 0.5f, static_cast<float> (width) - 1.0f, static_cast<float> (height) - 1.0f),
                                3.0f);
    }

    void drawTextEditorOutline (juce::Graphics& g,
                                int width,
                                int height,
                                juce::TextEditor& editor) override
    {
        const auto focused = editor.hasKeyboardFocus (true);
        g.setColour (editor.findColour (focused ? juce::TextEditor::focusedOutlineColourId
                                                : juce::TextEditor::outlineColourId));
        g.drawRoundedRectangle (juce::Rectangle<float> (0.5f, 0.5f, static_cast<float> (width) - 1.0f, static_cast<float> (height) - 1.0f),
                                3.0f,
                                focused ? 1.1f : 0.7f);
    }

    void drawComboBox (juce::Graphics& g,
                       int width,
                       int height,
                       bool isButtonDown,
                       int,
                       int,
                       int,
                       int,
                       juce::ComboBox& box) override
    {
        auto bounds = juce::Rectangle<float> (0.5f, 0.5f, static_cast<float> (width) - 1.0f, static_cast<float> (height) - 1.0f);
        g.setColour (box.findColour (juce::ComboBox::backgroundColourId).brighter (isButtonDown ? 0.05f : 0.0f));
        g.fillRoundedRectangle (bounds, 4.0f);
        g.setColour (box.findColour (juce::ComboBox::outlineColourId));
        g.drawRoundedRectangle (bounds, 4.0f, 0.8f);

        const auto cx = static_cast<float> (width - 17);
        const auto cy = static_cast<float> (height) * 0.52f;
        juce::Path arrow;
        arrow.startNewSubPath (cx - 4.0f, cy - 2.0f);
        arrow.lineTo (cx, cy + 2.5f);
        arrow.lineTo (cx + 4.0f, cy - 2.0f);
        g.setColour (box.findColour (juce::ComboBox::arrowColourId));
        g.strokePath (arrow, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    void drawLinearSlider (juce::Graphics& g,
                           int x,
                           int y,
                           int width,
                           int height,
                           float sliderPos,
                           float minSliderPos,
                           float maxSliderPos,
                           const juce::Slider::SliderStyle style,
                           juce::Slider& slider) override
    {
        if (style != juce::Slider::LinearHorizontal && style != juce::Slider::LinearBar)
        {
            juce::LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
            return;
        }

        auto bounds = juce::Rectangle<float> (static_cast<float> (x),
                                             static_cast<float> (y),
                                             static_cast<float> (width),
                                             static_cast<float> (height)).reduced (2.0f, 0.0f);
        auto track = bounds.withHeight (4.0f).withCentre ({ bounds.getCentreX(), bounds.getCentreY() });
        sliderPos = juce::jlimit (track.getX(), track.getRight(), sliderPos);

        g.setColour (slider.findColour (juce::Slider::backgroundColourId));
        g.fillRoundedRectangle (track, 2.0f);

        auto active = track.withRight (sliderPos);
        g.setColour (slider.findColour (juce::Slider::trackColourId));
        g.fillRoundedRectangle (active, 2.0f);

        g.setColour (slider.findColour (juce::Slider::thumbColourId));
        g.fillEllipse (sliderPos - 5.0f, track.getCentreY() - 5.0f, 10.0f, 10.0f);
    }
};

class CodeTextEditor final : public juce::TextEditor
{
public:
    enum class SyntaxMode
    {
        chuck,
        conductor
    };

    CodeTextEditor()
    {
        setMultiLine (true, false);
        setReturnKeyStartsNewLine (true);
        setTabKeyUsedAsCharacter (true);
        setScrollbarsShown (true);
        setScrollBarThickness (7);
        setPopupMenuEnabled (true);
        setLineSpacing (1.08f);
        setBorder ({ 0, 0, 0, 0 });
        setIndents (gutterWidth + 10, 8);
        setCodeFontSize (12.0f);
    }

    void setSyntaxMode (SyntaxMode mode)
    {
        syntaxMode = mode;
        repaint();
    }

    void setCodeFontSize (float size)
    {
        const juce::Font font { juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), size, juce::Font::plain) };
        setFont (font);
        applyFontToAllText (font);
    }

    void refreshBaseTextColour()
    {
        applyColourToAllText (findColour (juce::TextEditor::textColourId), true);
        repaint();
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        const auto modifiers = key.getModifiers();
        const auto keyCode = key.getKeyCode();

        if (keyCode == juce::KeyPress::tabKey)
        {
            if (modifiers.isShiftDown())
                unindentSelection();
            else
                indentSelection();

            return true;
        }

        if (modifiers.isCommandDown() && keyCode == '/')
        {
            toggleLineComments();
            return true;
        }

        if (keyCode == juce::KeyPress::returnKey && ! modifiers.isCommandDown() && ! modifiers.isAltDown())
        {
            insertSmartNewLine();
            return true;
        }

        if (! modifiers.isCommandDown() && ! modifiers.isCtrlDown() && ! modifiers.isAltDown())
        {
            const auto character = key.getTextCharacter();
            if (handleBracketKey (character))
                return true;
        }

        return juce::TextEditor::keyPressed (key);
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        juce::TextEditor::paintOverChildren (g);
        drawMatchingBracketHighlight (g);
        drawSyntaxOverlay (g);
        drawLineNumberGutter (g);
    }

private:
    static constexpr int gutterWidth = 44;
    static constexpr int tabSpaces = 4;

    SyntaxMode syntaxMode = SyntaxMode::chuck;

    static bool isLineBreak (juce::juce_wchar character) noexcept
    {
        return character == '\n' || character == '\r';
    }

    static bool isIndentCharacter (juce::juce_wchar character) noexcept
    {
        return character == ' ' || character == '\t';
    }

    static int findLineStart (const juce::String& text, int position)
    {
        position = juce::jlimit (0, text.length(), position);
        while (position > 0 && ! isLineBreak (text[position - 1]))
            --position;

        return position;
    }

    static int findLineEnd (const juce::String& text, int position)
    {
        position = juce::jlimit (0, text.length(), position);
        while (position < text.length() && ! isLineBreak (text[position]))
            ++position;

        return position;
    }

    static juce::String leadingIndentOf (const juce::String& text)
    {
        auto indentEnd = 0;
        while (indentEnd < text.length() && isIndentCharacter (text[indentEnd]))
            ++indentEnd;

        return text.substring (0, indentEnd);
    }

    static bool isIdentifierStart (juce::juce_wchar character) noexcept
    {
        return (character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z')
            || character == '_';
    }

    static bool isIdentifierBody (juce::juce_wchar character) noexcept
    {
        return isIdentifierStart (character)
            || (character >= '0' && character <= '9');
    }

    static bool isDigit (juce::juce_wchar character) noexcept
    {
        return character >= '0' && character <= '9';
    }

    static bool isOperatorCharacter (juce::juce_wchar character) noexcept
    {
        return character == '=' || character == '>' || character == '<'
            || character == '+' || character == '-' || character == '*'
            || character == '/' || character == '%' || character == '!'
            || character == ':' || character == ';' || character == ','
            || character == '.';
    }

    static bool isOpeningBracket (juce::juce_wchar character) noexcept
    {
        return character == '(' || character == '[' || character == '{';
    }

    static bool isClosingBracket (juce::juce_wchar character) noexcept
    {
        return character == ')' || character == ']' || character == '}';
    }

    static bool isBracket (juce::juce_wchar character) noexcept
    {
        return isOpeningBracket (character) || isClosingBracket (character);
    }

    static juce::juce_wchar matchingCloseFor (juce::juce_wchar character) noexcept
    {
        if (character == '(') return ')';
        if (character == '[') return ']';
        if (character == '{') return '}';
        return 0;
    }

    static juce::juce_wchar matchingOpenFor (juce::juce_wchar character) noexcept
    {
        if (character == ')') return '(';
        if (character == ']') return '[';
        if (character == '}') return '{';
        return 0;
    }

    static bool wordIn (const juce::String& word, std::initializer_list<const char*> words, bool ignoreCase = false)
    {
        for (const auto* candidate : words)
            if (ignoreCase ? word.equalsIgnoreCase (candidate) : word == candidate)
                return true;

        return false;
    }

    static bool isChucKKeyword (const juce::String& word)
    {
        return wordIn (word,
                       { "adc", "blackhole", "break", "continue", "dac", "else", "float",
                         "for", "fun", "if", "int", "me", "new", "now", "return", "spork",
                         "string", "time", "while" });
    }

    static bool isChucKTypeOrUGen (const juce::String& word)
    {
        return wordIn (word,
                       { "ADSR", "BiQuad", "BPF", "Delay", "Dyno", "Envelope", "Gain",
                         "HPF", "LPF", "Noise", "Pan2", "Phasor", "SinOsc", "SndBuf",
                         "SqrOsc", "Step", "TriOsc" });
    }

    static bool isHostWord (const juce::String& word)
    {
        return word.startsWith ("laneActive")
            || wordIn (word, { "bright", "didTick", "intensity", "master", "orbit", "stepPhase", "tick" });
    }

    static bool isConductorWord (const juce::String& word)
    {
        return wordIn (word, { "bar", "bars", "bpm", "meter", "playState", "state", "stop", "tempo", "timeSig" }, true);
    }

    static bool isPunctuationBracket (juce::juce_wchar character) noexcept
    {
        return character == '(' || character == ')' || character == '[' || character == ']'
            || character == '{' || character == '}';
    }

    static juce::String indentLine (const juce::String& line)
    {
        return juce::String::repeatedString (" ", tabSpaces) + line;
    }

    static juce::String unindentLine (const juce::String& line)
    {
        if (line.startsWithChar ('\t'))
            return line.substring (1);

        auto spacesToRemove = 0;
        while (spacesToRemove < juce::jmin (tabSpaces, line.length()) && line[spacesToRemove] == ' ')
            ++spacesToRemove;

        return line.substring (spacesToRemove);
    }

    static std::vector<juce::String> splitLinesPreservingEmptyLines (const juce::String& text)
    {
        std::vector<juce::String> lines;
        auto lineStart = 0;

        for (int i = 0; i < text.length(); ++i)
        {
            if (isLineBreak (text[i]))
            {
                lines.push_back (text.substring (lineStart, i));

                if (text[i] == '\r' && i + 1 < text.length() && text[i + 1] == '\n')
                    ++i;

                lineStart = i + 1;
            }
        }

        lines.push_back (text.substring (lineStart));
        return lines;
    }

    static juce::String joinLines (const std::vector<juce::String>& lines)
    {
        juce::String joined;

        for (size_t i = 0; i < lines.size(); ++i)
        {
            if (i > 0)
                joined << "\n";

            joined << lines[i];
        }

        return joined;
    }

    bool handleBracketKey (juce::juce_wchar character)
    {
        const auto close = matchingCloseFor (character);
        if (close != 0)
        {
            const auto selection = getHighlightedRegion();
            const auto selectedText = getTextInRange (selection);
            const auto insertionStart = selection.getStart();
            insertTextAtCaret (juce::String::charToString (character)
                               + selectedText
                               + juce::String::charToString (close));

            if (selectedText.isEmpty())
                setCaretPosition (insertionStart + 1);
            else
                setHighlightedRegion ({ insertionStart + 1, insertionStart + 1 + selectedText.length() });

            return true;
        }

        if (isClosingBracket (character) && getHighlightedRegion().isEmpty())
        {
            const auto text = getText();
            const auto caret = getCaretPosition();
            if (caret < text.length() && text[caret] == character)
            {
                setCaretPosition (caret + 1);
                return true;
            }
        }

        return false;
    }

    juce::Range<int> getSelectedLineRange() const
    {
        const auto text = getText();
        const auto selection = getHighlightedRegion();
        const auto start = juce::jlimit (0, text.length(), selection.getStart());
        auto end = juce::jlimit (0, text.length(), selection.getEnd());

        if (end > start && end <= text.length() && isLineBreak (text[end - 1]))
            --end;

        return { findLineStart (text, start), findLineEnd (text, end) };
    }

    void replaceRangeAndSelect (juce::Range<int> range, const juce::String& replacement)
    {
        setHighlightedRegion (range);
        insertTextAtCaret (replacement);
        setHighlightedRegion ({ range.getStart(), range.getStart() + replacement.length() });
    }

    void indentSelection()
    {
        const auto selection = getHighlightedRegion();
        if (selection.isEmpty())
        {
            insertTextAtCaret (juce::String::repeatedString (" ", tabSpaces));
            return;
        }

        const auto range = getSelectedLineRange();
        auto lines = splitLinesPreservingEmptyLines (getTextInRange (range));
        for (auto& line : lines)
            line = indentLine (line);

        replaceRangeAndSelect (range, joinLines (lines));
    }

    void unindentSelection()
    {
        const auto range = getSelectedLineRange();
        auto lines = splitLinesPreservingEmptyLines (getTextInRange (range));
        for (auto& line : lines)
            line = unindentLine (line);

        replaceRangeAndSelect (range, joinLines (lines));
    }

    void toggleLineComments()
    {
        const auto range = getSelectedLineRange();
        auto lines = splitLinesPreservingEmptyLines (getTextInRange (range));
        auto allCommented = true;

        for (const auto& line : lines)
        {
            const auto trimmed = line.trimStart();
            if (trimmed.isNotEmpty() && ! trimmed.startsWith ("//"))
            {
                allCommented = false;
                break;
            }
        }

        for (auto& line : lines)
        {
            const auto indent = leadingIndentOf (line);
            auto body = line.substring (indent.length());

            if (allCommented)
            {
                if (body.startsWith ("// "))
                    body = body.substring (3);
                else if (body.startsWith ("//"))
                    body = body.substring (2);
            }
            else if (body.isNotEmpty())
            {
                body = "// " + body;
            }

            line = indent + body;
        }

        replaceRangeAndSelect (range, joinLines (lines));
    }

    void insertSmartNewLine()
    {
        const auto text = getText();
        const auto caret = getCaretPosition();
        const auto lineStart = findLineStart (text, caret);
        const auto lineBeforeCaret = text.substring (lineStart, caret);
        auto indent = leadingIndentOf (lineBeforeCaret);
        auto scan = caret - 1;

        while (scan >= 0 && (text[scan] == ' ' || text[scan] == '\t'))
            --scan;

        auto next = caret;
        while (next < text.length() && (text[next] == ' ' || text[next] == '\t'))
            ++next;

        if (scan >= 0
            && next < text.length()
            && matchingCloseFor (text[scan]) == text[next])
        {
            auto innerIndent = indent + juce::String::repeatedString (" ", tabSpaces);
            insertTextAtCaret ("\n" + innerIndent + "\n" + indent);
            setCaretPosition (caret + 1 + innerIndent.length());
            return;
        }

        if (scan >= 0 && (text[scan] == '{' || text[scan] == '(' || text[scan] == '['))
            indent << juce::String::repeatedString (" ", tabSpaces);

        insertTextAtCaret ("\n" + indent);
    }

    std::optional<std::pair<int, int>> findMatchingBracketPair() const
    {
        const auto text = getText();
        if (text.isEmpty())
            return {};

        const auto caret = juce::jlimit (0, text.length(), getCaretPosition());
        auto bracketIndex = -1;

        if (caret < text.length() && isBracket (text[caret]))
            bracketIndex = caret;
        else if (caret > 0 && isBracket (text[caret - 1]))
            bracketIndex = caret - 1;

        if (bracketIndex < 0)
            return {};

        const auto bracket = text[bracketIndex];
        if (isOpeningBracket (bracket))
        {
            const auto close = matchingCloseFor (bracket);
            auto depth = 0;
            for (auto index = bracketIndex; index < text.length(); ++index)
            {
                if (text[index] == bracket)
                    ++depth;
                else if (text[index] == close && --depth == 0)
                    return std::pair<int, int> { bracketIndex, index };
            }
        }
        else
        {
            const auto open = matchingOpenFor (bracket);
            auto depth = 0;
            for (auto index = bracketIndex; index >= 0; --index)
            {
                if (text[index] == bracket)
                    ++depth;
                else if (text[index] == open && --depth == 0)
                    return std::pair<int, int> { index, bracketIndex };
            }
        }

        return {};
    }

    void drawMatchingBracketHighlight (juce::Graphics& g) const
    {
        const auto pair = findMatchingBracketPair();
        if (! pair.has_value())
            return;

        drawBracketHighlight (g, pair->first);
        drawBracketHighlight (g, pair->second);
    }

    void drawBracketHighlight (juce::Graphics& g, int characterIndex) const
    {
        const auto bounds = getTextBounds ({ characterIndex, characterIndex + 1 }).getBounds().toFloat().expanded (2.0f, 1.0f);
        if (bounds.isEmpty())
            return;

        g.setColour (amber().withAlpha (0.18f));
        g.fillRoundedRectangle (bounds, 2.0f);
        g.setColour (amber().withAlpha (0.58f));
        g.drawRoundedRectangle (bounds, 2.0f, 1.0f);
    }

    void drawSyntaxOverlay (juce::Graphics& g) const
    {
        const auto text = getText();
        if (text.isEmpty())
            return;

        g.setFont (getFont());

        for (auto index = 0; index < text.length();)
        {
            const auto character = text[index];

            if (isLineBreak (character) || character == ' ' || character == '\t')
            {
                ++index;
                continue;
            }

            if (character == '/' && index + 1 < text.length() && text[index + 1] == '/')
            {
                auto end = index + 2;
                while (end < text.length() && ! isLineBreak (text[end]))
                    ++end;

                const auto comment = text.substring (index, end);
                const auto colour = comment.startsWith (laneDeclarationMarker) || comment.startsWith (laneControlMarker)
                    ? cyan().withAlpha (0.88f)
                    : mutedInk().withAlpha (0.74f);
                drawTextRange (g, text, index, end, colour);
                index = end;
                continue;
            }

            if (character == '"' || character == '\'')
            {
                const auto start = index++;
                auto escaped = false;
                while (index < text.length())
                {
                    const auto current = text[index++];
                    if (escaped)
                    {
                        escaped = false;
                        continue;
                    }

                    if (current == '\\')
                    {
                        escaped = true;
                        continue;
                    }

                    if (current == character)
                        break;
                }

                drawTextRange (g, text, start, index, amber().withAlpha (0.94f));
                continue;
            }

            if (isDigit (character) || (character == '.' && index + 1 < text.length() && isDigit (text[index + 1])))
            {
                const auto start = index++;
                while (index < text.length() && (isDigit (text[index]) || text[index] == '.'))
                    ++index;

                drawTextRange (g, text, start, index, rose().withAlpha (0.96f));
                continue;
            }

            if (isIdentifierStart (character))
            {
                const auto start = index++;
                while (index < text.length() && isIdentifierBody (text[index]))
                    ++index;

                const auto token = text.substring (start, index);
                drawTextRange (g, text, start, index, colourForWord (token, nextNonWhitespaceCharacter (text, index)));
                continue;
            }

            if (isPunctuationBracket (character))
            {
                drawTextRange (g, text, index, index + 1, violet().withAlpha (0.90f));
                ++index;
                continue;
            }

            if (isOperatorCharacter (character))
            {
                const auto start = index++;
                while (index < text.length() && isOperatorCharacter (text[index]))
                    ++index;

                drawTextRange (g, text, start, index, amber().withAlpha (0.76f));
                continue;
            }

            drawTextRange (g, text, index, index + 1, ink().withAlpha (0.88f));
            ++index;
        }
    }

    juce::Colour colourForWord (const juce::String& word, juce::juce_wchar nextNonWhitespace) const
    {
        if (syntaxMode == SyntaxMode::conductor && isConductorWord (word))
            return word.equalsIgnoreCase ("bar") || word.equalsIgnoreCase ("bars")
                ? amber().withAlpha (0.92f)
                : green().withAlpha (0.96f);

        if (syntaxMode == SyntaxMode::chuck)
        {
            if (isHostWord (word))
                return blue().withAlpha (0.94f);

            if (isChucKTypeOrUGen (word))
                return cyan().withAlpha (0.94f);

            if (isChucKKeyword (word))
                return violet().withAlpha (0.94f);
        }

        if (nextNonWhitespace == '(')
            return green().withAlpha (0.92f);

        return ink().withAlpha (0.90f);
    }

    static juce::juce_wchar nextNonWhitespaceCharacter (const juce::String& text, int index)
    {
        while (index < text.length() && (text[index] == ' ' || text[index] == '\t'))
            ++index;

        return index < text.length() ? text[index] : 0;
    }

    void drawTextRange (juce::Graphics& g, const juce::String& text, int start, int end, juce::Colour colour) const
    {
        if (end <= start)
            return;

        auto bounds = getTextBounds ({ start, end }).getBounds();
        if (bounds.isEmpty())
            return;

        bounds.setWidth (bounds.getWidth() + 5);
        g.setColour (colour);
        g.drawText (text.substring (start, end), bounds, juce::Justification::centredLeft, false);
    }

    void drawLineNumberGutter (juce::Graphics& g)
    {
        const auto bounds = getLocalBounds();
        g.setColour (juce::Colour (0xff0b120d).withAlpha (0.84f));
        g.fillRect (bounds.withWidth (gutterWidth));

        g.setColour (blue().withAlpha (0.18f));
        g.drawVerticalLine (gutterWidth, 4.0f, static_cast<float> (bounds.getBottom() - 4));

        const auto text = getText();
        const auto totalChars = text.length();
        const auto caretLineStart = findLineStart (text, getCaretPosition());
        auto lineStart = 0;
        auto lineNumber = 1;

        g.setFont (juce::FontOptions (10.5f, juce::Font::plain));

        while (lineStart <= totalChars)
        {
            const auto sampleEnd = lineStart < totalChars ? juce::jmin (totalChars, lineStart + 1) : totalChars;
            const auto textBounds = getTextBounds ({ lineStart, sampleEnd }).getBounds();
            const auto lineY = textBounds.isEmpty() ? 8 + (lineNumber - 1) * 15 : textBounds.getY();
            const auto lineHeight = textBounds.isEmpty() ? 15 : juce::jmax (14, textBounds.getHeight());

            if (lineY > bounds.getBottom())
                break;

            if (lineY + lineHeight >= 0)
            {
                const auto currentLine = lineStart == caretLineStart;
                if (currentLine)
                {
                    g.setColour (blue().withAlpha (0.12f));
                    g.fillRoundedRectangle (juce::Rectangle<float> (4.0f,
                                                                    static_cast<float> (lineY),
                                                                    static_cast<float> (gutterWidth - 8),
                                                                    static_cast<float> (lineHeight)),
                                            2.0f);
                }

                g.setColour ((currentLine ? blue() : mutedInk()).withAlpha (currentLine ? 0.86f : 0.34f));
                g.drawText (juce::String (lineNumber),
                            0,
                            lineY,
                            gutterWidth - 8,
                            lineHeight,
                            juce::Justification::centredRight,
                            false);
            }

            auto nextLineStart = lineStart;
            while (nextLineStart < totalChars && ! isLineBreak (text[nextLineStart]))
                ++nextLineStart;

            if (nextLineStart >= totalChars)
                break;

            if (text[nextLineStart] == '\r' && nextLineStart + 1 < totalChars && text[nextLineStart + 1] == '\n')
                ++nextLineStart;

            lineStart = nextLineStart + 1;
            ++lineNumber;
        }
    }
};
}

class WfAudioCallback final : public juce::AudioIODeviceCallback
{
    struct PlaybackMode
    {
        enum Type
        {
            chuckEngine = 0,
            importedAudio = 1
        };
    };

    struct ImportedPlaybackClip
    {
        int effectGroupIndex = -1;
        std::shared_ptr<const juce::AudioBuffer<float>> audio;
        std::vector<ImportedAudioRegion> regions;
        double sampleRate = 48000.0;
        std::shared_ptr<std::atomic<float>> gain;
        std::shared_ptr<std::atomic<float>> pan;
    };

    struct ImportedTrackEffectGroup
    {
        int stateIndex = -1;
        int trackIndex = -1;
        juce::AudioBuffer<float> buffer;
        std::array<std::unique_ptr<juce::AudioPluginInstance>, maxTrackEffectSlots> effects;
        juce::MidiBuffer midi;

        ~ImportedTrackEffectGroup()
        {
            releaseResources();
        }

        void releaseResources()
        {
            for (auto& effect : effects)
            {
                if (effect != nullptr)
                    effect->releaseResources();

                effect.reset();
            }
        }
    };

    struct ImportedPlaybackSet
    {
        std::vector<ImportedPlaybackClip> clips;
        std::vector<std::unique_ptr<ImportedTrackEffectGroup>> trackEffectGroups;
        std::unique_ptr<ImportedTrackEffectGroup> masterEffectGroup;
        double lengthSeconds = 0.0;
    };

public:
    WfAudioCallback()
    {
        juce::addDefaultFormatsToManager (pluginFormatManager);
    }

    void audioDeviceAboutToStart (juce::AudioIODevice* device) override
    {
        const auto sampleRate = device != nullptr ? device->getCurrentSampleRate() : 44100.0;
        const auto reportedBlockSize = device != nullptr ? device->getCurrentBufferSizeSamples() : 0;
        prepare (sampleRate, reportedBlockSize);
    }

    void audioDeviceStopped() override
    {
        ready.store (false, std::memory_order_release);
        importedPlaying.store (false, std::memory_order_release);

        for (auto& slot : slots)
        {
            slot.inUse.store (false, std::memory_order_release);
            slot.targetGain.store (0.0f, std::memory_order_release);
            slot.gain = 0.0f;
            slot.engine.release();
            slot.output.setSize (0, 0);
            slot.indexesReady = false;
        }
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
            || numSamples > preparedBlockSize.load (std::memory_order_relaxed)
            || numOutputChannels <= 0
            || numOutputChannels > maxHostChannels)
            return;

        juce::ScopedNoDenormals noDenormals;

        if (playbackMode.load (std::memory_order_acquire) == PlaybackMode::importedAudio)
        {
            processImportedAudio (outputChannelData, numOutputChannels, numSamples);
            return;
        }

        for (auto& slot : slots)
        {
            if (! slot.inUse.load (std::memory_order_acquire)
                || ! slot.engine.isReady()
                || numSamples > slot.output.getNumSamples()
                || numOutputChannels > slot.output.getNumChannels())
                continue;

            slot.output.clear (0, numSamples);
            juce::AudioBuffer<float> slotView (slot.output.getArrayOfWritePointers(),
                                               numOutputChannels,
                                               numSamples);
            slot.engine.process (emptyInput, slotView);

            const auto startGain = slot.gain;
            const auto targetGain = slot.targetGain.load (std::memory_order_acquire);
            const auto step = static_cast<float> (static_cast<double> (numSamples)
                                                  / juce::jmax (1.0, lastSampleRate.load (std::memory_order_relaxed) * tailFadeSeconds));
            const auto endGain = targetGain > startGain
                                   ? juce::jmin (targetGain, startGain + step)
                                   : juce::jmax (targetGain, startGain - step);

            for (int channel = 0; channel < numOutputChannels; ++channel)
            {
                auto* dst = outputChannelData[channel];
                const auto* src = slot.output.getReadPointer (channel);

                if (dst == nullptr || src == nullptr)
                    continue;

                for (int sample = 0; sample < numSamples; ++sample)
                {
                    const auto alpha = numSamples > 1 ? static_cast<float> (sample) / static_cast<float> (numSamples - 1) : 1.0f;
                    dst[sample] += src[sample] * (startGain + (endGain - startGain) * alpha);
                }
            }

            slot.gain = endGain;
            if (targetGain <= 0.0f && endGain <= 0.0001f && slot.index != activeSlot.load (std::memory_order_acquire))
                slot.inUse.store (false, std::memory_order_release);
        }
    }

    bool loadState (const Wf::StateSpec& state)
    {
        return loadStateWithControls (state,
                                      lastMasterGain.load (std::memory_order_relaxed),
                                      lastTempoHz.load (std::memory_order_relaxed),
                                      lastIntensity.load (std::memory_order_relaxed),
                                      lastBrightness.load (std::memory_order_relaxed),
                                      lastOrbitPhase.load (std::memory_order_relaxed));
    }

    bool loadStateWithControls (const Wf::StateSpec& state,
                                float masterGain,
                                float tempoHz,
                                float intensity,
                                float brightness,
                                float orbitPhase)
    {
        if (! ready.load (std::memory_order_acquire))
            return false;

        lastMasterGain.store (masterGain, std::memory_order_relaxed);
        lastTempoHz.store (tempoHz, std::memory_order_relaxed);
        lastIntensity.store (intensity, std::memory_order_relaxed);
        lastBrightness.store (brightness, std::memory_order_relaxed);
        lastOrbitPhase.store (orbitPhase, std::memory_order_relaxed);

        const auto previousActive = activeSlot.load (std::memory_order_acquire);
        const auto nextSlot = previousActive == 0 ? 1 : 0;
        auto& incoming = slots[static_cast<size_t> (nextSlot)];

        incoming.inUse.store (false, std::memory_order_release);
        incoming.targetGain.store (0.0f, std::memory_order_release);
        incoming.gain = 0.0f;
        incoming.indexesReady = false;

        if (! incoming.engine.loadProgram (Wf::buildStateProgram (state), Wf::makeWfParameterBindings()))
            return false;

        refreshWfParameterIndexes (incoming);
        if (! incoming.indexesReady)
            return false;

        applyControlsToSlot (incoming,
                             masterGain,
                             tempoHz,
                             intensity,
                             brightness,
                             orbitPhase);

        incoming.gain = previousActive < 0 ? 1.0f : 0.0f;
        incoming.inUse.store (true, std::memory_order_release);
        incoming.targetGain.store (1.0f, std::memory_order_release);

        if (previousActive >= 0)
        {
            auto& outgoing = slots[static_cast<size_t> (previousActive)];
            outgoing.targetGain.store (0.0f, std::memory_order_release);
        }

        activeSlot.store (nextSlot, std::memory_order_release);
        return true;
    }

    void setControls (float masterGain, float tempoHz, float intensity, float brightness, float orbitPhase)
    {
        lastMasterGain.store (masterGain, std::memory_order_relaxed);
        lastTempoHz.store (tempoHz, std::memory_order_relaxed);
        lastIntensity.store (intensity, std::memory_order_relaxed);
        lastBrightness.store (brightness, std::memory_order_relaxed);
        lastOrbitPhase.store (orbitPhase, std::memory_order_relaxed);

        const auto active = activeSlot.load (std::memory_order_acquire);
        if (active < 0 || active >= static_cast<int> (slots.size()))
            return;

        auto& slot = slots[static_cast<size_t> (active)];
        if (! slot.inUse.load (std::memory_order_acquire))
            return;

        if (! slot.indexesReady)
            refreshWfParameterIndexes (slot);

        if (slot.indexesReady)
            applyControlsToSlot (slot, masterGain, tempoHz, intensity, brightness, orbitPhase);
    }

    bool loadImportedAudioClips (const std::vector<ImportedLaneAudioClip>& clips,
                                 const std::array<std::optional<std::vector<Wf::StateSpec>>, maxTopLevelStates>* states = nullptr,
                                 bool restartPosition = true,
                                 const std::array<Wf::TrackEffectSlotSpec, maxTrackEffectSlots>* masterEffectSlots = nullptr)
    {
        auto playbackSet = std::make_shared<ImportedPlaybackSet>();
        playbackSet->clips.reserve (clips.size());
        const auto currentSampleRate = lastSampleRate.load (std::memory_order_relaxed);
        const auto sampleRate = currentSampleRate > 0.0 ? currentSampleRate : 48000.0;
        const auto blockSize = juce::jmax (fallbackBlockSize, preparedBlockSize.load (std::memory_order_relaxed));

        for (const auto& clip : clips)
        {
            if (clip.audioData == nullptr || clip.audioData->getNumSamples() <= 0)
                continue;

            const auto clipSampleRate = juce::jmax (1.0, clip.audioSampleRate);
            const auto clipLengthSeconds = static_cast<double> (clip.audioData->getNumSamples()) / clipSampleRate;
            auto regions = sanitisedImportedRegions (clip.regions, clipLengthSeconds);
            if (regions.empty())
                continue;

            ImportedPlaybackClip playbackClip;
            playbackClip.effectGroupIndex = getOrCreateImportedTrackEffectGroup (*playbackSet,
                                                                                  clip.stateIndex,
                                                                                  clip.trackIndex,
                                                                                  states,
                                                                                  sampleRate,
                                                                                  blockSize);
            playbackClip.audio = clip.audioData;
            playbackClip.regions = std::move (regions);
            playbackClip.sampleRate = clipSampleRate;
            playbackClip.gain = std::make_shared<std::atomic<float>> (clip.mixGain);
            playbackClip.pan = std::make_shared<std::atomic<float>> (clip.mixPan);
            for (const auto& region : playbackClip.regions)
                playbackSet->lengthSeconds = juce::jmax (playbackSet->lengthSeconds, regionEndSeconds (region));
            playbackSet->clips.push_back (std::move (playbackClip));
        }

        if (masterEffectSlots != nullptr)
        {
            auto group = std::make_unique<ImportedTrackEffectGroup>();
            group->stateIndex = -1;
            group->trackIndex = -1;
            group->buffer.setSize (engineOutputChannels, blockSize, false, false, true);
            group->buffer.clear();
            loadImportedEffectSlots (*group, *masterEffectSlots, sampleRate, blockSize);
            playbackSet->masterEffectGroup = std::move (group);
        }

        const auto previousPosition = importedPositionSamples.load (std::memory_order_acquire);
        const auto wasPlaying = importedPlaying.load (std::memory_order_acquire);
        const juce::SpinLock::ScopedLockType lock (importedPlaybackLock);
        importedPlaybackSet = std::move (playbackSet);
        pendingImportedSeekSamples.store (-1, std::memory_order_release);
        importedPositionSamples.store (restartPosition ? 0 : previousPosition, std::memory_order_release);
        const auto hasImportedClips = importedPlaybackSet != nullptr && ! importedPlaybackSet->clips.empty();

        if (hasImportedClips)
        {
            importedPlaying.store (restartPosition ? false : wasPlaying, std::memory_order_release);
            for (auto& slot : slots)
            {
                slot.inUse.store (false, std::memory_order_release);
                slot.targetGain.store (0.0f, std::memory_order_release);
                slot.gain = 0.0f;
            }

            activeSlot.store (-1, std::memory_order_release);
            playbackMode.store (PlaybackMode::importedAudio, std::memory_order_release);
        }

        return hasImportedClips;
    }

    void startImportedAudioPlayback (float masterGain)
    {
        lastMasterGain.store (masterGain, std::memory_order_relaxed);
        pendingImportedSeekSamples.store (-1, std::memory_order_release);
        importedPositionSamples.store (0, std::memory_order_release);
        importedPlaying.store (true, std::memory_order_release);

        for (auto& slot : slots)
        {
            slot.inUse.store (false, std::memory_order_release);
            slot.targetGain.store (0.0f, std::memory_order_release);
            slot.gain = 0.0f;
        }

        activeSlot.store (-1, std::memory_order_release);

        playbackMode.store (PlaybackMode::importedAudio, std::memory_order_release);
    }

    void stopImportedAudioPlayback()
    {
        importedPlaying.store (false, std::memory_order_release);
        pendingImportedSeekSamples.store (-1, std::memory_order_release);
        importedPositionSamples.store (0, std::memory_order_release);
    }

    bool isImportedAudioPlaying() const noexcept
    {
        return importedPlaying.load (std::memory_order_acquire);
    }

    double getImportedPlaybackPositionSeconds() const noexcept
    {
        const auto sampleRate = lastSampleRate.load (std::memory_order_relaxed);
        if (sampleRate <= 0.0)
            return 0.0;

        const auto pendingSeek = pendingImportedSeekSamples.load (std::memory_order_acquire);
        const auto position = pendingSeek >= 0 ? pendingSeek : importedPositionSamples.load (std::memory_order_acquire);
        return static_cast<double> (position) / sampleRate;
    }

    void setImportedPlaybackPositionSeconds (double seconds)
    {
        const auto sampleRate = lastSampleRate.load (std::memory_order_relaxed);
        if (sampleRate <= 0.0)
            return;

        const auto sample = static_cast<int64_t> (std::round (juce::jmax (0.0, seconds) * sampleRate));
        pendingImportedSeekSamples.store (juce::jmax<int64_t> (0, sample), std::memory_order_release);
    }

    void useChucKPlayback()
    {
        importedPlaying.store (false, std::memory_order_release);
        playbackMode.store (PlaybackMode::chuckEngine, std::memory_order_release);
    }

    void clearImportedAudioPlayback()
    {
        importedPlaying.store (false, std::memory_order_release);
        pendingImportedSeekSamples.store (-1, std::memory_order_release);
        importedPositionSamples.store (0, std::memory_order_release);
        {
            const juce::SpinLock::ScopedLockType lock (importedPlaybackLock);
            importedPlaybackSet.reset();
        }
        playbackMode.store (PlaybackMode::chuckEngine, std::memory_order_release);
    }

    void setImportedMasterGain (float masterGain)
    {
        lastMasterGain.store (masterGain, std::memory_order_relaxed);
    }

    void setImportedClipMix (int clipIndex, float gain, float pan)
    {
        std::shared_ptr<ImportedPlaybackSet> playbackSet;
        {
            const juce::SpinLock::ScopedLockType lock (importedPlaybackLock);
            playbackSet = importedPlaybackSet;
        }

        if (playbackSet == nullptr || clipIndex < 0 || clipIndex >= static_cast<int> (playbackSet->clips.size()))
            return;

        auto& clip = playbackSet->clips[static_cast<size_t> (clipIndex)];
        if (clip.gain != nullptr)
            clip.gain->store (gain, std::memory_order_release);
        if (clip.pan != nullptr)
            clip.pan->store (pan, std::memory_order_release);
    }

    juce::String diagnostics() const
    {
        if (playbackMode.load (std::memory_order_acquire) == PlaybackMode::importedAudio)
        {
            auto clipCount = 0;
            {
                const juce::SpinLock::ScopedLockType lock (importedPlaybackLock);
                clipCount = importedPlaybackSet == nullptr ? 0 : static_cast<int> (importedPlaybackSet->clips.size());
            }

            return "src=audio files"
                 + juce::String (" clips=") + juce::String (clipCount)
                 + " sr=" + juce::String (lastSampleRate.load (std::memory_order_relaxed), 0)
                 + " bs=" + juce::String (lastReportedBlockSize.load (std::memory_order_relaxed));
        }

        uint64_t renderedBlocks = 0;
        uint64_t silentBlocks = 0;
        int voices = 0;

        for (const auto& slot : slots)
        {
            renderedBlocks += slot.engine.getRenderedBlockCount();
            silentBlocks += slot.engine.getSilentProcessCount();
            if (slot.inUse.load (std::memory_order_acquire))
                ++voices;
        }

        return "blocks=" + juce::String (static_cast<juce::int64> (renderedBlocks))
             + " voices=" + juce::String (voices)
             + " silent=" + juce::String (static_cast<juce::int64> (silentBlocks))
             + " sr=" + juce::String (lastSampleRate.load (std::memory_order_relaxed), 0)
             + " bs=" + juce::String (lastReportedBlockSize.load (std::memory_order_relaxed));
    }

private:
    struct EngineSlot
    {
        int index = 0;
        EmbeddedChucKEngine engine;
        juce::AudioBuffer<float> output;
        std::array<int, 5> parameterIndexes { -1, -1, -1, -1, -1 };
        bool indexesReady = false;
        float gain = 0.0f;
        std::atomic<float> targetGain { 0.0f };
        std::atomic<bool> inUse { false };
    };

    static std::vector<ImportedAudioRegion> sanitisedImportedRegions (const std::vector<ImportedAudioRegion>& regions,
                                                                      double clipLengthSeconds)
    {
        std::vector<ImportedAudioRegion> sanitised;
        if (clipLengthSeconds <= 0.0)
            return sanitised;

        const auto sourceLength = juce::jmax (0.0, clipLengthSeconds);
        if (regions.empty())
        {
            sanitised.push_back ({ 0.0, 0.0, sourceLength, 0.0, 0.0 });
            return sanitised;
        }

        sanitised.reserve (regions.size());
        for (auto region : regions)
        {
            if (! std::isfinite (region.startSeconds)
                || ! std::isfinite (region.sourceStartSeconds)
                || ! std::isfinite (region.lengthSeconds))
                continue;

            region.startSeconds = juce::jmax (0.0, region.startSeconds);
            region.sourceStartSeconds = juce::jlimit (0.0, sourceLength, region.sourceStartSeconds);
            region.lengthSeconds = juce::jlimit (0.0, sourceLength - region.sourceStartSeconds, region.lengthSeconds);
            if (region.lengthSeconds <= 0.000001)
                continue;

            region.fadeInSeconds = juce::jlimit (0.0, region.lengthSeconds, std::isfinite (region.fadeInSeconds) ? region.fadeInSeconds : 0.0);
            region.fadeOutSeconds = juce::jlimit (0.0, region.lengthSeconds - region.fadeInSeconds, std::isfinite (region.fadeOutSeconds) ? region.fadeOutSeconds : 0.0);
            region.gain = juce::jlimit (0.0f, 2.0f, std::isfinite (region.gain) ? region.gain : 1.0f);
            region.fadeInCurve = juce::jlimit (0, 3, region.fadeInCurve);
            region.fadeOutCurve = juce::jlimit (0, 3, region.fadeOutCurve);
            sanitised.push_back (region);
        }

        std::sort (sanitised.begin(), sanitised.end(), [] (const auto& a, const auto& b)
        {
            return a.startSeconds < b.startSeconds;
        });
        applyAutomaticCrossfades (sanitised);

        return sanitised;
    }

    void processImportedAudio (float* const* outputChannelData, int numOutputChannels, int numSamples)
    {
        if (! importedPlaying.load (std::memory_order_acquire))
            return;

        std::shared_ptr<ImportedPlaybackSet> playbackSet;
        {
            const juce::SpinLock::ScopedLockType lock (importedPlaybackLock);
            playbackSet = importedPlaybackSet;
        }

        const auto deviceSampleRate = lastSampleRate.load (std::memory_order_relaxed);
        if (playbackSet == nullptr || playbackSet->clips.empty() || deviceSampleRate <= 0.0)
            return;

        const auto requestedSeekSample = pendingImportedSeekSamples.exchange (-1, std::memory_order_acq_rel);
        const auto startSample = requestedSeekSample >= 0
            ? requestedSeekSample
            : importedPositionSamples.load (std::memory_order_acquire);
        if (playbackSet->lengthSeconds > 0.0
            && static_cast<double> (startSample) / deviceSampleRate >= playbackSet->lengthSeconds)
        {
            importedPositionSamples.store (startSample, std::memory_order_release);
            importedPlaying.store (false, std::memory_order_release);
            return;
        }

        const auto masterGain = lastMasterGain.load (std::memory_order_relaxed);
        if (masterGain > 0.000001f)
        {
            for (auto& group : playbackSet->trackEffectGroups)
                if (group != nullptr && group->buffer.getNumSamples() >= numSamples)
                    group->buffer.clear (0, numSamples);

            auto* masterGroup = playbackSet->masterEffectGroup.get();
            if (masterGroup != nullptr
                && masterGroup->buffer.getNumChannels() >= engineOutputChannels
                && masterGroup->buffer.getNumSamples() >= numSamples)
            {
                masterGroup->buffer.clear (0, numSamples);
            }
            else
            {
                masterGroup = nullptr;
            }

            const auto blockStartSeconds = static_cast<double> (startSample) / deviceSampleRate;
            const auto blockEndSeconds = static_cast<double> (startSample + numSamples) / deviceSampleRate;

            for (const auto& clip : playbackSet->clips)
            {
                if (clip.audio == nullptr || clip.audio->getNumSamples() <= 0)
                    continue;

                const auto gain = clip.gain != nullptr ? clip.gain->load (std::memory_order_acquire) : 1.0f;
                if (gain <= 0.000001f)
                    continue;

                const auto pan = clip.pan != nullptr ? clip.pan->load (std::memory_order_acquire) : 0.0f;
                const auto panGains = linearPanGains (gain, pan);
                auto* group = getImportedTrackEffectGroup (*playbackSet, clip.effectGroupIndex);
                if (group == nullptr || group->buffer.getNumChannels() < engineOutputChannels || group->buffer.getNumSamples() < numSamples)
                    continue;

                for (const auto& region : clip.regions)
                {
                    const auto regionStart = region.startSeconds;
                    const auto regionEnd = regionEndSeconds (region);
                    if (regionEnd <= blockStartSeconds || regionStart >= blockEndSeconds)
                        continue;

                    const auto firstSample = juce::jlimit (0,
                                                           numSamples,
                                                           static_cast<int> (std::floor ((regionStart - blockStartSeconds) * deviceSampleRate)));
                    const auto lastSample = juce::jlimit (0,
                                                          numSamples,
                                                          static_cast<int> (std::ceil ((regionEnd - blockStartSeconds) * deviceSampleRate)));

                    for (int sample = firstSample; sample < lastSample; ++sample)
                    {
                        const auto timelineSample = startSample + sample;
                        const auto timeSeconds = static_cast<double> (timelineSample) / deviceSampleRate;
                        if (! regionContainsTime (region, timeSeconds))
                            continue;

                        const auto localSeconds = timeSeconds - region.startSeconds;
                        const auto sourcePosition = sourceSamplePositionForRegionTime (region,
                                                                                       timelineSample,
                                                                                       deviceSampleRate,
                                                                                       clip.sampleRate);
                        if (sourcePosition < 0.0 || sourcePosition >= static_cast<double> (clip.audio->getNumSamples()))
                            break;

                        const auto regionGain = importedRegionFadeGain (region, localSeconds);
                        const auto left = readInterpolatedSample (*clip.audio, 0, sourcePosition);
                        const auto right = readInterpolatedSample (*clip.audio, clip.audio->getNumChannels() > 1 ? 1 : 0, sourcePosition);

                        group->buffer.addSample (0, sample, left * panGains[0] * regionGain);
                        group->buffer.addSample (1, sample, right * panGains[1] * regionGain);
                    }
                }
            }

            for (auto& group : playbackSet->trackEffectGroups)
            {
                if (group == nullptr || group->buffer.getNumChannels() < engineOutputChannels || group->buffer.getNumSamples() < numSamples)
                    continue;

                juce::AudioBuffer<float> groupView (group->buffer.getArrayOfWritePointers(),
                                                    engineOutputChannels,
                                                    numSamples);
                processImportedEffectChain (*group, groupView);

                if (masterGroup != nullptr)
                {
                    for (int channel = 0; channel < engineOutputChannels; ++channel)
                        masterGroup->buffer.addFrom (channel, 0, groupView, channel, 0, numSamples);

                    continue;
                }

                if (numOutputChannels > 0 && outputChannelData[0] != nullptr)
                    juce::FloatVectorOperations::addWithMultiply (outputChannelData[0], groupView.getReadPointer (0), masterGain, numSamples);
                if (numOutputChannels > 1 && outputChannelData[1] != nullptr)
                    juce::FloatVectorOperations::addWithMultiply (outputChannelData[1], groupView.getReadPointer (1), masterGain, numSamples);

                for (int sample = 0; sample < numSamples && numOutputChannels > 2; ++sample)
                {
                    const auto mono = 0.5f * (groupView.getSample (0, sample) + groupView.getSample (1, sample)) * masterGain;
                    for (int channel = 2; channel < numOutputChannels; ++channel)
                        if (outputChannelData[channel] != nullptr)
                            outputChannelData[channel][sample] += mono;
                }
            }

            if (masterGroup != nullptr)
            {
                juce::AudioBuffer<float> masterView (masterGroup->buffer.getArrayOfWritePointers(),
                                                     engineOutputChannels,
                                                     numSamples);
                processImportedEffectChain (*masterGroup, masterView);

                if (numOutputChannels > 0 && outputChannelData[0] != nullptr)
                    juce::FloatVectorOperations::addWithMultiply (outputChannelData[0], masterView.getReadPointer (0), masterGain, numSamples);
                if (numOutputChannels > 1 && outputChannelData[1] != nullptr)
                    juce::FloatVectorOperations::addWithMultiply (outputChannelData[1], masterView.getReadPointer (1), masterGain, numSamples);

                for (int sample = 0; sample < numSamples && numOutputChannels > 2; ++sample)
                {
                    const auto mono = 0.5f * (masterView.getSample (0, sample) + masterView.getSample (1, sample)) * masterGain;
                    for (int channel = 2; channel < numOutputChannels; ++channel)
                        if (outputChannelData[channel] != nullptr)
                            outputChannelData[channel][sample] += mono;
                }
            }
        }

        const auto nextSample = startSample + numSamples;
        const auto hasPendingSeek = pendingImportedSeekSamples.load (std::memory_order_acquire) >= 0;
        if (! hasPendingSeek)
            importedPositionSamples.store (nextSample, std::memory_order_release);

        if (! hasPendingSeek
            && playbackSet->lengthSeconds > 0.0
            && static_cast<double> (nextSample) / deviceSampleRate >= playbackSet->lengthSeconds)
            importedPlaying.store (false, std::memory_order_release);
    }

    bool prepare (double sampleRate, int reportedBlockSize)
    {
        ready.store (false, std::memory_order_release);

        const auto blockSize = juce::jlimit (64,
                                             EmbeddedChucKEngine::maximumBlockSizeLimit,
                                             juce::jmax (reportedBlockSize * 4, fallbackBlockSize));
        lastSampleRate.store (sampleRate, std::memory_order_relaxed);
        lastReportedBlockSize.store (reportedBlockSize, std::memory_order_relaxed);
        preparedBlockSize.store (blockSize, std::memory_order_relaxed);

        bool prepared = true;
        for (int slotIndex = 0; slotIndex < static_cast<int> (slots.size()); ++slotIndex)
        {
            auto& slot = slots[static_cast<size_t> (slotIndex)];
            slot.index = slotIndex;
            slot.inUse.store (false, std::memory_order_release);
            slot.targetGain.store (0.0f, std::memory_order_release);
            slot.gain = 0.0f;
            slot.indexesReady = false;
            slot.engine.release();
            slot.output.setSize (maxHostChannels, blockSize, false, false, true);
            slot.output.clear();
            prepared = slot.engine.prepare (sampleRate, blockSize, 0, engineOutputChannels) && prepared;
        }

        activeSlot.store (-1, std::memory_order_release);
        pendingImportedSeekSamples.store (-1, std::memory_order_release);
        importedPositionSamples.store (0, std::memory_order_release);
        ready.store (prepared, std::memory_order_release);
        return prepared;
    }

    void refreshWfParameterIndexes (EngineSlot& slot)
    {
        if (slot.indexesReady || slot.engine.getParameterCount() != 5)
            return;

        const std::array<juce::String, 5> names
        {
            "hostMasterGain",
            "hostTempoHz",
            "hostIntensity",
            "hostBrightness",
            "hostOrbitPhase"
        };

        std::array<int, 5> indexes {};
        for (int i = 0; i < static_cast<int> (names.size()); ++i)
        {
            indexes[static_cast<size_t> (i)] = slot.engine.getParameterIndex (names[static_cast<size_t> (i)]);
            if (indexes[static_cast<size_t> (i)] < 0)
                return;
        }

        slot.parameterIndexes = indexes;
        slot.indexesReady = true;
    }

    static void applyControlsToSlot (EngineSlot& slot, float masterGain, float tempoHz, float intensity, float brightness, float orbitPhase)
    {
        static_cast<void> (slot.engine.setParameterValue (slot.parameterIndexes[0], masterGain));
        static_cast<void> (slot.engine.setParameterValue (slot.parameterIndexes[1], tempoHz));
        static_cast<void> (slot.engine.setParameterValue (slot.parameterIndexes[2], intensity));
        static_cast<void> (slot.engine.setParameterValue (slot.parameterIndexes[3], brightness));
        static_cast<void> (slot.engine.setParameterValue (slot.parameterIndexes[4], orbitPhase));
    }

    int getOrCreateImportedTrackEffectGroup (ImportedPlaybackSet& playbackSet,
                                             int stateIndex,
                                             int trackIndex,
                                             const std::array<std::optional<std::vector<Wf::StateSpec>>, maxTopLevelStates>* states,
                                             double sampleRate,
                                             int blockSize)
    {
        for (int index = 0; index < static_cast<int> (playbackSet.trackEffectGroups.size()); ++index)
        {
            const auto* group = playbackSet.trackEffectGroups[static_cast<size_t> (index)].get();
            if (group != nullptr && group->stateIndex == stateIndex && group->trackIndex == trackIndex)
                return index;
        }

        auto group = std::make_unique<ImportedTrackEffectGroup>();
        group->stateIndex = stateIndex;
        group->trackIndex = trackIndex;
        group->buffer.setSize (engineOutputChannels, blockSize, false, false, true);
        group->buffer.clear();

        if (const auto* track = getTrackForImportedEffects (states, stateIndex, trackIndex))
            loadImportedEffectChain (*group, *track, sampleRate, blockSize);

        playbackSet.trackEffectGroups.push_back (std::move (group));
        return static_cast<int> (playbackSet.trackEffectGroups.size()) - 1;
    }

    static const Wf::StateSpec* getTrackForImportedEffects (const std::array<std::optional<std::vector<Wf::StateSpec>>, maxTopLevelStates>* states,
                                                            int stateIndex,
                                                            int trackIndex)
    {
        if (states == nullptr || stateIndex < 0 || stateIndex >= maxTopLevelStates)
            return nullptr;

        const auto& stateSlot = (*states)[static_cast<size_t> (stateIndex)];
        if (! stateSlot.has_value())
            return nullptr;

        const auto& tracks = *stateSlot;
        if (trackIndex < 0 || trackIndex >= static_cast<int> (tracks.size()))
            return nullptr;

        return &tracks[static_cast<size_t> (trackIndex)];
    }

    static ImportedTrackEffectGroup* getImportedTrackEffectGroup (ImportedPlaybackSet& playbackSet, int groupIndex)
    {
        if (groupIndex < 0 || groupIndex >= static_cast<int> (playbackSet.trackEffectGroups.size()))
            return nullptr;

        return playbackSet.trackEffectGroups[static_cast<size_t> (groupIndex)].get();
    }

    void loadImportedEffectChain (ImportedTrackEffectGroup& group,
                                  const Wf::StateSpec& track,
                                  double sampleRate,
                                  int blockSize)
    {
        loadImportedEffectSlots (group, track.effectSlots, sampleRate, blockSize);
    }

    void loadImportedEffectSlots (ImportedTrackEffectGroup& group,
                                  const std::array<Wf::TrackEffectSlotSpec, maxTrackEffectSlots>& effectSlots,
                                  double sampleRate,
                                  int blockSize)
    {
        for (int effectIndex = 0; effectIndex < maxTrackEffectSlots; ++effectIndex)
        {
            const auto& spec = effectSlots[static_cast<size_t> (effectIndex)];
            if (! spec.active || spec.pluginFileOrIdentifier.isEmpty())
                continue;

            if (auto description = findPluginDescription (spec))
            {
                juce::String error;
                auto plugin = pluginFormatManager.createPluginInstance (*description, sampleRate, blockSize, error);
                if (plugin == nullptr)
                    continue;

                plugin->setPlayConfigDetails (engineOutputChannels, engineOutputChannels, sampleRate, blockSize);
                plugin->prepareToPlay (sampleRate, blockSize);
                plugin->reset();
                group.effects[static_cast<size_t> (effectIndex)] = std::move (plugin);
            }
        }
    }

    static void processImportedEffectChain (ImportedTrackEffectGroup& group, juce::AudioBuffer<float>& buffer)
    {
        group.midi.clear();

        for (auto& effect : group.effects)
        {
            if (effect == nullptr)
                continue;

            const auto inputs = effect->getTotalNumInputChannels();
            const auto outputs = effect->getTotalNumOutputChannels();
            if (inputs <= 0 || outputs <= 0)
                continue;

            effect->processBlock (buffer, group.midi);
        }
    }

    std::unique_ptr<juce::PluginDescription> findPluginDescription (const Wf::TrackEffectSlotSpec& spec)
    {
        for (auto* format : pluginFormatManager.getFormats())
        {
            if (format == nullptr)
                continue;

            juce::OwnedArray<juce::PluginDescription> descriptions;
            format->findAllTypesForFile (descriptions, spec.pluginFileOrIdentifier);

            for (auto* description : descriptions)
            {
                if (description == nullptr)
                    continue;

                const auto identifier = description->createIdentifierString();
                if (spec.pluginIdentifier.isEmpty() || identifier == spec.pluginIdentifier)
                    return std::make_unique<juce::PluginDescription> (*description);
            }
        }

        return {};
    }

    juce::AudioBuffer<float> emptyInput;
    std::array<EngineSlot, 2> slots;
    juce::AudioPluginFormatManager pluginFormatManager;
    mutable juce::SpinLock importedPlaybackLock;
    std::shared_ptr<ImportedPlaybackSet> importedPlaybackSet;
    static constexpr double tailFadeSeconds = 0.42;
    std::atomic<bool> ready { false };
    std::atomic<int> playbackMode { PlaybackMode::chuckEngine };
    std::atomic<bool> importedPlaying { false };
    std::atomic<int64_t> importedPositionSamples { 0 };
    std::atomic<int64_t> pendingImportedSeekSamples { -1 };
    std::atomic<int> activeSlot { -1 };
    std::atomic<int> preparedBlockSize { 0 };
    std::atomic<double> lastSampleRate { 0.0 };
    std::atomic<int> lastReportedBlockSize { 0 };
    std::atomic<float> lastMasterGain { 0.18f };
    std::atomic<float> lastTempoHz { 1.0f };
    std::atomic<float> lastIntensity { 0.58f };
    std::atomic<float> lastBrightness { 0.48f };
    std::atomic<float> lastOrbitPhase { 0.0f };
};

class OrbitCanvas final : public juce::Component
{
public:
    OrbitCanvas()
    {
        setWantsKeyboardFocus (true);

        for (int i = 0; i < maxGraphTransitions; ++i)
        {
            auto& editor = transitionProbabilityEditors[static_cast<size_t> (i)];
            editor.setInputRestrictions (4, "0123456789%");
            editor.setTextToShowWhenEmpty ("%", mutedInk().withAlpha (0.36f));
            editor.setJustification (juce::Justification::centred);
            editor.setColour (juce::TextEditor::backgroundColourId, panel().withAlpha (0.86f));
            editor.setColour (juce::TextEditor::textColourId, ink());
            editor.setColour (juce::TextEditor::outlineColourId, mutedInk().withAlpha (0.22f));
            editor.setColour (juce::TextEditor::focusedOutlineColourId, amber().withAlpha (0.68f));
            editor.setColour (juce::TextEditor::highlightColourId, amber().withAlpha (0.24f));
            editor.setFont (juce::FontOptions (12.0f, juce::Font::bold));
            editor.onTextChange = [this, i]
            {
                if (suppressTransitionCallbacks || onTransitionProbabilityChanged == nullptr)
                    return;

                onTransitionProbabilityChanged (i, parseTransitionProbability (transitionProbabilityEditors[static_cast<size_t> (i)].getText()));
            };
            addAndMakeVisible (editor);
            editor.setVisible (false);
        }
    }

    std::function<void (int)> onStateSelected;
    std::function<void (int)> onStateDoubleClicked;
    std::function<void (int, std::optional<int>)> onTransitionProbabilityChanged;

    bool isEditingTransitionProbability() const
    {
        for (const auto& editor : transitionProbabilityEditors)
            if (editor.hasKeyboardFocus (true))
                return true;

        return false;
    }

    void setState (const std::vector<Wf::StateSpec>* statesToUse, int selectedIndexToUse, float phaseToUse, bool runningToUse)
    {
        states = statesToUse;
        selectedIndex = selectedIndexToUse;
        phase = phaseToUse;
        running = runningToUse;
        rebuildNodeCentres();
        syncTransitionEditors();

        if (states == nullptr || states->empty())
            nodeCentres.clear();

        repaint();
    }

    void resized() override
    {
        rebuildNodeCentres();
        updateTransitionEditorBounds();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff0a130e));

        auto area = getLocalBounds().toFloat().reduced (18.0f);
        const auto radius = juce::jmin (area.getWidth(), area.getHeight()) * 0.42f;
        const auto centre = area.getCentre();

        g.setColour (panelSoft().withAlpha (0.28f));
        g.fillEllipse (centre.x - radius, centre.y - radius, radius * 2.0f, radius * 2.0f);

        g.setColour (cyan().withAlpha (0.13f));
        g.drawEllipse (centre.x - radius, centre.y - radius, radius * 2.0f, radius * 2.0f, 1.0f);

        if (states == nullptr || states->empty())
            return;

        const auto count = static_cast<int> (states->size());
        rebuildNodeCentres();

        for (int i = 0; i < count; ++i)
        {
            const auto next = (i + 1) % count;
            const auto lineColour = stateAccentForIndex (i);
            g.setColour (lineColour.withAlpha (i == selectedIndex ? 0.27f : 0.14f));
            g.drawLine ({ nodeCentres[static_cast<size_t> (i)], nodeCentres[static_cast<size_t> (next)] }, 1.0f);
        }

        for (int i = 0; i < count; ++i)
        {
            const auto selected = i == selectedIndex;
            const auto point = nodeCentres[static_cast<size_t> (i)];
            const auto colour = stateAccentForIndex (i);
            const auto size = selected ? 88.0f : 68.0f;
            const auto laneCount = static_cast<int> ((*states)[static_cast<size_t> (i)].lanes.size());

            g.setColour (colour.withAlpha (selected ? 0.27f : 0.12f));
            g.fillEllipse (point.x - size * 0.5f, point.y - size * 0.5f, size, size);
            g.setColour (colour.withAlpha (selected ? 0.98f : 0.70f));
            g.drawEllipse (point.x - size * 0.5f, point.y - size * 0.5f, size, size, selected ? 2.2f : 1.3f);
            g.setColour (colour.withAlpha (selected ? 0.32f : 0.14f));
            g.drawEllipse (point.x - size * 0.5f + 8.0f,
                           point.y - size * 0.5f + 8.0f,
                           size - 16.0f,
                           size - 16.0f,
                           selected ? 0.9f : 0.6f);
            drawClockTicks (g, point, size * 0.5f, colour, selected);
            drawLaneDots (g, point, size, laneCount, selected);

            g.setColour (ink().withAlpha (selected ? 0.98f : 0.86f));
            g.setFont (juce::FontOptions (selected ? 16.0f : 13.5f, juce::Font::bold));
            g.drawFittedText (trackDisplayName ((*states)[static_cast<size_t> (i)].name),
                              juce::Rectangle<int> (static_cast<int> (point.x - 74.0f),
                                                    static_cast<int> (point.y - 11.0f),
                                                    148,
                                                    24),
                              juce::Justification::centred,
                              1);
        }

        if (running && selectedIndex >= 0 && selectedIndex < count)
        {
            const auto base = nodeCentres[static_cast<size_t> (selectedIndex)];
            const auto angle = juce::MathConstants<float>::twoPi * phase - juce::MathConstants<float>::halfPi;
            drawClockHand (g, base, angle, 34.0f, stateAccentForIndex (selectedIndex), 1.8f, 0.92f, 3.4f);
        }
    }

    void mouseDown (const juce::MouseEvent& event) override
    {
        grabKeyboardFocus();

        if (onStateSelected == nullptr)
            return;

        if (const auto index = nodeIndexAt (event.position, 80.0f); index >= 0)
            onStateSelected (index);
    }

    void mouseDoubleClick (const juce::MouseEvent& event) override
    {
        grabKeyboardFocus();

        if (onStateDoubleClicked == nullptr)
            return;

        if (const auto index = nodeIndexAt (event.position, 80.0f); index >= 0)
            onStateDoubleClicked (index);
    }

private:
    int nodeIndexAt (juce::Point<float> position, float maximumDistance) const
    {
        if (nodeCentres.empty())
            return -1;

        auto bestIndex = -1;
        auto bestDistance = std::numeric_limits<float>::max();

        for (int i = 0; i < static_cast<int> (nodeCentres.size()); ++i)
        {
            const auto distance = position.getDistanceFrom (nodeCentres[static_cast<size_t> (i)]);
            if (distance < bestDistance)
            {
                bestDistance = distance;
                bestIndex = i;
            }
        }

        return bestDistance < maximumDistance ? bestIndex : -1;
    }

    void rebuildNodeCentres()
    {
        nodeCentres.clear();

        if (states == nullptr || states->empty())
            return;

        auto area = getLocalBounds().toFloat().reduced (18.0f);
        const auto radius = juce::jmin (area.getWidth(), area.getHeight()) * 0.42f;
        const auto centre = area.getCentre();
        const auto count = static_cast<int> (states->size());

        nodeCentres.reserve (static_cast<size_t> (count));

        for (int i = 0; i < count; ++i)
        {
            const auto angle = juce::MathConstants<float>::twoPi * (static_cast<float> (i) / static_cast<float> (count)) - juce::MathConstants<float>::halfPi;
            nodeCentres.push_back ({ centre.x + std::cos (angle) * radius,
                                     centre.y + std::sin (angle) * radius });
        }
    }

    void syncTransitionEditors()
    {
        juce::ScopedValueSetter<bool> guard (suppressTransitionCallbacks, true);
        const auto count = states == nullptr ? 0 : juce::jmin (static_cast<int> (states->size()), maxGraphTransitions);

        for (int i = 0; i < maxGraphTransitions; ++i)
        {
            auto& editor = transitionProbabilityEditors[static_cast<size_t> (i)];
            const auto active = i < count && count > 1;
            const auto hasProbability = active && (*states)[static_cast<size_t> (i)].transitionProbabilityPercent.has_value();
            editor.setVisible (active);
            editor.setEnabled (active);
            styleTransitionProbabilityEditor (editor, hasProbability);

            if (active && ! editor.hasKeyboardFocus (true))
                editor.setText (formatTransitionProbability ((*states)[static_cast<size_t> (i)].transitionProbabilityPercent),
                                juce::dontSendNotification);
        }

        updateTransitionEditorBounds();
    }

    void updateTransitionEditorBounds()
    {
        const auto count = states == nullptr ? 0 : juce::jmin (static_cast<int> (states->size()), maxGraphTransitions);

        for (int i = 0; i < maxGraphTransitions; ++i)
        {
            auto& editor = transitionProbabilityEditors[static_cast<size_t> (i)];
            if (i >= count || count <= 1 || i >= static_cast<int> (nodeCentres.size()))
            {
                editor.setVisible (false);
                continue;
            }

            const auto next = (i + 1) % count;
            if (next >= static_cast<int> (nodeCentres.size()))
            {
                editor.setVisible (false);
                continue;
            }

            const auto from = nodeCentres[static_cast<size_t> (i)];
            const auto to = nodeCentres[static_cast<size_t> (next)];
            const auto midpoint = (from + to) * 0.5f;
            editor.setBounds (juce::Rectangle<int> (40, 18).withCentre ({ static_cast<int> (std::round (midpoint.x)),
                                                                          static_cast<int> (std::round (midpoint.y)) }));
        }
    }

    static void styleTransitionProbabilityEditor (juce::TextEditor& editor, bool hasProbability)
    {
        editor.setColour (juce::TextEditor::backgroundColourId,
                          hasProbability ? panel().withAlpha (0.78f) : juce::Colours::transparentBlack);
        editor.setColour (juce::TextEditor::outlineColourId,
                          hasProbability ? amber().withAlpha (0.28f) : juce::Colours::transparentBlack);
        editor.setColour (juce::TextEditor::textColourId,
                          hasProbability ? ink() : mutedInk().withAlpha (0.30f));
        editor.setTextToShowWhenEmpty ("%", mutedInk().withAlpha (0.18f));
    }

    static std::optional<int> parseTransitionProbability (juce::String text)
    {
        text = text.retainCharacters ("0123456789").trim();
        if (text.isEmpty())
            return {};

        return juce::jlimit (0, 100, text.getIntValue());
    }

    static juce::String formatTransitionProbability (std::optional<int> probability)
    {
        if (! probability.has_value())
            return {};

        return juce::String (*probability) + "%";
    }

    static juce::Colour laneDotColour (int index)
    {
        return laneAccentForIndex (index);
    }

    static void drawLaneDots (juce::Graphics& g, juce::Point<float> centre, float nodeSize, int laneCount, bool selected)
    {
        if (laneCount <= 0)
            return;

        const auto dotRadius = selected ? 3.8f : 3.0f;
        const auto dotOrbitRadius = nodeSize * 0.5f + dotRadius + 2.0f;

        for (int lane = 0; lane < laneCount; ++lane)
        {
            const auto angle = juce::MathConstants<float>::twoPi * (static_cast<float> (lane) / static_cast<float> (laneCount))
                             - juce::MathConstants<float>::halfPi;
            const juce::Point<float> dotCentre { centre.x + std::cos (angle) * dotOrbitRadius,
                                                 centre.y + std::sin (angle) * dotOrbitRadius };
            const auto colour = laneDotColour (lane);

            g.setColour (juce::Colour (0xff101412).withAlpha (0.92f));
            g.fillEllipse (dotCentre.x - dotRadius - 1.5f,
                           dotCentre.y - dotRadius - 1.5f,
                           (dotRadius + 1.5f) * 2.0f,
                           (dotRadius + 1.5f) * 2.0f);

            g.setColour (colour.withAlpha (selected ? 0.90f : 0.62f));
            g.fillEllipse (dotCentre.x - dotRadius, dotCentre.y - dotRadius, dotRadius * 2.0f, dotRadius * 2.0f);

            g.setColour (ink().withAlpha (selected ? 0.42f : 0.22f));
            g.drawEllipse (dotCentre.x - dotRadius, dotCentre.y - dotRadius, dotRadius * 2.0f, dotRadius * 2.0f, 0.8f);
        }
    }

    static void drawClockTicks (juce::Graphics& g, juce::Point<float> centre, float radius, juce::Colour colour, bool selected)
    {
        for (int tick = 0; tick < 4; ++tick)
        {
            const auto isStart = tick == 0;
            const auto angle = juce::MathConstants<float>::halfPi * static_cast<float> (tick)
                             - juce::MathConstants<float>::halfPi;
            const auto innerRadius = radius - (isStart ? 12.0f : 8.0f);
            const auto outerRadius = radius + (isStart ? 8.0f : 5.0f);
            const juce::Point<float> start { centre.x + std::cos (angle) * innerRadius,
                                             centre.y + std::sin (angle) * innerRadius };
            const juce::Point<float> end { centre.x + std::cos (angle) * outerRadius,
                                           centre.y + std::sin (angle) * outerRadius };

            g.setColour ((isStart ? ink() : colour).withAlpha (isStart ? (selected ? 0.58f : 0.36f)
                                                                        : (selected ? 0.30f : 0.18f)));
            g.drawLine (juce::Line<float> (start, end), isStart ? 1.6f : 1.0f);
        }
    }

    static void drawClockHand (juce::Graphics& g,
                               juce::Point<float> centre,
                               float angle,
                               float length,
                               juce::Colour colour,
                               float thickness,
                               float alpha,
                               float hubRadius)
    {
        const juce::Point<float> handEnd { centre.x + std::cos (angle) * length,
                                           centre.y + std::sin (angle) * length };

        g.setColour (juce::Colour (0xff050806).withAlpha (0.70f));
        g.drawLine (juce::Line<float> (centre, handEnd), thickness + 2.0f);
        g.setColour (colour.withAlpha (alpha));
        g.drawLine (juce::Line<float> (centre, handEnd), thickness);

        g.setColour (juce::Colour (0xff050806).withAlpha (0.92f));
        g.fillEllipse (centre.x - hubRadius - 1.3f,
                       centre.y - hubRadius - 1.3f,
                       (hubRadius + 1.3f) * 2.0f,
                       (hubRadius + 1.3f) * 2.0f);
        g.setColour (colour.withAlpha (juce::jmin (1.0f, alpha + 0.12f)));
        g.fillEllipse (centre.x - hubRadius, centre.y - hubRadius, hubRadius * 2.0f, hubRadius * 2.0f);
    }

    const std::vector<Wf::StateSpec>* states = nullptr;
    std::vector<juce::Point<float>> nodeCentres;
    std::array<juce::TextEditor, maxGraphTransitions> transitionProbabilityEditors;
    int selectedIndex = 0;
    float phase = 0.0f;
    bool running = false;
    bool suppressTransitionCallbacks = false;
};

class TrackFocusCanvas final : public juce::Component
{
public:
    TrackFocusCanvas()
    {
        setWantsKeyboardFocus (true);
    }

    std::function<void (int)> onLaneSelected;
    std::function<void (int)> onLanePhaseOffsetEditStarted;
    std::function<void (int, float)> onLanePhaseOffsetChanged;

    void setTrack (const Wf::StateSpec* trackToUse,
                   int selectedLaneToUse,
                   int beatsPerBarToUse,
                   float defaultTempoBpmToUse,
                   double trackElapsedBarsToUse,
                   float phaseToUse,
                   bool runningToUse)
    {
        track = trackToUse;
        selectedLane = selectedLaneToUse;
        beatsPerBar = juce::jlimit (1, 16, beatsPerBarToUse);
        defaultTempoBpm = juce::jlimit (30.0f, 220.0f, defaultTempoBpmToUse);
        trackElapsedBars = juce::jmax (0.0, trackElapsedBarsToUse);
        phase = phaseToUse;
        running = runningToUse;
        rebuildLaneCentres();
        repaint();
    }

    void resized() override
    {
        rebuildLaneCentres();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff0d1510));

        if (track == nullptr)
        {
            g.setColour (mutedInk().withAlpha (0.48f));
            g.setFont (juce::FontOptions (18.0f, juce::Font::bold));
            g.drawFittedText ("EMPTY STATE", getLocalBounds().reduced (32), juce::Justification::centred, 1);
            return;
        }

        const auto local = getLocalBounds().toFloat().reduced (54.0f);
        const auto centre = local.getCentre();
        const auto nodeRadius = juce::jmin (local.getWidth(), local.getHeight()) * 0.22f;
        const auto ringRadius = nodeRadius + 34.0f;

        g.setColour (panelSoft().withAlpha (0.28f));
        g.fillEllipse (centre.x - ringRadius, centre.y - ringRadius, ringRadius * 2.0f, ringRadius * 2.0f);

        g.setColour (mutedInk().withAlpha (0.15f));
        g.drawEllipse (centre.x - ringRadius, centre.y - ringRadius, ringRadius * 2.0f, ringRadius * 2.0f, 1.0f);

        if (running)
        {
            const auto angle = juce::MathConstants<float>::twoPi * phase - juce::MathConstants<float>::halfPi;
            drawClockHand (g, centre, angle, ringRadius - 7.0f, amber(), 2.0f, 0.74f, 4.2f);
        }

        g.setColour (ink());
        g.setFont (juce::FontOptions (31.0f, juce::Font::bold));
        g.drawFittedText (trackDisplayName (track->name),
                          juce::Rectangle<int> (static_cast<int> (centre.x - nodeRadius * 1.10f),
                                                static_cast<int> (centre.y - 24.0f),
                                                static_cast<int> (nodeRadius * 2.20f),
                                                48),
                          juce::Justification::centred,
                          1);

        g.setColour (mutedInk().withAlpha (0.72f));
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.drawFittedText (juce::String (track->lanes.size()) + " lanes",
                          juce::Rectangle<int> (static_cast<int> (centre.x - 84.0f),
                                                static_cast<int> (centre.y + 30.0f),
                                                168,
                                                20),
                          juce::Justification::centred,
                          1);

        rebuildLaneCentres();
        drawLanes (g, centre, nodeRadius);
    }

    void mouseDown (const juce::MouseEvent& event) override
    {
        grabKeyboardFocus();

        if (const auto index = startHandleIndexAt (event.position); index >= 0)
        {
            draggedStartHandleLane = index;
            if (onLaneSelected != nullptr)
                onLaneSelected (index);
            if (onLanePhaseOffsetEditStarted != nullptr)
                onLanePhaseOffsetEditStarted (index);
            updateDraggedStartHandle (event.position);
            return;
        }

        if (onLaneSelected == nullptr)
            return;

        for (int i = 0; i < static_cast<int> (laneCentres.size()); ++i)
        {
            if (event.position.getDistanceFrom (laneCentres[static_cast<size_t> (i)]) < 62.0f)
            {
                onLaneSelected (i);
                return;
            }
        }
    }

    void mouseDrag (const juce::MouseEvent& event) override
    {
        if (draggedStartHandleLane >= 0)
            updateDraggedStartHandle (event.position);
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        draggedStartHandleLane = -1;
    }

private:
    static juce::Colour laneDotColour (int index)
    {
        return laneAccentForIndex (index);
    }

    static float phaseOffsetToAngle (float phaseOffset)
    {
        return juce::MathConstants<float>::twoPi * juce::jlimit (0.0f, 0.999f, phaseOffset)
             - juce::MathConstants<float>::halfPi;
    }

    static float angleToPhaseOffset (juce::Point<float> laneCentre, juce::Point<float> position)
    {
        auto phase = (std::atan2 (position.y - laneCentre.y, position.x - laneCentre.x) + juce::MathConstants<float>::halfPi)
                   / juce::MathConstants<float>::twoPi;
        phase -= std::floor (phase);
        return juce::jlimit (0.0f, 0.999f, phase);
    }

    static void drawClockHand (juce::Graphics& g,
                               juce::Point<float> centre,
                               float angle,
                               float length,
                               juce::Colour colour,
                               float thickness,
                               float alpha,
                               float hubRadius)
    {
        const juce::Point<float> handEnd { centre.x + std::cos (angle) * length,
                                           centre.y + std::sin (angle) * length };

        g.setColour (juce::Colour (0xff050806).withAlpha (0.72f));
        g.drawLine (juce::Line<float> (centre, handEnd), thickness + 2.0f);

        g.setColour (colour.withAlpha (alpha));
        g.drawLine (juce::Line<float> (centre, handEnd), thickness);

        g.setColour (juce::Colour (0xff050806).withAlpha (0.92f));
        g.fillEllipse (centre.x - hubRadius - 1.4f,
                       centre.y - hubRadius - 1.4f,
                       (hubRadius + 1.4f) * 2.0f,
                       (hubRadius + 1.4f) * 2.0f);

        g.setColour (colour.withAlpha (juce::jmin (1.0f, alpha + 0.14f)));
        g.fillEllipse (centre.x - hubRadius, centre.y - hubRadius, hubRadius * 2.0f, hubRadius * 2.0f);
    }

    void rebuildLaneCentres()
    {
        laneCentres.clear();

        if (track == nullptr || track->lanes.empty())
            return;

        const auto local = getLocalBounds().toFloat().reduced (54.0f);
        const auto centre = local.getCentre();
        const auto nodeRadius = juce::jmin (local.getWidth(), local.getHeight()) * 0.22f;
        const auto orbitRadius = nodeRadius + 34.0f;
        const auto count = static_cast<int> (track->lanes.size());

        laneCentres.reserve (static_cast<size_t> (count));
        for (int i = 0; i < count; ++i)
        {
            const auto angle = juce::MathConstants<float>::twoPi * (static_cast<float> (i) / static_cast<float> (count))
                             - juce::MathConstants<float>::halfPi;
            laneCentres.push_back ({ centre.x + std::cos (angle) * orbitRadius,
                                     centre.y + std::sin (angle) * orbitRadius });
        }
    }

    void drawLanes (juce::Graphics& g, juce::Point<float> centre, float nodeRadius)
    {
        if (track == nullptr)
            return;

        for (int i = 0; i < static_cast<int> (laneCentres.size()); ++i)
        {
            const auto point = laneCentres[static_cast<size_t> (i)];
            const auto laneIsSelected = i == selectedLane;
            const auto& lane = track->lanes[static_cast<size_t> (i)];
            const auto colour = laneDotColour (i);
            const auto dotRadius = laneIsSelected ? 54.0f : 43.5f;
            const auto distanceFromCentre = point.getDistanceFrom (centre);
            const juce::Point<float> lineStart { centre.x + (point.x - centre.x) * (nodeRadius / distanceFromCentre),
                                                 centre.y + (point.y - centre.y) * (nodeRadius / distanceFromCentre) };

            g.setColour (mutedInk().withAlpha (0.14f));
            g.drawLine (juce::Line<float> (lineStart, point), 1.0f);

            g.setColour (juce::Colour (0xff0d1510).withAlpha (0.94f));
            g.fillEllipse (point.x - dotRadius - 2.5f,
                           point.y - dotRadius - 2.5f,
                           (dotRadius + 2.5f) * 2.0f,
                           (dotRadius + 2.5f) * 2.0f);

            g.setColour (colour.withAlpha (laneIsSelected ? 0.96f : 0.68f));
            g.drawEllipse (point.x - dotRadius, point.y - dotRadius, dotRadius * 2.0f, dotRadius * 2.0f, laneIsSelected ? 3.0f : 2.1f);
            g.setColour (colour.withAlpha (laneIsSelected ? 0.32f : 0.15f));
            g.drawEllipse (point.x - dotRadius + 8.0f,
                           point.y - dotRadius + 8.0f,
                           (dotRadius - 8.0f) * 2.0f,
                           (dotRadius - 8.0f) * 2.0f,
                           laneIsSelected ? 0.8f : 0.5f);

            drawLaneTimingMarks (g, point, dotRadius, colour, laneIsSelected, lane.phaseOffsetBars);

            if (running)
                drawLanePlayhead (g, point, dotRadius, lane);

            auto labelBounds = juce::Rectangle<int> (180, 24).withCentre ({ static_cast<int> (point.x),
                                                                           static_cast<int> (point.y + (point.y < centre.y ? -82.0f : 82.0f)) });
            const auto labelAlpha = lane.muted ? 0.42f : (laneIsSelected ? 0.96f : 0.72f);
            g.setColour ((laneIsSelected ? ink() : mutedInk()).withAlpha (labelAlpha));
            g.setFont (juce::FontOptions (laneIsSelected ? 13.5f : 12.0f, juce::Font::bold));
            g.drawFittedText (lane.name, labelBounds, juce::Justification::centred, 1);
        }
    }

    void drawLaneTimingMarks (juce::Graphics& g, juce::Point<float> laneCentre, float ringRadius, juce::Colour colour, bool laneIsSelected, float phaseOffset) const
    {
        const auto count = juce::jmax (1, beatsPerBar);

        for (int beat = 0; beat < count; ++beat)
        {
            const auto isBarStart = beat == 0;
            const auto angle = phaseOffsetToAngle (phaseOffset + static_cast<float> (beat) / static_cast<float> (count));
            const auto innerRadius = ringRadius - (isBarStart ? 12.0f : 8.0f);
            const auto outerRadius = ringRadius + (isBarStart ? 11.0f : 7.0f);
            const juce::Point<float> start { laneCentre.x + std::cos (angle) * innerRadius,
                                             laneCentre.y + std::sin (angle) * innerRadius };
            const juce::Point<float> end { laneCentre.x + std::cos (angle) * outerRadius,
                                           laneCentre.y + std::sin (angle) * outerRadius };
            const auto tickAlpha = isBarStart ? (laneIsSelected ? 0.72f : 0.52f)
                                              : (laneIsSelected ? 0.48f : 0.32f);
            const auto tickWidth = isBarStart ? (laneIsSelected ? 2.1f : 1.7f)
                                              : (laneIsSelected ? 1.35f : 1.1f);

            g.setColour ((isBarStart ? ink() : colour).withAlpha (tickAlpha));
            g.drawLine (juce::Line<float> (start, end), tickWidth);

            if (isBarStart)
            {
                const auto dotRadius = laneIsSelected ? 2.8f : 2.2f;
                const juce::Point<float> dot { laneCentre.x + std::cos (angle) * (outerRadius + 3.0f),
                                               laneCentre.y + std::sin (angle) * (outerRadius + 3.0f) };
                g.setColour (ink().withAlpha (laneIsSelected ? 0.64f : 0.42f));
                g.fillEllipse (dot.x - dotRadius, dot.y - dotRadius, dotRadius * 2.0f, dotRadius * 2.0f);
            }
        }
    }

    void drawLanePlayhead (juce::Graphics& g, juce::Point<float> laneCentre, float ringRadius, const Wf::LaneSpec& lane)
    {
        const auto laneTempoBpm = lane.tempoBpm.value_or (defaultTempoBpm);
        const auto laneElapsedBars = trackElapsedBars * (static_cast<double> (laneTempoBpm) / static_cast<double> (defaultTempoBpm))
                                   - static_cast<double> (juce::jlimit (0.0f, 0.999f, lane.phaseOffsetBars));

        if (laneElapsedBars < 0.0)
            return;

        if (lane.duration.has_value())
        {
            const auto durationBars = static_cast<double> (lane.duration->bars)
                                    + static_cast<double> (lane.duration->beats) / static_cast<double> (beatsPerBar);
            if (durationBars > 0.0 && laneElapsedBars >= durationBars)
                return;
        }

        auto lanePhase = static_cast<float> (std::fmod (laneElapsedBars, 1.0));
        lanePhase += juce::jlimit (0.0f, 0.999f, lane.phaseOffsetBars);
        lanePhase -= std::floor (lanePhase);
        const auto angle = phaseOffsetToAngle (lanePhase);
        const auto alpha = lane.muted ? 0.34f : 0.96f;
        drawClockHand (g, laneCentre, angle, ringRadius - 8.0f, amber(), 1.8f, alpha, 3.0f);
    }

    int startHandleIndexAt (juce::Point<float> position) const
    {
        if (track == nullptr)
            return -1;

        for (int i = 0; i < static_cast<int> (laneCentres.size()); ++i)
        {
            const auto& lane = track->lanes[static_cast<size_t> (i)];
            const auto laneIsSelected = i == selectedLane;
            const auto ringRadius = laneIsSelected ? 54.0f : 43.5f;
            const auto angle = phaseOffsetToAngle (lane.phaseOffsetBars);
            const auto handleRadius = ringRadius + 14.0f;
            const juce::Point<float> handle { laneCentres[static_cast<size_t> (i)].x + std::cos (angle) * handleRadius,
                                              laneCentres[static_cast<size_t> (i)].y + std::sin (angle) * handleRadius };

            if (position.getDistanceFrom (handle) <= 12.0f)
                return i;
        }

        return -1;
    }

    void updateDraggedStartHandle (juce::Point<float> position)
    {
        if (track == nullptr || draggedStartHandleLane < 0 || draggedStartHandleLane >= static_cast<int> (laneCentres.size()))
            return;

        const auto phaseOffset = angleToPhaseOffset (laneCentres[static_cast<size_t> (draggedStartHandleLane)], position);
        if (onLanePhaseOffsetChanged != nullptr)
            onLanePhaseOffsetChanged (draggedStartHandleLane, phaseOffset);
    }

    const Wf::StateSpec* track = nullptr;
    std::vector<juce::Point<float>> laneCentres;
    int draggedStartHandleLane = -1;
    int selectedLane = 0;
    int beatsPerBar = 4;
    float defaultTempoBpm = 88.0f;
    double trackElapsedBars = 0.0;
    float phase = 0.0f;
    bool running = false;
};

class MixerCanvas final : public juce::Component
{
public:
    std::function<void (int, int, int)> onChannelSelected;
    std::function<void()> onMixEditStarted;
    std::function<void (int, int, int, float)> onLaneVolumeChanged;
    std::function<void (int, int, int, float)> onLanePanChanged;
    std::function<void (float)> onMasterVolumeChanged;
    std::function<void (int, int, int)> onEffectSlotClicked;
    std::function<void (int)> onMasterEffectSlotClicked;

    void setProject (std::array<std::optional<std::vector<Wf::StateSpec>>, maxTopLevelStates>* statesToUse,
                     int viewedStateToUse,
                     int selectedTrackToUse,
                     int selectedLaneToUse,
                     int playingStateToUse,
                     int playingTrackToUse,
                     bool runningToUse,
                     std::vector<ImportedLaneAudioClip>* importedLaneClipsToUse,
                     bool arrangementPlusToUse,
                     float masterGainToUse,
                     const std::array<Wf::TrackEffectSlotSpec, maxTrackEffectSlots>* masterEffectSlotsToUse)
    {
        states = statesToUse;
        viewedState = viewedStateToUse;
        selectedTrack = selectedTrackToUse;
        selectedLane = selectedLaneToUse;
        playingState = playingStateToUse;
        playingTrack = playingTrackToUse;
        running = runningToUse;
        importedLaneClips = importedLaneClipsToUse;
        arrangementPlus = arrangementPlusToUse;
        masterGain = masterGainToUse;
        masterEffectSlots = masterEffectSlotsToUse;
        rebuildChannels();
        repaint();
    }

    int getPreferredWidth() const
    {
        return juce::jmax (1, horizontalPadding * 2 + static_cast<int> (channels.size()) * channelWidth);
    }

    std::optional<juce::Range<int>> getPlayingChannelRange() const
    {
        if (! running || arrangementPlus)
            return {};

        auto first = -1;
        auto last = -1;

        for (int i = 0; i < static_cast<int> (channels.size()); ++i)
        {
            const auto& channel = channels[static_cast<size_t> (i)];
            if (isPlayingChannel (channel))
            {
                if (first < 0)
                    first = i;

                last = i;
            }
        }

        if (first < 0 || last < 0)
            return {};

        return juce::Range<int> (horizontalPadding + first * channelWidth,
                                 horizontalPadding + (last + 1) * channelWidth - channelGap);
    }

    bool isDraggingFader() const noexcept
    {
        return activeChannel >= 0;
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff0d1510));
        drawMixerHeader (g);

        if (channels.empty())
        {
            g.setColour (mutedInk().withAlpha (0.55f));
            g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
            g.drawFittedText (arrangementPlus ? "No imported audio clips to mix" : "No lanes to mix",
                              getLocalBounds().withTrimmedTop (mixerHeaderHeight).reduced (24),
                              juce::Justification::centred,
                              1);
            return;
        }

        auto clip = g.getClipBounds().toFloat();

        for (int i = 0; i < static_cast<int> (channels.size()); ++i)
        {
            auto strip = channelBounds (i);
            if (! strip.intersects (clip))
                continue;

            const auto& channel = channels[static_cast<size_t> (i)];
            const auto selected = ! arrangementPlus
                               && ! channel.isStereoOutput
                               && channel.stateIndex == viewedState
                               && channel.trackIndex == selectedTrack
                               && channel.laneIndex == selectedLane;
            const auto playing = ! arrangementPlus && ! channel.isStereoOutput && isPlayingChannel (channel);
            const auto accent = channelAccentColour (channel);

            if (channel.isStereoOutput)
            {
                g.setColour (accent.withAlpha (0.38f));
                g.drawVerticalLine (static_cast<int> (strip.getX()) - 8,
                                    strip.getY() + 2.0f,
                                    strip.getBottom() - 2.0f);
            }
            else if (i == 0 || channels[static_cast<size_t> (i - 1)].stateIndex != channel.stateIndex)
            {
                g.setColour (mutedInk().withAlpha (0.18f));
                g.drawVerticalLine (static_cast<int> (strip.getX()) - 6,
                                    strip.getY() + 4.0f,
                                    strip.getBottom() - 4.0f);
            }

            g.setColour (channel.isStereoOutput ? accent.withAlpha (0.15f)
                                                : (playing || selected ? accent.withAlpha (playing ? 0.20f : 0.16f)
                                                                       : panelSoft().withAlpha (0.30f)));
            g.fillRoundedRectangle (strip, 5.0f);
            g.setColour (accent.withAlpha (channel.isStereoOutput ? 0.68f : (playing ? 0.64f : (selected ? 0.48f : 0.24f))));
            g.drawRoundedRectangle (strip, 5.0f, channel.isStereoOutput ? 1.2f : (playing ? 1.35f : 1.0f));

            if (playing)
            {
                g.setColour (accent.withAlpha (0.88f));
                g.fillRoundedRectangle (strip.reduced (7.0f, 0.0f).removeFromTop (3.0f), 1.5f);
            }

            auto metaArea = strip.removeFromTop (28).reduced (8.0f, 7.0f);
            g.setColour (accent.withAlpha (channel.isStereoOutput ? 0.26f : 0.17f));
            g.fillRoundedRectangle (metaArea, 3.0f);
            g.setColour (accent.withAlpha (channel.isStereoOutput ? 0.90f : 0.70f));
            g.setFont (juce::FontOptions (9.5f, juce::Font::bold));
            g.drawFittedText (channelMetaLabel (channel), metaArea.toNearestInt().reduced (3, 0), juce::Justification::centred, 1);

            auto labelArea = strip.removeFromTop (55).reduced (8.0f, 2.0f);
            g.setColour (ink().withAlpha (channel.isStereoOutput ? 0.90f : 0.82f));
            g.setFont (juce::FontOptions (10.7f, juce::Font::bold));
            g.drawFittedText (channel.trackName, labelArea.removeFromTop (21).toNearestInt(), juce::Justification::centred, 1);
            g.setColour (mutedInk().withAlpha (channel.isStereoOutput ? 0.82f : 0.72f));
            g.setFont (juce::FontOptions (10.5f, juce::Font::bold));
            g.drawFittedText (channel.laneName, labelArea.removeFromTop (20).toNearestInt(), juce::Justification::centred, 1);

            if (! channel.isStereoOutput)
            {
                auto pan = panBounds (i);
                const auto panNorm = (juce::jlimit (-1.0f, 1.0f, channel.pan) + 1.0f) * 0.5f;
                const auto panX = pan.getX() + pan.getWidth() * panNorm;
                g.setColour (mutedInk().withAlpha (0.14f));
                g.fillRoundedRectangle (pan, 3.0f);
                g.setColour (mutedInk().withAlpha (0.24f));
                g.drawVerticalLine (static_cast<int> (pan.getCentreX()), pan.getY() - 3.0f, pan.getBottom() + 3.0f);
                g.setColour (accent.withAlpha (0.74f));
                g.fillEllipse (panX - 5.0f, pan.getCentreY() - 5.0f, 10.0f, 10.0f);
                g.setColour (mutedInk().withAlpha (0.52f));
                g.setFont (juce::FontOptions (9.0f, juce::Font::bold));
                g.drawFittedText ("pan", pan.translated (0.0f, -14.0f).toNearestInt(), juce::Justification::centred, 1);
            }

            auto fader = faderBounds (i);
            const auto norm = juce::jlimit (0.0f, 1.0f, channel.volume / getMaxChannelGain (channel));
            const auto thumbY = fader.getBottom() - fader.getHeight() * norm;

            g.setColour (mutedInk().withAlpha (0.14f));
            auto rail = fader.withWidth (7.0f).withCentre ({ fader.getCentreX(), fader.getCentreY() });
            g.fillRoundedRectangle (rail, 3.5f);
            g.setColour (mutedInk().withAlpha (0.18f));
            for (int tick = 0; tick <= 4; ++tick)
            {
                const auto y = fader.getY() + fader.getHeight() * static_cast<float> (tick) / 4.0f;
                g.drawLine (fader.getCentreX() - 11.0f, y, fader.getCentreX() - 6.0f, y, 1.0f);
                g.drawLine (fader.getCentreX() + 6.0f, y, fader.getCentreX() + 11.0f, y, 1.0f);
            }
            g.setColour (accent.withAlpha (0.74f));
            g.fillRoundedRectangle (juce::Rectangle<float> (7.0f, fader.getBottom() - thumbY).withPosition (fader.getCentreX() - 3.5f, thumbY), 3.5f);
            g.setColour (ink());
            g.fillRoundedRectangle (fader.getCentreX() - 10.0f, thumbY - 7.0f, 20.0f, 14.0f, 7.0f);
            g.setColour (juce::Colour (0xff07100a).withAlpha (0.36f));
            g.drawHorizontalLine (static_cast<int> (std::round (thumbY)), fader.getCentreX() - 6.0f, fader.getCentreX() + 6.0f);

            g.setColour (ink().withAlpha (channel.isStereoOutput ? 0.92f : 0.84f));
            g.setFont (juce::FontOptions (10.8f, juce::Font::bold));
            g.drawFittedText (juce::String (channel.volume, channel.isStereoOutput ? 3 : 2),
                              valueBounds (i).toNearestInt(),
                              juce::Justification::centred,
                              1);

            if (arrangementPlus)
            {
                for (int slotIndex = 0; slotIndex < maxTrackEffectSlots; ++slotIndex)
                {
                    const auto slot = effectSlotBounds (i, slotIndex);
                    const auto active = channel.effectActive[static_cast<size_t> (slotIndex)];
                    const auto hasPlugin = channel.effectNames[static_cast<size_t> (slotIndex)].isNotEmpty();
                    g.setColour (active ? accent.withAlpha (0.26f) : panel().withAlpha (0.78f));
                    g.fillRoundedRectangle (slot, 3.5f);
                    g.setColour ((active ? accent : mutedInk()).withAlpha (active ? 0.78f : (hasPlugin ? 0.36f : 0.18f)));
                    g.drawRoundedRectangle (slot, 3.5f, active ? 1.0f : 0.8f);
                    g.setColour ((active ? ink() : mutedInk()).withAlpha (active ? 0.88f : (hasPlugin ? 0.58f : 0.38f)));
                    g.setFont (juce::FontOptions (9.0f, juce::Font::bold));
                    g.drawFittedText (hasPlugin ? shortEffectName (channel.effectNames[static_cast<size_t> (slotIndex)])
                                                 : "FX" + juce::String (slotIndex + 1),
                                      slot.toNearestInt(),
                                      juce::Justification::centred,
                                      1);
                }
            }
        }
    }

    void mouseDown (const juce::MouseEvent& event) override
    {
        activeChannel = findChannelAt (event.position);
        if (activeChannel < 0)
            return;

        selectActiveChannel();

        const auto& channel = channels[static_cast<size_t> (activeChannel)];

        if (arrangementPlus && findEffectSlotAt (activeChannel, event.position) >= 0)
        {
            const auto slot = findEffectSlotAt (activeChannel, event.position);
            if (channel.isStereoOutput && onMasterEffectSlotClicked != nullptr)
                onMasterEffectSlotClicked (slot);
            else if (activeChannel < static_cast<int> (channels.size()) && onEffectSlotClicked != nullptr)
                onEffectSlotClicked (channel.stateIndex, channel.trackIndex, slot);

            activeControl = ActiveControl::none;
            activeChannel = -1;
        }
        else if (! channel.isStereoOutput && panBounds (activeChannel).expanded (8.0f, 8.0f).contains (event.position))
        {
            activeControl = ActiveControl::pan;
            if (onMixEditStarted != nullptr)
                onMixEditStarted();
            setActiveChannelPanFromX (event.position.x);
        }
        else if (faderBounds (activeChannel).expanded (14.0f, 8.0f).contains (event.position))
        {
            activeControl = ActiveControl::volume;
            if (onMixEditStarted != nullptr)
                onMixEditStarted();
            setActiveChannelVolumeFromY (event.position.y);
        }
        else
        {
            activeControl = ActiveControl::none;
        }
    }

    void mouseDrag (const juce::MouseEvent& event) override
    {
        if (activeChannel >= 0 && activeControl == ActiveControl::volume)
            setActiveChannelVolumeFromY (event.position.y);
        else if (activeChannel >= 0 && activeControl == ActiveControl::pan)
            setActiveChannelPanFromX (event.position.x);
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        activeChannel = -1;
        activeControl = ActiveControl::none;
    }

private:
    enum class ActiveControl
    {
        none,
        volume,
        pan
    };

    struct Channel
    {
        int stateIndex = 0;
        int trackIndex = 0;
        int laneIndex = 0;
        juce::String trackName;
        juce::String laneName;
        juce::String fileName;
        int importedClipIndex = -1;
        float volume = 0.0f;
        float pan = 0.0f;
        std::array<bool, maxTrackEffectSlots> effectActive {};
        std::array<juce::String, maxTrackEffectSlots> effectNames {};
        bool isStereoOutput = false;
    };

    void rebuildChannels()
    {
        channels.clear();

        if (states == nullptr)
            return;

        if (arrangementPlus && importedLaneClips != nullptr && ! importedLaneClips->empty())
        {
            for (int clipIndex = 0; clipIndex < static_cast<int> (importedLaneClips->size()); ++clipIndex)
            {
                auto channel = channelForClip (clipIndex, importedLaneClips->at (static_cast<size_t> (clipIndex)));
                if (channel.has_value())
                    channels.push_back (*channel);
            }

            if (! channels.empty())
                channels.push_back (stereoOutputChannel());

            return;
        }

        for (int stateIndex = 0; stateIndex < maxTopLevelStates; ++stateIndex)
        {
            const auto& stateSlot = (*states)[static_cast<size_t> (stateIndex)];
            if (! stateSlot.has_value())
                continue;

            const auto& tracks = *stateSlot;
            for (int trackIndex = 0; trackIndex < static_cast<int> (tracks.size()); ++trackIndex)
            {
                const auto& track = tracks[static_cast<size_t> (trackIndex)];
                for (int laneIndex = 0; laneIndex < static_cast<int> (track.lanes.size()); ++laneIndex)
                {
                    const auto& lane = track.lanes[static_cast<size_t> (laneIndex)];
                    Channel channel;
                    channel.stateIndex = stateIndex;
                    channel.trackIndex = trackIndex;
                    channel.laneIndex = laneIndex;
                    channel.trackName = trackDisplayName (track.name);
                    channel.laneName = lane.name;
                    channel.volume = lane.volume;
                    channel.pan = lane.pan;

                    for (int effectIndex = 0; effectIndex < maxTrackEffectSlots; ++effectIndex)
                    {
                        const auto& effect = track.effectSlots[static_cast<size_t> (effectIndex)];
                        channel.effectActive[static_cast<size_t> (effectIndex)] = effect.active && effect.pluginName.isNotEmpty();
                        channel.effectNames[static_cast<size_t> (effectIndex)] = effect.pluginName;
                    }

                    channels.push_back (std::move (channel));
                }
            }
        }
    }

    std::optional<Channel> channelForClip (int clipIndex, const ImportedLaneAudioClip& clip) const
    {
        if (states == nullptr || clip.stateIndex < 0 || clip.stateIndex >= maxTopLevelStates)
            return {};

        auto& stateSlot = (*states)[static_cast<size_t> (clip.stateIndex)];
        if (! stateSlot.has_value())
            return {};

        auto& tracks = *stateSlot;
        if (clip.trackIndex < 0 || clip.trackIndex >= static_cast<int> (tracks.size()))
            return {};

        const auto& track = tracks[static_cast<size_t> (clip.trackIndex)];
        if (clip.laneIndex < 0 || clip.laneIndex >= static_cast<int> (track.lanes.size()))
            return {};

        const auto& lane = track.lanes[static_cast<size_t> (clip.laneIndex)];
        Channel channel;
        channel.stateIndex = clip.stateIndex;
        channel.trackIndex = clip.trackIndex;
        channel.laneIndex = clip.laneIndex;
        channel.trackName = trackDisplayName (track.name);
        channel.laneName = lane.name;
        channel.fileName = clip.file.getFileName();
        channel.importedClipIndex = clipIndex;
        channel.volume = clip.mixGain;
        channel.pan = clip.mixPan;

        for (int effectIndex = 0; effectIndex < maxTrackEffectSlots; ++effectIndex)
        {
            const auto& effect = track.effectSlots[static_cast<size_t> (effectIndex)];
            channel.effectActive[static_cast<size_t> (effectIndex)] = effect.active && effect.pluginName.isNotEmpty();
            channel.effectNames[static_cast<size_t> (effectIndex)] = effect.pluginName;
        }

        return channel;
    }

    Channel stereoOutputChannel() const
    {
        Channel channel;
        channel.stateIndex = -1;
        channel.trackIndex = -1;
        channel.laneIndex = -1;
        channel.trackName = "STEREO";
        channel.laneName = "Output";
        channel.volume = masterGain;
        channel.isStereoOutput = true;

        if (masterEffectSlots != nullptr)
        {
            for (int effectIndex = 0; effectIndex < maxTrackEffectSlots; ++effectIndex)
            {
                const auto& effect = (*masterEffectSlots)[static_cast<size_t> (effectIndex)];
                channel.effectActive[static_cast<size_t> (effectIndex)] = effect.active && effect.pluginName.isNotEmpty();
                channel.effectNames[static_cast<size_t> (effectIndex)] = effect.pluginName;
            }
        }

        return channel;
    }

    void drawMixerHeader (juce::Graphics& g) const
    {
        auto header = getLocalBounds().toFloat().removeFromTop (mixerHeaderHeight).reduced (horizontalPadding, 8.0f);
        if (header.getWidth() <= 0.0f)
            return;

        const auto accent = arrangementPlus ? cyan() : amber();
        g.setColour (panelSoft().withAlpha (0.30f));
        g.fillRoundedRectangle (header, 5.0f);
        g.setColour (accent.withAlpha (0.16f));
        g.drawRoundedRectangle (header, 5.0f, 1.0f);
        g.setColour (accent.withAlpha (0.24f));
        g.fillRoundedRectangle (header.reduced (10.0f, 0.0f).removeFromTop (2.0f), 1.0f);

        const auto title = arrangementPlus ? "Mixer+" : "Mixer";
        auto left = header.reduced (12.0f, 0.0f);
        auto right = left.removeFromRight (220.0f);

        g.setColour (ink().withAlpha (0.88f));
        g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        g.drawFittedText (title, left.toNearestInt(), juce::Justification::centredLeft, 1);

        const auto outputCount = channels.empty() ? 0 : static_cast<int> (channels.size()) - (arrangementPlus ? 1 : 0);
        g.setColour (mutedInk().withAlpha (0.58f));
        g.setFont (juce::FontOptions (10.5f, juce::Font::bold));
        g.drawFittedText (juce::String (juce::jmax (0, outputCount)) + (outputCount == 1 ? " channel" : " channels"),
                          right.toNearestInt(),
                          juce::Justification::centredRight,
                          1);
    }

    static juce::String channelMetaLabel (const Channel& channel)
    {
        if (channel.isStereoOutput)
            return "OUT";

        return juce::String ("S") + juce::String (channel.stateIndex + 1)
             + "  T" + juce::String (channel.trackIndex + 1)
             + "  L" + juce::String (channel.laneIndex + 1);
    }

    static juce::Colour channelAccentColour (const Channel& channel)
    {
        if (channel.isStereoOutput)
            return amber();

        return laneAccentForIndex (channel.laneIndex);
    }

    bool isPlayingChannel (const Channel& channel) const noexcept
    {
        return ! arrangementPlus
            && running
            && channel.stateIndex == playingState
            && channel.trackIndex == playingTrack;
    }

    juce::Rectangle<float> channelBounds (int channelIndex) const
    {
        return { static_cast<float> (horizontalPadding + channelIndex * channelWidth),
                 mixerHeaderHeight + 12.0f,
                 static_cast<float> (channelWidth - channelGap),
                 static_cast<float> (juce::jmax (120, getHeight() - mixerHeaderHeight - 24)) };
    }

    juce::Rectangle<float> faderBounds (int channelIndex) const
    {
        auto strip = channelBounds (channelIndex).reduced (0.0f, 12.0f);
        const auto& channel = channels[static_cast<size_t> (channelIndex)];
        strip.removeFromTop (channel.isStereoOutput ? 92.0f : 126.0f);
        strip.removeFromBottom (arrangementPlus ? 104.0f : 44.0f);
        const auto height = juce::jlimit (58.0f, 218.0f, strip.getHeight());
        return { strip.getCentreX() - 13.0f, strip.getCentreY() - height * 0.5f, 26.0f, height };
    }

    juce::Rectangle<float> panBounds (int channelIndex) const
    {
        auto strip = channelBounds (channelIndex).reduced (8.0f, 0.0f);
        return { strip.getX(), strip.getY() + 100.0f, strip.getWidth(), 6.0f };
    }

    juce::Rectangle<float> valueBounds (int channelIndex) const
    {
        auto strip = channelBounds (channelIndex).reduced (9.0f, 0.0f);
        const auto y = arrangementPlus ? effectSlotBounds (channelIndex, 0).getY() - 25.0f
                                       : strip.getBottom() - 30.0f;
        return { strip.getX(), y, strip.getWidth(), 17.0f };
    }

    juce::Rectangle<float> effectSlotBounds (int channelIndex, int slotIndex) const
    {
        auto strip = channelBounds (channelIndex).reduced (7.0f, 0.0f);
        const auto y = strip.getBottom() - 70.0f + static_cast<float> (slotIndex) * 21.0f;
        return { strip.getX(), y, strip.getWidth(), 17.0f };
    }

    int findChannelAt (juce::Point<float> point) const
    {
        if (point.x < static_cast<float> (horizontalPadding))
            return -1;

        const auto channelIndex = static_cast<int> ((point.x - static_cast<float> (horizontalPadding)) / static_cast<float> (channelWidth));
        if (channelIndex < 0 || channelIndex >= static_cast<int> (channels.size()))
            return -1;

        return channelBounds (channelIndex).contains (point) ? channelIndex : -1;
    }

    int findEffectSlotAt (int channelIndex, juce::Point<float> point) const
    {
        for (int slotIndex = 0; slotIndex < maxTrackEffectSlots; ++slotIndex)
            if (effectSlotBounds (channelIndex, slotIndex).contains (point))
                return slotIndex;

        return -1;
    }

    Wf::LaneSpec* getLane (const Channel& channel) const
    {
        if (states == nullptr || channel.stateIndex < 0 || channel.stateIndex >= maxTopLevelStates)
            return nullptr;

        auto& stateSlot = (*states)[static_cast<size_t> (channel.stateIndex)];
        if (! stateSlot.has_value())
            return nullptr;

        auto& tracks = *stateSlot;
        if (channel.trackIndex < 0 || channel.trackIndex >= static_cast<int> (tracks.size()))
            return nullptr;

        auto& lanes = tracks[static_cast<size_t> (channel.trackIndex)].lanes;
        if (channel.laneIndex < 0 || channel.laneIndex >= static_cast<int> (lanes.size()))
            return nullptr;

        return &lanes[static_cast<size_t> (channel.laneIndex)];
    }

    ImportedLaneAudioClip* getImportedClip (const Channel& channel) const
    {
        if (importedLaneClips == nullptr || channel.importedClipIndex < 0)
            return nullptr;

        if (channel.importedClipIndex >= static_cast<int> (importedLaneClips->size()))
            return nullptr;

        return &importedLaneClips->at (static_cast<size_t> (channel.importedClipIndex));
    }

    void selectActiveChannel()
    {
        if (activeChannel < 0 || activeChannel >= static_cast<int> (channels.size()))
            return;

        const auto& channel = channels[static_cast<size_t> (activeChannel)];
        if (channel.isStereoOutput)
            return;

        if (onChannelSelected != nullptr)
            onChannelSelected (channel.stateIndex, channel.trackIndex, channel.laneIndex);
    }

    void setActiveChannelVolumeFromY (float y)
    {
        if (activeChannel < 0 || activeChannel >= static_cast<int> (channels.size()))
            return;

        auto& channel = channels[static_cast<size_t> (activeChannel)];
        const auto fader = faderBounds (activeChannel);
        const auto norm = 1.0f - juce::jlimit (0.0f, 1.0f, (y - fader.getY()) / juce::jmax (1.0f, fader.getHeight()));
        const auto maxGain = getMaxChannelGain (channel);
        const auto volume = juce::jlimit (0.0f, maxGain, norm * maxGain);

        if (channel.isStereoOutput)
        {
            channel.volume = volume;
            masterGain = volume;

            if (onMasterVolumeChanged != nullptr)
                onMasterVolumeChanged (volume);

            repaint();
            return;
        }

        if (arrangementPlus)
        {
            auto* clip = getImportedClip (channel);
            if (clip == nullptr)
                return;

            clip->mixGain = volume;
        }
        else
        {
            auto* lane = getLane (channel);
            if (lane == nullptr)
                return;

            lane->volume = volume;
        }

        channel.volume = volume;

        if (onLaneVolumeChanged != nullptr)
            onLaneVolumeChanged (channel.stateIndex, channel.trackIndex, channel.laneIndex, volume);

        repaint();
    }

    void setActiveChannelPanFromX (float x)
    {
        if (activeChannel < 0 || activeChannel >= static_cast<int> (channels.size()))
            return;

        auto& channel = channels[static_cast<size_t> (activeChannel)];
        if (channel.isStereoOutput)
            return;

        const auto panArea = panBounds (activeChannel);
        const auto norm = juce::jlimit (0.0f, 1.0f, (x - panArea.getX()) / juce::jmax (1.0f, panArea.getWidth()));
        const auto pan = norm * 2.0f - 1.0f;

        if (arrangementPlus)
        {
            auto* clip = getImportedClip (channel);
            if (clip == nullptr)
                return;

            clip->mixPan = pan;
        }
        else
        {
            auto* lane = getLane (channel);
            if (lane == nullptr)
                return;

            lane->pan = pan;
        }

        channel.pan = pan;

        if (onLanePanChanged != nullptr)
            onLanePanChanged (channel.stateIndex, channel.trackIndex, channel.laneIndex, pan);

        repaint();
    }

    static juce::String shortEffectName (const juce::String& name)
    {
        if (name.isEmpty())
            return "FX";

        return name.substring (0, 5);
    }

    float getMaxChannelGain() const noexcept
    {
        return arrangementPlus ? maxImportedClipGain : maxLaneVolume;
    }

    float getMaxChannelGain (const Channel& channel) const noexcept
    {
        return channel.isStereoOutput ? maxMasterGain : getMaxChannelGain();
    }

    static constexpr int mixerHeaderHeight = 44;
    static constexpr int horizontalPadding = 18;
    static constexpr int channelWidth = 102;
    static constexpr int channelGap = 10;
    static constexpr float maxLaneVolume = 0.8f;
    static constexpr float maxImportedClipGain = 1.5f;
    static constexpr float maxMasterGain = 0.8f;

    std::array<std::optional<std::vector<Wf::StateSpec>>, maxTopLevelStates>* states = nullptr;
    std::vector<ImportedLaneAudioClip>* importedLaneClips = nullptr;
    const std::array<Wf::TrackEffectSlotSpec, maxTrackEffectSlots>* masterEffectSlots = nullptr;
    std::vector<Channel> channels;
    int viewedState = 0;
    int selectedTrack = 0;
    int selectedLane = 0;
    int playingState = 0;
    int playingTrack = 0;
    int activeChannel = -1;
    ActiveControl activeControl = ActiveControl::none;
    bool running = false;
    bool arrangementPlus = false;
    float masterGain = 0.18f;
};

class TrackFocusDivider final : public juce::Component
{
public:
    TrackFocusDivider()
    {
        setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
    }

    std::function<void()> onDragStart;
    std::function<void (int)> onDragDelta;

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();
        const auto hover = isMouseOverOrDragging();
        const auto x = bounds.getCentreX();

        g.setColour (mutedInk().withAlpha (hover ? 0.13f : 0.055f));
        g.fillRoundedRectangle (bounds.reduced (6.0f, 0.0f), 2.0f);

        g.setColour ((hover ? blue() : mutedInk()).withAlpha (hover ? 0.45f : 0.16f));
        g.drawLine (x, bounds.getY() + 12.0f, x, bounds.getBottom() - 12.0f, hover ? 1.4f : 1.0f);
    }

    void mouseEnter (const juce::MouseEvent&) override
    {
        repaint();
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        repaint();
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        if (onDragStart != nullptr)
            onDragStart();
    }

    void mouseDrag (const juce::MouseEvent& event) override
    {
        if (onDragDelta != nullptr)
            onDragDelta (event.getDistanceFromDragStartX());
    }
};

class CodeViewDivider final : public juce::Component
{
public:
    CodeViewDivider()
    {
        setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
    }

    std::function<void()> onDragStart;
    std::function<void (int)> onDragDelta;

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();
        const auto hover = isMouseOverOrDragging();
        const auto y = bounds.getCentreY();

        g.setColour (mutedInk().withAlpha (hover ? 0.13f : 0.045f));
        g.fillRoundedRectangle (bounds.reduced (0.0f, 4.0f), 2.0f);

        g.setColour ((hover ? blue() : mutedInk()).withAlpha (hover ? 0.48f : 0.16f));
        g.drawLine (bounds.getX() + 14.0f, y, bounds.getRight() - 14.0f, y, hover ? 1.4f : 1.0f);
    }

    void mouseEnter (const juce::MouseEvent&) override
    {
        repaint();
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        repaint();
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        if (onDragStart != nullptr)
            onDragStart();
    }

    void mouseDrag (const juce::MouseEvent& event) override
    {
        if (onDragDelta != nullptr)
            onDragDelta (event.getDistanceFromDragStartY());
    }
};

class OverallCanvas final : public juce::Component
{
public:
    std::function<void (int)> onStateSelected;

    void setProject (const std::array<std::optional<std::vector<Wf::StateSpec>>, maxTopLevelStates>* statesToUse,
                     int viewedStateToUse,
                     int playingStateToUse,
                     bool runningToUse)
    {
        states = statesToUse;
        viewedState = viewedStateToUse;
        playingState = playingStateToUse;
        running = runningToUse;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff0a130e));

        if (states == nullptr)
            return;

        const auto area = getLocalBounds().reduced (34);
        const auto columns = 4;
        const auto rows = 4;
        const auto gap = 16;
        const auto cellW = (area.getWidth() - gap * (columns - 1)) / columns;
        const auto cellH = (area.getHeight() - gap * (rows - 1)) / rows;

        stateBounds.clear();
        stateBounds.reserve (maxTopLevelStates);

        for (int i = 0; i < maxTopLevelStates; ++i)
        {
            const auto col = i % columns;
            const auto row = i / columns;
            auto cell = juce::Rectangle<int> (area.getX() + col * (cellW + gap),
                                              area.getY() + row * (cellH + gap),
                                              cellW,
                                              cellH);
            stateBounds.push_back (cell);
            drawStateCell (g, cell, i);
        }
    }

    void mouseDown (const juce::MouseEvent& event) override
    {
        if (onStateSelected == nullptr)
            return;

        for (int i = 0; i < static_cast<int> (stateBounds.size()); ++i)
            if (stateBounds[static_cast<size_t> (i)].contains (event.getPosition()))
            {
                onStateSelected (i);
                return;
            }
    }

private:
    void drawStateCell (juce::Graphics& g, juce::Rectangle<int> cell, int index)
    {
        const auto populated = states != nullptr
            && states->at (static_cast<size_t> (index)).has_value()
            && ! states->at (static_cast<size_t> (index))->empty();
        const auto selected = index == viewedState;
        const auto playing = running && index == playingState;
        const auto accent = playing ? amber() : stateAccentForIndex (index);

        g.setColour (populated ? accent.withAlpha (0.085f) : panel().withAlpha (0.38f));
        g.fillRoundedRectangle (cell.toFloat(), 4.0f);

        g.setColour ((populated ? accent : stateAccentForIndex (index)).withAlpha (populated ? (selected || playing ? 0.72f : 0.28f) : 0.08f));
        g.drawRoundedRectangle (cell.toFloat().reduced (0.5f), 4.0f, selected || playing ? 1.2f : 0.8f);

        auto inner = cell.reduced (14);
        g.setColour ((populated ? ink() : mutedInk()).withAlpha (populated ? 0.92f : 0.26f));
        g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        g.drawFittedText ("State " + juce::String (index + 1), inner.removeFromTop (22), juce::Justification::centredLeft, 1);

        if (! populated)
            return;

        const auto& tracks = *states->at (static_cast<size_t> (index));
        const auto trackCount = static_cast<int> (tracks.size());
        const auto laneCount = std::accumulate (tracks.begin(), tracks.end(), 0, [] (int total, const Wf::StateSpec& track)
        {
            return total + static_cast<int> (track.lanes.size());
        });

        g.setColour (accent.withAlpha (selected || playing ? 0.34f : 0.18f));
        const auto circleSize = juce::jmin (inner.getWidth(), inner.getHeight() - 34);
        auto circle = juce::Rectangle<int> (circleSize, circleSize).withCentre (inner.getCentre());
        circle.translate (0, -4);
        g.fillEllipse (circle.toFloat());
        g.setColour (accent.withAlpha (selected || playing ? 0.92f : 0.58f));
        g.drawEllipse (circle.toFloat(), selected || playing ? 1.8f : 1.1f);

        const auto dotCount = juce::jmin (6, laneCount);
        for (int dot = 0; dot < dotCount; ++dot)
        {
            const auto dotColour = laneAccentForIndex (dot);
            const auto dotX = static_cast<float> (inner.getX() + dot * 12);
            const auto dotY = static_cast<float> (inner.getBottom() - 45);
            g.setColour (dotColour.withAlpha (selected || playing ? 0.88f : 0.56f));
            g.fillEllipse (dotX, dotY, 5.0f, 5.0f);
        }

        g.setColour (ink().withAlpha (0.90f));
        g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        g.drawFittedText (trackCount == 1 ? "1 track" : juce::String (trackCount) + " tracks",
                          inner.removeFromBottom (18),
                          juce::Justification::centredLeft,
                          1);

        g.setColour (mutedInk().withAlpha (0.70f));
        g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        g.drawFittedText (laneCount == 1 ? "1 lane" : juce::String (laneCount) + " lanes",
                          inner.removeFromBottom (18),
                          juce::Justification::centredLeft,
                          1);
    }

    const std::array<std::optional<std::vector<Wf::StateSpec>>, maxTopLevelStates>* states = nullptr;
    std::vector<juce::Rectangle<int>> stateBounds;
    int viewedState = 0;
    int playingState = 0;
    bool running = false;
};

class ArrangementTimelineCanvas final : public juce::Component
{
public:
    struct RegionHit
    {
        int clipIndex = -1;
        int regionIndex = -1;
        double timeSeconds = 0.0;
        bool nearStart = false;
        bool nearEnd = false;
        bool nearFadeIn = false;
        bool nearFadeOut = false;
        bool nearGain = false;
    };

    enum class ActiveDrag { none, fade, trimStart, trimEnd, gain, playhead, marquee };

    std::function<void (int, int, bool)> onImportedRegionClicked;
    std::function<void (std::vector<ArrangementAudioSelection>, bool)> onImportedRegionsMarqueeSelected;
    std::function<void (int, int, double)> onImportedRegionSplit;
    std::function<void (int, int)> onImportedRegionTrimEditStarted;
    std::function<void (int, int, double, double, double)> onImportedRegionTrimChanged;
    std::function<void()> onImportedRegionTrimEditEnded;
    std::function<void (int, int)> onImportedRegionFadeEditStarted;
    std::function<void (int, int, double, double)> onImportedRegionFadeChanged;
    std::function<void()> onImportedRegionFadeEditEnded;
    std::function<void (int, int, bool)> onImportedRegionFadeCurveCycle;
    std::function<void (int, int)> onImportedRegionGainEditStarted;
    std::function<void (int, int, float)> onImportedRegionGainChanged;
    std::function<void()> onImportedRegionGainEditEnded;
    std::function<void (double)> onArrangementPlayheadChanged;

    void setProject (const std::array<std::optional<std::vector<Wf::StateSpec>>, maxTopLevelStates>* statesToUse,
                     std::vector<GlobalScriptStep> stepsToUse,
                     int playingStateToUse,
                     int playingTrackToUse,
                     size_t playingStepToUse,
                     double playingStepElapsedBarsToUse,
                     bool runningToUse,
                     const std::vector<ImportedLaneAudioClip>* importedLaneClipsToUse,
                     bool arrangementPlusToUse,
                     const std::array<float, maxTopLevelStates>* stateTemposToUse,
                     const std::array<int, maxTopLevelStates>* stateNumeratorsToUse,
                     const std::array<int, maxTopLevelStates>* stateDenominatorsToUse,
                     ArrangementAudioSelection selectedImportedRegionToUse,
                     std::vector<ArrangementAudioSelection> selectedImportedRegionsToUse,
                     double editPlayheadSecondsToUse,
                     ArrangementAudioTool audioToolToUse)
    {
        states = statesToUse;
        steps = std::move (stepsToUse);
        playingState = playingStateToUse;
        playingTrack = playingTrackToUse;
        playingStep = playingStepToUse;
        playingStepElapsedBars = playingStepElapsedBarsToUse;
        running = runningToUse;
        importedLaneClips = importedLaneClipsToUse;
        arrangementPlus = arrangementPlusToUse;
        stateTempos = stateTemposToUse;
        stateNumerators = stateNumeratorsToUse;
        stateDenominators = stateDenominatorsToUse;
        selectedImportedRegion = selectedImportedRegionToUse;
        selectedImportedRegions = std::move (selectedImportedRegionsToUse);
        editPlayheadSeconds = juce::jmax (0.0, editPlayheadSecondsToUse);
        audioTool = audioToolToUse;
        rebuildMetrics();
        repaint();
    }

    int getPreferredWidth() const
    {
        return juce::jmax (760, leftHeaderWidth + 60 + static_cast<int> (totalBars * getBarWidth()));
    }

    int getPreferredHeight() const
    {
        return juce::jmax (360, headerHeight + maxTracks * (getTrackHeaderHeight() + maxLanesInAnyTrack * getLaneRowHeight() + getTrackGap()) + 34);
    }

    void setZoom (double horizontalZoomToUse, double verticalZoomToUse)
    {
        horizontalZoom = juce::jlimit (0.35, 3.25, horizontalZoomToUse);
        verticalZoom = juce::jlimit (0.55, 3.0, verticalZoomToUse);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff0d1510));

        if (states == nullptr)
            return;

        const auto bounds = getLocalBounds();
        const auto timelineX = leftHeaderWidth;
        const auto timelineRight = getWidth() - 24;
        const auto contentTop = headerHeight;

        g.setColour (panel().withAlpha (0.82f));
        g.fillRect (bounds.withWidth (leftHeaderWidth));

        g.setColour (mutedInk().withAlpha (0.15f));
        g.drawVerticalLine (timelineX, 0.0f, static_cast<float> (getHeight()));
        g.drawHorizontalLine (headerHeight - 1, 0.0f, static_cast<float> (getWidth()));

        if (steps.empty())
        {
            g.setColour (mutedInk().withAlpha (0.58f));
            g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
            g.drawFittedText ("No playable state sequence", getLocalBounds().reduced (24), juce::Justification::centred, 1);
            return;
        }

        drawRuler (g, timelineX, timelineRight);
        drawTrackLabels (g, contentTop);

        double barOffset = 0.0;
        const auto barWidth = getBarWidth();
        for (int stepIndex = 0; stepIndex < static_cast<int> (steps.size()); ++stepIndex)
        {
            const auto& step = steps[static_cast<size_t> (stepIndex)];
            const auto stepX = static_cast<float> (timelineX) + static_cast<float> (barOffset * barWidth);
            const auto stepW = static_cast<float> (juce::jmax (0.25, step.bars) * barWidth);
            drawStep (g, step, stepIndex, barOffset, { stepX, 0.0f, stepW, static_cast<float> (getHeight()) });
            barOffset += juce::jmax (0.0, step.bars);
        }

        if (arrangementPlus)
        {
            const auto x = static_cast<float> (leftHeaderWidth + barAtSeconds (currentPlayheadSeconds()) * getBarWidth());
            const auto colour = running && playingStep < steps.size() ? stateAccentForIndex (steps[playingStep].stateIndex) : cyan();
            g.setColour (colour.withAlpha (0.94f));
            g.drawLine (x, 0.0f, x, static_cast<float> (getHeight()), 1.9f);
            g.fillRoundedRectangle (x - 6.0f, 6.0f, 12.0f, 16.0f, 2.0f);
        }
        else if (running && playingStep < steps.size())
        {
            double playheadBars = 0.0;
            for (size_t i = 0; i < playingStep; ++i)
                playheadBars += juce::jmax (0.0, steps[i].bars);

            playheadBars += juce::jlimit (0.0, juce::jmax (0.0, steps[playingStep].bars), playingStepElapsedBars);
            const auto x = static_cast<float> (timelineX) + static_cast<float> (playheadBars * getBarWidth());
            const auto playheadColour = stateAccentForIndex (steps[playingStep].stateIndex);
            g.setColour (playheadColour.withAlpha (0.92f));
            g.drawLine (x, 0.0f, x, static_cast<float> (getHeight()), 1.8f);
            g.fillRoundedRectangle (x - 5.0f, 6.0f, 10.0f, 16.0f, 2.0f);
        }

        if (activeDrag == ActiveDrag::marquee)
        {
            const auto marquee = marqueeBounds();
            g.setColour (blue().withAlpha (0.12f));
            g.fillRect (marquee);
            g.setColour (blue().withAlpha (0.55f));
            g.drawRect (marquee, 1.0f);
        }

        drawDragReadout (g);
    }

    void mouseDown (const juce::MouseEvent& event) override
    {
        if (! arrangementPlus)
            return;

        if (isNearPlayhead (event.position)
            || (event.position.y <= static_cast<float> (headerHeight)
                && event.position.x >= static_cast<float> (leftHeaderWidth)))
        {
            activeDrag = ActiveDrag::playhead;
            updateArrangementPlayhead (event.position, ! event.mods.isCommandDown());
            return;
        }

        if (const auto hit = findImportedRegionAt (event.position); hit.has_value())
        {
            if (onImportedRegionClicked != nullptr)
                onImportedRegionClicked (hit->clipIndex, hit->regionIndex, event.mods.isShiftDown());

            if (audioTool == ArrangementAudioTool::scissors)
            {
                if (onImportedRegionSplit != nullptr)
                    onImportedRegionSplit (hit->clipIndex,
                                           hit->regionIndex,
                                           timeForMouseEvent (hit->timeSeconds, event));

                return;
            }

            if (audioTool == ArrangementAudioTool::fade || hit->nearFadeIn || hit->nearFadeOut)
            {
                activeFadeDrag = *hit;
                activeDrag = ActiveDrag::fade;
                activeFadeDragEditingOut = hit->nearFadeOut || (! hit->nearFadeIn && regionDragPrefersFadeOut (*hit));
                if (onImportedRegionFadeEditStarted != nullptr)
                    onImportedRegionFadeEditStarted (hit->clipIndex, hit->regionIndex);

                updateActiveFadeDrag (event.position);
                return;
            }

            if (hit->nearStart || hit->nearEnd)
            {
                activeFadeDrag = *hit;
                activeDrag = hit->nearStart ? ActiveDrag::trimStart : ActiveDrag::trimEnd;
                if (onImportedRegionTrimEditStarted != nullptr)
                    onImportedRegionTrimEditStarted (hit->clipIndex, hit->regionIndex);

                updateActiveTrimDrag (event.position, ! event.mods.isCommandDown());
                return;
            }

            if (hit->nearGain)
            {
                activeFadeDrag = *hit;
                activeDrag = ActiveDrag::gain;
                if (onImportedRegionGainEditStarted != nullptr)
                    onImportedRegionGainEditStarted (hit->clipIndex, hit->regionIndex);

                updateActiveGainDrag (event.position);
                return;
            }

            return;
        }

        activeFadeDrag.reset();
        activeDrag = ActiveDrag::marquee;
        marqueeStart = event.position;
        marqueeCurrent = event.position;
        marqueeAddsToSelection = event.mods.isShiftDown();

        if (! marqueeAddsToSelection && onImportedRegionsMarqueeSelected != nullptr)
            onImportedRegionsMarqueeSelected ({}, false);

        repaint();
    }

    void mouseDrag (const juce::MouseEvent& event) override
    {
        if (activeDrag == ActiveDrag::fade && activeFadeDrag.has_value())
            updateActiveFadeDrag (event.position);
        else if ((activeDrag == ActiveDrag::trimStart || activeDrag == ActiveDrag::trimEnd) && activeFadeDrag.has_value())
            updateActiveTrimDrag (event.position, ! event.mods.isCommandDown());
        else if (activeDrag == ActiveDrag::gain && activeFadeDrag.has_value())
            updateActiveGainDrag (event.position);
        else if (activeDrag == ActiveDrag::playhead)
            updateArrangementPlayhead (event.position, ! event.mods.isCommandDown());
        else if (activeDrag == ActiveDrag::marquee)
        {
            marqueeCurrent = event.position;
            repaint();
        }
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (activeDrag == ActiveDrag::fade && activeFadeDrag.has_value() && onImportedRegionFadeEditEnded != nullptr)
            onImportedRegionFadeEditEnded();

        if ((activeDrag == ActiveDrag::trimStart || activeDrag == ActiveDrag::trimEnd)
            && activeFadeDrag.has_value()
            && onImportedRegionTrimEditEnded != nullptr)
            onImportedRegionTrimEditEnded();

        if (activeDrag == ActiveDrag::gain && activeFadeDrag.has_value() && onImportedRegionGainEditEnded != nullptr)
            onImportedRegionGainEditEnded();

        if (activeDrag == ActiveDrag::marquee && onImportedRegionsMarqueeSelected != nullptr)
            onImportedRegionsMarqueeSelected (regionsInMarquee(), marqueeAddsToSelection);

        activeFadeDrag.reset();
        activeDrag = ActiveDrag::none;
        dragReadoutText = {};
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent& event) override
    {
        if (! arrangementPlus)
            return;

        if (const auto hit = findImportedRegionAt (event.position); hit.has_value())
        {
            if ((hit->nearFadeIn || hit->nearFadeOut || audioTool == ArrangementAudioTool::fade)
                && onImportedRegionFadeCurveCycle != nullptr)
                onImportedRegionFadeCurveCycle (hit->clipIndex, hit->regionIndex, hit->nearFadeOut || regionDragPrefersFadeOut (*hit));
        }
    }

    void mouseMove (const juce::MouseEvent& event) override
    {
        if (! arrangementPlus || activeDrag != ActiveDrag::none)
        {
            setMouseCursor (juce::MouseCursor::NormalCursor);
            return;
        }

        if (isNearPlayhead (event.position)
            || (event.position.y <= static_cast<float> (headerHeight)
                && event.position.x >= static_cast<float> (leftHeaderWidth)))
        {
            setMouseCursor (juce::MouseCursor::DraggingHandCursor);
            return;
        }

        if (const auto hit = findImportedRegionAt (event.position); hit.has_value())
        {
            if (audioTool == ArrangementAudioTool::scissors)
                setMouseCursor (juce::MouseCursor::CrosshairCursor);
            else if (audioTool == ArrangementAudioTool::fade || hit->nearFadeIn || hit->nearFadeOut)
                setMouseCursor (juce::MouseCursor::DraggingHandCursor);
            else if (hit->nearStart || hit->nearEnd)
                setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
            else if (hit->nearGain)
                setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
            else
                setMouseCursor (juce::MouseCursor::NormalCursor);

            return;
        }

        setMouseCursor (juce::MouseCursor::NormalCursor);
    }

private:
    void rebuildMetrics()
    {
        totalBars = 0.0;
        maxTracks = 1;
        maxLanesInAnyTrack = 1;

        for (const auto& step : steps)
        {
            totalBars += juce::jmax (0.0, step.bars);
            const auto* tracks = getTracks (step.stateIndex);
            if (tracks == nullptr)
                continue;

            maxTracks = juce::jmax (maxTracks, static_cast<int> (tracks->size()));
            for (const auto& track : *tracks)
                maxLanesInAnyTrack = juce::jmax (maxLanesInAnyTrack, static_cast<int> (track.lanes.size()));
        }
    }

    const std::vector<Wf::StateSpec>* getTracks (int stateIndex) const
    {
        if (states == nullptr || stateIndex < 0 || stateIndex >= maxTopLevelStates)
            return nullptr;

        const auto& slot = states->at (static_cast<size_t> (stateIndex));
        if (! slot.has_value())
            return nullptr;

        return &*slot;
    }

    void drawRuler (juce::Graphics& g, int timelineX, int timelineRight)
    {
        const auto visibleBars = static_cast<int> (std::ceil (totalBars));
        const auto barWidth = getBarWidth();
        const auto labelEveryBars = juce::jmax (1, static_cast<int> (std::ceil (34.0 / juce::jmax (1.0, barWidth))));
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));

        for (int bar = 0; bar <= visibleBars; ++bar)
        {
            const auto x = static_cast<float> (static_cast<double> (timelineX) + static_cast<double> (bar) * barWidth);
            if (x > static_cast<float> (timelineRight))
                break;

            g.setColour (mutedInk().withAlpha (bar == 0 ? 0.30f : 0.15f));
            g.drawLine (x, 0.0f, x, static_cast<float> (getHeight()), bar == 0 ? 1.2f : 0.8f);

            if (bar > 0 && bar % labelEveryBars == 0)
            {
                g.setColour (mutedInk().withAlpha (0.82f));
                g.drawFittedText (juce::String (bar), juce::Rectangle<int> (static_cast<int> (x) + 5, 8, 44, 16), juce::Justification::centredLeft, 1);
            }
        }
    }

    void drawTrackLabels (juce::Graphics& g, int contentTop)
    {
        auto y = contentTop;
        const auto trackHeaderHeight = getTrackHeaderHeight();
        const auto laneRowHeight = getLaneRowHeight();
        const auto trackGap = getTrackGap();
        for (int trackIndex = 0; trackIndex < maxTracks; ++trackIndex)
        {
            const auto trackHeight = trackHeaderHeight + maxLanesInAnyTrack * laneRowHeight;
            auto trackArea = juce::Rectangle<int> (0, y, leftHeaderWidth, trackHeight);

            g.setColour (mutedInk().withAlpha (0.09f));
            g.drawHorizontalLine (trackArea.getY(), 0.0f, static_cast<float> (getWidth()));

            g.setColour (ink().withAlpha (0.82f));
            g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
            g.drawFittedText ("TRACK " + juce::String (trackIndex + 1),
                              trackArea.removeFromTop (trackHeaderHeight).reduced (14, 2),
                              juce::Justification::centredLeft,
                              1);

            g.setColour (mutedInk().withAlpha (0.48f));
            g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
            for (int laneIndex = 0; laneIndex < maxLanesInAnyTrack; ++laneIndex)
                g.drawFittedText ("lane " + juce::String (laneIndex + 1),
                                  juce::Rectangle<int> (22, y + trackHeaderHeight + laneIndex * laneRowHeight, leftHeaderWidth - 34, laneRowHeight),
                                  juce::Justification::centredLeft,
                                  1);

            y += trackHeight + trackGap;
        }
    }

    void drawStep (juce::Graphics& g, const GlobalScriptStep& step, int stepIndex, double stepStartBars, juce::Rectangle<float> bounds)
    {
        const auto* tracks = getTracks (step.stateIndex);
        if (tracks == nullptr)
            return;

        const auto accent = stateAccentForIndex (step.stateIndex);
        const auto playingStepNow = running && static_cast<size_t> (stepIndex) == playingStep;
        const auto trackHeaderHeight = getTrackHeaderHeight();
        const auto laneRowHeight = getLaneRowHeight();
        const auto trackGap = getTrackGap();

        auto header = bounds.withY (0.0f).withHeight (static_cast<float> (headerHeight - 1)).reduced (2.0f, 4.0f);
        g.setColour (accent.withAlpha (playingStepNow ? 0.32f : 0.20f));
        g.fillRoundedRectangle (header, 3.0f);
        g.setColour (accent.withAlpha (playingStepNow ? 0.80f : 0.48f));
        g.drawRoundedRectangle (header, 3.0f, playingStepNow ? 1.3f : 0.9f);

        g.setColour (ink().withAlpha (0.90f));
        g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        g.drawFittedText ("State " + juce::String (step.stateIndex + 1) + "  " + juce::String (step.bars, 1) + " bars",
                          header.toNearestInt().reduced (8, 0),
                          juce::Justification::centredLeft,
                          1);

        for (int trackIndex = 0; trackIndex < static_cast<int> (tracks->size()); ++trackIndex)
        {
            const auto& track = (*tracks)[static_cast<size_t> (trackIndex)];
            const auto trackY = headerHeight + trackIndex * (trackHeaderHeight + maxLanesInAnyTrack * laneRowHeight + trackGap);
            auto trackNameArea = juce::Rectangle<float> (bounds.getX(), static_cast<float> (trackY), bounds.getWidth(), static_cast<float> (trackHeaderHeight)).reduced (2.0f, 2.0f);

            g.setColour ((playingStepNow && trackIndex == playingTrack && step.stateIndex == playingState ? accent : mutedInk()).withAlpha (0.17f));
            g.fillRoundedRectangle (trackNameArea, 2.0f);
            g.setColour (ink().withAlpha (0.78f));
            g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            g.drawFittedText (trackDisplayName (track.name), trackNameArea.toNearestInt().reduced (6, 0), juce::Justification::centredLeft, 1);

            for (int laneIndex = 0; laneIndex < static_cast<int> (track.lanes.size()); ++laneIndex)
            {
                const auto& lane = track.lanes[static_cast<size_t> (laneIndex)];
                auto laneArea = juce::Rectangle<float> (bounds.getX() + 3.0f,
                                                        static_cast<float> (trackY + trackHeaderHeight + laneIndex * laneRowHeight + 2),
                                                        bounds.getWidth() - 6.0f,
                                                        static_cast<float> (laneRowHeight - 4));
                const auto laneColour = laneColourForIndex (laneIndex);
                const auto active = ! lane.muted;
                const auto importedClipIndex = findImportedClipIndex (step.stateIndex, trackIndex, laneIndex);
                const auto* importedClip = importedClipIndex >= 0 && importedLaneClips != nullptr
                    ? &importedLaneClips->at (static_cast<size_t> (importedClipIndex))
                    : nullptr;

                g.setColour (laneColour.withAlpha (active ? (importedClip != nullptr ? 0.36f : 0.26f) : 0.08f));
                g.fillRoundedRectangle (laneArea, 2.0f);

                if (importedClip != nullptr)
                    drawImportedAudioClipRegions (g,
                                                  laneArea,
                                                  laneColour,
                                                  *importedClip,
                                                  importedClipIndex,
                                                  stepStartBars,
                                                  step.bars);
                else
                {
                    g.setColour (laneColour.withAlpha (active ? 0.76f : 0.24f));
                    g.drawRoundedRectangle (laneArea, 2.0f, 0.8f);

                    g.setColour ((active ? ink() : mutedInk()).withAlpha (active ? 0.86f : 0.34f));
                    g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
                    g.drawFittedText (lane.name, laneArea.toNearestInt().reduced (6, 0), juce::Justification::centredLeft, 1);
                }
            }
        }
    }

    const ImportedLaneAudioClip* findImportedClip (int stateIndex, int trackIndex, int laneIndex) const
    {
        const auto index = findImportedClipIndex (stateIndex, trackIndex, laneIndex);
        if (index < 0 || importedLaneClips == nullptr)
            return nullptr;

        return &importedLaneClips->at (static_cast<size_t> (index));
    }

    int findImportedClipIndex (int stateIndex, int trackIndex, int laneIndex) const
    {
        if (! arrangementPlus || importedLaneClips == nullptr)
            return -1;

        for (int i = 0; i < static_cast<int> (importedLaneClips->size()); ++i)
        {
            const auto& clip = importedLaneClips->at (static_cast<size_t> (i));
            if (clip.stateIndex == stateIndex
                && clip.trackIndex == trackIndex
                && clip.laneIndex == laneIndex
                && clip.file.existsAsFile())
                return i;
        }

        return -1;
    }

    void drawImportedAudioClipRegions (juce::Graphics& g,
                                       juce::Rectangle<float> laneArea,
                                       juce::Colour laneColour,
                                       const ImportedLaneAudioClip& clip,
                                       int clipIndex,
                                       double stepStartBars,
                                       double stepBars) const
    {
        auto drewAnyRegion = false;
        for (int regionIndex = 0; regionIndex < static_cast<int> (clip.regions.size()); ++regionIndex)
        {
            const auto& region = clip.regions[static_cast<size_t> (regionIndex)];
            if (region.lengthSeconds <= 0.0)
                continue;

            const auto regionStartBars = barAtSeconds (region.startSeconds);
            const auto regionEndBars = barAtSeconds (regionEndSeconds (region));
            const auto regionBounds = regionBoundsForStep (laneArea,
                                                           stepStartBars,
                                                           stepBars,
                                                           regionStartBars,
                                                           regionEndBars);
            if (regionBounds.getWidth() <= 0.5f)
                continue;

            drewAnyRegion = true;
            const auto selected = isImportedRegionSelected (clipIndex, regionIndex);
            const auto visibleStartSeconds = secondsAtBar (stepStartBars + ((regionBounds.getX() - laneArea.getX()) / juce::jmax (1.0f, laneArea.getWidth())) * stepBars);
            const auto visibleEndSeconds = secondsAtBar (stepStartBars + ((regionBounds.getRight() - laneArea.getX()) / juce::jmax (1.0f, laneArea.getWidth())) * stepBars);
            drawImportedAudioRegion (g,
                                     regionBounds,
                                     laneColour,
                                     clip,
                                     region,
                                     visibleStartSeconds,
                                     visibleEndSeconds,
                                     selected);
        }

        if (! drewAnyRegion)
        {
            g.setColour (laneColour.withAlpha (0.16f));
            g.drawRoundedRectangle (laneArea.reduced (0.5f), 2.0f, 0.8f);
            g.setColour (mutedInk().withAlpha (0.44f));
            g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
            g.drawFittedText ("no audio regions", laneArea.toNearestInt().reduced (6, 0), juce::Justification::centredLeft, 1);
        }
    }

    juce::Rectangle<float> regionBoundsForStep (juce::Rectangle<float> laneArea,
                                                double stepStartBars,
                                                double stepBars,
                                                double regionStartBars,
                                                double regionEndBars) const
    {
        const auto stepEndBars = stepStartBars + stepBars;
        const auto drawStartBars = juce::jmax (stepStartBars, regionStartBars);
        const auto drawEndBars = juce::jmin (stepEndBars, regionEndBars);
        if (drawEndBars <= drawStartBars || stepBars <= 0.0)
            return {};

        const auto x = laneArea.getX() + static_cast<float> ((drawStartBars - stepStartBars) / stepBars) * laneArea.getWidth();
        const auto right = laneArea.getX() + static_cast<float> ((drawEndBars - stepStartBars) / stepBars) * laneArea.getWidth();
        return { x, laneArea.getY(), juce::jmax (0.0f, right - x), laneArea.getHeight() };
    }

    std::optional<RegionHit> findImportedRegionAt (juce::Point<float> position) const
    {
        if (! arrangementPlus || importedLaneClips == nullptr || states == nullptr || steps.empty())
            return {};

        double stepStartBars = 0.0;
        for (const auto& step : steps)
        {
            const auto* tracks = getTracks (step.stateIndex);
            if (tracks == nullptr)
            {
                stepStartBars += juce::jmax (0.0, step.bars);
                continue;
            }

            const auto stepX = static_cast<float> (leftHeaderWidth) + static_cast<float> (stepStartBars * getBarWidth());
            const auto stepW = static_cast<float> (juce::jmax (0.25, step.bars) * getBarWidth());
            const auto trackHeaderHeight = getTrackHeaderHeight();
            const auto laneRowHeight = getLaneRowHeight();
            const auto trackGap = getTrackGap();

            for (int trackIndex = 0; trackIndex < static_cast<int> (tracks->size()); ++trackIndex)
            {
                const auto& track = tracks->at (static_cast<size_t> (trackIndex));
                const auto trackY = headerHeight + trackIndex * (trackHeaderHeight + maxLanesInAnyTrack * laneRowHeight + trackGap);

                for (int laneIndex = 0; laneIndex < static_cast<int> (track.lanes.size()); ++laneIndex)
                {
                    auto laneArea = juce::Rectangle<float> (stepX + 3.0f,
                                                            static_cast<float> (trackY + trackHeaderHeight + laneIndex * laneRowHeight + 2),
                                                            stepW - 6.0f,
                                                            static_cast<float> (laneRowHeight - 4));
                    if (! laneArea.expanded (0.0f, 2.0f).contains (position))
                        continue;

                    const auto clipIndex = findImportedClipIndex (step.stateIndex, trackIndex, laneIndex);
                    if (clipIndex < 0)
                        continue;

                    const auto& clip = importedLaneClips->at (static_cast<size_t> (clipIndex));
                    for (int regionIndex = 0; regionIndex < static_cast<int> (clip.regions.size()); ++regionIndex)
                    {
                        const auto& region = clip.regions[static_cast<size_t> (regionIndex)];
                        const auto regionBounds = regionBoundsForStep (laneArea,
                                                                       stepStartBars,
                                                                       step.bars,
                                                                       barAtSeconds (region.startSeconds),
                                                                       barAtSeconds (regionEndSeconds (region)));
                        if (! regionBounds.expanded (2.0f, 3.0f).contains (position))
                            continue;

                        const auto bar = stepStartBars
                                       + (static_cast<double> (position.x - laneArea.getX()) / juce::jmax (1.0, static_cast<double> (laneArea.getWidth())))
                                       * step.bars;
                        RegionHit hit;
                        hit.clipIndex = clipIndex;
                        hit.regionIndex = regionIndex;
                        hit.timeSeconds = juce::jlimit (region.startSeconds, regionEndSeconds (region), secondsAtBar (bar));
                        hit.nearStart = std::abs (position.x - regionBounds.getX()) <= fadeHandleHitWidth;
                        hit.nearEnd = std::abs (position.x - regionBounds.getRight()) <= fadeHandleHitWidth;
                        const auto nearTopHandle = position.y <= regionBounds.getY() + juce::jmin (10.0f, regionBounds.getHeight() * 0.55f);
                        hit.nearFadeIn = hit.nearStart && nearTopHandle;
                        hit.nearFadeOut = hit.nearEnd && nearTopHandle;
                        hit.nearGain = std::abs (position.y - gainYForRegion (regionBounds, region.gain)) <= 5.0f
                                    && ! hit.nearFadeIn
                                    && ! hit.nearFadeOut;
                        return hit;
                    }
                }
            }

            stepStartBars += juce::jmax (0.0, step.bars);
        }

        return {};
    }

    std::optional<juce::Rectangle<float>> boundsForImportedRegion (int wantedClipIndex, int wantedRegionIndex) const
    {
        if (! arrangementPlus || importedLaneClips == nullptr || states == nullptr || steps.empty())
            return {};

        double stepStartBars = 0.0;
        for (const auto& step : steps)
        {
            const auto* tracks = getTracks (step.stateIndex);
            if (tracks == nullptr)
            {
                stepStartBars += juce::jmax (0.0, step.bars);
                continue;
            }

            const auto stepX = static_cast<float> (leftHeaderWidth) + static_cast<float> (stepStartBars * getBarWidth());
            const auto stepW = static_cast<float> (juce::jmax (0.25, step.bars) * getBarWidth());
            const auto trackHeaderHeight = getTrackHeaderHeight();
            const auto laneRowHeight = getLaneRowHeight();
            const auto trackGap = getTrackGap();

            for (int trackIndex = 0; trackIndex < static_cast<int> (tracks->size()); ++trackIndex)
            {
                const auto& track = tracks->at (static_cast<size_t> (trackIndex));
                const auto trackY = headerHeight + trackIndex * (trackHeaderHeight + maxLanesInAnyTrack * laneRowHeight + trackGap);

                for (int laneIndex = 0; laneIndex < static_cast<int> (track.lanes.size()); ++laneIndex)
                {
                    const auto clipIndex = findImportedClipIndex (step.stateIndex, trackIndex, laneIndex);
                    if (clipIndex != wantedClipIndex)
                        continue;

                    const auto& clip = importedLaneClips->at (static_cast<size_t> (clipIndex));
                    if (wantedRegionIndex < 0 || wantedRegionIndex >= static_cast<int> (clip.regions.size()))
                        return {};

                    auto laneArea = juce::Rectangle<float> (stepX + 3.0f,
                                                            static_cast<float> (trackY + trackHeaderHeight + laneIndex * laneRowHeight + 2),
                                                            stepW - 6.0f,
                                                            static_cast<float> (laneRowHeight - 4));
                    const auto& region = clip.regions[static_cast<size_t> (wantedRegionIndex)];
                    return regionBoundsForStep (laneArea,
                                                stepStartBars,
                                                step.bars,
                                                barAtSeconds (region.startSeconds),
                                                barAtSeconds (regionEndSeconds (region)));
                }
            }

            stepStartBars += juce::jmax (0.0, step.bars);
        }

        return {};
    }

    void updateActiveFadeDrag (juce::Point<float> position)
    {
        if (! activeFadeDrag.has_value()
            || importedLaneClips == nullptr
            || activeFadeDrag->clipIndex < 0
            || activeFadeDrag->clipIndex >= static_cast<int> (importedLaneClips->size()))
            return;

        const auto& clip = importedLaneClips->at (static_cast<size_t> (activeFadeDrag->clipIndex));
        if (activeFadeDrag->regionIndex < 0 || activeFadeDrag->regionIndex >= static_cast<int> (clip.regions.size()))
            return;

        const auto& region = clip.regions[static_cast<size_t> (activeFadeDrag->regionIndex)];
        const auto timeSeconds = juce::jlimit (region.startSeconds,
                                               regionEndSeconds (region),
                                               secondsAtX (position.x));
        auto fadeIn = region.fadeInSeconds;
        auto fadeOut = region.fadeOutSeconds;

        if (activeFadeDragEditingOut)
            fadeOut = regionEndSeconds (region) - timeSeconds;
        else
            fadeIn = timeSeconds - region.startSeconds;

        if (onImportedRegionFadeChanged != nullptr)
            onImportedRegionFadeChanged (activeFadeDrag->clipIndex,
                                         activeFadeDrag->regionIndex,
                                         fadeIn,
                                         fadeOut);

        setDragReadout ((activeFadeDragEditingOut ? "fade out " : "fade in ")
                         + formatTimelineSeconds (activeFadeDragEditingOut ? fadeOut : fadeIn),
                         position);
    }

    void updateActiveTrimDrag (juce::Point<float> position, bool snapToGrid)
    {
        if (! activeFadeDrag.has_value()
            || importedLaneClips == nullptr
            || activeFadeDrag->clipIndex < 0
            || activeFadeDrag->clipIndex >= static_cast<int> (importedLaneClips->size()))
            return;

        const auto& clip = importedLaneClips->at (static_cast<size_t> (activeFadeDrag->clipIndex));
        if (activeFadeDrag->regionIndex < 0 || activeFadeDrag->regionIndex >= static_cast<int> (clip.regions.size()))
            return;

        const auto& region = clip.regions[static_cast<size_t> (activeFadeDrag->regionIndex)];
        const auto originalEnd = regionEndSeconds (region);
        auto startSeconds = region.startSeconds;
        auto sourceStartSeconds = region.sourceStartSeconds;
        auto lengthSeconds = region.lengthSeconds;
        const auto timeSeconds = timeForPosition (position.x, snapToGrid);

        if (activeDrag == ActiveDrag::trimStart)
        {
            const auto earliestStart = region.startSeconds - region.sourceStartSeconds;
            startSeconds = juce::jlimit (juce::jmax (0.0, earliestStart),
                                         originalEnd - minRegionEditLengthSeconds,
                                         timeSeconds);
            sourceStartSeconds = juce::jlimit (0.0,
                                               clip.lengthSeconds,
                                               region.sourceStartSeconds + (startSeconds - region.startSeconds));
            lengthSeconds = originalEnd - startSeconds;
        }
        else if (activeDrag == ActiveDrag::trimEnd)
        {
            const auto latestEnd = region.startSeconds + (clip.lengthSeconds - region.sourceStartSeconds);
            const auto endSeconds = juce::jlimit (region.startSeconds + minRegionEditLengthSeconds,
                                                  latestEnd,
                                                  timeSeconds);
            lengthSeconds = endSeconds - region.startSeconds;
        }

        if (onImportedRegionTrimChanged != nullptr)
            onImportedRegionTrimChanged (activeFadeDrag->clipIndex,
                                         activeFadeDrag->regionIndex,
                                         startSeconds,
                                         sourceStartSeconds,
                                         lengthSeconds);

        setDragReadout ("start " + formatTimelineSeconds (startSeconds)
                         + "  length " + formatTimelineSeconds (lengthSeconds),
                         position);
    }

    void updateActiveGainDrag (juce::Point<float> position)
    {
        if (! activeFadeDrag.has_value())
            return;

        const auto bounds = boundsForImportedRegion (activeFadeDrag->clipIndex, activeFadeDrag->regionIndex);
        if (! bounds.has_value() || bounds->getHeight() <= 0.0f)
            return;

        const auto normalised = juce::jlimit (0.0f, 1.0f, 1.0f - (position.y - bounds->getY()) / bounds->getHeight());
        const auto gain = juce::jlimit (0.0f, 2.0f, normalised * 2.0f);

        if (onImportedRegionGainChanged != nullptr)
            onImportedRegionGainChanged (activeFadeDrag->clipIndex, activeFadeDrag->regionIndex, gain);

        setDragReadout ("gain " + juce::String (gain, 2), position);
    }

    void updateArrangementPlayhead (juce::Point<float> position, bool snapToGrid)
    {
        editPlayheadSeconds = juce::jlimit (0.0, totalDurationSeconds(), timeForPosition (position.x, snapToGrid));
        if (onArrangementPlayheadChanged != nullptr)
            onArrangementPlayheadChanged (editPlayheadSeconds);
        setDragReadout ("playhead " + formatTimelineSeconds (editPlayheadSeconds), position);
        repaint();
    }

    double currentPlayheadSeconds() const
    {
        if (arrangementPlus || ! running)
            return editPlayheadSeconds;

        auto seconds = 0.0;
        for (size_t i = 0; i < playingStep && i < steps.size(); ++i)
            seconds += stepDurationSeconds (steps[i]);

        if (playingStep < steps.size())
            seconds += stepDurationSeconds (steps[playingStep])
                     * juce::jlimit (0.0, 1.0, playingStepElapsedBars / juce::jmax (0.0001, steps[playingStep].bars));

        return seconds;
    }

    double totalDurationSeconds() const
    {
        auto seconds = 0.0;
        for (const auto& step : steps)
            seconds += stepDurationSeconds (step);
        return seconds;
    }

    bool isNearPlayhead (juce::Point<float> position) const
    {
        if (! arrangementPlus)
            return false;

        const auto x = static_cast<float> (leftHeaderWidth + barAtSeconds (currentPlayheadSeconds()) * getBarWidth());
        return position.x >= static_cast<float> (leftHeaderWidth)
            && std::abs (position.x - x) <= 7.0f;
    }

    bool isImportedRegionSelected (int clipIndex, int regionIndex) const
    {
        const ArrangementAudioSelection selection { clipIndex, regionIndex };
        if (selectedImportedRegion == selection)
            return true;

        return std::find (selectedImportedRegions.begin(), selectedImportedRegions.end(), selection) != selectedImportedRegions.end();
    }

    std::vector<ArrangementAudioSelection> regionsInMarquee() const
    {
        std::vector<ArrangementAudioSelection> matches;
        if (! arrangementPlus || importedLaneClips == nullptr || states == nullptr || steps.empty())
            return matches;

        const auto marquee = marqueeBounds().getIntersection (getLocalBounds().toFloat());
        if (marquee.getWidth() < 2.0f && marquee.getHeight() < 2.0f)
            return matches;

        double stepStartBars = 0.0;
        for (const auto& step : steps)
        {
            const auto* tracks = getTracks (step.stateIndex);
            if (tracks == nullptr)
            {
                stepStartBars += juce::jmax (0.0, step.bars);
                continue;
            }

            const auto stepX = static_cast<float> (leftHeaderWidth) + static_cast<float> (stepStartBars * getBarWidth());
            const auto stepW = static_cast<float> (juce::jmax (0.25, step.bars) * getBarWidth());
            const auto trackHeaderHeight = getTrackHeaderHeight();
            const auto laneRowHeight = getLaneRowHeight();
            const auto trackGap = getTrackGap();

            for (int trackIndex = 0; trackIndex < static_cast<int> (tracks->size()); ++trackIndex)
            {
                const auto& track = tracks->at (static_cast<size_t> (trackIndex));
                const auto trackY = headerHeight + trackIndex * (trackHeaderHeight + maxLanesInAnyTrack * laneRowHeight + trackGap);

                for (int laneIndex = 0; laneIndex < static_cast<int> (track.lanes.size()); ++laneIndex)
                {
                    const auto clipIndex = findImportedClipIndex (step.stateIndex, trackIndex, laneIndex);
                    if (clipIndex < 0)
                        continue;

                    auto laneArea = juce::Rectangle<float> (stepX + 3.0f,
                                                            static_cast<float> (trackY + trackHeaderHeight + laneIndex * laneRowHeight + 2),
                                                            stepW - 6.0f,
                                                            static_cast<float> (laneRowHeight - 4));
                    const auto& clip = importedLaneClips->at (static_cast<size_t> (clipIndex));
                    for (int regionIndex = 0; regionIndex < static_cast<int> (clip.regions.size()); ++regionIndex)
                    {
                        const auto& region = clip.regions[static_cast<size_t> (regionIndex)];
                        const auto regionBounds = regionBoundsForStep (laneArea,
                                                                       stepStartBars,
                                                                       step.bars,
                                                                       barAtSeconds (region.startSeconds),
                                                                       barAtSeconds (regionEndSeconds (region)));
                        if (regionBounds.isEmpty() || ! marquee.intersects (regionBounds))
                            continue;

                        ArrangementAudioSelection selection { clipIndex, regionIndex };
                        if (std::find (matches.begin(), matches.end(), selection) == matches.end())
                            matches.push_back (selection);
                    }
                }
            }

            stepStartBars += juce::jmax (0.0, step.bars);
        }

        return matches;
    }

    juce::Rectangle<float> marqueeBounds() const
    {
        const auto left = juce::jmin (marqueeStart.x, marqueeCurrent.x);
        const auto top = juce::jmin (marqueeStart.y, marqueeCurrent.y);
        const auto right = juce::jmax (marqueeStart.x, marqueeCurrent.x);
        const auto bottom = juce::jmax (marqueeStart.y, marqueeCurrent.y);
        return { left, top, right - left, bottom - top };
    }

    bool regionDragPrefersFadeOut (const RegionHit& hit) const
    {
        if (importedLaneClips == nullptr
            || hit.clipIndex < 0
            || hit.clipIndex >= static_cast<int> (importedLaneClips->size()))
            return false;

        const auto& clip = importedLaneClips->at (static_cast<size_t> (hit.clipIndex));
        if (hit.regionIndex < 0 || hit.regionIndex >= static_cast<int> (clip.regions.size()))
            return false;

        const auto& region = clip.regions[static_cast<size_t> (hit.regionIndex)];
        return hit.timeSeconds >= region.startSeconds + region.lengthSeconds * 0.5;
    }

    static float gainYForRegion (juce::Rectangle<float> area, float gain)
    {
        const auto normalised = juce::jlimit (0.0f, 1.0f, gain / 2.0f);
        return area.getBottom() - normalised * area.getHeight();
    }

    void drawImportedAudioRegion (juce::Graphics& g,
                                  juce::Rectangle<float> laneArea,
                                  juce::Colour laneColour,
                                  const ImportedLaneAudioClip& clip,
                                  const ImportedAudioRegion& region,
                                  double visibleStartSeconds,
                                  double visibleEndSeconds,
                                  bool selected) const
    {
        laneArea = laneArea.reduced (0.5f);
        if (selected)
        {
            g.setColour (cyan().withAlpha (0.10f));
            g.fillRoundedRectangle (laneArea.expanded (1.0f), 3.0f);
        }

        g.setColour (laneColour.withAlpha (selected ? 0.96f : 0.82f));
        g.drawRoundedRectangle (laneArea, 2.0f, selected ? 1.65f : 1.1f);

        auto waveArea = laneArea.reduced (5.0f, 4.0f);
        const auto centreY = waveArea.getCentreY();
        g.setColour (mutedInk().withAlpha (0.18f));
        g.drawHorizontalLine (static_cast<int> (centreY), waveArea.getX(), waveArea.getRight());

        if (! clip.waveformPeaks.empty())
        {
            const auto columns = juce::jmax (1, static_cast<int> (waveArea.getWidth()));
            g.setColour (laneColour.withAlpha (selected ? 0.96f : 0.86f));
            for (int column = 0; column < columns; column += 2)
            {
                const auto columnNorm = static_cast<double> (column) / juce::jmax (1.0, static_cast<double> (columns - 1));
                const auto timeSeconds = visibleStartSeconds + (visibleEndSeconds - visibleStartSeconds) * columnNorm;
                const auto sourceSeconds = region.sourceStartSeconds + (timeSeconds - region.startSeconds);
                const auto sourceNorm = clip.lengthSeconds > 0.0 ? juce::jlimit (0.0, 1.0, sourceSeconds / clip.lengthSeconds) : 0.0;
                const auto peakIndex = juce::jlimit (0,
                                                     static_cast<int> (clip.waveformPeaks.size()) - 1,
                                                     static_cast<int> (sourceNorm * static_cast<double> (clip.waveformPeaks.size())));
                const auto peak = juce::jlimit (0.0f, 1.0f, clip.waveformPeaks[static_cast<size_t> (peakIndex)]);
                const auto height = juce::jmax (1.0f, peak * waveArea.getHeight() * 0.50f);
                const auto x = waveArea.getX() + static_cast<float> (column);
                g.drawLine (x, centreY - height, x, centreY + height, 1.15f);
            }
        }
        else
        {
            g.setColour (laneColour.withAlpha (0.38f));
            for (int x = static_cast<int> (waveArea.getX()); x < static_cast<int> (waveArea.getRight()); x += 5)
            {
                const auto phase = static_cast<float> ((x / 5) % 7) / 6.0f;
                const auto height = 2.0f + std::sin (phase * juce::MathConstants<float>::pi) * juce::jmax (1.0f, waveArea.getHeight() * 0.34f);
                g.drawLine (static_cast<float> (x), centreY - height, static_cast<float> (x), centreY + height, 0.8f);
            }
        }

        drawFadeOverlay (g, laneArea, laneColour, region, selected);

        const auto gainY = gainYForRegion (laneArea.reduced (4.0f, 3.0f), region.gain);
        g.setColour ((selected ? cyan() : ink()).withAlpha (selected ? 0.70f : 0.30f));
        g.drawLine (laneArea.getX() + 5.0f, gainY, laneArea.getRight() - 5.0f, gainY, selected ? 1.2f : 0.7f);
        if (selected)
        {
            g.setColour (cyan().withAlpha (0.88f));
            g.fillRoundedRectangle (laneArea.getCentreX() - 3.0f, gainY - 3.0f, 6.0f, 6.0f, 3.0f);
            g.setColour (ink().withAlpha (0.86f));
            g.fillRoundedRectangle (laneArea.getX() - 1.0f, laneArea.getY() + 3.0f, 3.0f, laneArea.getHeight() - 6.0f, 1.5f);
            g.fillRoundedRectangle (laneArea.getRight() - 2.0f, laneArea.getY() + 3.0f, 3.0f, laneArea.getHeight() - 6.0f, 1.5f);
        }

        auto labelArea = laneArea.withWidth (juce::jmin (laneArea.getWidth(), 320.0f)).reduced (4.0f, 2.0f);
        g.setColour (juce::Colour (0xff080c09).withAlpha (0.72f));
        g.fillRoundedRectangle (labelArea, 2.0f);
        g.setColour (ink().withAlpha (0.88f));
        g.setFont (juce::FontOptions (9.2f, juce::Font::bold));
        g.drawFittedText (clip.file.getFileName(), labelArea.toNearestInt().reduced (5, 0), juce::Justification::centredLeft, 1);
    }

    static juce::Path fadeCurvePath (juce::Rectangle<float> laneArea, float fadeWidth, bool fadeIn, int curve)
    {
        juce::Path path;
        const auto samples = 20;
        for (int i = 0; i <= samples; ++i)
        {
            const auto norm = static_cast<float> (i) / static_cast<float> (samples);
            const auto shaped = applyRegionFadeCurve (fadeIn ? norm : (1.0f - norm), curve);
            const auto x = fadeIn ? laneArea.getX() + fadeWidth * norm
                                  : laneArea.getRight() - fadeWidth + fadeWidth * norm;
            const auto y = laneArea.getBottom() - shaped * laneArea.getHeight();
            if (i == 0)
                path.startNewSubPath (x, y);
            else
                path.lineTo (x, y);
        }
        return path;
    }

    static void drawFadeOverlay (juce::Graphics& g,
                                 juce::Rectangle<float> laneArea,
                                 juce::Colour laneColour,
                                 const ImportedAudioRegion& region,
                                 bool selected)
    {
        if (region.lengthSeconds <= 0.0)
            return;

        const auto fadeAlpha = selected ? 0.30f : 0.18f;
        if (region.fadeInSeconds > 0.000001)
        {
            const auto fadeWidth = laneArea.getWidth() * static_cast<float> (juce::jlimit (0.0, 1.0, region.fadeInSeconds / region.lengthSeconds));
            juce::Path fade;
            fade.startNewSubPath (laneArea.getX(), laneArea.getBottom());
            fade.lineTo (laneArea.getX(), laneArea.getY());
            fade.lineTo (laneArea.getX() + fadeWidth, laneArea.getY());
            fade.lineTo (laneArea.getX(), laneArea.getBottom());
            fade.closeSubPath();
            g.setColour (laneColour.withAlpha (fadeAlpha));
            g.fillPath (fade);
            g.setColour (ink().withAlpha (selected ? 0.68f : 0.38f));
            g.strokePath (fadeCurvePath (laneArea, fadeWidth, true, region.fadeInCurve),
                          juce::PathStrokeType (selected ? 1.35f : 0.9f));

            if (selected)
                g.drawFittedText (regionFadeCurveName (region.fadeInCurve),
                                  juce::Rectangle<int> (static_cast<int> (laneArea.getX() + 6.0f),
                                                        static_cast<int> (laneArea.getBottom() - 14.0f),
                                                        54,
                                                        12),
                                  juce::Justification::centredLeft,
                                  1);
        }

        if (region.fadeOutSeconds > 0.000001)
        {
            const auto fadeWidth = laneArea.getWidth() * static_cast<float> (juce::jlimit (0.0, 1.0, region.fadeOutSeconds / region.lengthSeconds));
            juce::Path fade;
            fade.startNewSubPath (laneArea.getRight(), laneArea.getBottom());
            fade.lineTo (laneArea.getRight(), laneArea.getY());
            fade.lineTo (laneArea.getRight() - fadeWidth, laneArea.getY());
            fade.lineTo (laneArea.getRight(), laneArea.getBottom());
            fade.closeSubPath();
            g.setColour (laneColour.withAlpha (fadeAlpha));
            g.fillPath (fade);
            g.setColour (ink().withAlpha (selected ? 0.68f : 0.38f));
            g.strokePath (fadeCurvePath (laneArea, fadeWidth, false, region.fadeOutCurve),
                          juce::PathStrokeType (selected ? 1.35f : 0.9f));

            if (selected)
                g.drawFittedText (regionFadeCurveName (region.fadeOutCurve),
                                  juce::Rectangle<int> (static_cast<int> (laneArea.getRight() - 58.0f),
                                                        static_cast<int> (laneArea.getBottom() - 14.0f),
                                                        54,
                                                        12),
                                  juce::Justification::centredRight,
                                  1);
        }

        g.setColour ((selected ? ink() : mutedInk()).withAlpha (selected ? 0.62f : 0.26f));
        g.fillRoundedRectangle (laneArea.getX() + 2.0f, laneArea.getY() + 2.0f, 6.0f, 6.0f, 3.0f);
        g.fillRoundedRectangle (laneArea.getRight() - 8.0f, laneArea.getY() + 2.0f, 6.0f, 6.0f, 3.0f);
        g.drawLine (laneArea.getX(), laneArea.getY() + 3.0f, laneArea.getX(), laneArea.getBottom() - 3.0f, selected ? 1.1f : 0.7f);
        g.drawLine (laneArea.getRight(), laneArea.getY() + 3.0f, laneArea.getRight(), laneArea.getBottom() - 3.0f, selected ? 1.1f : 0.7f);
    }

    double secondsAtX (float x) const
    {
        const auto bar = (static_cast<double> (x) - static_cast<double> (leftHeaderWidth)) / getBarWidth();
        return secondsAtBar (bar);
    }

    double timeForMouseEvent (double rawSeconds, const juce::MouseEvent& event) const
    {
        return event.mods.isCommandDown() ? rawSeconds : snapSecondsToGrid (rawSeconds);
    }

    double timeForPosition (float x, bool snapToGrid) const
    {
        const auto rawSeconds = secondsAtX (x);
        return snapToGrid ? snapSecondsToGrid (rawSeconds) : rawSeconds;
    }

    double snapSecondsToGrid (double seconds) const
    {
        if (steps.empty())
            return seconds;

        const auto targetBar = barAtSeconds (juce::jlimit (0.0, totalDurationSeconds(), seconds));
        auto barOffset = 0.0;

        for (const auto& step : steps)
        {
            const auto stepBars = juce::jmax (0.0, step.bars);
            if (stepBars <= 0.0)
                continue;

            if (targetBar <= barOffset + stepBars)
            {
                const auto quartersPerBar = juce::jmax (1.0, stepQuarterNotesPerBar (step));
                const auto gridBars = 1.0 / quartersPerBar;
                const auto localBar = juce::jlimit (0.0, stepBars, targetBar - barOffset);
                const auto snappedLocal = juce::jlimit (0.0, stepBars, std::round (localBar / gridBars) * gridBars);
                return secondsAtBar (barOffset + snappedLocal);
            }

            barOffset += stepBars;
        }

        return secondsAtBar (barOffset);
    }

    static juce::String formatTimelineSeconds (double seconds)
    {
        return juce::String (juce::jmax (0.0, seconds), 2) + "s";
    }

    void setDragReadout (const juce::String& text, juce::Point<float> position)
    {
        dragReadoutText = text;
        dragReadoutPosition = position + juce::Point<float> (12.0f, -30.0f);
        repaint();
    }

    void drawDragReadout (juce::Graphics& g) const
    {
        if (dragReadoutText.isEmpty())
            return;

        auto readout = juce::Rectangle<float> (dragReadoutPosition.x,
                                               dragReadoutPosition.y,
                                               146.0f,
                                               24.0f);
        readout = readout.getIntersection (getLocalBounds().toFloat().reduced (4.0f));
        if (readout.isEmpty())
            return;

        g.setColour (juce::Colour (0xff050806).withAlpha (0.88f));
        g.fillRoundedRectangle (readout, 4.0f);
        g.setColour (cyan().withAlpha (0.62f));
        g.drawRoundedRectangle (readout, 4.0f, 1.0f);
        g.setColour (ink().withAlpha (0.90f));
        g.setFont (juce::FontOptions (10.5f, juce::Font::bold));
        g.drawFittedText (dragReadoutText, readout.toNearestInt().reduced (8, 0), juce::Justification::centredLeft, 1);
    }

    double secondsAtBar (double barPosition) const
    {
        auto bars = 0.0;
        auto seconds = 0.0;

        for (const auto& step : steps)
        {
            const auto stepBars = juce::jmax (0.0, step.bars);
            const auto stepSeconds = stepDurationSeconds (step);
            if (barPosition <= bars + stepBars)
            {
                const auto fraction = stepBars > 0.0 ? juce::jlimit (0.0, 1.0, (barPosition - bars) / stepBars) : 0.0;
                return seconds + stepSeconds * fraction;
            }

            bars += stepBars;
            seconds += stepSeconds;
        }

        return seconds;
    }

    double barAtSeconds (double targetSeconds) const
    {
        auto bars = 0.0;
        auto seconds = 0.0;

        for (const auto& step : steps)
        {
            const auto stepBars = juce::jmax (0.0, step.bars);
            const auto stepSeconds = stepDurationSeconds (step);
            if (targetSeconds <= seconds + stepSeconds)
            {
                const auto fraction = stepSeconds > 0.0 ? juce::jlimit (0.0, 1.0, (targetSeconds - seconds) / stepSeconds) : 0.0;
                return bars + stepBars * fraction;
            }

            bars += stepBars;
            seconds += stepSeconds;
        }

        return bars;
    }

    double stepDurationSeconds (const GlobalScriptStep& step) const
    {
        const auto tempo = stepTempoBpm (step);
        const auto quarterNotesPerBar = stepQuarterNotesPerBar (step);
        return juce::jmax (0.0, step.bars) * quarterNotesPerBar * 60.0 / juce::jmax (1.0, tempo);
    }

    double stepTempoBpm (const GlobalScriptStep& step) const
    {
        if (step.tempoBpm.has_value())
            return *step.tempoBpm;

        if (stateTempos == nullptr)
            return 88.0;

        return stateTempos->at (static_cast<size_t> (juce::jlimit (0, maxTopLevelStates - 1, step.stateIndex)));
    }

    double stepQuarterNotesPerBar (const GlobalScriptStep& step) const
    {
        const auto index = static_cast<size_t> (juce::jlimit (0, maxTopLevelStates - 1, step.stateIndex));
        const auto numerator = static_cast<double> (step.timeSigNumerator.value_or (stateNumerators != nullptr ? stateNumerators->at (index) : 4));
        const auto denominator = static_cast<double> (juce::jmax (1, step.timeSigDenominator.value_or (stateDenominators != nullptr ? stateDenominators->at (index) : 4)));
        return numerator * 4.0 / denominator;
    }

    static juce::Colour laneColourForIndex (int laneIndex)
    {
        return laneAccentForIndex (laneIndex);
    }

    static constexpr int leftHeaderWidth = 170;
    static constexpr int headerHeight = 42;
    static constexpr int baseTrackHeaderHeight = 24;
    static constexpr int baseLaneRowHeight = 24;
    static constexpr int baseTrackGap = 8;
    static constexpr double baseBarWidth = 68.0;
    static constexpr double minRegionEditLengthSeconds = 0.025;

    double getBarWidth() const noexcept
    {
        return baseBarWidth * horizontalZoom;
    }

    int getTrackHeaderHeight() const noexcept
    {
        return static_cast<int> (std::round (static_cast<double> (baseTrackHeaderHeight)
                                             * juce::jlimit (0.85, 1.45, verticalZoom)));
    }

    int getLaneRowHeight() const noexcept
    {
        return static_cast<int> (std::round (static_cast<double> (baseLaneRowHeight) * verticalZoom));
    }

    int getTrackGap() const noexcept
    {
        return static_cast<int> (std::round (static_cast<double> (baseTrackGap)
                                             * juce::jlimit (0.75, 1.8, verticalZoom)));
    }

    const std::array<std::optional<std::vector<Wf::StateSpec>>, maxTopLevelStates>* states = nullptr;
    std::vector<GlobalScriptStep> steps;
    const std::vector<ImportedLaneAudioClip>* importedLaneClips = nullptr;
    const std::array<float, maxTopLevelStates>* stateTempos = nullptr;
    const std::array<int, maxTopLevelStates>* stateNumerators = nullptr;
    const std::array<int, maxTopLevelStates>* stateDenominators = nullptr;
    ArrangementAudioSelection selectedImportedRegion;
    std::vector<ArrangementAudioSelection> selectedImportedRegions;
    std::optional<RegionHit> activeFadeDrag;
    ActiveDrag activeDrag = ActiveDrag::none;
    juce::Point<float> marqueeStart;
    juce::Point<float> marqueeCurrent;
    juce::Point<float> dragReadoutPosition;
    juce::String dragReadoutText;
    double totalBars = 0.0;
    double editPlayheadSeconds = 0.0;
    int maxTracks = 1;
    int maxLanesInAnyTrack = 1;
    int playingState = 0;
    int playingTrack = 0;
    size_t playingStep = 0;
    double playingStepElapsedBars = 0.0;
    bool running = false;
    bool arrangementPlus = false;
    bool activeFadeDragEditingOut = false;
    bool marqueeAddsToSelection = false;
    ArrangementAudioTool audioTool = ArrangementAudioTool::pointer;
    double horizontalZoom = 1.0;
    double verticalZoom = 1.0;
    static constexpr float fadeHandleHitWidth = 10.0f;
};

class MainComponent final : public juce::Component,
                            public juce::MenuBarModel,
                            private juce::Timer
{
    enum class MainView
    {
        overall,
        arrangement,
        track,
        code,
        timeline,
        mixer
    };

    enum class ObjectClipboardKind
    {
        none,
        topLevelState,
        track,
        lane,
        audioRegions
    };

    struct ImportedRegionClipboardItem
    {
        int clipIndex = -1;
        ImportedAudioRegion region;
        double anchorStartSeconds = 0.0;
    };

    struct ProjectUndoSnapshot
    {
        juce::var project;
        bool projectWasDirty = false;
        juce::String label;
    };

    enum MenuIds
    {
        menuNewProject = 1,
        menuLoadProject,
        menuSaveProject,
        menuSaveProjectAs,
        menuScanPlugins,
        menuRenderLanesAndImport,
        menuRenderLanes,
        menuRenderWav,
        menuRemoveRenderedAudio,
        menuAbout,
        menuUndo,
        menuRedo,
        menuCopy,
        menuPaste,
        menuDuplicate
    };

public:
    MainComponent()
    {
        setLookAndFeel (&minimalLookAndFeel);
        setWantsKeyboardFocus (true);
        juce::addDefaultFormatsToManager (pluginFormatManager);
        loadRememberedPluginScan();

        topLevelStates[0] = Wf::makeDefaultStates();
        topLevelStates[1] = Wf::makeDefaultStates();

        titleLabel.setText ("ChucK-ME", juce::dontSendNotification);
        titleLabel.setFont (juce::FontOptions (22.0f, juce::Font::bold));
        styleLabel (titleLabel);
        addAndMakeVisible (titleLabel);

        statusLabel.setText ("embedded ChucK ready", juce::dontSendNotification);
        styleLabel (statusLabel, 0.72f);
        addAndMakeVisible (statusLabel);

        volumeLabel.setText ("volume", juce::dontSendNotification);
        volumeLabel.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        styleLabel (volumeLabel, 0.78f);
        addAndMakeVisible (volumeLabel);

        setupSlider (gainSlider, "Volume", 0.0, 0.8, 0.18, green());
        gainSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 56, 22);
        gainSlider.onDragStart = [this] { pushUndoSnapshot ("change master volume"); };
        gainSlider.onValueChange = [this]
        {
            markProjectDirty();
            applyCurrentAudioControls();
            if (mainView == MainView::mixer)
                syncMixerView();
        };

        for (int i = 0; i < maxTopLevelStates; ++i)
        {
            auto& button = stateButtons[static_cast<size_t> (i)];
            setupButton (button,
                         "State " + juce::String (i + 1),
                         stateAccentForIndex (i),
                         [this, i]
                         {
                             selectViewedTopLevelState (i);
                             grabKeyboardFocus();
                         });
            button.setWantsKeyboardFocus (false);

            if (! isTopLevelStatePopulated (i))
                styleEmptyStateButton (button);
        }

        setupButton (runScriptButton, running ? "Stop" : "Play", amber(), [this] { toggleMainTransport(); });
        setupButton (newStateButton, "New", blue(), [this] { createTopLevelState(); });
        setupButton (duplicateStateButton, "Duplicate", amber(), [this] { duplicateViewedTopLevelState(); });
        setupButton (deleteStateButton, "Delete", coral(), [this] { deleteViewedTopLevelState(); });

        globalScriptEditor.setMultiLine (true, false);
        globalScriptEditor.setReturnKeyStartsNewLine (true);
        globalScriptEditor.setSyntaxMode (CodeTextEditor::SyntaxMode::conductor);
        globalScriptEditor.setText (defaultGlobalScriptText(), juce::dontSendNotification);
        globalScriptEditor.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff101812));
        globalScriptEditor.setColour (juce::TextEditor::textColourId, ink().withAlpha (0.28f));
        globalScriptEditor.refreshBaseTextColour();
        globalScriptEditor.setColour (juce::TextEditor::outlineColourId, mutedInk().withAlpha (0.11f));
        globalScriptEditor.setColour (juce::TextEditor::focusedOutlineColourId, amber().withAlpha (0.46f));
        globalScriptEditor.setColour (juce::TextEditor::highlightColourId, amber().withAlpha (0.24f));
        globalScriptEditor.setCodeFontSize (12.6f);
        globalScriptEditor.onTextChange = [this]
        {
            beginUndoSnapshotForTextEdit (globalScriptEditor, "edit state code");
            markProjectDirty();
            if (mainView == MainView::timeline)
                syncArrangementTimelineView();
        };
        globalScriptEditor.onFocusLost = [this] { endUndoSnapshotForTextEdit (globalScriptEditor); };
        addAndMakeVisible (globalScriptEditor);

        laneCodeHeader.setText ("lane code", juce::dontSendNotification);
        laneCodeHeader.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        styleLabel (laneCodeHeader, 0.82f);
        addAndMakeVisible (laneCodeHeader);

        laneCodeMetadataLabel.setText ("No lane selected", juce::dontSendNotification);
        laneCodeMetadataLabel.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        laneCodeMetadataLabel.setMinimumHorizontalScale (0.62f);
        laneCodeMetadataLabel.setColour (juce::Label::backgroundColourId, panelSoft().withAlpha (0.22f));
        laneCodeMetadataLabel.setColour (juce::Label::textColourId, mutedInk().withAlpha (0.82f));
        laneCodeMetadataLabel.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (laneCodeMetadataLabel);

        trackNameLabel.setText ("track name", juce::dontSendNotification);
        trackNameLabel.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        styleLabel (trackNameLabel, 0.72f);
        addAndMakeVisible (trackNameLabel);

        setupTextEditor (trackNameEditor, "Track name");
        trackNameEditor.onTextChange = [this] { applyTrackNameEdit(); };
        trackNameEditor.onFocusLost = [this] { endUndoSnapshotForTextEdit (trackNameEditor); };

        trackDurationLabel.setText ("track duration (Bar.Beat)", juce::dontSendNotification);
        trackDurationLabel.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        styleLabel (trackDurationLabel, 0.72f);
        addAndMakeVisible (trackDurationLabel);

        setupTextEditor (trackDurationEditor, "1.0");
        trackDurationEditor.onTextChange = [this] { applyTrackDurationEdit(); };
        trackDurationEditor.onFocusLost = [this] { endUndoSnapshotForTextEdit (trackDurationEditor); };

        trackSectionLabel.setText ("track", juce::dontSendNotification);
        trackSectionLabel.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        styleLabel (trackSectionLabel, 0.88f);
        addAndMakeVisible (trackSectionLabel);

        setupTextEditor (laneNameEditor, "Lane name");
        laneNameEditor.onTextChange = [this] { applyLaneNameEdit(); };
        laneNameEditor.onFocusLost = [this] { endUndoSnapshotForTextEdit (laneNameEditor); };

        laneSectionLabel.setText ("lane", juce::dontSendNotification);
        laneSectionLabel.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        styleLabel (laneSectionLabel, 0.88f);
        addAndMakeVisible (laneSectionLabel);

        laneNameLabel.setText ("lane name", juce::dontSendNotification);
        laneNameLabel.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        styleLabel (laneNameLabel, 0.72f);
        addAndMakeVisible (laneNameLabel);

        laneTempoLabel.setText ("bpm override", juce::dontSendNotification);
        laneTempoLabel.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        styleLabel (laneTempoLabel, 0.72f);
        addAndMakeVisible (laneTempoLabel);

        setupTextEditor (laneTempoEditor, "track");
        laneTempoEditor.setInputRestrictions (6, "0123456789.");
        laneTempoEditor.onTextChange = [this] { applyLaneTempoEdit (false); };
        laneTempoEditor.onReturnKey = [this] { applyLaneTempoEdit (true); };
        laneTempoEditor.onFocusLost = [this]
        {
            applyLaneTempoEdit (true);
            endUndoSnapshotForTextEdit (laneTempoEditor);
        };

        laneDurationLabel.setText ("lane duration (Bar.Beat)", juce::dontSendNotification);
        laneDurationLabel.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        styleLabel (laneDurationLabel, 0.72f);
        addAndMakeVisible (laneDurationLabel);

        setupTextEditor (laneDurationEditor, "track");
        laneDurationEditor.onTextChange = [this] { applyLaneDurationEdit (false); };
        laneDurationEditor.onReturnKey = [this] { applyLaneDurationEdit (true); };
        laneDurationEditor.onFocusLost = [this]
        {
            applyLaneDurationEdit (true);
            endUndoSnapshotForTextEdit (laneDurationEditor);
        };

        laneCountLabel.setText ("Lanes", juce::dontSendNotification);
        styleLabel (laneCountLabel, 0.76f);
        addAndMakeVisible (laneCountLabel);

        setupTextEditor (laneCountEditor, "0");
        laneCountEditor.setInputRestrictions (1, "0123456789");
        laneCountEditor.setSelectAllWhenFocused (true);
        laneCountEditor.onTextChange = [this] { applyLaneCountEdit(); };
        laneCountEditor.onReturnKey = [this] { applyLaneCountEdit(); };
        laneCountEditor.onFocusLost = [this]
        {
            applyLaneCountEdit();
            endUndoSnapshotForTextEdit (laneCountEditor);
        };

        auto exitFocusedTrackView = [this]
        {
            if (mainView == MainView::track)
                setMainView (MainView::arrangement);
        };

        laneNameEditor.onEscapeKey = exitFocusedTrackView;
        laneTempoEditor.onEscapeKey = exitFocusedTrackView;
        laneDurationEditor.onEscapeKey = exitFocusedTrackView;

        setupButton (muteLaneButton, "Mute", coral(), [this] { toggleSelectedLaneMute(); });
        setupButton (soloLaneButton, "Solo", amber(), [this] { toggleSelectedLaneSolo(); });
        setupButton (duplicateLaneButton, "Duplicate", blue(), [this] { duplicateSelectedLane(); });
        setupButton (deleteLaneButton, "Delete", coral(), [this] { deleteSelectedLane(); });

        laneCodeEditor.setMultiLine (true, false);
        laneCodeEditor.setReturnKeyStartsNewLine (true);
        laneCodeEditor.setSyntaxMode (CodeTextEditor::SyntaxMode::chuck);
        laneCodeEditor.setReadOnly (false);
        laneCodeEditor.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff0d150f));
        laneCodeEditor.setColour (juce::TextEditor::textColourId, ink().withAlpha (0.28f));
        laneCodeEditor.refreshBaseTextColour();
        laneCodeEditor.setColour (juce::TextEditor::outlineColourId, mutedInk().withAlpha (0.12f));
        laneCodeEditor.setColour (juce::TextEditor::focusedOutlineColourId, blue().withAlpha (0.42f));
        laneCodeEditor.setColour (juce::TextEditor::highlightColourId, blue().withAlpha (0.24f));
        laneCodeEditor.setCodeFontSize (11.8f);
        laneCodeEditor.onTextChange = [this] { markLaneCodeEdited(); };
        laneCodeEditor.onEscapeKey = exitFocusedTrackView;
        addAndMakeVisible (laneCodeEditor);

        setupButton (laneCodeRunButton, "Run", green(), [this] { applyLaneCodeEdit(); });
        laneCodeRunButton.setEnabled (false);

        stateCodeHeader.setText ("state code", juce::dontSendNotification);
        stateCodeHeader.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        styleLabel (stateCodeHeader, 0.82f);
        addAndMakeVisible (stateCodeHeader);

        selectedLabel.setFont (juce::FontOptions (18.0f, juce::Font::bold));
        styleLabel (selectedLabel);
        addAndMakeVisible (selectedLabel);

        stateSettingsLabel.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        styleLabel (stateSettingsLabel, 0.82f);
        addAndMakeVisible (stateSettingsLabel);

        styleLabel (stateTempoLabel, 0.76f);
        addAndMakeVisible (stateTempoLabel);

        setupSlider (stateTempoSlider, "State tempo", 30.0, 220.0, 88.0, amber());
        stateTempoSlider.setSkewFactorFromMidPoint (100.0);
        stateTempoSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 62, 22);
        stateTempoSlider.onDragStart = [this] { pushUndoSnapshot ("change state tempo"); };
        stateTempoSlider.onValueChange = [this]
        {
            if (suppressStateControlCallbacks)
                return;

            topLevelTemposBpm[static_cast<size_t> (viewedTopLevelState)] = static_cast<float> (stateTempoSlider.getValue());
            markProjectDirty();
            refreshLabels();

            if (viewedTopLevelState == performingTopLevelState)
                applyCurrentAudioControls();
        };

        styleLabel (stateTimeSigLabel, 0.76f);
        addAndMakeVisible (stateTimeSigLabel);

        stateTrackCountLabel.setText ("Tracks", juce::dontSendNotification);
        styleLabel (stateTrackCountLabel, 0.76f);
        addAndMakeVisible (stateTrackCountLabel);

        setupTextEditor (stateTrackCountEditor, "0");
        stateTrackCountEditor.setInputRestrictions (2, "0123456789");
        stateTrackCountEditor.setSelectAllWhenFocused (true);
        stateTrackCountEditor.onTextChange = [this] { applyStateTrackCountEdit(); };
        stateTrackCountEditor.onFocusLost = [this]
        {
            applyStateTrackCountEdit();
            endUndoSnapshotForTextEdit (stateTrackCountEditor);
        };

        for (int numerator = 1; numerator <= 16; ++numerator)
            timeSigNumeratorBox.addItem (juce::String (numerator), numerator);

        for (const auto denominator : { 2, 4, 8, 16 })
            timeSigDenominatorBox.addItem (juce::String (denominator), denominator);

        styleComboBox (timeSigNumeratorBox);
        styleComboBox (timeSigDenominatorBox);
        addAndMakeVisible (timeSigNumeratorBox);
        addAndMakeVisible (timeSigDenominatorBox);

        timeSigNumeratorBox.onChange = [this]
        {
            if (suppressStateControlCallbacks)
                return;

            pushUndoSnapshot ("change time signature");
            topLevelTimeSigNumerators[static_cast<size_t> (viewedTopLevelState)] = timeSigNumeratorBox.getSelectedId();
            markProjectDirty();
            refreshLabels();
        };

        timeSigDenominatorBox.onChange = [this]
        {
            if (suppressStateControlCallbacks)
                return;

            pushUndoSnapshot ("change time signature");
            topLevelTimeSigDenominators[static_cast<size_t> (viewedTopLevelState)] = timeSigDenominatorBox.getSelectedId();
            markProjectDirty();
            refreshLabels();
        };

        orbitCanvas.onStateSelected = [this] (int index) { selectState (index); };
        orbitCanvas.onStateDoubleClicked = [this] (int index) { openTrackFocusView (index); };
        orbitCanvas.onTransitionProbabilityChanged = [this] (int index, std::optional<int> probability)
        {
            setTransitionProbability (index, probability);
        };
        addAndMakeVisible (orbitCanvas);

        overallCanvas.onStateSelected = [this] (int index)
        {
            selectViewedTopLevelState (index);
            setMainView (MainView::arrangement);
        };
        addAndMakeVisible (overallCanvas);

        trackFocusCanvas.onLaneSelected = [this] (int index) { selectFocusedLane (index); };
        trackFocusCanvas.onLanePhaseOffsetEditStarted = [this] (int) { pushUndoSnapshot ("change lane phase"); };
        trackFocusCanvas.onLanePhaseOffsetChanged = [this] (int index, float phaseOffset) { setLanePhaseOffset (index, phaseOffset); };
        addAndMakeVisible (trackFocusCanvas);
        trackFocusDivider.onDragStart = [this] { trackFocusCodePaneDragStartWidth = trackFocusCodePaneWidthPx; };
        trackFocusDivider.onDragDelta = [this] (int delta) { setTrackFocusCodePaneWidth (trackFocusCodePaneDragStartWidth + delta); };
        addAndMakeVisible (trackFocusDivider);
        codeViewDivider.onDragStart = [this] { codeViewSplitDragStartHeightPx = codeViewStatePaneHeightForCurrentBounds(); };
        codeViewDivider.onDragDelta = [this] (int delta) { setCodeViewStatePaneHeight (codeViewSplitDragStartHeightPx + delta); };
        addAndMakeVisible (codeViewDivider);

        mixerViewport.setViewedComponent (&mixerCanvas, false);
        mixerViewport.setScrollBarsShown (false, true);
        mixerCanvas.onChannelSelected = [this] (int stateIndex, int trackIndex, int laneIndex)
        {
            selectMixerChannel (stateIndex, trackIndex, laneIndex);
        };
        mixerCanvas.onMixEditStarted = [this] { pushUndoSnapshot ("mix lane"); };
        mixerCanvas.onLaneVolumeChanged = [this] (int stateIndex, int trackIndex, int laneIndex, float volume)
        {
            applyMixerLaneVolumeChange (stateIndex, trackIndex, laneIndex, volume);
        };
        mixerCanvas.onLanePanChanged = [this] (int stateIndex, int trackIndex, int laneIndex, float pan)
        {
            applyMixerLanePanChange (stateIndex, trackIndex, laneIndex, pan);
        };
        mixerCanvas.onMasterVolumeChanged = [this] (float volume)
        {
            applyMixerMasterVolumeChange (volume);
        };
        mixerCanvas.onEffectSlotClicked = [this] (int stateIndex, int trackIndex, int slotIndex)
        {
            showEffectSlotMenu (stateIndex, trackIndex, slotIndex);
        };
        mixerCanvas.onMasterEffectSlotClicked = [this] (int slotIndex)
        {
            showEffectSlotMenu (-1, -1, slotIndex);
        };
        addAndMakeVisible (mixerViewport);
        arrangementTimelineViewport.setViewedComponent (&arrangementTimelineCanvas, false);
        arrangementTimelineViewport.setScrollBarsShown (true, true);
        arrangementTimelineViewport.setScrollBarThickness (8);
        arrangementTimelineCanvas.onImportedRegionClicked = [this] (int clipIndex, int regionIndex, bool toggle)
        {
            selectImportedAudioRegion (clipIndex, regionIndex, toggle);
        };
        arrangementTimelineCanvas.onImportedRegionsMarqueeSelected = [this] (std::vector<ArrangementAudioSelection> selections, bool addToSelection)
        {
            selectImportedAudioRegions (std::move (selections), addToSelection);
        };
        arrangementTimelineCanvas.onImportedRegionSplit = [this] (int clipIndex, int regionIndex, double splitSeconds)
        {
            splitImportedAudioRegion (clipIndex, regionIndex, splitSeconds);
        };
        arrangementTimelineCanvas.onImportedRegionTrimEditStarted = [this] (int clipIndex, int regionIndex)
        {
            beginImportedRegionTrimEdit (clipIndex, regionIndex);
        };
        arrangementTimelineCanvas.onImportedRegionTrimChanged = [this] (int clipIndex, int regionIndex, double startSeconds, double sourceStartSeconds, double lengthSeconds)
        {
            setImportedAudioRegionTrim (clipIndex, regionIndex, startSeconds, sourceStartSeconds, lengthSeconds);
        };
        arrangementTimelineCanvas.onImportedRegionTrimEditEnded = [this]
        {
            importedTrimDragUndoStarted = false;
        };
        arrangementTimelineCanvas.onImportedRegionFadeEditStarted = [this] (int clipIndex, int regionIndex)
        {
            beginImportedRegionFadeEdit (clipIndex, regionIndex);
        };
        arrangementTimelineCanvas.onImportedRegionFadeChanged = [this] (int clipIndex, int regionIndex, double fadeInSeconds, double fadeOutSeconds)
        {
            setImportedAudioRegionFades (clipIndex, regionIndex, fadeInSeconds, fadeOutSeconds);
        };
        arrangementTimelineCanvas.onImportedRegionFadeEditEnded = [this]
        {
            importedFadeDragUndoStarted = false;
        };
        arrangementTimelineCanvas.onImportedRegionFadeCurveCycle = [this] (int clipIndex, int regionIndex, bool fadeOut)
        {
            cycleImportedRegionFadeCurve (clipIndex, regionIndex, fadeOut);
        };
        arrangementTimelineCanvas.onImportedRegionGainEditStarted = [this] (int clipIndex, int regionIndex)
        {
            beginImportedRegionGainEdit (clipIndex, regionIndex);
        };
        arrangementTimelineCanvas.onImportedRegionGainChanged = [this] (int clipIndex, int regionIndex, float gain)
        {
            setImportedAudioRegionGain (clipIndex, regionIndex, gain);
        };
        arrangementTimelineCanvas.onImportedRegionGainEditEnded = [this]
        {
            importedGainDragUndoStarted = false;
        };
        arrangementTimelineCanvas.onArrangementPlayheadChanged = [this] (double seconds)
        {
            setArrangementEditPlayheadSeconds (seconds);
        };
        addAndMakeVisible (arrangementTimelineViewport);

        laneListViewport.setViewedComponent (&laneListContent, false);
        laneListViewport.setScrollBarsShown (true, false);
        laneListViewport.setScrollBarThickness (6);
        addAndMakeVisible (laneListViewport);

        setupButton (overallButton, "Overall", blue(), [this] { setMainView (MainView::overall); });
        setupButton (arrangementButton, "MAIN", green(), [this] { setMainView (MainView::arrangement); });
        setupButton (codeViewButton, "Code", blue(), [this] { setMainView (MainView::code); });
        setupButton (timelineViewButton, "Arrangement", green(), [this] { setMainView (MainView::timeline); });
        setupButton (mixerViewButton, "Mixer", amber(), [this] { setMainView (MainView::mixer); });
        setupButton (removeRenderedAudioButton, "Remove audio", coral(), [this] { removeRenderedAudioAndReturnToCodePlayback(); });
        setupButton (arrangementPointerToolButton, "Pointer", blue(), [this] { setArrangementAudioTool (ArrangementAudioTool::pointer); });
        setupButton (arrangementScissorsToolButton, "Scissors", coral(), [this] { setArrangementAudioTool (ArrangementAudioTool::scissors); });
        setupButton (arrangementFadeToolButton, "Fade", amber(), [this] { setArrangementAudioTool (ArrangementAudioTool::fade); });
        setupButton (splitImportedRegionButton, "Split", cyan(), [this] { splitSelectedImportedRegionsAtPlayhead(); });
        setupButton (deleteImportedRegionButton, "Delete", coral(), [this] { deleteSelectedImportedAudioRegion(); });

        arrangementHorizontalZoomLabel.setText ("H zoom", juce::dontSendNotification);
        styleLabel (arrangementHorizontalZoomLabel, 0.72f);
        addAndMakeVisible (arrangementHorizontalZoomLabel);
        setupSlider (arrangementHorizontalZoomSlider, "Arrangement horizontal zoom", 0.35, 3.25, arrangementHorizontalZoom, green());
        arrangementHorizontalZoomSlider.setRange (0.35, 3.25, 0.01);
        arrangementHorizontalZoomSlider.setNumDecimalPlacesToDisplay (2);
        arrangementHorizontalZoomSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 44, 22);
        arrangementHorizontalZoomSlider.onDragStart = [this] { pushUndoSnapshot ("change arrangement zoom"); };
        arrangementHorizontalZoomSlider.onValueChange = [this] { applyArrangementZoomChange(); };

        arrangementVerticalZoomLabel.setText ("V zoom", juce::dontSendNotification);
        styleLabel (arrangementVerticalZoomLabel, 0.72f);
        addAndMakeVisible (arrangementVerticalZoomLabel);
        setupSlider (arrangementVerticalZoomSlider, "Arrangement vertical zoom", 0.55, 3.0, arrangementVerticalZoom, blue());
        arrangementVerticalZoomSlider.setRange (0.55, 3.0, 0.01);
        arrangementVerticalZoomSlider.setNumDecimalPlacesToDisplay (2);
        arrangementVerticalZoomSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 44, 22);
        arrangementVerticalZoomSlider.onDragStart = [this] { pushUndoSnapshot ("change arrangement zoom"); };
        arrangementVerticalZoomSlider.onValueChange = [this] { applyArrangementZoomChange(); };

        auto setupRegionLabel = [this] (juce::Label& label, const juce::String& text)
        {
            label.setText (text, juce::dontSendNotification);
            label.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            styleLabel (label, 0.68f);
            addAndMakeVisible (label);
        };

        regionInspectorLabel.setText ("region", juce::dontSendNotification);
        regionInspectorLabel.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        styleLabel (regionInspectorLabel, 0.84f);
        addAndMakeVisible (regionInspectorLabel);
        setupRegionLabel (regionStartLabel, "start");
        setupRegionLabel (regionLengthLabel, "length");
        setupRegionLabel (regionSourceLabel, "source");
        setupRegionLabel (regionGainLabel, "gain");
        setupRegionLabel (regionFadeInLabel, "fade in");
        setupRegionLabel (regionFadeOutLabel, "fade out");
        setupRegionLabel (regionFadeInCurveLabel, "in curve");
        setupRegionLabel (regionFadeOutCurveLabel, "out curve");

        auto setupRegionEditor = [this] (juce::TextEditor& editor, const juce::String& emptyText, std::function<void()> apply)
        {
            setupTextEditor (editor, emptyText);
            editor.setInputRestrictions (8, "0123456789.");
            editor.setSelectAllWhenFocused (true);
            editor.onReturnKey = apply;
            editor.onFocusLost = apply;
        };

        setupRegionEditor (regionStartEditor, "0.00", [this] { applyRegionInspectorStartEdit(); });
        setupRegionEditor (regionLengthEditor, "0.00", [this] { applyRegionInspectorLengthEdit(); });
        setupRegionEditor (regionSourceEditor, "0.00", [this] { applyRegionInspectorSourceEdit(); });
        setupRegionEditor (regionGainEditor, "1.00", [this] { applyRegionInspectorGainEdit(); });
        setupRegionEditor (regionFadeInEditor, "0.00", [this] { applyRegionInspectorFadeInEdit(); });
        setupRegionEditor (regionFadeOutEditor, "0.00", [this] { applyRegionInspectorFadeOutEdit(); });

        for (int curve = 0; curve < 4; ++curve)
        {
            regionFadeInCurveBox.addItem (regionFadeCurveName (curve), curve + 1);
            regionFadeOutCurveBox.addItem (regionFadeCurveName (curve), curve + 1);
        }
        styleComboBox (regionFadeInCurveBox);
        styleComboBox (regionFadeOutCurveBox);
        regionFadeInCurveBox.onChange = [this] { applyRegionInspectorFadeCurveEdit (false); };
        regionFadeOutCurveBox.onChange = [this] { applyRegionInspectorFadeCurveEdit (true); };
        addAndMakeVisible (regionFadeInCurveBox);
        addAndMakeVisible (regionFadeOutCurveBox);

        addAndMakeVisible (laneHeader);
        laneHeader.setText ("lanes", juce::dontSendNotification);
        laneHeader.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        styleLabel (laneHeader, 0.78f);

        for (int i = 0; i < static_cast<int> (laneButtons.size()); ++i)
        {
            auto& button = laneButtons[static_cast<size_t> (i)];
            setupButton (button, juce::String (i + 1), laneAccentForIndex (i), [this, i] { selectLane (i); });
            laneListContent.addAndMakeVisible (button);
            button.setClickingTogglesState (false);

            auto& soloButton = laneSoloButtons[static_cast<size_t> (i)];
            setupButton (soloButton, "S", amber(), [this, i] { toggleLaneSolo (i); });
            laneListContent.addAndMakeVisible (soloButton);
            soloButton.setClickingTogglesState (false);

            auto& muteButton = laneMuteButtons[static_cast<size_t> (i)];
            setupButton (muteButton, "M", coral(), [this, i] { toggleLaneMute (i); });
            laneListContent.addAndMakeVisible (muteButton);
            muteButton.setClickingTogglesState (false);
        }

        audioDeviceManager.initialiseWithDefaultDevices (0, 2);
        audioDeviceManager.addAudioCallback (&audioCallback);

        selectedState = 0;
        selectedLane = 0;
        refreshLabels();
        setMainView (MainView::arrangement);
        setSize (1240, 760);
        startTimerHz (30);
    }

    ~MainComponent() override
    {
        pluginScanAbortRequested.store (true, std::memory_order_release);
        if (pluginScanThread.joinable())
            pluginScanThread.join();

        audioDeviceManager.removeAudioCallback (&audioCallback);
        setLookAndFeel (nullptr);
    }

    juce::StringArray getMenuBarNames() override
    {
        return { "File", "Edit" };
    }

    juce::PopupMenu getMenuForIndex (int menuIndex, const juce::String&) override
    {
        juce::PopupMenu menu;
        auto addShortcutItem = [&menu] (int itemID,
                                        const juce::String& text,
                                        bool isEnabled,
                                        const juce::String& shortcut)
        {
            juce::PopupMenu::Item item (text);
            item.itemID = itemID;
            item.isEnabled = isEnabled;
            item.shortcutKeyDescription = shortcut;
            menu.addItem (std::move (item));
        };

        if (menuIndex == 0)
        {
            menu.addItem (menuNewProject, "New Project");
            menu.addSeparator();
            menu.addItem (menuLoadProject, "Load Project...");
            menu.addItem (menuSaveProject, "Save Project", projectDirty || laneCodeDirty || currentProjectFile == juce::File());
            menu.addItem (menuSaveProjectAs, "Save Project As...");
            menu.addSeparator();
            menu.addItem (menuScanPlugins,
                          getScannedEffectPlugins().isEmpty() ? "Scan AU/VST3 Plugins..." : "Rescan AU/VST3 Plugins...",
                          ! pluginScanInProgress);
            menu.addSeparator();
            menu.addItem (menuRenderLanesAndImport, "Render lanes and import");
            menu.addItem (menuRenderLanes, "Render lanes");
            menu.addItem (menuRenderWav, arrangementPlusMode ? "Render WAV+..." : "Render WAV...");
            menu.addItem (menuRemoveRenderedAudio, "Remove Rendered Audio", arrangementPlusMode);
            menu.addSeparator();
            menu.addItem (menuAbout, "About ChucK-ME");
        }
        else if (menuIndex == 1)
        {
            addShortcutItem (menuUndo, "Undo", canUndo(), "Cmd+Z");
            addShortcutItem (menuRedo, "Redo", canRedo(), "Cmd+Shift+Z");
            menu.addSeparator();
            addShortcutItem (menuCopy, "Copy", canCopy(), "Cmd+C");
            addShortcutItem (menuPaste, "Paste", canPaste(), "Cmd+V");
            menu.addSeparator();
            addShortcutItem (menuDuplicate, "Duplicate", canDuplicate(), "Cmd+D");
        }

        return menu;
    }

    void menuItemSelected (int menuItemID, int) override
    {
        switch (menuItemID)
        {
            case menuNewProject: confirmUnsavedChangesThen ([this] { newProject(); }); break;
            case menuLoadProject: confirmUnsavedChangesThen ([this] { chooseProjectToLoad(); }); break;
            case menuSaveProject: saveProject(); break;
            case menuSaveProjectAs: saveProjectAs(); break;
            case menuScanPlugins: scanOrRescanEffectPlugins(); break;
            case menuRenderLanesAndImport: confirmPendingLaneCodeThen ([this] { chooseArrangementLaneRenderDirectory (true); }); break;
            case menuRenderLanes: confirmPendingLaneCodeThen ([this] { chooseArrangementLaneRenderDirectory (false); }); break;
            case menuRenderWav: confirmPendingLaneCodeThen ([this] { chooseArrangementRenderFile(); }); break;
            case menuRemoveRenderedAudio: removeRenderedAudioAndReturnToCodePlayback(); break;
            case menuAbout: showAboutDialog(); break;
            case menuUndo: performUndo(); break;
            case menuRedo: performRedo(); break;
            case menuCopy: performCopy(); break;
            case menuPaste: performPaste(); break;
            case menuDuplicate: performDuplicate(); break;
            default: break;
        }
    }

    void showAboutDialog()
    {
        juce::AlertWindow::showMessageBoxAsync (
            juce::AlertWindow::InfoIcon,
            "About ChucK-ME",
            "ChucK-ME by matd.space is an app for Mac (Apple silicon only) that enables the building of ephemeral musical systems made up of states, tracks, and lanes (and linear or probabilistic transitions between them) in the ChucK programming language, then transforms (renders) the generative playback into audio files in a DAW-like arrangement for audio editing, mixing (including processing with AU and/or VST3 plugins), and WAV export. The transformation is integrated and seamless - no external ChucK application or process is required.",
            "OK");
    }

    void confirmQuitThen (std::function<void()> quitAction)
    {
        confirmUnsavedChangesThen (std::move (quitAction));
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        const auto keyCode = key.getKeyCode();
        const auto modifiers = key.getModifiers();
        if (modifiers.isCommandDown())
        {
            if (keyCode == 'z' || keyCode == 'Z')
            {
                if (modifiers.isShiftDown())
                    performRedo();
                else
                    performUndo();
                return true;
            }

            if (keyCode == 'c' || keyCode == 'C')
            {
                performCopy();
                return true;
            }

            if (keyCode == 'v' || keyCode == 'V')
            {
                performPaste();
                return true;
            }

            if (keyCode == 'd' || keyCode == 'D')
            {
                performDuplicate();
                return true;
            }
        }

        if (keyCode == juce::KeyPress::escapeKey && mainView == MainView::track)
        {
            setMainView (MainView::arrangement);
            return true;
        }

        if ((keyCode == juce::KeyPress::backspaceKey || keyCode == juce::KeyPress::deleteKey)
            && ! isInlineTextEditorFocused())
        {
            if (mainView == MainView::timeline && arrangementPlusMode && hasImportedRegionSelection())
            {
                deleteSelectedImportedAudioRegion();
                return true;
            }

            deleteViewedTopLevelState();
            return true;
        }

        return false;
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff09110c));

        auto area = getLocalBounds().reduced (18);
        g.setColour (panel());
        g.fillRoundedRectangle (area.toFloat(), 7.0f);

        g.setColour (mutedInk().withAlpha (0.10f));
        g.drawRoundedRectangle (area.toFloat(), 7.0f, 1.0f);

        const auto viewAccent = currentViewAccent();
        g.setColour (viewAccent.withAlpha (0.16f));
        g.drawRoundedRectangle (area.toFloat().reduced (0.5f), 7.0f, 1.0f);
        g.setColour (viewAccent.withAlpha (0.34f));
        g.fillRoundedRectangle (juce::Rectangle<float> (static_cast<float> (area.getX() + 24),
                                                        static_cast<float> (area.getY() + 1),
                                                        126.0f,
                                                        2.0f),
                                1.0f);

        auto content = area.reduced (18);
        auto top = content.removeFromTop (48);
        auto navigation = content.removeFromBottom (48);

        g.setColour (mutedInk().withAlpha (0.11f));
        g.drawLine (static_cast<float> (content.getX()),
                    static_cast<float> (top.getBottom()),
                    static_cast<float> (content.getRight()),
                    static_cast<float> (top.getBottom()),
                    1.0f);

        g.setColour (mutedInk().withAlpha (0.10f));
        g.drawLine (static_cast<float> (navigation.getX()),
                    static_cast<float> (navigation.getY()),
                    static_cast<float> (navigation.getRight()),
                    static_cast<float> (navigation.getY()),
                    1.0f);

        if (mainView == MainView::arrangement)
        {
            auto stateRow = content.removeFromTop (60);
            auto scriptRow = content.removeFromTop (64);
            auto body = content;
            body.removeFromTop (14);
            auto leftPane = body.removeFromLeft (arrangementLeftPaneWidth);
            body.removeFromLeft (arrangementPaneGap);
            auto rightPane = body.removeFromRight (arrangementRightPaneWidth);

            g.setColour (viewAccent.withAlpha (0.055f));
            g.fillRoundedRectangle (leftPane.toFloat(), 3.0f);
            g.fillRoundedRectangle (rightPane.toFloat(), 3.0f);

            g.setColour (viewAccent.withAlpha (0.095f));
            g.drawRoundedRectangle (leftPane.toFloat(), 3.0f, 1.0f);
            g.drawRoundedRectangle (rightPane.toFloat(), 3.0f, 1.0f);

            g.setColour (mutedInk().withAlpha (0.11f));
            g.drawLine (static_cast<float> (stateRow.getX()),
                        static_cast<float> (stateRow.getBottom()),
                        static_cast<float> (stateRow.getRight()),
                        static_cast<float> (stateRow.getBottom()),
                        1.0f);
            g.drawLine (static_cast<float> (scriptRow.getX()),
                        static_cast<float> (scriptRow.getBottom()),
                        static_cast<float> (scriptRow.getRight()),
                        static_cast<float> (scriptRow.getBottom()),
                        1.0f);
        }
        else if (mainView == MainView::code)
        {
            content.removeFromTop (12);
            const auto split = codeViewStatePaneHeightForAvailableHeight (content.getHeight());
            auto stateCodePane = content.removeFromTop (split);
            content.removeFromTop (codeViewDividerHeight);
            auto laneCodePane = content;

            g.setColour (viewAccent.withAlpha (0.045f));
            g.fillRoundedRectangle (stateCodePane.toFloat(), 3.0f);
            g.fillRoundedRectangle (laneCodePane.toFloat(), 3.0f);

            g.setColour (viewAccent.withAlpha (0.095f));
            g.drawRoundedRectangle (stateCodePane.toFloat(), 3.0f, 1.0f);
            g.drawRoundedRectangle (laneCodePane.toFloat(), 3.0f, 1.0f);
        }
        else if (mainView == MainView::track)
        {
            content.removeFromTop (12);
            auto codePane = content.removeFromLeft (clampedTrackFocusCodePaneWidth (content.getWidth()));
            content.removeFromLeft (trackFocusPaneGap);
            auto trackPane = content;

            g.setColour (viewAccent.withAlpha (0.045f));
            g.fillRoundedRectangle (trackPane.toFloat(), 3.0f);
            g.fillRoundedRectangle (codePane.toFloat(), 3.0f);

            g.setColour (viewAccent.withAlpha (0.095f));
            g.drawRoundedRectangle (trackPane.toFloat(), 3.0f, 1.0f);
            g.drawRoundedRectangle (codePane.toFloat(), 3.0f, 1.0f);
        }
        else if (mainView == MainView::mixer || mainView == MainView::overall || mainView == MainView::timeline)
        {
            content.removeFromTop (12);
            g.setColour (viewAccent.withAlpha (0.045f));
            g.fillRoundedRectangle (content.toFloat(), 3.0f);

            g.setColour (viewAccent.withAlpha (0.095f));
            g.drawRoundedRectangle (content.toFloat(), 3.0f, 1.0f);
        }
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (34);
        const auto stateGridRight = stateButtonRowRightEdge (area);
        auto header = area.removeFromTop (44);
        titleLabel.setBounds (header.removeFromLeft (260));
        const auto playButtonWidth = 72;
        const auto headerGap = 16;
        const auto playBounds = juce::Rectangle<int> (playButtonWidth, header.getHeight() - 14)
                                    .withPosition (stateGridRight - playButtonWidth, header.getY() + 7);
        runScriptButton.setBounds (playBounds);

        const auto volumeBounds = juce::Rectangle<int> (270, header.getHeight())
                                      .withPosition (playBounds.getX() - headerGap - 270, header.getY());
        auto volumeArea = volumeBounds;
        volumeLabel.setBounds (volumeArea.removeFromLeft (66).reduced (0, 11));
        gainSlider.setBounds (volumeArea.reduced (0, 7));
        header.setRight (volumeBounds.getX() - headerGap);
        statusLabel.setBounds (header);

        auto navigation = area.removeFromBottom (48);
        navigation.removeFromTop (10);
        auto viewButtons = navigation.removeFromTop (34);
        overallButton.setBounds (viewButtons.removeFromLeft (108).reduced (0, 2));
        arrangementButton.setBounds (viewButtons.removeFromLeft (118).reduced (0, 2));
        codeViewButton.setBounds (viewButtons.removeFromLeft (78).reduced (6, 2));
        timelineViewButton.setBounds (viewButtons.removeFromLeft (126).reduced (6, 2));
        mixerViewButton.setBounds (viewButtons.removeFromLeft (84).reduced (6, 2));

        if (mainView == MainView::overall)
        {
            area.removeFromTop (12);
            overallCanvas.setBounds (area.reduced (8, 0));
            return;
        }

        if (mainView == MainView::code)
        {
            area.removeFromTop (12);
            auto stateCodePane = area.removeFromTop (codeViewStatePaneHeightForAvailableHeight (area.getHeight()));
            auto dividerArea = area.removeFromTop (codeViewDividerHeight);
            codeViewDivider.setBounds (dividerArea);
            auto laneCodePane = area;

            auto stateCodeHeaderRow = stateCodePane.removeFromTop (32);
            stateCodeHeader.setBounds (stateCodeHeaderRow.reduced (8, 2));
            globalScriptEditor.setBounds (stateCodePane.reduced (8, 0));

            auto laneCodeHeaderRow = laneCodePane.removeFromTop (32);
            laneCodeRunButton.setBounds (laneCodeHeaderRow.removeFromRight (68).reduced (0, 3));
            laneCodeHeader.setBounds (laneCodeHeaderRow.reduced (8, 2));
            auto laneCodeMetadataRow = laneCodePane.removeFromTop (30);
            laneCodeMetadataLabel.setBounds (laneCodeMetadataRow.reduced (8, 3));
            laneCodeEditor.setBounds (laneCodePane.reduced (8, 0));
            return;
        }

        if (mainView == MainView::mixer)
        {
            area.removeFromTop (12);
            mixerViewport.setBounds (area.reduced (8, 0));
            resizeMixerCanvas();
            return;
        }

        if (mainView == MainView::timeline)
        {
            area.removeFromTop (12);
            auto toolbar = area.removeFromTop (38);
            if (arrangementPlusMode)
            {
                auto toolArea = toolbar.removeFromLeft (404);
                arrangementPointerToolButton.setBounds (toolArea.removeFromLeft (78).reduced (0, 2));
                toolArea.removeFromLeft (6);
                arrangementScissorsToolButton.setBounds (toolArea.removeFromLeft (86).reduced (0, 2));
                toolArea.removeFromLeft (6);
                arrangementFadeToolButton.setBounds (toolArea.removeFromLeft (66).reduced (0, 2));
                toolArea.removeFromLeft (6);
                splitImportedRegionButton.setBounds (toolArea.removeFromLeft (68).reduced (0, 2));
                toolArea.removeFromLeft (12);
                deleteImportedRegionButton.setBounds (toolArea.removeFromLeft (76).reduced (0, 2));
                removeRenderedAudioButton.setBounds (toolbar.removeFromRight (122).reduced (0, 2));
            }
            toolbar.removeFromRight (18);
            auto verticalZoomArea = toolbar.removeFromRight (158);
            arrangementVerticalZoomLabel.setBounds (verticalZoomArea.removeFromLeft (52).reduced (0, 10));
            arrangementVerticalZoomSlider.setBounds (verticalZoomArea.reduced (0, 7));
            toolbar.removeFromRight (12);
            auto horizontalZoomArea = toolbar.removeFromRight (158);
            arrangementHorizontalZoomLabel.setBounds (horizontalZoomArea.removeFromLeft (52).reduced (0, 10));
            arrangementHorizontalZoomSlider.setBounds (horizontalZoomArea.reduced (0, 7));
            area.removeFromTop (8);
            if (arrangementPlusMode)
            {
                auto inspector = area.removeFromTop (62).reduced (8, 4);
                regionInspectorLabel.setBounds (inspector.removeFromLeft (86).reduced (0, 12));
                inspector.removeFromLeft (10);

                auto placeRegionField = [&inspector] (juce::Label& label, juce::Component& field, int width)
                {
                    auto block = inspector.removeFromLeft (width);
                    label.setBounds (block.removeFromTop (20).reduced (0, 1));
                    field.setBounds (block.removeFromTop (28));
                    inspector.removeFromLeft (8);
                };

                placeRegionField (regionStartLabel, regionStartEditor, 78);
                placeRegionField (regionLengthLabel, regionLengthEditor, 78);
                placeRegionField (regionSourceLabel, regionSourceEditor, 78);
                placeRegionField (regionGainLabel, regionGainEditor, 72);
                placeRegionField (regionFadeInLabel, regionFadeInEditor, 78);
                placeRegionField (regionFadeOutLabel, regionFadeOutEditor, 78);
                placeRegionField (regionFadeInCurveLabel, regionFadeInCurveBox, 96);
                placeRegionField (regionFadeOutCurveLabel, regionFadeOutCurveBox, 96);
                area.removeFromTop (6);
            }
            arrangementTimelineViewport.setBounds (area.reduced (8, 0));
            resizeArrangementTimelineCanvas();
            return;
        }

        if (mainView == MainView::track)
        {
            area.removeFromTop (12);
            auto codePane = area.removeFromLeft (clampedTrackFocusCodePaneWidth (area.getWidth()));
            auto dividerArea = area.removeFromLeft (trackFocusPaneGap);
            trackFocusDivider.setBounds (dividerArea);
            trackFocusCanvas.setBounds (area.reduced (8, 0));

            auto laneNameRow = codePane.removeFromTop (30);
            laneNameLabel.setBounds (laneNameRow.removeFromLeft (92).reduced (8, 2));
            laneNameRow.removeFromLeft (8);
            laneNameEditor.setBounds (laneNameRow.reduced (0, 2));
            codePane.removeFromTop (8);
            auto laneTempoRow = codePane.removeFromTop (30);
            laneTempoLabel.setBounds (laneTempoRow.removeFromLeft (104).reduced (8, 2));
            laneTempoRow.removeFromLeft (8);
            laneTempoEditor.setBounds (laneTempoRow.removeFromLeft (82).reduced (0, 2));
            codePane.removeFromTop (6);
            auto laneDurationRow = codePane.removeFromTop (30);
            laneDurationLabel.setBounds (laneDurationRow.removeFromLeft (174).reduced (8, 2));
            laneDurationRow.removeFromLeft (8);
            laneDurationEditor.setBounds (laneDurationRow.removeFromLeft (74).reduced (0, 2));
            codePane.removeFromTop (8);
            auto laneCodeHeaderRow = codePane.removeFromTop (32);
            laneCodeRunButton.setBounds (laneCodeHeaderRow.removeFromRight (68).reduced (0, 3));
            laneCodeHeader.setBounds (laneCodeHeaderRow.reduced (8, 2));
            auto laneCodeMetadataRow = codePane.removeFromTop (34);
            laneCodeMetadataLabel.setBounds (laneCodeMetadataRow.reduced (8, 4));
            laneCodeEditor.setBounds (codePane.reduced (8, 0));
            return;
        }

        auto stateRow = area.removeFromTop (60);
        stateRow.removeFromLeft (10);

        const auto buttonGap = 6;
        auto firstStateRow = stateRow.removeFromTop (30);
        auto secondStateRow = stateRow.removeFromTop (30);
        const auto buttonWidth = juce::jmax (52, (firstStateRow.getWidth() - buttonGap * 7) / 8);

        for (int i = 0; i < maxTopLevelStates; ++i)
        {
            auto& row = i < 8 ? firstStateRow : secondStateRow;
            auto buttonArea = row.removeFromLeft (buttonWidth);
            stateButtons[static_cast<size_t> (i)].setBounds (buttonArea.reduced (0, 3));
            row.removeFromLeft (buttonGap);
        }

        auto scriptRow = area.removeFromTop (64);
        scriptRow.removeFromLeft (10);
        globalScriptEditor.setBounds (scriptRow.reduced (0, 6));
        area.removeFromTop (14);
        auto codePane = area.removeFromLeft (arrangementLeftPaneWidth);
        area.removeFromLeft (arrangementPaneGap);
        auto right = area.removeFromRight (arrangementRightPaneWidth);
        area.removeFromRight (arrangementPaneGap);

        auto inspector = codePane.reduced (10, 4);
        trackSectionLabel.setBounds (inspector.removeFromTop (24));
        inspector.removeFromTop (6);

        auto trackNameBlock = inspector.removeFromTop (52);
        trackNameLabel.setBounds (trackNameBlock.removeFromTop (18));
        trackNameEditor.setBounds (trackNameBlock.reduced (0, 2));
        inspector.removeFromTop (7);

        auto trackDurationBlock = inspector.removeFromTop (52);
        trackDurationLabel.setBounds (trackDurationBlock.removeFromTop (18));
        trackDurationEditor.setBounds (trackDurationBlock.removeFromLeft (116).reduced (0, 2));
        inspector.removeFromTop (16);

        laneSectionLabel.setBounds (inspector.removeFromTop (24));
        inspector.removeFromTop (6);

        auto laneNameBlock = inspector.removeFromTop (52);
        laneNameLabel.setBounds (laneNameBlock.removeFromTop (18));
        laneNameEditor.setBounds (laneNameBlock.reduced (0, 2));
        inspector.removeFromTop (7);

        auto laneTempoBlock = inspector.removeFromTop (52);
        laneTempoLabel.setBounds (laneTempoBlock.removeFromTop (18));
        laneTempoEditor.setBounds (laneTempoBlock.removeFromLeft (116).reduced (0, 2));
        inspector.removeFromTop (7);

        auto laneDurationBlock = inspector.removeFromTop (52);
        laneDurationLabel.setBounds (laneDurationBlock.removeFromTop (18));
        laneDurationEditor.setBounds (laneDurationBlock.removeFromLeft (116).reduced (0, 2));
        orbitCanvas.setBounds (area);
        right = right.reduced (10, 4);
        stateSettingsLabel.setBounds (right.removeFromTop (24));
        stateTempoLabel.setBounds (right.removeFromTop (22));
        stateTempoSlider.setBounds (right.removeFromTop (30));
        stateTimeSigLabel.setBounds (right.removeFromTop (22));
        auto timeSigRow = right.removeFromTop (30);
        timeSigNumeratorBox.setBounds (timeSigRow.removeFromLeft (78));
        timeSigRow.removeFromLeft (8);
        timeSigDenominatorBox.setBounds (timeSigRow.removeFromLeft (78));
        right.removeFromTop (8);
        auto trackCountRow = right.removeFromTop (30);
        stateTrackCountLabel.setBounds (trackCountRow.removeFromLeft (72).reduced (0, 2));
        trackCountRow.removeFromLeft (8);
        stateTrackCountEditor.setBounds (trackCountRow.removeFromLeft (62).reduced (0, 2));
        right.removeFromTop (8);
        auto stateActionRow = right.removeFromTop (34);
        newStateButton.setBounds (stateActionRow.removeFromLeft (66).reduced (0, 3));
        duplicateStateButton.setBounds (stateActionRow.removeFromLeft (112).reduced (6, 3));
        deleteStateButton.setBounds (stateActionRow.removeFromLeft (82).reduced (6, 3));
        right.removeFromTop (10);
        selectedLabel.setBounds (right.removeFromTop (34));
        auto laneCountRow = right.removeFromTop (30);
        laneCountLabel.setBounds (laneCountRow.removeFromLeft (72).reduced (0, 2));
        laneCountRow.removeFromLeft (8);
        laneCountEditor.setBounds (laneCountRow.removeFromLeft (54).reduced (0, 2));
        right.removeFromTop (4);
        laneHeader.setBounds (right.removeFromTop (28));

        laneListViewport.setBounds (right);
        const auto laneListWidth = juce::jmax (0, right.getWidth() - 8);
        const auto laneRowHeight = 34;
        laneListContent.setSize (laneListWidth, laneRowHeight * maxTrackLanes);

        for (int i = 0; i < static_cast<int> (laneButtons.size()); ++i)
        {
            auto row = juce::Rectangle<int> (0, i * laneRowHeight, laneListWidth, laneRowHeight).reduced (0, 3);
            auto muteArea = row.removeFromRight (24);
            row.removeFromRight (4);
            auto soloArea = row.removeFromRight (24);
            row.removeFromRight (6);

            laneButtons[static_cast<size_t> (i)].setBounds (row);
            laneSoloButtons[static_cast<size_t> (i)].setBounds (soloArea);
            laneMuteButtons[static_cast<size_t> (i)].setBounds (muteArea);
        }
    }

private:
    juce::Colour currentViewAccent() const
    {
        switch (mainView)
        {
            case MainView::overall:     return cyan();
            case MainView::arrangement: return stateAccentForIndex (viewedTopLevelState);
            case MainView::code:        return blue();
            case MainView::track:       return laneAccentForIndex (selectedLane);
            case MainView::timeline:    return arrangementPlusMode ? cyan() : green();
            case MainView::mixer:       return arrangementPlusMode ? cyan() : amber();
        }

        return green();
    }

    static int stateButtonRowRightEdge (juce::Rectangle<int> area)
    {
        area.removeFromLeft (10);
        const auto buttonGap = 6;
        const auto buttonWidth = juce::jmax (52, (area.getWidth() - buttonGap * 7) / 8);
        return area.getX() + buttonWidth * 8 + buttonGap * 7;
    }

    int clampedTrackFocusCodePaneWidth (int availableWidth) const
    {
        const auto maximumWidth = juce::jmax (minTrackFocusCodePaneWidth,
                                              availableWidth - trackFocusPaneGap - minTrackFocusCanvasWidth);
        return juce::jlimit (minTrackFocusCodePaneWidth, maximumWidth, trackFocusCodePaneWidthPx);
    }

    void setTrackFocusCodePaneWidth (int requestedWidth)
    {
        const auto availableWidth = getLocalBounds().reduced (34).getWidth();
        trackFocusCodePaneWidthPx = clampedTrackFocusCodePaneWidthForAvailableWidth (requestedWidth, availableWidth);
        resized();
        repaint();
    }

    static int clampedTrackFocusCodePaneWidthForAvailableWidth (int requestedWidth, int availableWidth)
    {
        const auto maximumWidth = juce::jmax (minTrackFocusCodePaneWidth,
                                              availableWidth - trackFocusPaneGap - minTrackFocusCanvasWidth);
        return juce::jlimit (minTrackFocusCodePaneWidth, maximumWidth, requestedWidth);
    }

    int codeViewAvailableHeightForCurrentBounds() const
    {
        auto area = getLocalBounds().reduced (34);
        area.removeFromTop (44);
        area.removeFromBottom (48);
        area.removeFromTop (12);
        return juce::jmax (0, area.getHeight());
    }

    int codeViewStatePaneHeightForCurrentBounds() const
    {
        return codeViewStatePaneHeightForAvailableHeight (codeViewAvailableHeightForCurrentBounds());
    }

    int codeViewStatePaneHeightForAvailableHeight (int availableHeight) const
    {
        const auto usableHeight = juce::jmax (0, availableHeight - codeViewDividerHeight);
        if (usableHeight <= minCodeViewPaneHeight * 2)
            return juce::jmax (0, usableHeight / 2);

        const auto requested = static_cast<int> (std::round (static_cast<float> (usableHeight) * codeViewStatePaneRatio));
        return juce::jlimit (minCodeViewPaneHeight, usableHeight - minCodeViewPaneHeight, requested);
    }

    void setCodeViewStatePaneHeight (int requestedHeight)
    {
        const auto availableHeight = codeViewAvailableHeightForCurrentBounds();
        const auto usableHeight = juce::jmax (0, availableHeight - codeViewDividerHeight);
        if (usableHeight <= 0)
            return;

        const auto height = usableHeight <= minCodeViewPaneHeight * 2
                                ? juce::jmax (0, usableHeight / 2)
                                : juce::jlimit (minCodeViewPaneHeight, usableHeight - minCodeViewPaneHeight, requestedHeight);
        codeViewStatePaneRatio = juce::jlimit (0.12f, 0.88f, static_cast<float> (height) / static_cast<float> (usableHeight));
        resized();
        repaint();
    }

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

    void setupTextEditor (juce::TextEditor& editor, const juce::String& emptyText)
    {
        editor.setTextToShowWhenEmpty (emptyText, mutedInk().withAlpha (0.66f));
        editor.setColour (juce::TextEditor::backgroundColourId, panelSoft().withAlpha (0.36f));
        editor.setColour (juce::TextEditor::textColourId, ink());
        editor.setColour (juce::TextEditor::outlineColourId, mutedInk().withAlpha (0.09f));
        editor.setColour (juce::TextEditor::focusedOutlineColourId, blue().withAlpha (0.30f));
        editor.setColour (juce::TextEditor::highlightColourId, blue().withAlpha (0.24f));
        editor.setFont (juce::FontOptions (13.0f));
        addAndMakeVisible (editor);
    }

    void setMainView (MainView nextView)
    {
        mainView = nextView;
        syncOverallView();
        syncMixerView();
        syncArrangementTimelineView();
        syncViewVisibility();
        syncViewButtons();
        resized();
        repaint();
    }

    void syncViewButtons()
    {
        overallButton.setToggleState (mainView == MainView::overall, juce::dontSendNotification);
        arrangementButton.setToggleState (mainView == MainView::arrangement, juce::dontSendNotification);
        codeViewButton.setToggleState (mainView == MainView::code, juce::dontSendNotification);
        timelineViewButton.setButtonText (arrangementPlusMode ? "Arrangement+" : "Arrangement");
        timelineViewButton.setToggleState (mainView == MainView::timeline, juce::dontSendNotification);
        mixerViewButton.setButtonText (arrangementPlusMode ? "Mixer+" : "Mixer");
        mixerViewButton.setToggleState (mainView == MainView::mixer, juce::dontSendNotification);
        arrangementPointerToolButton.setToggleState (arrangementAudioTool == ArrangementAudioTool::pointer, juce::dontSendNotification);
        arrangementScissorsToolButton.setToggleState (arrangementAudioTool == ArrangementAudioTool::scissors, juce::dontSendNotification);
        arrangementFadeToolButton.setToggleState (arrangementAudioTool == ArrangementAudioTool::fade, juce::dontSendNotification);
        splitImportedRegionButton.setEnabled (arrangementPlusMode && hasImportedRegionSelection());
        deleteImportedRegionButton.setEnabled (hasImportedRegionSelection());
        syncRegionInspectorControls();
        syncTransportButtons();
    }

    void syncViewVisibility()
    {
        const auto overall = mainView == MainView::overall;
        const auto arrangement = mainView == MainView::arrangement;
        const auto track = mainView == MainView::track;
        const auto code = mainView == MainView::code;
        const auto timeline = mainView == MainView::timeline;
        const auto mixer = mainView == MainView::mixer;

        for (auto& button : stateButtons)
            button.setVisible (arrangement);

        globalScriptEditor.setVisible (arrangement || code);
        runScriptButton.setVisible (true);
        trackSectionLabel.setVisible (arrangement);
        trackNameLabel.setVisible (arrangement);
        trackNameEditor.setVisible (arrangement);
        trackDurationLabel.setVisible (arrangement);
        trackDurationEditor.setVisible (arrangement);
        laneSectionLabel.setVisible (arrangement);
        laneNameLabel.setVisible (arrangement || track);
        laneNameEditor.setVisible (arrangement || track);
        laneTempoLabel.setVisible (arrangement || track);
        laneTempoEditor.setVisible (arrangement || track);
        laneDurationLabel.setVisible (arrangement || track);
        laneDurationEditor.setVisible (arrangement || track);
        laneCountLabel.setVisible (arrangement);
        laneCountEditor.setVisible (arrangement);
        laneListViewport.setVisible (arrangement);
        muteLaneButton.setVisible (false);
        soloLaneButton.setVisible (false);
        duplicateLaneButton.setVisible (false);
        deleteLaneButton.setVisible (false);
        orbitCanvas.setVisible (arrangement);
        overallCanvas.setVisible (overall);
        trackFocusCanvas.setVisible (track);
        trackFocusDivider.setVisible (track);
        codeViewDivider.setVisible (code);
        stateSettingsLabel.setVisible (arrangement);
        stateTempoLabel.setVisible (arrangement);
        stateTempoSlider.setVisible (arrangement);
        stateTimeSigLabel.setVisible (arrangement);
        timeSigNumeratorBox.setVisible (arrangement);
        timeSigDenominatorBox.setVisible (arrangement);
        stateTrackCountLabel.setVisible (arrangement);
        stateTrackCountEditor.setVisible (arrangement);
        newStateButton.setVisible (arrangement);
        duplicateStateButton.setVisible (arrangement);
        deleteStateButton.setVisible (arrangement);
        selectedLabel.setVisible (arrangement);
        laneHeader.setVisible (arrangement);

        for (auto& button : laneButtons)
            button.setVisible (arrangement && button.isEnabled());

        for (auto& button : laneSoloButtons)
            button.setVisible (arrangement && button.isEnabled());

        for (auto& button : laneMuteButtons)
            button.setVisible (arrangement && button.isEnabled());

        laneCodeHeader.setVisible (track || code);
        laneCodeMetadataLabel.setVisible (track || code);
        laneCodeEditor.setVisible (track || code);
        laneCodeRunButton.setVisible (track || code);
        stateCodeHeader.setVisible (code);
        arrangementTimelineViewport.setVisible (timeline);
        removeRenderedAudioButton.setVisible (timeline && arrangementPlusMode);
        arrangementPointerToolButton.setVisible (timeline && arrangementPlusMode);
        arrangementScissorsToolButton.setVisible (timeline && arrangementPlusMode);
        arrangementFadeToolButton.setVisible (timeline && arrangementPlusMode);
        splitImportedRegionButton.setVisible (timeline && arrangementPlusMode);
        deleteImportedRegionButton.setVisible (timeline && arrangementPlusMode);
        arrangementHorizontalZoomLabel.setVisible (timeline);
        arrangementHorizontalZoomSlider.setVisible (timeline);
        arrangementVerticalZoomLabel.setVisible (timeline);
        arrangementVerticalZoomSlider.setVisible (timeline);
        const auto showRegionInspector = timeline && arrangementPlusMode;
        regionInspectorLabel.setVisible (showRegionInspector);
        regionStartLabel.setVisible (showRegionInspector);
        regionLengthLabel.setVisible (showRegionInspector);
        regionSourceLabel.setVisible (showRegionInspector);
        regionGainLabel.setVisible (showRegionInspector);
        regionFadeInLabel.setVisible (showRegionInspector);
        regionFadeOutLabel.setVisible (showRegionInspector);
        regionFadeInCurveLabel.setVisible (showRegionInspector);
        regionFadeOutCurveLabel.setVisible (showRegionInspector);
        regionStartEditor.setVisible (showRegionInspector);
        regionLengthEditor.setVisible (showRegionInspector);
        regionSourceEditor.setVisible (showRegionInspector);
        regionGainEditor.setVisible (showRegionInspector);
        regionFadeInEditor.setVisible (showRegionInspector);
        regionFadeOutEditor.setVisible (showRegionInspector);
        regionFadeInCurveBox.setVisible (showRegionInspector);
        regionFadeOutCurveBox.setVisible (showRegionInspector);
        mixerViewport.setVisible (mixer);

        overallButton.setVisible (true);
        arrangementButton.setVisible (true);
        codeViewButton.setVisible (true);
        timelineViewButton.setVisible (true);
        mixerViewButton.setVisible (true);
    }

    void syncMixerView()
    {
        mixerCanvas.setProject (&topLevelStates,
                                viewedTopLevelState,
                                selectedState,
                                selectedLane,
                                performingTopLevelState,
                                performingTrackIndex,
                                running,
                                &importedLaneAudioClips,
                                arrangementPlusMode,
                                static_cast<float> (gainSlider.getValue()),
                                &masterEffectSlots);
        resizeMixerCanvas();

        if (mainView == MainView::mixer)
            scrollMixerToPlayingChannels();
    }

    void syncOverallView()
    {
        overallCanvas.setProject (&topLevelStates, viewedTopLevelState, performingTopLevelState, running);
    }

    void syncArrangementTimelineView()
    {
        auto steps = parseGlobalScript();

        if (steps.empty() && isTopLevelStatePopulated (viewedTopLevelState))
            steps.push_back ({ viewedTopLevelState, 4.0, {}, {}, {} });

        const auto visibleArrangementPlayheadSeconds = running && isUsingImportedAudioPlayback()
            ? audioCallback.getImportedPlaybackPositionSeconds()
            : arrangementEditPlayheadSeconds;

        arrangementTimelineCanvas.setProject (&topLevelStates,
                                              std::move (steps),
                                              performingTopLevelState,
                                              performingTrackIndex,
                                              scriptRunning ? scriptStepIndex : 0,
                                              scriptRunning ? scriptStepElapsedBars : 0.0,
                                              running,
                                              &importedLaneAudioClips,
                                              arrangementPlusMode,
                                              &topLevelTemposBpm,
                                              &topLevelTimeSigNumerators,
                                              &topLevelTimeSigDenominators,
                                              selectedImportedRegion,
                                              selectedImportedRegions,
                                              visibleArrangementPlayheadSeconds,
                                              arrangementAudioTool);
        arrangementTimelineCanvas.setZoom (arrangementHorizontalZoom, arrangementVerticalZoom);
        resizeArrangementTimelineCanvas();
    }

    void syncTrackFocusCanvas (const std::vector<Wf::StateSpec>* viewedTracks)
    {
        if (viewedTracks == nullptr || viewedTracks->empty())
        {
            focusedTrackIndex = 0;
            trackFocusCanvas.setTrack (nullptr, 0, 4, 88.0f, 0.0, 0.0f, false);
            return;
        }

        focusedTrackIndex = juce::jlimit (0, static_cast<int> (viewedTracks->size()) - 1, focusedTrackIndex);
        const auto& track = (*viewedTracks)[static_cast<size_t> (focusedTrackIndex)];
        const auto selectedLaneForFocus = focusedTrackIndex == selectedState
                                            ? juce::jlimit (0, juce::jmax (0, static_cast<int> (track.lanes.size()) - 1), selectedLane)
                                            : 0;
        const auto focusedTrackIsPlaying = running
                                        && viewedTopLevelState == performingTopLevelState
                                        && focusedTrackIndex == performingTrackIndex;

        trackFocusCanvas.setTrack (&track,
                                   selectedLaneForFocus,
                                   topLevelTimeSigNumerators[static_cast<size_t> (juce::jlimit (0, maxTopLevelStates - 1, viewedTopLevelState))],
                                   viewedTopLevelState == performingTopLevelState ? getPerformingTempoBpm()
                                                                                  : topLevelTemposBpm[static_cast<size_t> (juce::jlimit (0, maxTopLevelStates - 1, viewedTopLevelState))],
                                   focusedTrackIsPlaying ? trackElapsedBars : 0.0,
                                   focusedTrackIsPlaying ? orbitPhase : 0.0f,
                                   focusedTrackIsPlaying);
    }

    void resizeMixerCanvas()
    {
        const auto width = juce::jmax (mixerViewport.getWidth(), mixerCanvas.getPreferredWidth());
        const auto height = juce::jmax (1, mixerViewport.getHeight());
        mixerCanvas.setSize (width, height);
    }

    void resizeArrangementTimelineCanvas()
    {
        const auto width = juce::jmax (arrangementTimelineViewport.getWidth(), arrangementTimelineCanvas.getPreferredWidth());
        const auto height = juce::jmax (arrangementTimelineViewport.getHeight(), arrangementTimelineCanvas.getPreferredHeight());
        arrangementTimelineCanvas.setSize (width, height);
    }

    void applyArrangementZoomChange()
    {
        const auto previousWidth = juce::jmax (1, arrangementTimelineCanvas.getWidth());
        const auto previousHeight = juce::jmax (1, arrangementTimelineCanvas.getHeight());
        const auto centreXFraction = static_cast<double> (arrangementTimelineViewport.getViewPositionX()
                                                          + arrangementTimelineViewport.getViewWidth() / 2)
                                   / static_cast<double> (previousWidth);
        const auto centreYFraction = static_cast<double> (arrangementTimelineViewport.getViewPositionY()
                                                          + arrangementTimelineViewport.getViewHeight() / 2)
                                   / static_cast<double> (previousHeight);

        arrangementHorizontalZoom = arrangementHorizontalZoomSlider.getValue();
        arrangementVerticalZoom = arrangementVerticalZoomSlider.getValue();
        markProjectDirty();
        arrangementTimelineCanvas.setZoom (arrangementHorizontalZoom, arrangementVerticalZoom);
        resizeArrangementTimelineCanvas();

        const auto targetX = static_cast<int> (centreXFraction * static_cast<double> (arrangementTimelineCanvas.getWidth())
                                               - arrangementTimelineViewport.getViewWidth() / 2);
        const auto targetY = static_cast<int> (centreYFraction * static_cast<double> (arrangementTimelineCanvas.getHeight())
                                               - arrangementTimelineViewport.getViewHeight() / 2);
        arrangementTimelineViewport.setViewPosition (juce::jmax (0, targetX), juce::jmax (0, targetY));
    }

    void chooseArrangementRenderFile()
    {
        if (! globalScriptHasStop())
        {
            statusLabel.setText ("render needs stop() in state code", juce::dontSendNotification);
            return;
        }

        const auto defaultFile = juce::File::getSpecialLocation (juce::File::userDesktopDirectory)
            .getChildFile (isUsingImportedAudioPlayback() ? "ChucK-ME arrangement+.wav"
                                                           : "ChucK-ME arrangement.wav");
        renderChooser = std::make_unique<juce::FileChooser> ("Render Arrangement to WAV",
                                                             defaultFile,
                                                             "*.wav");
        renderChooser->launchAsync (juce::FileBrowserComponent::saveMode
                                        | juce::FileBrowserComponent::canSelectFiles
                                        | juce::FileBrowserComponent::warnAboutOverwriting,
                                    [this] (const juce::FileChooser& chooser)
                                    {
                                        auto file = chooser.getResult();
                                        if (file == juce::File())
                                            return;

                                        if (file.getFileExtension().isEmpty())
                                            file = file.withFileExtension ("wav");

                                        if (isUsingImportedAudioPlayback())
                                            renderImportedArrangementToWav (file);
                                        else
                                            renderArrangementToWav (file);
                                    });
    }

    void chooseArrangementLaneRenderDirectory (bool importAfterRender)
    {
        if (! globalScriptHasStop())
        {
            statusLabel.setText ("lane render needs stop() in state code", juce::dontSendNotification);
            return;
        }

        renderChooser = std::make_unique<juce::FileChooser> ("Render lane WAVs to folder",
                                                             juce::File::getSpecialLocation (juce::File::userDesktopDirectory),
                                                             juce::String());
        renderChooser->launchAsync (juce::FileBrowserComponent::openMode
                                        | juce::FileBrowserComponent::canSelectDirectories,
                                    [this, importAfterRender] (const juce::FileChooser& chooser)
                                    {
                                        const auto directory = chooser.getResult();
                                        if (directory == juce::File())
                                            return;

                                        renderArrangementLanesToDirectory (directory, importAfterRender);
                                    });
    }

    bool globalScriptHasStop() const
    {
        return juce::String (stripGlobalScriptComments (globalScriptEditor.getText().toStdString())).containsIgnoreCase ("stop");
    }

    static bool hasJsonValue (const juce::var& value)
    {
        return ! value.isVoid() && ! value.isUndefined();
    }

    static void setJsonProperty (juce::DynamicObject& object, const char* name, const juce::var& value)
    {
        object.setProperty (juce::Identifier (name), value);
    }

    static juce::var getJsonProperty (const juce::DynamicObject& object, const char* name)
    {
        return object.getProperty (juce::Identifier (name));
    }

    static juce::String stringFromJson (const juce::DynamicObject& object,
                                        const char* name,
                                        const juce::String& fallback = {})
    {
        const auto value = getJsonProperty (object, name);
        return hasJsonValue (value) ? value.toString() : fallback;
    }

    static int intFromJson (const juce::DynamicObject& object, const char* name, int fallback)
    {
        const auto value = getJsonProperty (object, name);
        return hasJsonValue (value) ? value.toString().getIntValue() : fallback;
    }

    static double doubleFromJson (const juce::DynamicObject& object, const char* name, double fallback)
    {
        const auto value = getJsonProperty (object, name);
        return hasJsonValue (value) ? static_cast<double> (value) : fallback;
    }

    static bool boolFromJson (const juce::DynamicObject& object, const char* name, bool fallback)
    {
        const auto value = getJsonProperty (object, name);
        return hasJsonValue (value) ? static_cast<bool> (value) : fallback;
    }

    static juce::var durationToVar (const Wf::TrackDurationSpec& duration)
    {
        auto* object = new juce::DynamicObject();
        setJsonProperty (*object, "bars", duration.bars);
        setJsonProperty (*object, "beats", duration.beats);
        return juce::var (object);
    }

    static std::optional<Wf::TrackDurationSpec> durationFromVar (const juce::var& value)
    {
        if (auto* object = value.getDynamicObject())
        {
            const auto bars = juce::jlimit (0, 128, intFromJson (*object, "bars", 0));
            const auto beats = juce::jlimit (0, 1024, intFromJson (*object, "beats", 0));

            if (bars == 0 && beats == 0)
                return {};

            return Wf::TrackDurationSpec { bars, beats };
        }

        if (value.isString())
            return parseTrackDuration (value.toString());

        return {};
    }

    static juce::var importedRegionToVar (const ImportedAudioRegion& region)
    {
        auto* object = new juce::DynamicObject();
        setJsonProperty (*object, "startSeconds", region.startSeconds);
        setJsonProperty (*object, "sourceStartSeconds", region.sourceStartSeconds);
        setJsonProperty (*object, "lengthSeconds", region.lengthSeconds);
        setJsonProperty (*object, "fadeInSeconds", region.fadeInSeconds);
        setJsonProperty (*object, "fadeOutSeconds", region.fadeOutSeconds);
        setJsonProperty (*object, "gain", static_cast<double> (region.gain));
        setJsonProperty (*object, "fadeInCurve", region.fadeInCurve);
        setJsonProperty (*object, "fadeOutCurve", region.fadeOutCurve);
        return juce::var (object);
    }

    static std::optional<ImportedAudioRegion> importedRegionFromVar (const juce::var& value)
    {
        auto* object = value.getDynamicObject();
        if (object == nullptr)
            return {};

        ImportedAudioRegion region;
        region.startSeconds = doubleFromJson (*object, "startSeconds", 0.0);
        region.sourceStartSeconds = doubleFromJson (*object, "sourceStartSeconds", 0.0);
        region.lengthSeconds = doubleFromJson (*object, "lengthSeconds", 0.0);
        region.fadeInSeconds = doubleFromJson (*object, "fadeInSeconds", 0.0);
        region.fadeOutSeconds = doubleFromJson (*object, "fadeOutSeconds", 0.0);
        region.gain = juce::jlimit (0.0f, 2.0f, static_cast<float> (doubleFromJson (*object, "gain", 1.0)));
        region.fadeInCurve = juce::jlimit (0, 3, intFromJson (*object, "fadeInCurve", 1));
        region.fadeOutCurve = juce::jlimit (0, 3, intFromJson (*object, "fadeOutCurve", 1));
        return region;
    }

    static juce::var effectSlotToVar (const Wf::TrackEffectSlotSpec& slot)
    {
        auto* object = new juce::DynamicObject();
        setJsonProperty (*object, "active", slot.active);
        setJsonProperty (*object, "pluginName", slot.pluginName);
        setJsonProperty (*object, "pluginFormatName", slot.pluginFormatName);
        setJsonProperty (*object, "pluginFileOrIdentifier", slot.pluginFileOrIdentifier);
        setJsonProperty (*object, "pluginIdentifier", slot.pluginIdentifier);
        return juce::var (object);
    }

    static Wf::TrackEffectSlotSpec effectSlotFromVar (const juce::var& value)
    {
        Wf::TrackEffectSlotSpec slot;

        if (auto* object = value.getDynamicObject())
        {
            slot.active = boolFromJson (*object, "active", false);
            slot.pluginName = stringFromJson (*object, "pluginName");
            slot.pluginFormatName = stringFromJson (*object, "pluginFormatName");
            slot.pluginFileOrIdentifier = stringFromJson (*object, "pluginFileOrIdentifier");
            slot.pluginIdentifier = stringFromJson (*object, "pluginIdentifier");
        }

        return slot;
    }

    static juce::var laneToVar (const Wf::LaneSpec& lane)
    {
        auto* object = new juce::DynamicObject();
        setJsonProperty (*object, "name", lane.name);
        setJsonProperty (*object, "role", lane.role);
        setJsonProperty (*object, "baseHz", static_cast<double> (lane.baseHz));
        setJsonProperty (*object, "volume", static_cast<double> (lane.volume));
        setJsonProperty (*object, "pan", static_cast<double> (lane.pan));
        setJsonProperty (*object, "pulseTicks", lane.pulseTicks);
        setJsonProperty (*object, "openTicks", lane.openTicks);
        setJsonProperty (*object, "phaseOffsetBars", static_cast<double> (lane.phaseOffsetBars));
        setJsonProperty (*object, "muted", lane.muted);
        setJsonProperty (*object, "solo", lane.solo);

        if (lane.tempoBpm.has_value())
            setJsonProperty (*object, "tempoBpm", static_cast<double> (*lane.tempoBpm));

        if (lane.duration.has_value())
            setJsonProperty (*object, "duration", durationToVar (*lane.duration));

        if (lane.customDeclarationCode.has_value())
            setJsonProperty (*object, "customDeclarationCode", *lane.customDeclarationCode);

        if (lane.customControlCode.has_value())
            setJsonProperty (*object, "customControlCode", *lane.customControlCode);

        return juce::var (object);
    }

    static Wf::LaneSpec laneFromVar (const juce::var& value)
    {
        Wf::LaneSpec lane;

        if (auto* object = value.getDynamicObject())
        {
            lane.name = stringFromJson (*object, "name", "Lane");
            lane.role = stringFromJson (*object, "role");
            lane.baseHz = juce::jlimit (10.0f, 20000.0f, static_cast<float> (doubleFromJson (*object, "baseHz", lane.baseHz)));
            lane.volume = juce::jlimit (0.0f, 1.0f, static_cast<float> (doubleFromJson (*object, "volume", lane.volume)));
            lane.pan = juce::jlimit (-1.0f, 1.0f, static_cast<float> (doubleFromJson (*object, "pan", lane.pan)));
            lane.pulseTicks = juce::jlimit (1, 1024, intFromJson (*object, "pulseTicks", lane.pulseTicks));
            lane.openTicks = juce::jlimit (1, 1024, intFromJson (*object, "openTicks", lane.openTicks));
            lane.phaseOffsetBars = juce::jlimit (0.0f, 1.0f, static_cast<float> (doubleFromJson (*object, "phaseOffsetBars", lane.phaseOffsetBars)));
            lane.muted = boolFromJson (*object, "muted", lane.muted);
            lane.solo = boolFromJson (*object, "solo", lane.solo);

            const auto tempo = getJsonProperty (*object, "tempoBpm");
            if (hasJsonValue (tempo))
                lane.tempoBpm = juce::jlimit (30.0f, 220.0f, static_cast<float> (static_cast<double> (tempo)));

            lane.duration = durationFromVar (getJsonProperty (*object, "duration"));

            const auto declarationCode = getJsonProperty (*object, "customDeclarationCode");
            if (hasJsonValue (declarationCode))
                lane.customDeclarationCode = declarationCode.toString();

            const auto controlCode = getJsonProperty (*object, "customControlCode");
            if (hasJsonValue (controlCode))
                lane.customControlCode = controlCode.toString();
        }

        return lane;
    }

    static juce::var trackToVar (const Wf::StateSpec& track)
    {
        auto* object = new juce::DynamicObject();
        setJsonProperty (*object, "name", track.name);
        setJsonProperty (*object, "tempoBpm", track.tempoBpm);
        setJsonProperty (*object, "clockBeatsPerBar", track.clockBeatsPerBar);
        setJsonProperty (*object, "clockQuarterNotesPerBar", track.clockQuarterNotesPerBar);

        if (track.duration.has_value())
            setJsonProperty (*object, "duration", durationToVar (*track.duration));

        if (track.transitionProbabilityPercent.has_value())
            setJsonProperty (*object, "transitionProbabilityPercent", *track.transitionProbabilityPercent);

        juce::Array<juce::var> lanes;
        for (const auto& lane : track.lanes)
            lanes.add (laneToVar (lane));
        setJsonProperty (*object, "lanes", lanes);

        juce::Array<juce::var> effectSlots;
        for (const auto& slot : track.effectSlots)
            effectSlots.add (effectSlotToVar (slot));
        setJsonProperty (*object, "effectSlots", effectSlots);

        return juce::var (object);
    }

    static Wf::StateSpec trackFromVar (const juce::var& value)
    {
        Wf::StateSpec track;

        if (auto* object = value.getDynamicObject())
        {
            track.name = trackDisplayName (stringFromJson (*object, "name", "Track"));
            track.tempoBpm = juce::jlimit (30.0, 220.0, doubleFromJson (*object, "tempoBpm", track.tempoBpm));
            track.clockBeatsPerBar = juce::jlimit (1, 16, intFromJson (*object, "clockBeatsPerBar", track.clockBeatsPerBar));
            track.clockQuarterNotesPerBar = juce::jlimit (1.0, 16.0, doubleFromJson (*object, "clockQuarterNotesPerBar", track.clockQuarterNotesPerBar));
            track.duration = durationFromVar (getJsonProperty (*object, "duration"));

            const auto probability = getJsonProperty (*object, "transitionProbabilityPercent");
            if (hasJsonValue (probability))
                track.transitionProbabilityPercent = juce::jlimit (0, 100, probability.toString().getIntValue());

            track.lanes.clear();
            const auto lanesValue = getJsonProperty (*object, "lanes");
            if (auto* lanes = lanesValue.getArray())
            {
                const auto laneCount = juce::jmin (maxTrackLanes, lanes->size());
                for (int i = 0; i < laneCount; ++i)
                    track.lanes.push_back (laneFromVar ((*lanes)[i]));
            }

            const auto effectsValue = getJsonProperty (*object, "effectSlots");
            if (auto* effects = effectsValue.getArray())
            {
                const auto effectCount = juce::jmin (maxTrackEffectSlots, effects->size());
                for (int i = 0; i < effectCount; ++i)
                    track.effectSlots[static_cast<size_t> (i)] = effectSlotFromVar ((*effects)[i]);
            }
        }

        return track;
    }

    juce::var projectToVar() const
    {
        auto* object = new juce::DynamicObject();
        setJsonProperty (*object, "format", "ChucK-ME Project");
        setJsonProperty (*object, "version", 4);
        setJsonProperty (*object, "stateCode", globalScriptEditor.getText());
        setJsonProperty (*object, "masterVolume", gainSlider.getValue());
        juce::Array<juce::var> masterEffects;
        for (const auto& slot : masterEffectSlots)
            masterEffects.add (effectSlotToVar (slot));
        setJsonProperty (*object, "masterEffectSlots", masterEffects);
        setJsonProperty (*object, "viewedState", viewedTopLevelState);
        setJsonProperty (*object, "selectedTrack", selectedState);
        setJsonProperty (*object, "selectedLane", selectedLane);
        setJsonProperty (*object, "arrangementPlusMode", arrangementPlusMode);
        setJsonProperty (*object, "arrangementHorizontalZoom", arrangementHorizontalZoom);
        setJsonProperty (*object, "arrangementVerticalZoom", arrangementVerticalZoom);

        juce::Array<juce::var> states;
        for (int i = 0; i < maxTopLevelStates; ++i)
        {
            auto* stateObject = new juce::DynamicObject();
            setJsonProperty (*stateObject, "tempoBpm", static_cast<double> (topLevelTemposBpm[static_cast<size_t> (i)]));
            setJsonProperty (*stateObject, "timeSigNumerator", topLevelTimeSigNumerators[static_cast<size_t> (i)]);
            setJsonProperty (*stateObject, "timeSigDenominator", topLevelTimeSigDenominators[static_cast<size_t> (i)]);

            if (topLevelStates[static_cast<size_t> (i)].has_value())
            {
                juce::Array<juce::var> tracks;
                for (const auto& track : *topLevelStates[static_cast<size_t> (i)])
                    tracks.add (trackToVar (track));
                setJsonProperty (*stateObject, "tracks", tracks);
            }

            states.add (juce::var (stateObject));
        }
        setJsonProperty (*object, "states", states);

        juce::Array<juce::var> importedAudio;
        for (const auto& clip : importedLaneAudioClips)
        {
            auto* clipObject = new juce::DynamicObject();
            setJsonProperty (*clipObject, "stateIndex", clip.stateIndex);
            setJsonProperty (*clipObject, "trackIndex", clip.trackIndex);
            setJsonProperty (*clipObject, "laneIndex", clip.laneIndex);
            setJsonProperty (*clipObject, "file", clip.file.getFullPathName());
            if (currentProjectFile != juce::File()
                && clip.file.getParentDirectory() == projectMediaDirectoryFor (currentProjectFile))
                setJsonProperty (*clipObject, "mediaFile", clip.file.getFileName());
            setJsonProperty (*clipObject, "mixGain", static_cast<double> (clip.mixGain));
            setJsonProperty (*clipObject, "mixPan", static_cast<double> (clip.mixPan));
            juce::Array<juce::var> regions;
            for (const auto& region : clip.regions)
                regions.add (importedRegionToVar (region));
            setJsonProperty (*clipObject, "regions", regions);
            importedAudio.add (juce::var (clipObject));
        }
        setJsonProperty (*object, "importedAudio", importedAudio);

        return juce::var (object);
    }

    bool applyProjectVar (const juce::var& project)
    {
        auto* object = project.getDynamicObject();
        if (object == nullptr)
            return false;

        juce::ScopedValueSetter<bool> dirtyGuard (suppressProjectDirty, true);
        running = false;
        scriptRunning = false;
        audioCallback.clearImportedAudioPlayback();
        importedLaneAudioClips.clear();
        selectedImportedRegion = {};
        selectedImportedRegions.clear();
        arrangementEditPlayheadSeconds = 0.0;
        arrangementAudioTool = ArrangementAudioTool::pointer;
        importedFadeDragUndoStarted = false;
        importedTrimDragUndoStarted = false;
        importedGainDragUndoStarted = false;
        arrangementPlusMode = false;
        missingImportedAudioOnLastLoad = 0;

        for (auto& slot : topLevelStates)
            slot.reset();
        topLevelTemposBpm = defaultTopLevelTempos();
        topLevelTimeSigNumerators = defaultTopLevelTimeSigNumerators();
        topLevelTimeSigDenominators = defaultTopLevelTimeSigDenominators();
        masterEffectSlots = {};

        const auto statesValue = getJsonProperty (*object, "states");
        if (auto* states = statesValue.getArray())
        {
            const auto stateCount = juce::jmin (maxTopLevelStates, states->size());

            for (int stateIndex = 0; stateIndex < stateCount; ++stateIndex)
            {
                auto* stateObject = (*states)[stateIndex].getDynamicObject();
                if (stateObject == nullptr)
                    continue;

                topLevelTemposBpm[static_cast<size_t> (stateIndex)] =
                    juce::jlimit (30.0f,
                                  220.0f,
                                  static_cast<float> (doubleFromJson (*stateObject,
                                                                      "tempoBpm",
                                                                      topLevelTemposBpm[static_cast<size_t> (stateIndex)])));
                topLevelTimeSigNumerators[static_cast<size_t> (stateIndex)] =
                    sanitizeScriptTimeSigNumerator (intFromJson (*stateObject, "timeSigNumerator", 4));
                topLevelTimeSigDenominators[static_cast<size_t> (stateIndex)] =
                    sanitizeScriptTimeSigDenominator (intFromJson (*stateObject, "timeSigDenominator", 4));

                const auto tracksValue = getJsonProperty (*stateObject, "tracks");
                if (auto* tracks = tracksValue.getArray())
                {
                    std::vector<Wf::StateSpec> loadedTracks;
                    const auto trackCount = juce::jmin (maxStateTracks, tracks->size());
                    loadedTracks.reserve (static_cast<size_t> (trackCount));

                    for (int trackIndex = 0; trackIndex < trackCount; ++trackIndex)
                    {
                        auto track = trackFromVar ((*tracks)[trackIndex]);
                        if (track.lanes.empty())
                            track.lanes.push_back (Wf::LaneSpec { "Lane 1", {}, 220.0f, 0.2f, 96, 18 });
                        loadedTracks.push_back (std::move (track));
                    }

                    if (! loadedTracks.empty())
                        topLevelStates[static_cast<size_t> (stateIndex)] = std::move (loadedTracks);
                }
            }
        }

        if (! isTopLevelStatePopulated (0))
            topLevelStates[0] = Wf::makeDefaultStates();

        {
            juce::ScopedValueSetter<bool> guard (suppressEditCallbacks, true);
            juce::ScopedValueSetter<bool> laneGuard (suppressLaneCodeCallbacks, true);
            globalScriptEditor.setText (stringFromJson (*object, "stateCode", defaultGlobalScriptText()), juce::dontSendNotification);
            globalScriptEditor.refreshBaseTextColour();
            gainSlider.setValue (juce::jlimit (0.0, 0.8, doubleFromJson (*object, "masterVolume", 0.18)), juce::dontSendNotification);
            arrangementHorizontalZoom = juce::jlimit (0.35, 3.25, doubleFromJson (*object, "arrangementHorizontalZoom", 1.0));
            arrangementVerticalZoom = juce::jlimit (0.55, 3.0, doubleFromJson (*object, "arrangementVerticalZoom", 1.0));
            arrangementHorizontalZoomSlider.setValue (arrangementHorizontalZoom, juce::dontSendNotification);
            arrangementVerticalZoomSlider.setValue (arrangementVerticalZoom, juce::dontSendNotification);
            laneCodeDirty = false;
        }

        if (auto* effects = getJsonProperty (*object, "masterEffectSlots").getArray())
        {
            const auto effectCount = juce::jmin (maxTrackEffectSlots, effects->size());
            for (int i = 0; i < effectCount; ++i)
                masterEffectSlots[static_cast<size_t> (i)] = effectSlotFromVar ((*effects)[i]);
        }

        viewedTopLevelState = juce::jlimit (0, maxTopLevelStates - 1, intFromJson (*object, "viewedState", 0));
        if (! isTopLevelStatePopulated (viewedTopLevelState))
            viewedTopLevelState = 0;
        performingTopLevelState = viewedTopLevelState;
        selectedState = juce::jmax (0, intFromJson (*object, "selectedTrack", 0));
        selectedLane = juce::jmax (0, intFromJson (*object, "selectedLane", 0));
        focusedTrackIndex = selectedState;
        performingTrackIndex = selectedState;
        trackElapsedBars = 0.0;
        nextBarTransitionCheck = 1.0;
        orbitPhase = 0.0f;

        std::vector<ImportedLaneAudioClip> loadedClips;
        const auto importedValue = getJsonProperty (*object, "importedAudio");
        if (auto* imported = importedValue.getArray())
        {
            loadedClips.reserve (static_cast<size_t> (imported->size()));

            for (const auto& clipValue : *imported)
            {
                auto* clipObject = clipValue.getDynamicObject();
                if (clipObject == nullptr)
                    continue;

                ImportedLaneAudioClip clip;
                clip.stateIndex = intFromJson (*clipObject, "stateIndex", 0);
                clip.trackIndex = intFromJson (*clipObject, "trackIndex", 0);
                clip.laneIndex = intFromJson (*clipObject, "laneIndex", 0);
                clip.file = juce::File (stringFromJson (*clipObject, "file"));
                const auto mediaFileName = stringFromJson (*clipObject, "mediaFile");
                if (! clip.file.existsAsFile()
                    && mediaFileName.isNotEmpty()
                    && loadingProjectFile != juce::File())
                {
                    const auto mediaCandidate = projectMediaDirectoryFor (loadingProjectFile).getChildFile (mediaFileName);
                    if (mediaCandidate.existsAsFile())
                        clip.file = mediaCandidate;
                }
                clip.mixGain = juce::jlimit (0.0f, 1.5f, static_cast<float> (doubleFromJson (*clipObject, "mixGain", 1.0)));
                clip.mixPan = juce::jlimit (-1.0f, 1.0f, static_cast<float> (doubleFromJson (*clipObject, "mixPan", 0.0)));
                if (auto* regions = getJsonProperty (*clipObject, "regions").getArray())
                {
                    clip.regions.reserve (static_cast<size_t> (regions->size()));
                    for (const auto& regionValue : *regions)
                        if (auto region = importedRegionFromVar (regionValue))
                            clip.regions.push_back (*region);
                }

                if (isTopLevelStatePopulated (clip.stateIndex))
                {
                    const auto& tracks = *topLevelStates[static_cast<size_t> (clip.stateIndex)];
                    if (clip.trackIndex >= 0
                        && clip.trackIndex < static_cast<int> (tracks.size())
                        && clip.laneIndex >= 0
                        && clip.laneIndex < static_cast<int> (tracks[static_cast<size_t> (clip.trackIndex)].lanes.size())
                        && clip.file.existsAsFile())
                    {
                        loadedClips.push_back (std::move (clip));
                        continue;
                    }
                }

                ++missingImportedAudioOnLastLoad;
            }
        }

        refreshLabels();
        syncTransportButtons();
        syncArrangementTimelineView();
        syncMixerView();

        if (boolFromJson (*object, "arrangementPlusMode", false) && ! loadedClips.empty())
            importRenderedLaneFiles (std::move (loadedClips), false);
        else
            setMainView (MainView::arrangement);

        return true;
    }

    void resetProjectToEmptyProject()
    {
        running = false;
        scriptRunning = false;
        scriptStepIndex = 0;
        scriptStepElapsedBars = 0.0;
        globalScriptSteps.clear();
        audioCallback.clearImportedAudioPlayback();
        audioCallback.useChucKPlayback();
        importedLaneAudioClips.clear();
        arrangementPlusMode = false;
        selectedImportedRegion = {};
        selectedImportedRegions.clear();
        arrangementEditPlayheadSeconds = 0.0;
        arrangementAudioTool = ArrangementAudioTool::pointer;
        importedFadeDragUndoStarted = false;
        importedTrimDragUndoStarted = false;
        importedGainDragUndoStarted = false;

        for (auto& slot : topLevelStates)
            slot.reset();

        topLevelTemposBpm = defaultTopLevelTempos();
        topLevelTimeSigNumerators = defaultTopLevelTimeSigNumerators();
        topLevelTimeSigDenominators = defaultTopLevelTimeSigDenominators();
        masterEffectSlots = {};

        {
            juce::ScopedValueSetter<bool> editGuard (suppressEditCallbacks, true);
            juce::ScopedValueSetter<bool> laneGuard (suppressLaneCodeCallbacks, true);
            globalScriptEditor.setText ({}, juce::dontSendNotification);
            globalScriptEditor.refreshBaseTextColour();
            gainSlider.setValue (0.18, juce::dontSendNotification);
            laneCodeDirty = false;
            laneCodeRunButton.setEnabled (false);
        }

        viewedTopLevelState = 0;
        performingTopLevelState = 0;
        selectedState = 0;
        selectedLane = 0;
        focusedTrackIndex = 0;
        performingTrackIndex = 0;
        trackElapsedBars = 0.0;
        nextBarTransitionCheck = 1.0;
        orbitPhase = 0.0f;
        arrangementHorizontalZoom = 1.0;
        arrangementVerticalZoom = 1.0;
        arrangementHorizontalZoomSlider.setValue (arrangementHorizontalZoom, juce::dontSendNotification);
        arrangementVerticalZoomSlider.setValue (arrangementVerticalZoom, juce::dontSendNotification);
        currentProjectFile = juce::File();

        refreshLabels();
        syncTransportButtons();
        setMainView (MainView::arrangement);
    }

    static juce::File withProjectExtension (juce::File file)
    {
        return file.getFileExtension().isEmpty() ? file.withFileExtension ("chuckme") : file;
    }

    juce::TextEditor* getFocusedTextEditor()
    {
        for (auto* component = juce::Component::getCurrentlyFocusedComponent();
             component != nullptr;
             component = component->getParentComponent())
        {
            if (auto* editor = dynamic_cast<juce::TextEditor*> (component))
                return editor;
        }

        return nullptr;
    }

    bool canUndo()
    {
        return getFocusedTextEditor() != nullptr || ! undoStack.empty();
    }

    bool canRedo()
    {
        return ! redoStack.empty();
    }

    bool canCopy()
    {
        if (getFocusedTextEditor() != nullptr)
            return true;

        if (mainView == MainView::overall)
            return isTopLevelStatePopulated (viewedTopLevelState);

        if (mainView == MainView::track)
            return getSelectedViewedLane() != nullptr;

        if (mainView == MainView::timeline && arrangementPlusMode)
            return hasImportedRegionSelection();

        return getSelectedViewedTrack() != nullptr;
    }

    bool canPaste()
    {
        if (getFocusedTextEditor() != nullptr)
            return true;

        if (objectClipboardKind == ObjectClipboardKind::lane)
        {
            const auto* track = getSelectedViewedTrack();
            return track != nullptr && static_cast<int> (track->lanes.size()) < maxTrackLanes;
        }

        if (objectClipboardKind == ObjectClipboardKind::track)
        {
            const auto* tracks = getViewedTracks();
            return tracks != nullptr && static_cast<int> (tracks->size()) < maxStateTracks;
        }

        if (objectClipboardKind == ObjectClipboardKind::topLevelState)
            return firstEmptyTopLevelState() >= 0 || ! isTopLevelStatePopulated (viewedTopLevelState);

        if (objectClipboardKind == ObjectClipboardKind::audioRegions)
            return arrangementPlusMode && ! copiedImportedRegions.empty();

        return false;
    }

    bool canDuplicate()
    {
        if (getFocusedTextEditor() != nullptr)
            return false;

        if (mainView == MainView::track)
        {
            const auto* track = getSelectedViewedTrack();
            return track != nullptr && static_cast<int> (track->lanes.size()) < maxTrackLanes;
        }

        if (mainView == MainView::overall)
            return firstEmptyTopLevelState() >= 0 && isTopLevelStatePopulated (viewedTopLevelState);

        if (mainView == MainView::timeline && arrangementPlusMode)
            return hasImportedRegionSelection();

        const auto* tracks = getViewedTracks();
        return tracks != nullptr && static_cast<int> (tracks->size()) < maxStateTracks;
    }

    void pushUndoSnapshot (const juce::String& label)
    {
        if (suppressProjectDirty || suppressProjectUndo)
            return;

        ProjectUndoSnapshot snapshot;
        snapshot.project = projectToVar();
        snapshot.projectWasDirty = projectDirty;
        snapshot.label = label;
        undoStack.push_back (std::move (snapshot));
        redoStack.clear();

        constexpr size_t maxUndoSnapshots = 64;
        while (undoStack.size() > maxUndoSnapshots)
            undoStack.erase (undoStack.begin());

        menuItemsChanged();
    }

    void clearUndoHistory()
    {
        undoStack.clear();
        redoStack.clear();
        activeUndoTextEditor = nullptr;
        menuItemsChanged();
    }

    void beginUndoSnapshotForTextEdit (juce::TextEditor& editor, const juce::String& label)
    {
        if (suppressEditCallbacks || suppressProjectUndo || suppressProjectDirty)
            return;

        if (activeUndoTextEditor == &editor)
            return;

        pushUndoSnapshot (label);
        activeUndoTextEditor = &editor;
    }

    void endUndoSnapshotForTextEdit (juce::TextEditor& editor)
    {
        if (activeUndoTextEditor == &editor)
            activeUndoTextEditor = nullptr;
    }

    void performUndo()
    {
        if (auto* editor = getFocusedTextEditor())
        {
            if (editor->undo())
            {
                syncLaneCodeDirtyAfterTextChange();
                return;
            }
        }

        if (undoStack.empty())
            return;

        auto snapshot = std::move (undoStack.back());
        undoStack.pop_back();

        ProjectUndoSnapshot redoSnapshot;
        redoSnapshot.project = projectToVar();
        redoSnapshot.projectWasDirty = projectDirty;
        redoSnapshot.label = snapshot.label;
        redoStack.push_back (std::move (redoSnapshot));

        juce::ScopedValueSetter<bool> undoGuard (suppressProjectUndo, true);
        const auto restoredDirty = snapshot.projectWasDirty;
        const auto restoredLabel = snapshot.label;

        if (applyProjectVar (snapshot.project))
        {
            updateProjectDirtyState (restoredDirty);
            statusLabel.setText (restoredLabel.isNotEmpty() ? "undid " + restoredLabel : "undo", juce::dontSendNotification);
            menuItemsChanged();
        }
    }

    void performRedo()
    {
        if (redoStack.empty())
            return;

        auto snapshot = std::move (redoStack.back());
        redoStack.pop_back();

        ProjectUndoSnapshot undoSnapshot;
        undoSnapshot.project = projectToVar();
        undoSnapshot.projectWasDirty = projectDirty;
        undoSnapshot.label = snapshot.label;
        undoStack.push_back (std::move (undoSnapshot));

        constexpr size_t maxUndoSnapshots = 64;
        while (undoStack.size() > maxUndoSnapshots)
            undoStack.erase (undoStack.begin());

        juce::ScopedValueSetter<bool> undoGuard (suppressProjectUndo, true);
        const auto restoredDirty = snapshot.projectWasDirty;
        const auto restoredLabel = snapshot.label;

        if (applyProjectVar (snapshot.project))
        {
            updateProjectDirtyState (restoredDirty);
            statusLabel.setText (restoredLabel.isNotEmpty() ? "redid " + restoredLabel : "redo", juce::dontSendNotification);
            menuItemsChanged();
        }
    }

    void syncLaneCodeDirtyAfterTextChange()
    {
        if (! laneCodeEditor.hasKeyboardFocus (true))
            return;

        const auto matchesLastValidText = laneCodeEditor.getText() == laneCodeLastValidatedText;
        laneCodeDirty = ! matchesLastValidText;
        laneCodeRunButton.setEnabled (laneCodeDirty);
        updateLaneCodeHeader (laneCodeDirty ? "lane code - edited" : "lane code",
                              laneCodeDirty ? amber().withAlpha (0.62f) : mutedInk().withAlpha (0.22f));
        refreshProjectDirtyIndicator();
    }

    void performCopy()
    {
        if (auto* editor = getFocusedTextEditor())
        {
            static_cast<void> (editor->copyToClipboard());
            return;
        }

        if (mainView == MainView::overall)
        {
            if (isTopLevelStatePopulated (viewedTopLevelState))
            {
                copiedTopLevelState = topLevelStates[static_cast<size_t> (viewedTopLevelState)];
                objectClipboardKind = ObjectClipboardKind::topLevelState;
                statusLabel.setText ("copied State " + juce::String (viewedTopLevelState + 1), juce::dontSendNotification);
                menuItemsChanged();
            }
            return;
        }

        if (mainView == MainView::track)
        {
            if (const auto* lane = getSelectedViewedLane())
            {
                copiedLane = *lane;
                objectClipboardKind = ObjectClipboardKind::lane;
                statusLabel.setText ("copied lane", juce::dontSendNotification);
                menuItemsChanged();
            }
            return;
        }

        if (mainView == MainView::timeline && arrangementPlusMode && hasImportedRegionSelection())
        {
            copySelectedImportedAudioRegions();
            return;
        }

        if (const auto* track = getSelectedViewedTrack())
        {
            copiedTrack = *track;
            objectClipboardKind = ObjectClipboardKind::track;
            statusLabel.setText ("copied track", juce::dontSendNotification);
            menuItemsChanged();
        }
    }

    void performPaste()
    {
        if (auto* editor = getFocusedTextEditor())
        {
            static_cast<void> (editor->pasteFromClipboard());
            syncLaneCodeDirtyAfterTextChange();
            return;
        }

        confirmPendingLaneCodeThen ([this]
        {
            pasteCopiedObject();
        });
    }

    void pasteCopiedObject()
    {
        if (objectClipboardKind == ObjectClipboardKind::lane && copiedLane.has_value())
        {
            auto* track = getSelectedViewedTrack();
            if (track == nullptr || static_cast<int> (track->lanes.size()) >= maxTrackLanes)
                return;

            pushUndoSnapshot ("paste lane");
            auto lane = *copiedLane;
            lane.name = lane.name + " copy";
            const auto insertIndex = juce::jlimit (0, static_cast<int> (track->lanes.size()), selectedLane + 1);
            track->lanes.insert (track->lanes.begin() + insertIndex, std::move (lane));
            selectedLane = insertIndex;
            refreshAfterStructureEdit (true);
            return;
        }

        if (objectClipboardKind == ObjectClipboardKind::track && copiedTrack.has_value())
        {
            pasteCopiedTrack();
            return;
        }

        if (objectClipboardKind == ObjectClipboardKind::topLevelState && copiedTopLevelState.has_value())
        {
            const auto target = isTopLevelStatePopulated (viewedTopLevelState) ? firstEmptyTopLevelState() : viewedTopLevelState;
            if (target < 0 || target >= maxTopLevelStates)
                return;

            pushUndoSnapshot ("paste state");
            topLevelStates[static_cast<size_t> (target)] = *copiedTopLevelState;
            viewedTopLevelState = target;
            selectedState = 0;
            selectedLane = 0;
            markProjectDirty();
            selectViewedTopLevelState (target);
        }

        if (objectClipboardKind == ObjectClipboardKind::audioRegions && ! copiedImportedRegions.empty())
            pasteCopiedImportedAudioRegions();
    }

    void pasteCopiedTrack()
    {
        auto* tracks = getViewedTracks();
        if (tracks == nullptr || static_cast<int> (tracks->size()) >= maxStateTracks)
            return;

        pushUndoSnapshot ("paste track");
        auto track = *copiedTrack;
        track.name = trackDisplayName (track.name + " copy");
        const auto insertIndex = juce::jlimit (0, static_cast<int> (tracks->size()), selectedState + 1);
        tracks->insert (tracks->begin() + insertIndex, std::move (track));
        selectedState = insertIndex;
        selectedLane = 0;
        refreshAfterStructureEdit (true);
    }

    void performDuplicate()
    {
        if (getFocusedTextEditor() != nullptr)
            return;

        confirmPendingLaneCodeThen ([this]
        {
            if (mainView == MainView::timeline && arrangementPlusMode)
                duplicateSelectedImportedAudioRegions();
            else if (mainView == MainView::track)
                duplicateSelectedLane();
            else if (mainView == MainView::overall)
                duplicateViewedTopLevelState();
            else
                duplicateSelectedTrack();
        });
    }

    void duplicateSelectedTrack()
    {
        const auto* track = getSelectedViewedTrack();
        auto* tracks = getViewedTracks();
        if (track == nullptr || tracks == nullptr || static_cast<int> (tracks->size()) >= maxStateTracks)
            return;

        pushUndoSnapshot ("duplicate track");
        auto copy = *track;
        copy.name = trackDisplayName (copy.name + " copy");
        const auto insertIndex = juce::jlimit (0, static_cast<int> (tracks->size()), selectedState + 1);
        tracks->insert (tracks->begin() + insertIndex, std::move (copy));
        selectedState = insertIndex;
        selectedLane = 0;
        refreshAfterStructureEdit (true);
    }

    bool hasUnsavedWork() const
    {
        return projectDirty || laneCodeDirty;
    }

    void refreshProjectDirtyIndicator()
    {
        const auto dirty = hasUnsavedWork();
        titleLabel.setText (dirty ? "ChucK-ME *" : "ChucK-ME", juce::dontSendNotification);

        if (auto* window = dynamic_cast<juce::DocumentWindow*> (getTopLevelComponent()))
            window->setName (dirty ? "ChucK-ME *" : "ChucK-ME");
    }

    void updateProjectDirtyState (bool dirty)
    {
        projectDirty = dirty;
        refreshProjectDirtyIndicator();
    }

    void markProjectDirty()
    {
        if (! suppressProjectDirty)
            updateProjectDirtyState (true);
    }

    void discardPendingLaneCodeEdit()
    {
        laneCodeDirty = false;
        laneCodeRunButton.setEnabled (false);
        updateLaneCodeHeader ("lane code", mutedInk().withAlpha (0.22f));
        refreshProjectDirtyIndicator();
        updateLaneCode();
    }

    void confirmPendingLaneCodeThen (std::function<void()> continueAction)
    {
        if (! laneCodeDirty)
        {
            if (continueAction != nullptr)
                continueAction();
            return;
        }

        juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                        .withIconType (juce::MessageBoxIconType::QuestionIcon)
                                        .withTitle ("Lane code not run")
                                        .withMessage ("Run this lane-code edit before continuing?")
                                        .withButton ("Run")
                                        .withButton ("Discard")
                                        .withButton ("Cancel")
                                        .withAssociatedComponent (this),
                                      [this, actionAfterLaneCode = std::move (continueAction)] (int result) mutable
                                      {
                                          if (result == 1)
                                          {
                                              applyLaneCodeEdit();
                                              if (! laneCodeDirty && actionAfterLaneCode != nullptr)
                                                  actionAfterLaneCode();
                                          }
                                          else if (result == 2)
                                          {
                                              discardPendingLaneCodeEdit();
                                              if (actionAfterLaneCode != nullptr)
                                                  actionAfterLaneCode();
                                          }
                                      });
    }

    void saveCurrentProjectThen (std::function<void (bool)> afterSave)
    {
        if (currentProjectFile == juce::File())
        {
            chooseProjectSaveFile (std::move (afterSave));
            return;
        }

        const auto saved = saveProjectToFile (currentProjectFile);
        if (afterSave != nullptr)
            afterSave (saved);
    }

    void confirmUnsavedChangesThen (std::function<void()> continueAction)
    {
        if (laneCodeDirty)
        {
            confirmPendingLaneCodeThen ([this, actionAfterLaneCode = std::move (continueAction)] () mutable
            {
                confirmUnsavedChangesThen (std::move (actionAfterLaneCode));
            });
            return;
        }

        if (! projectDirty)
        {
            if (continueAction != nullptr)
                continueAction();
            return;
        }

        juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                        .withIconType (juce::MessageBoxIconType::WarningIcon)
                                        .withTitle ("Unsaved changes")
                                        .withMessage ("Save this project before continuing?")
                                        .withButton ("Save")
                                        .withButton ("Discard")
                                        .withButton ("Cancel")
                                        .withAssociatedComponent (this),
                                      [this, actionAfterPrompt = std::move (continueAction)] (int result) mutable
                                      {
                                          if (result == 1)
                                          {
                                              saveCurrentProjectThen ([actionAfterSave = std::move (actionAfterPrompt)] (bool saved) mutable
                                              {
                                                  if (saved && actionAfterSave != nullptr)
                                                      actionAfterSave();
                                              });
                                          }
                                          else if (result == 2 && actionAfterPrompt != nullptr)
                                          {
                                              actionAfterPrompt();
                                          }
                                      });
    }

    void newProject()
    {
        resetProjectToEmptyProject();
        clearUndoHistory();
        updateProjectDirtyState (false);
        statusLabel.setText ("new empty project", juce::dontSendNotification);
    }

    void chooseProjectToLoad()
    {
        projectChooser = std::make_unique<juce::FileChooser> ("Load ChucK-ME Project",
                                                              juce::File::getSpecialLocation (juce::File::userDocumentsDirectory),
                                                              "*.chuckme;*.json");
        projectChooser->launchAsync (juce::FileBrowserComponent::openMode
                                        | juce::FileBrowserComponent::canSelectFiles,
                                      [this] (const juce::FileChooser& chooser)
                                      {
                                          const auto file = chooser.getResult();
                                          if (file == juce::File())
                                              return;

                                          loadProjectFromFile (file);
                                      });
    }

    void chooseProjectSaveFile (std::function<void (bool)> afterSave = {})
    {
        const auto defaultFile = (currentProjectFile == juce::File()
                                      ? juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                                            .getChildFile ("ChucK-ME project.chuckme")
                                      : currentProjectFile);
        projectChooser = std::make_unique<juce::FileChooser> ("Save ChucK-ME Project",
                                                              defaultFile,
                                                              "*.chuckme");
        projectChooser->launchAsync (juce::FileBrowserComponent::saveMode
                                        | juce::FileBrowserComponent::canSelectFiles
                                        | juce::FileBrowserComponent::warnAboutOverwriting,
                                      [this, saveCallback = std::move (afterSave)] (const juce::FileChooser& chooser) mutable
                                      {
                                          auto file = chooser.getResult();
                                          if (file == juce::File())
                                          {
                                              if (saveCallback != nullptr)
                                                  saveCallback (false);
                                              return;
                                          }

                                          const auto saved = saveProjectToFile (withProjectExtension (file));
                                          if (saveCallback != nullptr)
                                              saveCallback (saved);
                                      });
    }

    void saveProject()
    {
        if (laneCodeDirty)
        {
            confirmPendingLaneCodeThen ([this] { saveProject(); });
            return;
        }

        if (currentProjectFile == juce::File())
        {
            chooseProjectSaveFile();
            return;
        }

        saveProjectToFile (currentProjectFile);
    }

    void saveProjectAs()
    {
        confirmPendingLaneCodeThen ([this] { chooseProjectSaveFile(); });
    }

    static juce::File projectMediaDirectoryFor (const juce::File& projectFile)
    {
        return projectFile.getSiblingFile (projectFile.getFileNameWithoutExtension() + " Media");
    }

    static juce::String safeProjectMediaFileName (const juce::File& file, int index)
    {
        auto baseName = file.getFileNameWithoutExtension().replaceCharacter (' ', '_')
                                                   .retainCharacters ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-");
        if (baseName.isEmpty())
            baseName = "audio";

        auto extension = file.getFileExtension();
        if (extension.isEmpty())
            extension = ".wav";

        return juce::String (index + 1).paddedLeft ('0', 3) + "_" + baseName + extension;
    }

    int copyImportedAudioToProjectMedia (const juce::File& projectFile)
    {
        if (projectFile == juce::File() || importedLaneAudioClips.empty())
            return 0;

        const auto mediaDirectory = projectMediaDirectoryFor (projectFile);
        if (! mediaDirectory.createDirectory())
            return 0;

        auto copiedCount = 0;
        for (int clipIndex = 0; clipIndex < static_cast<int> (importedLaneAudioClips.size()); ++clipIndex)
        {
            auto& clip = importedLaneAudioClips[static_cast<size_t> (clipIndex)];
            if (! clip.file.existsAsFile())
                continue;

            if (clip.file.getParentDirectory() == mediaDirectory)
                continue;

            const auto target = mediaDirectory.getChildFile (safeProjectMediaFileName (clip.file, clipIndex));
            if (target.existsAsFile())
                target.deleteFile();

            if (clip.file.copyFileTo (target))
            {
                clip.file = target;
                ++copiedCount;
            }
        }

        return copiedCount;
    }

    bool saveProjectToFile (const juce::File& file)
    {
        const auto previousProjectFile = currentProjectFile;
        currentProjectFile = file;
        const auto copiedMediaCount = copyImportedAudioToProjectMedia (file);
        const auto projectText = juce::JSON::toString (projectToVar(), false);
        if (projectText.isEmpty() || ! file.replaceWithText (projectText, false, false, "\n"))
        {
            currentProjectFile = previousProjectFile;
            statusLabel.setText ("project save failed", juce::dontSendNotification);
            return false;
        }

        currentProjectFile = file;
        clearUndoHistory();
        updateProjectDirtyState (false);
        statusLabel.setText ("saved " + file.getFileName()
                                + (copiedMediaCount > 0 ? " with " + juce::String (copiedMediaCount) + " media files" : juce::String()),
                             juce::dontSendNotification);
        return true;
    }

    bool loadProjectFromFile (const juce::File& file)
    {
        if (! file.existsAsFile())
        {
            statusLabel.setText ("project file not found", juce::dontSendNotification);
            return false;
        }

        const auto parsed = juce::JSON::parse (file.loadFileAsString());
        loadingProjectFile = file;
        if (! parsed.isObject() || ! applyProjectVar (parsed))
        {
            loadingProjectFile = juce::File();
            statusLabel.setText ("project load failed", juce::dontSendNotification);
            return false;
        }
        loadingProjectFile = juce::File();

        currentProjectFile = file;
        updateProjectDirtyState (false);
        statusLabel.setText ("loaded " + file.getFileName()
                                + (missingImportedAudioOnLastLoad > 0
                                    ? " (" + juce::String (missingImportedAudioOnLastLoad) + " missing audio files skipped)"
                                    : juce::String()),
                             juce::dontSendNotification);
        return true;
    }

    struct RenderStemTarget
    {
        int stateIndex = 0;
        int trackIndex = 0;
        int laneIndex = 0;
    };

    static bool isLaneAudibleInTrack (const Wf::StateSpec& track, int laneIndex)
    {
        if (laneIndex < 0 || laneIndex >= static_cast<int> (track.lanes.size()))
            return false;

        const auto hasSoloLane = std::any_of (track.lanes.begin(), track.lanes.end(), [] (const Wf::LaneSpec& lane)
        {
            return lane.solo;
        });

        const auto& lane = track.lanes[static_cast<size_t> (laneIndex)];
        return ! lane.muted && (! hasSoloLane || lane.solo);
    }

    static juce::String safeRenderFileToken (juce::String text)
    {
        text = text.trim();
        if (text.isEmpty())
            text = "untitled";

        text = text.replaceCharacter (' ', '_');
        return text.retainCharacters ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-");
    }

    std::vector<RenderStemTarget> collectArrangementLaneStems (const std::vector<GlobalScriptStep>& steps) const
    {
        std::vector<RenderStemTarget> stems;

        auto containsStem = [&stems] (const RenderStemTarget& target)
        {
            return std::any_of (stems.begin(), stems.end(), [&target] (const RenderStemTarget& existing)
            {
                return existing.stateIndex == target.stateIndex
                    && existing.trackIndex == target.trackIndex
                    && existing.laneIndex == target.laneIndex;
            });
        };

        for (const auto& step : steps)
        {
            if (! isTopLevelStatePopulated (step.stateIndex))
                continue;

            const auto& tracks = *topLevelStates[static_cast<size_t> (step.stateIndex)];
            for (int trackIndex = 0; trackIndex < static_cast<int> (tracks.size()); ++trackIndex)
            {
                const auto& track = tracks[static_cast<size_t> (trackIndex)];
                for (int laneIndex = 0; laneIndex < static_cast<int> (track.lanes.size()); ++laneIndex)
                {
                    if (! isLaneAudibleInTrack (track, laneIndex))
                        continue;

                    RenderStemTarget target { step.stateIndex, trackIndex, laneIndex };
                    if (! containsStem (target))
                        stems.push_back (target);
                }
            }
        }

        return stems;
    }

    juce::String renderStemFileName (const RenderStemTarget& target) const
    {
        const auto& tracks = *topLevelStates[static_cast<size_t> (target.stateIndex)];
        const auto& track = tracks[static_cast<size_t> (target.trackIndex)];
        const auto& lane = track.lanes[static_cast<size_t> (target.laneIndex)];

        return "S" + juce::String (target.stateIndex + 1)
             + "_T" + juce::String (target.trackIndex + 1)
             + "_L" + juce::String (target.laneIndex + 1)
             + "_" + safeRenderFileToken (track.name)
             + "_" + safeRenderFileToken (lane.name)
             + ".wav";
    }

    enum class RenderResult
    {
        success,
        failed,
        silent
    };

    struct OfflineTrackMix
    {
        int stateIndex = -1;
        int trackIndex = -1;
        juce::AudioBuffer<float> buffer;
        std::array<std::unique_ptr<juce::AudioPluginInstance>, maxTrackEffectSlots> effects;
        juce::MidiBuffer midi;

        ~OfflineTrackMix()
        {
            releaseResources();
        }

        void releaseResources()
        {
            for (auto& effect : effects)
            {
                if (effect != nullptr)
                    effect->releaseResources();

                effect.reset();
            }
        }
    };

    OfflineTrackMix* findOfflineTrackMix (std::vector<std::unique_ptr<OfflineTrackMix>>& trackMixes,
                                          int stateIndex,
                                          int trackIndex) const
    {
        for (auto& mix : trackMixes)
            if (mix != nullptr && mix->stateIndex == stateIndex && mix->trackIndex == trackIndex)
                return mix.get();

        return nullptr;
    }

    std::unique_ptr<juce::PluginDescription> findPluginDescriptionForEffectSlot (const Wf::TrackEffectSlotSpec& spec)
    {
        if (spec.pluginIdentifier.isNotEmpty())
            if (auto description = knownPluginList.getTypeForIdentifierString (spec.pluginIdentifier))
                return description;

        if (spec.pluginFileOrIdentifier.isNotEmpty())
            if (auto description = knownPluginList.getTypeForFile (spec.pluginFileOrIdentifier))
                return description;

        for (auto* format : pluginFormatManager.getFormats())
        {
            if (format == nullptr)
                continue;

            juce::OwnedArray<juce::PluginDescription> descriptions;
            format->findAllTypesForFile (descriptions, spec.pluginFileOrIdentifier);

            for (auto* description : descriptions)
            {
                if (description == nullptr)
                    continue;

                const auto identifier = description->createIdentifierString();
                if (spec.pluginIdentifier.isEmpty() || identifier == spec.pluginIdentifier)
                    return std::make_unique<juce::PluginDescription> (*description);
            }
        }

        return {};
    }

    bool loadOfflineTrackEffects (OfflineTrackMix& mix,
                                  const Wf::StateSpec& track,
                                  double sampleRate,
                                  int blockSize,
                                  juce::String& error)
    {
        return loadOfflineEffectSlots (mix, track.effectSlots, sampleRate, blockSize, error);
    }

    bool loadOfflineEffectSlots (OfflineTrackMix& mix,
                                 const std::array<Wf::TrackEffectSlotSpec, maxTrackEffectSlots>& effectSlots,
                                 double sampleRate,
                                 int blockSize,
                                 juce::String& error)
    {
        for (int effectIndex = 0; effectIndex < maxTrackEffectSlots; ++effectIndex)
        {
            const auto& spec = effectSlots[static_cast<size_t> (effectIndex)];
            if (! spec.active || spec.pluginFileOrIdentifier.isEmpty())
                continue;

            auto description = findPluginDescriptionForEffectSlot (spec);
            if (description == nullptr)
            {
                error = spec.pluginName.isNotEmpty() ? spec.pluginName : spec.pluginFileOrIdentifier;
                return false;
            }

            juce::String pluginError;
            auto plugin = pluginFormatManager.createPluginInstance (*description, sampleRate, blockSize, pluginError);
            if (plugin == nullptr)
            {
                error = spec.pluginName.isNotEmpty() ? spec.pluginName : pluginError;
                return false;
            }

            plugin->setPlayConfigDetails (2, 2, sampleRate, blockSize);
            plugin->prepareToPlay (sampleRate, blockSize);
            plugin->reset();
            mix.effects[static_cast<size_t> (effectIndex)] = std::move (plugin);
        }

        return true;
    }

    static void processOfflineTrackEffects (OfflineTrackMix& mix, juce::AudioBuffer<float>& buffer)
    {
        mix.midi.clear();

        for (auto& effect : mix.effects)
        {
            if (effect == nullptr)
                continue;

            const auto inputs = effect->getTotalNumInputChannels();
            const auto outputs = effect->getTotalNumOutputChannels();
            if (inputs <= 0 || outputs <= 0)
                continue;

            effect->processBlock (buffer, mix.midi);
        }
    }

    bool buildOfflineTrackMixes (std::vector<std::unique_ptr<OfflineTrackMix>>& trackMixes,
                                 double sampleRate,
                                 int blockSize,
                                 juce::String& error)
    {
        for (const auto& clip : importedLaneAudioClips)
        {
            if (clip.audioData == nullptr || clip.audioData->getNumSamples() <= 0)
                continue;

            if (findOfflineTrackMix (trackMixes, clip.stateIndex, clip.trackIndex) != nullptr)
                continue;

            auto* track = getTrack (clip.stateIndex, clip.trackIndex);
            if (track == nullptr)
                continue;

            auto mix = std::make_unique<OfflineTrackMix>();
            mix->stateIndex = clip.stateIndex;
            mix->trackIndex = clip.trackIndex;
            mix->buffer.setSize (2, blockSize, false, false, true);
            mix->buffer.clear();

            if (! loadOfflineTrackEffects (*mix, *track, sampleRate, blockSize, error))
                return false;

            trackMixes.push_back (std::move (mix));
        }

        return ! trackMixes.empty();
    }

    RenderResult renderImportedArrangementToWav (const juce::File& destination)
    {
        if (importedLaneAudioClips.empty())
        {
            statusLabel.setText ("render WAV+ failed: no reimported audio", juce::dontSendNotification);
            return RenderResult::failed;
        }

        constexpr double renderSampleRate = 48000.0;
        constexpr int renderBlockSize = 512;
        constexpr int renderChannels = 2;
        const auto renderMasterGain = static_cast<float> (gainSlider.getValue());
        if (renderMasterGain <= 0.0001f)
        {
            statusLabel.setText ("render WAV+ failed: master volume is zero", juce::dontSendNotification);
            return RenderResult::failed;
        }

        auto totalSeconds = 0.0;
        for (auto& clip : importedLaneAudioClips)
        {
            if (clip.audioData == nullptr)
                populateImportedClipWaveform (clip);

            if (clip.audioData != nullptr)
            {
                sanitiseImportedClipRegions (clip);
                for (const auto& region : clip.regions)
                    totalSeconds = juce::jmax (totalSeconds, regionEndSeconds (region));
            }
        }

        if (totalSeconds <= 0.0)
        {
            statusLabel.setText ("render WAV+ failed: imported audio is empty", juce::dontSendNotification);
            return RenderResult::failed;
        }

        std::vector<std::unique_ptr<OfflineTrackMix>> trackMixes;
        juce::String effectError;
        if (! buildOfflineTrackMixes (trackMixes, renderSampleRate, renderBlockSize, effectError))
        {
            statusLabel.setText (effectError.isNotEmpty()
                                    ? "render WAV+ failed: could not load " + effectError
                                    : "render WAV+ failed: no valid imported tracks",
                                 juce::dontSendNotification);
            return RenderResult::failed;
        }

        OfflineTrackMix masterMix;
        masterMix.buffer.setSize (renderChannels, renderBlockSize, false, false, true);
        masterMix.buffer.clear();
        if (! loadOfflineEffectSlots (masterMix, masterEffectSlots, renderSampleRate, renderBlockSize, effectError))
        {
            statusLabel.setText (effectError.isNotEmpty()
                                    ? "render WAV+ failed: could not load master " + effectError
                                    : "render WAV+ failed: could not load master effects",
                                 juce::dontSendNotification);
            return RenderResult::failed;
        }

        juce::WavAudioFormat wavFormat;
        std::unique_ptr<juce::OutputStream> stream = destination.createOutputStream();
        if (stream == nullptr)
        {
            statusLabel.setText ("render WAV+ failed: could not open file", juce::dontSendNotification);
            return RenderResult::failed;
        }

        auto writer = wavFormat.createWriterFor (stream,
                                                 juce::AudioFormatWriterOptions {}
                                                    .withSampleRate (renderSampleRate)
                                                    .withNumChannels (renderChannels)
                                                    .withBitsPerSample (24));
        if (writer == nullptr)
        {
            statusLabel.setText ("render WAV+ failed: could not create WAV writer", juce::dontSendNotification);
            return RenderResult::failed;
        }

        juce::AudioBuffer<float> mixBuffer (renderChannels, renderBlockSize);
        const auto totalSamples = static_cast<int64_t> (std::ceil (totalSeconds * renderSampleRate));
        auto renderedSamples = static_cast<int64_t> (0);
        double renderedEnergy = 0.0;
        float renderedPeak = 0.0f;

        while (renderedSamples < totalSamples)
        {
            const auto blockSamples = static_cast<int> (juce::jmin<int64_t> (renderBlockSize, totalSamples - renderedSamples));
            mixBuffer.clear (0, blockSamples);
            for (auto& trackMix : trackMixes)
                if (trackMix != nullptr)
                    trackMix->buffer.clear (0, blockSamples);

            for (const auto& clip : importedLaneAudioClips)
            {
                if (clip.audioData == nullptr || clip.audioData->getNumSamples() <= 0 || clip.mixGain <= 0.000001f)
                    continue;

                auto* trackMix = findOfflineTrackMix (trackMixes, clip.stateIndex, clip.trackIndex);
                if (trackMix == nullptr)
                    continue;

                const auto panGains = linearPanGains (clip.mixGain, clip.mixPan);
                const auto blockStartSeconds = static_cast<double> (renderedSamples) / renderSampleRate;
                const auto blockEndSeconds = static_cast<double> (renderedSamples + blockSamples) / renderSampleRate;

                for (const auto& region : clip.regions)
                {
                    const auto regionStart = region.startSeconds;
                    const auto regionEnd = regionEndSeconds (region);
                    if (regionEnd <= blockStartSeconds || regionStart >= blockEndSeconds)
                        continue;

                    const auto firstSample = juce::jlimit (0,
                                                           blockSamples,
                                                           static_cast<int> (std::floor ((regionStart - blockStartSeconds) * renderSampleRate)));
                    const auto lastSample = juce::jlimit (0,
                                                          blockSamples,
                                                          static_cast<int> (std::ceil ((regionEnd - blockStartSeconds) * renderSampleRate)));

                    for (int sample = firstSample; sample < lastSample; ++sample)
                    {
                        const auto timelineSample = renderedSamples + sample;
                        const auto timeSeconds = static_cast<double> (timelineSample) / renderSampleRate;
                        if (! regionContainsTime (region, timeSeconds))
                            continue;

                        const auto localSeconds = timeSeconds - region.startSeconds;
                        const auto sourcePosition = sourceSamplePositionForRegionTime (region,
                                                                                       timelineSample,
                                                                                       renderSampleRate,
                                                                                       juce::jmax (1.0, clip.audioSampleRate));
                        if (sourcePosition < 0.0 || sourcePosition >= static_cast<double> (clip.audioData->getNumSamples()))
                            break;

                        const auto regionGain = importedRegionFadeGain (region, localSeconds);
                        const auto left = readInterpolatedSample (*clip.audioData, 0, sourcePosition);
                        const auto right = readInterpolatedSample (*clip.audioData, clip.audioData->getNumChannels() > 1 ? 1 : 0, sourcePosition);
                        trackMix->buffer.addSample (0, sample, left * panGains[0] * regionGain);
                        trackMix->buffer.addSample (1, sample, right * panGains[1] * regionGain);
                    }
                }
            }

            for (auto& trackMix : trackMixes)
            {
                if (trackMix == nullptr)
                    continue;

                juce::AudioBuffer<float> trackView (trackMix->buffer.getArrayOfWritePointers(),
                                                    renderChannels,
                                                    blockSamples);
                processOfflineTrackEffects (*trackMix, trackView);

                for (int channel = 0; channel < renderChannels; ++channel)
                    mixBuffer.addFrom (channel, 0, trackView, channel, 0, blockSamples);
            }

            juce::AudioBuffer<float> masterView (mixBuffer.getArrayOfWritePointers(),
                                                 renderChannels,
                                                 blockSamples);
            processOfflineTrackEffects (masterMix, masterView);
            mixBuffer.applyGain (0, blockSamples, renderMasterGain);

            for (int channel = 0; channel < renderChannels; ++channel)
            {
                const auto* samples = mixBuffer.getReadPointer (channel);
                for (int sample = 0; sample < blockSamples; ++sample)
                {
                    const auto value = samples[sample];
                    renderedPeak = juce::jmax (renderedPeak, std::abs (value));
                    renderedEnergy += static_cast<double> (value) * static_cast<double> (value);
                }
            }

            if (! writer->writeFromAudioSampleBuffer (mixBuffer, 0, blockSamples))
            {
                statusLabel.setText ("render WAV+ failed: could not write WAV", juce::dontSendNotification);
                return RenderResult::failed;
            }

            renderedSamples += blockSamples;
        }

        writer.reset();
        if (renderedPeak <= 0.000001f || renderedEnergy <= 0.0000001)
        {
            destination.deleteFile();
            statusLabel.setText ("render WAV+ failed: silent output", juce::dontSendNotification);
            return RenderResult::silent;
        }

        statusLabel.setText ("rendered WAV+: " + destination.getFileName(), juce::dontSendNotification);
        return RenderResult::success;
    }

    void renderArrangementLanesToDirectory (const juce::File& directory, bool importAfterRender)
    {
        const auto steps = parseGlobalScript();
        if (steps.empty())
        {
            statusLabel.setText ("lane render failed: no playable states", juce::dontSendNotification);
            return;
        }

        const auto stems = collectArrangementLaneStems (steps);
        if (stems.empty())
        {
            statusLabel.setText ("lane render failed: no audible lanes", juce::dontSendNotification);
            return;
        }

        if (! directory.createDirectory())
        {
            statusLabel.setText ("lane render failed: could not create folder", juce::dontSendNotification);
            return;
        }

        int renderedCount = 0;
        int skippedSilentCount = 0;
        std::vector<ImportedLaneAudioClip> renderedClips;
        for (const auto& stem : stems)
        {
            const auto file = directory.getChildFile (renderStemFileName (stem));
            const auto result = renderArrangementToWav (file, stem);
            if (result == RenderResult::success)
            {
                ++renderedCount;
                ImportedLaneAudioClip clip;
                clip.stateIndex = stem.stateIndex;
                clip.trackIndex = stem.trackIndex;
                clip.laneIndex = stem.laneIndex;
                clip.file = file;
                renderedClips.push_back (std::move (clip));
            }
            else if (result == RenderResult::silent)
                ++skippedSilentCount;
            else
                return;
        }

        const auto renderMessage = "rendered " + juce::String (renderedCount) + " lane WAVs"
                                 + (skippedSilentCount > 0 ? " (" + juce::String (skippedSilentCount) + " silent skipped)" : juce::String());
        statusLabel.setText (renderMessage, juce::dontSendNotification);

        if (importAfterRender && ! renderedClips.empty())
            importRenderedLaneFiles (std::move (renderedClips));
    }

    void populateImportedClipWaveform (ImportedLaneAudioClip& clip)
    {
        clip.audioData.reset();
        clip.waveformPeaks.clear();
        clip.lengthSeconds = 0.0;
        clip.audioSampleRate = 48000.0;

        juce::AudioFormatManager formatManager;
        formatManager.registerBasicFormats();

        std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (clip.file));
        if (reader == nullptr || reader->lengthInSamples <= 0 || reader->numChannels == 0)
            return;

        const auto samplesToLoad = static_cast<int> (juce::jmin<juce::int64> (reader->lengthInSamples,
                                                                              static_cast<juce::int64> (std::numeric_limits<int>::max())));
        const auto channelsToRead = static_cast<int> (juce::jmin<juce::uint32> (reader->numChannels, 2));
        auto audio = std::make_shared<juce::AudioBuffer<float>> (channelsToRead, samplesToLoad);
        audio->clear();
        if (! reader->read (audio.get(), 0, samplesToLoad, 0, true, true))
            return;

        clip.audioData = audio;
        clip.audioSampleRate = juce::jmax (1.0, reader->sampleRate);
        clip.lengthSeconds = static_cast<double> (samplesToLoad) / clip.audioSampleRate;
        sanitiseImportedClipRegions (clip);

        constexpr int previewBins = 512;
        clip.waveformPeaks.assign (previewBins, 0.0f);

        const auto samplesPerBin = juce::jmax (1, samplesToLoad / previewBins);
        auto previewPeak = 0.0f;

        for (int bin = 0; bin < previewBins; ++bin)
        {
            const auto binStart = bin * samplesPerBin;
            if (binStart >= samplesToLoad)
                break;

            const auto binSamples = juce::jmin (samplesPerBin, samplesToLoad - binStart);
            auto peak = 0.0f;

            for (int channel = 0; channel < audio->getNumChannels(); ++channel)
                peak = juce::jmax (peak, audio->getMagnitude (channel, binStart, binSamples));

            peak = juce::jlimit (0.0f, 1.0f, peak);
            previewPeak = juce::jmax (previewPeak, peak);
            clip.waveformPeaks[static_cast<size_t> (bin)] = peak;
        }

        if (previewPeak > 0.000001f)
            for (auto& peak : clip.waveformPeaks)
                peak = juce::jlimit (0.0f, 1.0f, peak / previewPeak);
    }

    void importRenderedLaneFiles (std::vector<ImportedLaneAudioClip> renderedClips, bool markDirty = true)
    {
        for (auto& clip : renderedClips)
            populateImportedClipWaveform (clip);

        renderedClips.erase (std::remove_if (renderedClips.begin(), renderedClips.end(), [] (const ImportedLaneAudioClip& clip)
        {
            return clip.audioData == nullptr || clip.audioData->getNumSamples() <= 0;
        }), renderedClips.end());

        if (renderedClips.empty())
        {
            statusLabel.setText ("Arrangement+ import failed: no readable audio files", juce::dontSendNotification);
            return;
        }

        if (markDirty)
            pushUndoSnapshot ("import rendered audio");

        scriptRunning = false;
        running = false;
        audioCallback.clearImportedAudioPlayback();
        importedLaneAudioClips = std::move (renderedClips);
        arrangementPlusMode = true;
        selectedImportedRegion = importedLaneAudioClips.empty() || importedLaneAudioClips.front().regions.empty()
            ? ArrangementAudioSelection {}
            : ArrangementAudioSelection { 0, 0 };
        selectedImportedRegions = selectedImportedRegion.isValid() ? std::vector<ArrangementAudioSelection> { selectedImportedRegion }
                                                                   : std::vector<ArrangementAudioSelection> {};
        arrangementEditPlayheadSeconds = 0.0;
        arrangementAudioTool = ArrangementAudioTool::pointer;
        importedFadeDragUndoStarted = false;
        importedTrimDragUndoStarted = false;
        importedGainDragUndoStarted = false;
        if (! audioCallback.loadImportedAudioClips (importedLaneAudioClips, &topLevelStates, true, &masterEffectSlots))
        {
            importedLaneAudioClips.clear();
            arrangementPlusMode = false;
            statusLabel.setText ("Arrangement+ import failed: could not prepare audio playback", juce::dontSendNotification);
            setMainView (MainView::timeline);
            return;
        }

        setMainView (MainView::timeline);
        if (markDirty)
            markProjectDirty();
        statusLabel.setText ("Arrangement+ imported " + juce::String (importedLaneAudioClips.size()) + " audio files", juce::dontSendNotification);
    }

    void removeRenderedAudioAndReturnToCodePlayback()
    {
        const auto removedCount = static_cast<int> (importedLaneAudioClips.size());
        if (removedCount <= 0)
            return;

        pushUndoSnapshot ("remove rendered audio");
        importedLaneAudioClips.clear();
        arrangementPlusMode = false;
        selectedImportedRegion = {};
        selectedImportedRegions.clear();
        arrangementEditPlayheadSeconds = 0.0;
        arrangementAudioTool = ArrangementAudioTool::pointer;
        importedFadeDragUndoStarted = false;
        importedTrimDragUndoStarted = false;
        importedGainDragUndoStarted = false;
        scriptRunning = false;
        running = false;
        audioCallback.clearImportedAudioPlayback();
        syncTransportButtons();
        setMainView (MainView::timeline);
        markProjectDirty();
        statusLabel.setText ("removed " + juce::String (removedCount) + " rendered audio files; state/code playback restored",
                             juce::dontSendNotification);
    }

    RenderResult renderArrangementToWav (const juce::File& destination, std::optional<RenderStemTarget> stemTarget = {})
    {
        const auto steps = parseGlobalScript();
        if (steps.empty())
        {
            statusLabel.setText ("render failed: no playable states in state code", juce::dontSendNotification);
            return RenderResult::failed;
        }

        juce::ScopedNoDenormals noDenormals;
        constexpr double renderSampleRate = 48000.0;
        constexpr int renderBlockSize = 512;
        constexpr int renderChannels = 2;
        constexpr double transitionFadeSeconds = 0.42;
        constexpr double finalTailSeconds = 0.70;

        struct RenderSlot
        {
            EmbeddedChucKEngine engine;
            juce::AudioBuffer<float> output;
            std::array<int, 5> parameterIndexes { -1, -1, -1, -1, -1 };
            bool inUse = false;
            float gain = 0.0f;
            float targetGain = 0.0f;
        };

        std::array<RenderSlot, 2> slots;
        for (auto& slot : slots)
        {
            slot.output.setSize (renderChannels, renderBlockSize, false, false, true);
            if (! slot.engine.prepare (renderSampleRate, renderBlockSize, 0, renderChannels))
            {
                statusLabel.setText ("render failed: could not prepare ChucK", juce::dontSendNotification);
                return RenderResult::failed;
            }
        }

        juce::WavAudioFormat wavFormat;
        std::unique_ptr<juce::OutputStream> stream = destination.createOutputStream();
        if (stream == nullptr)
        {
            statusLabel.setText ("render failed: could not open file", juce::dontSendNotification);
            return RenderResult::failed;
        }

        auto writer = wavFormat.createWriterFor (stream,
                                                 juce::AudioFormatWriterOptions {}
                                                    .withSampleRate (renderSampleRate)
                                                    .withNumChannels (renderChannels)
                                                    .withBitsPerSample (24));
        if (writer == nullptr)
        {
            statusLabel.setText ("render failed: could not create WAV writer", juce::dontSendNotification);
            return RenderResult::failed;
        }

        juce::AudioBuffer<float> emptyInput (0, renderBlockSize);
        juce::AudioBuffer<float> mixBuffer (renderChannels, renderBlockSize);
        juce::AudioBuffer<float> writeBuffer (renderChannels, renderBlockSize);
        const auto fadeStepPerSample = static_cast<float> (1.0 / (renderSampleRate * transitionFadeSeconds));
        const auto renderMasterGain = static_cast<float> (gainSlider.getValue());
        if (renderMasterGain <= 0.0001f)
        {
            statusLabel.setText ("render failed: master volume is zero", juce::dontSendNotification);
            return RenderResult::failed;
        }

        double renderedEnergy = 0.0;
        float renderedPeak = 0.0f;
        int activeSlot = -1;
        int nextSlot = 0;
        std::mt19937 renderRandom { 0x51a7e5u };

        auto refreshParameterIndexes = [] (RenderSlot& slot)
        {
            const std::array<juce::String, 5> names
            {
                "hostMasterGain",
                "hostTempoHz",
                "hostIntensity",
                "hostBrightness",
                "hostOrbitPhase"
            };

            for (int i = 0; i < static_cast<int> (names.size()); ++i)
            {
                slot.parameterIndexes[static_cast<size_t> (i)] = slot.engine.getParameterIndex (names[static_cast<size_t> (i)]);
                if (slot.parameterIndexes[static_cast<size_t> (i)] < 0)
                    return false;
            }

            return true;
        };

        auto setSlotControls = [] (RenderSlot& slot, float masterGain, float tempoHz, float renderOrbitPhase)
        {
            static_cast<void> (slot.engine.setParameterValue (slot.parameterIndexes[0], masterGain));
            static_cast<void> (slot.engine.setParameterValue (slot.parameterIndexes[1], tempoHz));
            static_cast<void> (slot.engine.setParameterValue (slot.parameterIndexes[2], defaultIntensity));
            static_cast<void> (slot.engine.setParameterValue (slot.parameterIndexes[3], defaultBrightness));
            static_cast<void> (slot.engine.setParameterValue (slot.parameterIndexes[4], renderOrbitPhase));
        };

        auto stepTempoBpm = [this] (const GlobalScriptStep& step)
        {
            if (step.tempoBpm.has_value())
                return *step.tempoBpm;

            return topLevelTemposBpm[static_cast<size_t> (juce::jlimit (0, maxTopLevelStates - 1, step.stateIndex))];
        };

        auto stepBeatsPerBar = [this] (const GlobalScriptStep& step)
        {
            const auto index = static_cast<size_t> (juce::jlimit (0, maxTopLevelStates - 1, step.stateIndex));
            return static_cast<double> (step.timeSigNumerator.value_or (topLevelTimeSigNumerators[index]));
        };

        auto stepQuarterNotesPerBar = [this] (const GlobalScriptStep& step)
        {
            const auto index = static_cast<size_t> (juce::jlimit (0, maxTopLevelStates - 1, step.stateIndex));
            const auto numerator = static_cast<double> (step.timeSigNumerator.value_or (topLevelTimeSigNumerators[index]));
            const auto denominator = static_cast<double> (juce::jmax (1, step.timeSigDenominator.value_or (topLevelTimeSigDenominators[index])));
            return numerator * 4.0 / denominator;
        };

        auto trackDurationBars = [] (const Wf::StateSpec& track, double beatsPerBar) -> std::optional<double>
        {
            if (! track.duration.has_value())
                return {};

            const auto duration = *track.duration;
            const auto totalBars = static_cast<double> (duration.bars)
                                 + (static_cast<double> (duration.beats) / juce::jmax (1.0, beatsPerBar));
            if (totalBars <= 0.0)
                return {};

            return juce::jmax (0.25, totalBars);
        };

        auto shouldTransition = [&renderRandom] (int probabilityPercent)
        {
            if (probabilityPercent <= 0)
                return false;
            if (probabilityPercent >= 100)
                return true;

            std::uniform_int_distribution<int> distribution (1, 100);
            return distribution (renderRandom) <= probabilityPercent;
        };

        auto loadTrackIntoNextSlot = [&] (const Wf::StateSpec& track,
                                          int trackIndexForRender,
                                          const GlobalScriptStep& step,
                                          int beatsPerBar,
                                          double quarterNotesPerBar,
                                          float renderOrbitPhase)
        {
            auto audioTrack = track;
            if (stemTarget.has_value())
            {
                for (int laneIndex = 0; laneIndex < static_cast<int> (audioTrack.lanes.size()); ++laneIndex)
                {
                    auto& lane = audioTrack.lanes[static_cast<size_t> (laneIndex)];
                    const auto keepLane = step.stateIndex == stemTarget->stateIndex
                                       && trackIndexForRender == stemTarget->trackIndex
                                       && laneIndex == stemTarget->laneIndex;

                    if (! keepLane)
                    {
                        lane.volume = 0.0f;
                        lane.muted = true;
                        lane.solo = false;
                    }
                }
            }

            audioTrack.clockBeatsPerBar = beatsPerBar;
            audioTrack.clockQuarterNotesPerBar = quarterNotesPerBar;

            auto& incoming = slots[static_cast<size_t> (nextSlot)];
            incoming.inUse = false;
            incoming.gain = activeSlot < 0 ? 1.0f : 0.0f;
            incoming.targetGain = 1.0f;

            if (! incoming.engine.loadProgram (Wf::buildStateProgram (audioTrack), Wf::makeWfParameterBindings()))
                return false;

            if (! refreshParameterIndexes (incoming))
                return false;

            setSlotControls (incoming, renderMasterGain, stepTempoBpm (step) / 60.0f, renderOrbitPhase);
            incoming.inUse = true;

            if (activeSlot >= 0)
                slots[static_cast<size_t> (activeSlot)].targetGain = 0.0f;

            activeSlot = nextSlot;
            nextSlot = nextSlot == 0 ? 1 : 0;
            return true;
        };

        auto renderSamples = [&] (int samples, float tempoHz, float renderOrbitPhase) -> bool
        {
            auto samplesRemaining = samples;

            while (samplesRemaining > 0)
            {
                const auto blockSamples = juce::jmin (renderBlockSize, samplesRemaining);
                mixBuffer.clear (0, blockSamples);

                for (auto& slot : slots)
                {
                    if (! slot.inUse || ! slot.engine.isReady())
                        continue;

                    setSlotControls (slot, renderMasterGain, tempoHz, renderOrbitPhase);
                    slot.output.clear (0, blockSamples);
                    juce::AudioBuffer<float> slotView (slot.output.getArrayOfWritePointers(), renderChannels, blockSamples);
                    slot.engine.process (emptyInput, slotView);

                    for (int sample = 0; sample < blockSamples; ++sample)
                    {
                        const auto startGain = slot.gain;
                        if (slot.targetGain > slot.gain)
                            slot.gain = juce::jmin (slot.targetGain, slot.gain + fadeStepPerSample);
                        else if (slot.targetGain < slot.gain)
                            slot.gain = juce::jmax (slot.targetGain, slot.gain - fadeStepPerSample);

                        const auto sampleGain = 0.5f * (startGain + slot.gain);
                        for (int channel = 0; channel < renderChannels; ++channel)
                            mixBuffer.addSample (channel, sample, slot.output.getSample (channel, sample) * sampleGain);
                    }

                    if (slot.targetGain <= 0.0f && slot.gain <= 0.0001f && (&slot - slots.data()) != activeSlot)
                        slot.inUse = false;
                }

                writeBuffer.makeCopyOf (mixBuffer, true);
                for (int channel = 0; channel < renderChannels; ++channel)
                {
                    const auto* samplesToMeasure = mixBuffer.getReadPointer (channel);
                    for (int sample = 0; sample < blockSamples; ++sample)
                    {
                        const auto value = samplesToMeasure[sample];
                        renderedPeak = juce::jmax (renderedPeak, std::abs (value));
                        renderedEnergy += static_cast<double> (value) * static_cast<double> (value);
                    }
                }

                if (! writer->writeFromAudioSampleBuffer (writeBuffer, 0, blockSamples))
                    return false;

                samplesRemaining -= blockSamples;
            }

            return true;
        };

        for (const auto& step : steps)
        {
            if (! isTopLevelStatePopulated (step.stateIndex))
                continue;

            const auto& tracks = *topLevelStates[static_cast<size_t> (step.stateIndex)];
            if (tracks.empty())
                continue;

            const auto tempoBpm = stepTempoBpm (step);
            const auto quarterNotesPerBar = stepQuarterNotesPerBar (step);
            const auto beatsPerBar = static_cast<int> (juce::jlimit (1.0, 16.0, stepBeatsPerBar (step)));
            const auto barDurationSeconds = quarterNotesPerBar * 60.0 / juce::jmax (1.0f, tempoBpm);
            const auto totalStepSamples = static_cast<int64_t> (std::ceil (step.bars * barDurationSeconds * renderSampleRate));
            auto renderedStepSamples = static_cast<int64_t> (0);
            auto trackIndex = 0;
            auto renderTrackElapsedBars = 0.0;
            auto renderNextBarTransitionCheck = 1.0;
            auto orbitPhaseForRender = 0.0f;

            if (! loadTrackIntoNextSlot (tracks[static_cast<size_t> (trackIndex)], trackIndex, step, beatsPerBar, quarterNotesPerBar, orbitPhaseForRender))
            {
                statusLabel.setText ("render failed: could not load ChucK code", juce::dontSendNotification);
                return RenderResult::failed;
            }

            while (renderedStepSamples < totalStepSamples)
            {
                const auto blockSamples = static_cast<int> (juce::jmin<int64_t> (renderBlockSize, totalStepSamples - renderedStepSamples));
                if (! renderSamples (blockSamples, tempoBpm / 60.0f, orbitPhaseForRender))
                {
                    statusLabel.setText ("render failed: could not write WAV", juce::dontSendNotification);
                    return RenderResult::failed;
                }

                renderedStepSamples += blockSamples;
                renderTrackElapsedBars += (static_cast<double> (blockSamples) / renderSampleRate) * (tempoBpm / 60.0) / quarterNotesPerBar;

                const auto& currentTrack = tracks[static_cast<size_t> (trackIndex)];
                auto shouldAdvance = false;
                if (currentTrack.transitionProbabilityPercent.has_value())
                {
                    while (renderTrackElapsedBars >= renderNextBarTransitionCheck && ! shouldAdvance)
                    {
                        shouldAdvance = shouldTransition (*currentTrack.transitionProbabilityPercent);
                        if (! shouldAdvance)
                            renderNextBarTransitionCheck += 1.0;
                    }

                    orbitPhaseForRender = static_cast<float> (std::fmod (renderTrackElapsedBars, 1.0));
                }
                else if (const auto duration = trackDurationBars (currentTrack, static_cast<double> (beatsPerBar)))
                {
                    orbitPhaseForRender = static_cast<float> (juce::jlimit (0.0, 1.0, renderTrackElapsedBars / *duration));
                    shouldAdvance = renderTrackElapsedBars >= *duration;
                }
                else
                {
                    orbitPhaseForRender = static_cast<float> (std::fmod (renderTrackElapsedBars, 1.0));
                }

                if (shouldAdvance && ! tracks.empty())
                {
                    trackIndex = (trackIndex + 1) % static_cast<int> (tracks.size());
                    renderTrackElapsedBars = 0.0;
                    renderNextBarTransitionCheck = 1.0;
                    orbitPhaseForRender = 0.0f;

                    if (! loadTrackIntoNextSlot (tracks[static_cast<size_t> (trackIndex)], trackIndex, step, beatsPerBar, quarterNotesPerBar, orbitPhaseForRender))
                    {
                        statusLabel.setText ("render failed: could not transition track", juce::dontSendNotification);
                        return RenderResult::failed;
                    }
                }
            }
        }

        for (auto& slot : slots)
            slot.targetGain = 0.0f;

        if (! renderSamples (static_cast<int> (std::ceil (finalTailSeconds * renderSampleRate)),
                            topLevelTemposBpm[static_cast<size_t> (juce::jlimit (0, maxTopLevelStates - 1, performingTopLevelState))] / 60.0f,
                            0.0f))
        {
            statusLabel.setText ("render failed: could not write final tail", juce::dontSendNotification);
            return RenderResult::failed;
        }

        writer.reset();
        if (renderedPeak <= 0.000001f || renderedEnergy <= 0.0000001)
        {
            destination.deleteFile();
            if (! stemTarget.has_value())
                statusLabel.setText ("render failed: silent output, check volume/mutes", juce::dontSendNotification);
            return RenderResult::silent;
        }

        if (! stemTarget.has_value())
            statusLabel.setText ("rendered WAV: " + destination.getFileName(), juce::dontSendNotification);

        return RenderResult::success;
    }

    void scrollMixerToPlayingChannels()
    {
        if (! running || mixerCanvas.isDraggingFader())
            return;

        const auto playingRange = mixerCanvas.getPlayingChannelRange();
        if (! playingRange.has_value())
            return;

        const auto viewX = mixerViewport.getViewPositionX();
        const auto viewY = mixerViewport.getViewPositionY();
        const auto viewWidth = mixerViewport.getViewWidth();
        const auto padding = 24;
        auto targetX = viewX;

        if (playingRange->getStart() < viewX + padding)
            targetX = juce::jmax (0, playingRange->getStart() - padding);
        else if (playingRange->getEnd() > viewX + viewWidth - padding)
            targetX = juce::jmax (0, playingRange->getEnd() - viewWidth + padding);

        if (targetX != viewX)
            mixerViewport.setViewPosition (targetX, viewY);
    }

    void selectMixerChannel (int stateIndex, int trackIndex, int laneIndex)
    {
        if (! isTopLevelStatePopulated (stateIndex))
            return;

        viewedTopLevelState = juce::jlimit (0, maxTopLevelStates - 1, stateIndex);
        selectedState = juce::jlimit (0, getViewedTrackCount() - 1, trackIndex);

        if (const auto* viewedTracks = getViewedTracks())
        {
            const auto& track = (*viewedTracks)[static_cast<size_t> (selectedState)];
            selectedLane = juce::jlimit (0, juce::jmax (0, static_cast<int> (track.lanes.size()) - 1), laneIndex);
        }

        refreshLabels();
    }

    void applyMixerLaneVolumeChange (int stateIndex, int trackIndex, int laneIndex, float)
    {
        selectMixerChannel (stateIndex, trackIndex, laneIndex);
        markProjectDirty();

        if (arrangementPlusMode)
        {
            syncImportedPlaybackMix();
            return;
        }

        if (stateIndex == performingTopLevelState && trackIndex == performingTrackIndex)
            loadSelectedContentForCurrentState();
    }

    void applyMixerLanePanChange (int stateIndex, int trackIndex, int laneIndex, float)
    {
        selectMixerChannel (stateIndex, trackIndex, laneIndex);
        markProjectDirty();

        if (arrangementPlusMode)
        {
            syncImportedPlaybackMix();
            return;
        }

        if (stateIndex == performingTopLevelState && trackIndex == performingTrackIndex)
            loadSelectedContentForCurrentState();
    }

    void applyMixerMasterVolumeChange (float volume)
    {
        gainSlider.setValue (juce::jlimit (0.0f, 0.8f, volume), juce::dontSendNotification);
        markProjectDirty();
        applyCurrentAudioControls();
    }

    void syncImportedPlaybackMix()
    {
        for (int clipIndex = 0; clipIndex < static_cast<int> (importedLaneAudioClips.size()); ++clipIndex)
        {
            const auto& clip = importedLaneAudioClips[static_cast<size_t> (clipIndex)];
            audioCallback.setImportedClipMix (clipIndex, clip.mixGain, clip.mixPan);
        }
    }

    static void sanitiseImportedClipRegions (ImportedLaneAudioClip& clip)
    {
        if (clip.lengthSeconds <= 0.0 && clip.audioData != nullptr && clip.audioData->getNumSamples() > 0)
            clip.lengthSeconds = static_cast<double> (clip.audioData->getNumSamples()) / juce::jmax (1.0, clip.audioSampleRate);

        if (clip.lengthSeconds <= 0.0)
        {
            clip.regions.clear();
            return;
        }

        if (clip.regions.empty())
            clip.regions.push_back ({ 0.0, 0.0, clip.lengthSeconds, 0.0, 0.0 });

        std::vector<ImportedAudioRegion> regions;
        regions.reserve (clip.regions.size());
        for (auto region : clip.regions)
        {
            if (! std::isfinite (region.startSeconds)
                || ! std::isfinite (region.sourceStartSeconds)
                || ! std::isfinite (region.lengthSeconds))
                continue;

            region.startSeconds = juce::jmax (0.0, region.startSeconds);
            region.sourceStartSeconds = juce::jlimit (0.0, clip.lengthSeconds, region.sourceStartSeconds);
            region.lengthSeconds = juce::jlimit (0.0, clip.lengthSeconds - region.sourceStartSeconds, region.lengthSeconds);
            if (region.lengthSeconds <= 0.000001)
                continue;

            region.fadeInSeconds = juce::jlimit (0.0, region.lengthSeconds, std::isfinite (region.fadeInSeconds) ? region.fadeInSeconds : 0.0);
            region.fadeOutSeconds = juce::jlimit (0.0, region.lengthSeconds - region.fadeInSeconds, std::isfinite (region.fadeOutSeconds) ? region.fadeOutSeconds : 0.0);
            region.gain = juce::jlimit (0.0f, 2.0f, std::isfinite (region.gain) ? region.gain : 1.0f);
            region.fadeInCurve = juce::jlimit (0, 3, region.fadeInCurve);
            region.fadeOutCurve = juce::jlimit (0, 3, region.fadeOutCurve);
            regions.push_back (region);
        }

        std::sort (regions.begin(), regions.end(), [] (const auto& a, const auto& b)
        {
            return a.startSeconds < b.startSeconds;
        });
        applyAutomaticCrossfades (regions);
        clip.regions = std::move (regions);
    }

    bool importedRegionExists (ArrangementAudioSelection selection) const
    {
        return selection.clipIndex >= 0
            && selection.clipIndex < static_cast<int> (importedLaneAudioClips.size())
            && selection.regionIndex >= 0
            && selection.regionIndex < static_cast<int> (importedLaneAudioClips[static_cast<size_t> (selection.clipIndex)].regions.size());
    }

    static bool sameImportedRegionSelection (ArrangementAudioSelection a, ArrangementAudioSelection b)
    {
        return a.clipIndex == b.clipIndex && a.regionIndex == b.regionIndex;
    }

    void pruneImportedRegionSelection()
    {
        selectedImportedRegions.erase (std::remove_if (selectedImportedRegions.begin(), selectedImportedRegions.end(), [this] (const auto& selection)
        {
            return ! importedRegionExists (selection);
        }), selectedImportedRegions.end());

        std::sort (selectedImportedRegions.begin(), selectedImportedRegions.end(), [] (const auto& a, const auto& b)
        {
            if (a.clipIndex != b.clipIndex)
                return a.clipIndex < b.clipIndex;
            return a.regionIndex < b.regionIndex;
        });

        selectedImportedRegions.erase (std::unique (selectedImportedRegions.begin(), selectedImportedRegions.end(), sameImportedRegionSelection),
                                       selectedImportedRegions.end());

        if (! importedRegionExists (selectedImportedRegion))
            selectedImportedRegion = selectedImportedRegions.empty() ? ArrangementAudioSelection {} : selectedImportedRegions.front();

        if (importedRegionExists (selectedImportedRegion)
            && std::find (selectedImportedRegions.begin(), selectedImportedRegions.end(), selectedImportedRegion) == selectedImportedRegions.end())
            selectedImportedRegions.push_back (selectedImportedRegion);
    }

    bool hasImportedRegionSelection()
    {
        pruneImportedRegionSelection();
        return ! selectedImportedRegions.empty();
    }

    std::vector<ArrangementAudioSelection> importedRegionSelectionForEdit()
    {
        pruneImportedRegionSelection();
        if (! selectedImportedRegions.empty())
            return selectedImportedRegions;

        if (importedRegionExists (selectedImportedRegion))
            return { selectedImportedRegion };

        return {};
    }

    void setArrangementAudioTool (ArrangementAudioTool tool)
    {
        arrangementAudioTool = tool;
        syncViewButtons();
        syncArrangementTimelineView();
    }

    void selectImportedAudioRegion (int clipIndex, int regionIndex, bool toggle)
    {
        ArrangementAudioSelection selection { clipIndex, regionIndex };
        if (! importedRegionExists (selection))
        {
            selectedImportedRegion = {};
            selectedImportedRegions.clear();
        }
        else if (toggle)
        {
            const auto existing = std::find (selectedImportedRegions.begin(), selectedImportedRegions.end(), selection);
            if (existing != selectedImportedRegions.end())
            {
                selectedImportedRegions.erase (existing);
                selectedImportedRegion = selectedImportedRegions.empty() ? ArrangementAudioSelection {} : selectedImportedRegions.back();
            }
            else
            {
                selectedImportedRegions.push_back (selection);
                selectedImportedRegion = selection;
            }
        }
        else
        {
            selectedImportedRegion = selection;
            selectedImportedRegions = { selection };
        }

        pruneImportedRegionSelection();
        syncViewButtons();
        syncArrangementTimelineView();
    }

    void selectImportedAudioRegions (std::vector<ArrangementAudioSelection> selections, bool addToSelection)
    {
        if (! addToSelection)
            selectedImportedRegions.clear();

        for (const auto& selection : selections)
            if (importedRegionExists (selection)
                && std::find (selectedImportedRegions.begin(), selectedImportedRegions.end(), selection) == selectedImportedRegions.end())
                selectedImportedRegions.push_back (selection);

        selectedImportedRegion = selectedImportedRegions.empty() ? ArrangementAudioSelection {} : selectedImportedRegions.back();
        pruneImportedRegionSelection();

        syncViewButtons();
        syncArrangementTimelineView();
    }

    void splitImportedAudioRegion (int clipIndex, int regionIndex, double splitSeconds)
    {
        if (splitImportedAudioRegionInternal (clipIndex, regionIndex, splitSeconds, true))
            refreshAfterImportedAudioEdit ("split audio");
    }

    bool splitImportedAudioRegionInternal (int clipIndex, int regionIndex, double splitSeconds, bool pushSnapshot)
    {
        if (! arrangementPlusMode)
            return false;

        ArrangementAudioSelection selection { clipIndex, regionIndex };
        if (! importedRegionExists (selection))
            return false;

        auto& clip = importedLaneAudioClips[static_cast<size_t> (clipIndex)];
        auto& region = clip.regions[static_cast<size_t> (regionIndex)];
        if (region.lengthSeconds <= minImportedRegionLengthSeconds * 2.0)
            return false;

        splitSeconds = juce::jlimit (region.startSeconds + minImportedRegionLengthSeconds,
                                     regionEndSeconds (region) - minImportedRegionLengthSeconds,
                                     splitSeconds);

        if (splitSeconds <= region.startSeconds || splitSeconds >= regionEndSeconds (region))
            return false;

        if (pushSnapshot)
            pushUndoSnapshot ("split audio region");

        const auto leftLength = splitSeconds - region.startSeconds;
        const auto rightLength = regionEndSeconds (region) - splitSeconds;

        ImportedAudioRegion right = region;
        right.startSeconds = splitSeconds;
        right.sourceStartSeconds = region.sourceStartSeconds + leftLength;
        right.lengthSeconds = rightLength;
        right.fadeInSeconds = 0.0;
        right.fadeOutSeconds = region.fadeOutSeconds;

        region.lengthSeconds = leftLength;
        region.fadeOutSeconds = 0.0;

        clip.regions.insert (clip.regions.begin() + regionIndex + 1, right);
        sanitiseImportedClipRegions (clip);
        selectedImportedRegion = { clipIndex, juce::jlimit (0, static_cast<int> (clip.regions.size()) - 1, regionIndex + 1) };
        selectedImportedRegions = { selectedImportedRegion };
        return true;
    }

    void splitSelectedImportedRegionsAtPlayhead()
    {
        if (! arrangementPlusMode)
            return;

        auto selections = importedRegionSelectionForEdit();
        if (selections.empty())
        {
            for (int clipIndex = 0; clipIndex < static_cast<int> (importedLaneAudioClips.size()); ++clipIndex)
            {
                const auto& clip = importedLaneAudioClips[static_cast<size_t> (clipIndex)];
                for (int regionIndex = 0; regionIndex < static_cast<int> (clip.regions.size()); ++regionIndex)
                    if (regionContainsTime (clip.regions[static_cast<size_t> (regionIndex)], arrangementEditPlayheadSeconds))
                        selections.push_back ({ clipIndex, regionIndex });
            }
        }

        std::sort (selections.begin(), selections.end(), [] (const auto& a, const auto& b)
        {
            if (a.clipIndex != b.clipIndex)
                return a.clipIndex > b.clipIndex;
            return a.regionIndex > b.regionIndex;
        });

        selections.erase (std::remove_if (selections.begin(), selections.end(), [this] (const auto& selection)
        {
            if (! importedRegionExists (selection))
                return true;

            const auto& region = importedLaneAudioClips[static_cast<size_t> (selection.clipIndex)]
                                    .regions[static_cast<size_t> (selection.regionIndex)];
            return arrangementEditPlayheadSeconds <= region.startSeconds + minImportedRegionLengthSeconds
                || arrangementEditPlayheadSeconds >= regionEndSeconds (region) - minImportedRegionLengthSeconds;
        }), selections.end());

        if (selections.empty())
            return;

        auto splitAny = false;
        pushUndoSnapshot ("split audio regions");
        for (const auto& selection : selections)
            splitAny = splitImportedAudioRegionInternal (selection.clipIndex, selection.regionIndex, arrangementEditPlayheadSeconds, false) || splitAny;

        if (splitAny)
            refreshAfterImportedAudioEdit ("split audio at playhead");
    }

    void beginImportedRegionFadeEdit (int clipIndex, int regionIndex)
    {
        ArrangementAudioSelection selection { clipIndex, regionIndex };
        if (! importedRegionExists (selection))
            return;

        selectedImportedRegion = selection;
        if (std::find (selectedImportedRegions.begin(), selectedImportedRegions.end(), selection) == selectedImportedRegions.end())
            selectedImportedRegions = { selection };
        if (! importedFadeDragUndoStarted)
        {
            pushUndoSnapshot ("fade audio region");
            importedFadeDragUndoStarted = true;
        }
    }

    void setImportedAudioRegionFades (int clipIndex, int regionIndex, double fadeInSeconds, double fadeOutSeconds)
    {
        ArrangementAudioSelection selection { clipIndex, regionIndex };
        if (! importedRegionExists (selection))
            return;

        auto& clip = importedLaneAudioClips[static_cast<size_t> (clipIndex)];
        auto& region = clip.regions[static_cast<size_t> (regionIndex)];
        fadeInSeconds = juce::jlimit (0.0, region.lengthSeconds, fadeInSeconds);
        fadeOutSeconds = juce::jlimit (0.0, region.lengthSeconds - fadeInSeconds, fadeOutSeconds);

        if (std::abs (region.fadeInSeconds - fadeInSeconds) < 0.0001
            && std::abs (region.fadeOutSeconds - fadeOutSeconds) < 0.0001)
            return;

        region.fadeInSeconds = fadeInSeconds;
        region.fadeOutSeconds = fadeOutSeconds;
        selectedImportedRegion = selection;
        refreshAfterImportedAudioEdit ("fade audio");
    }

    void beginImportedRegionTrimEdit (int clipIndex, int regionIndex)
    {
        ArrangementAudioSelection selection { clipIndex, regionIndex };
        if (! importedRegionExists (selection))
            return;

        selectedImportedRegion = selection;
        if (std::find (selectedImportedRegions.begin(), selectedImportedRegions.end(), selection) == selectedImportedRegions.end())
            selectedImportedRegions = { selection };

        if (! importedTrimDragUndoStarted)
        {
            pushUndoSnapshot ("trim audio region");
            importedTrimDragUndoStarted = true;
        }
    }

    void setImportedAudioRegionTrim (int clipIndex, int regionIndex, double startSeconds, double sourceStartSeconds, double lengthSeconds)
    {
        ArrangementAudioSelection selection { clipIndex, regionIndex };
        if (! importedRegionExists (selection))
            return;

        auto& clip = importedLaneAudioClips[static_cast<size_t> (clipIndex)];
        auto& region = clip.regions[static_cast<size_t> (regionIndex)];
        startSeconds = juce::jmax (0.0, startSeconds);
        sourceStartSeconds = juce::jlimit (0.0, clip.lengthSeconds, sourceStartSeconds);
        const auto availableLength = clip.lengthSeconds - sourceStartSeconds;
        if (availableLength <= minImportedRegionLengthSeconds)
            return;
        lengthSeconds = juce::jlimit (minImportedRegionLengthSeconds, availableLength, lengthSeconds);

        if (std::abs (region.startSeconds - startSeconds) < 0.0001
            && std::abs (region.sourceStartSeconds - sourceStartSeconds) < 0.0001
            && std::abs (region.lengthSeconds - lengthSeconds) < 0.0001)
            return;

        region.startSeconds = startSeconds;
        region.sourceStartSeconds = sourceStartSeconds;
        region.lengthSeconds = lengthSeconds;
        region.fadeInSeconds = juce::jlimit (0.0, region.lengthSeconds, region.fadeInSeconds);
        region.fadeOutSeconds = juce::jlimit (0.0, region.lengthSeconds, region.fadeOutSeconds);
        selectedImportedRegion = selection;
        refreshAfterImportedAudioEdit ("trim audio");
    }

    void beginImportedRegionGainEdit (int clipIndex, int regionIndex)
    {
        ArrangementAudioSelection selection { clipIndex, regionIndex };
        if (! importedRegionExists (selection))
            return;

        selectedImportedRegion = selection;
        if (std::find (selectedImportedRegions.begin(), selectedImportedRegions.end(), selection) == selectedImportedRegions.end())
            selectedImportedRegions = { selection };

        if (! importedGainDragUndoStarted)
        {
            pushUndoSnapshot ("change region gain");
            importedGainDragUndoStarted = true;
        }
    }

    void setImportedAudioRegionGain (int clipIndex, int regionIndex, float gain)
    {
        ArrangementAudioSelection selection { clipIndex, regionIndex };
        if (! importedRegionExists (selection))
            return;

        auto& region = importedLaneAudioClips[static_cast<size_t> (clipIndex)].regions[static_cast<size_t> (regionIndex)];
        gain = juce::jlimit (0.0f, 2.0f, gain);
        if (std::abs (region.gain - gain) < 0.001f)
            return;

        region.gain = gain;
        selectedImportedRegion = selection;
        refreshAfterImportedAudioEdit ("changed region gain");
    }

    void cycleImportedRegionFadeCurve (int clipIndex, int regionIndex, bool fadeOut)
    {
        ArrangementAudioSelection selection { clipIndex, regionIndex };
        if (! importedRegionExists (selection))
            return;

        pushUndoSnapshot ("change fade curve");
        auto& region = importedLaneAudioClips[static_cast<size_t> (clipIndex)].regions[static_cast<size_t> (regionIndex)];
        auto& curve = fadeOut ? region.fadeOutCurve : region.fadeInCurve;
        curve = (juce::jlimit (0, 3, curve) + 1) % 4;
        selectedImportedRegion = selection;
        if (std::find (selectedImportedRegions.begin(), selectedImportedRegions.end(), selection) == selectedImportedRegions.end())
            selectedImportedRegions = { selection };
        refreshAfterImportedAudioEdit ("fade curve: " + regionFadeCurveName (curve));
    }

    void deleteSelectedImportedAudioRegion()
    {
        auto selections = importedRegionSelectionForEdit();
        if (selections.empty())
            return;

        std::sort (selections.begin(), selections.end(), [] (const auto& a, const auto& b)
        {
            if (a.clipIndex != b.clipIndex)
                return a.clipIndex > b.clipIndex;
            return a.regionIndex > b.regionIndex;
        });

        pushUndoSnapshot (selections.size() > 1 ? "delete audio regions" : "delete audio region");
        for (const auto& selection : selections)
        {
            if (! importedRegionExists (selection))
                continue;

            auto& clip = importedLaneAudioClips[static_cast<size_t> (selection.clipIndex)];
            clip.regions.erase (clip.regions.begin() + selection.regionIndex);
            if (clip.regions.empty())
                importedLaneAudioClips.erase (importedLaneAudioClips.begin() + selection.clipIndex);
            else
                sanitiseImportedClipRegions (clip);
        }

        selectedImportedRegion = {};
        selectedImportedRegions.clear();
        refreshAfterImportedAudioEdit (selections.size() > 1 ? "deleted audio regions" : "deleted audio region");
    }

    void setArrangementEditPlayheadSeconds (double seconds)
    {
        arrangementEditPlayheadSeconds = juce::jmax (0.0, seconds);
        if (running && isUsingImportedAudioPlayback())
            audioCallback.setImportedPlaybackPositionSeconds (arrangementEditPlayheadSeconds);
        syncViewButtons();
    }

    int findMatchingImportedRegionIndex (const ImportedLaneAudioClip& clip, const ImportedAudioRegion& target) const
    {
        for (int i = 0; i < static_cast<int> (clip.regions.size()); ++i)
        {
            const auto& region = clip.regions[static_cast<size_t> (i)];
            if (std::abs (region.startSeconds - target.startSeconds) < 0.0005
                && std::abs (region.sourceStartSeconds - target.sourceStartSeconds) < 0.0005
                && std::abs (region.lengthSeconds - target.lengthSeconds) < 0.0005)
                return i;
        }

        return -1;
    }

    void copySelectedImportedAudioRegions()
    {
        const auto selections = importedRegionSelectionForEdit();
        if (selections.empty())
            return;

        copiedImportedRegions.clear();
        auto anchorStart = std::numeric_limits<double>::max();
        for (const auto& selection : selections)
        {
            if (! importedRegionExists (selection))
                continue;

            const auto& region = importedLaneAudioClips[static_cast<size_t> (selection.clipIndex)]
                                    .regions[static_cast<size_t> (selection.regionIndex)];
            anchorStart = juce::jmin (anchorStart, region.startSeconds);
        }

        if (! std::isfinite (anchorStart))
            return;

        for (const auto& selection : selections)
        {
            if (! importedRegionExists (selection))
                continue;

            ImportedRegionClipboardItem item;
            item.clipIndex = selection.clipIndex;
            item.region = importedLaneAudioClips[static_cast<size_t> (selection.clipIndex)]
                            .regions[static_cast<size_t> (selection.regionIndex)];
            item.anchorStartSeconds = anchorStart;
            copiedImportedRegions.push_back (std::move (item));
        }

        if (! copiedImportedRegions.empty())
        {
            objectClipboardKind = ObjectClipboardKind::audioRegions;
            statusLabel.setText ("copied " + juce::String (static_cast<int> (copiedImportedRegions.size())) + " audio region"
                                 + (copiedImportedRegions.size() == 1 ? "" : "s"),
                                 juce::dontSendNotification);
            menuItemsChanged();
        }
    }

    void pasteCopiedImportedAudioRegions()
    {
        if (! arrangementPlusMode || copiedImportedRegions.empty())
            return;

        const auto anchor = copiedImportedRegions.front().anchorStartSeconds;
        const auto offset = juce::jmax (0.0, arrangementEditPlayheadSeconds) - anchor;
        pushUndoSnapshot ("paste audio regions");
        selectedImportedRegions.clear();

        for (const auto& item : copiedImportedRegions)
        {
            if (item.clipIndex < 0 || item.clipIndex >= static_cast<int> (importedLaneAudioClips.size()))
                continue;

            auto copy = item.region;
            copy.startSeconds = juce::jmax (0.0, copy.startSeconds + offset);
            auto& clip = importedLaneAudioClips[static_cast<size_t> (item.clipIndex)];
            clip.regions.push_back (copy);
            sanitiseImportedClipRegions (clip);

            const auto newIndex = findMatchingImportedRegionIndex (clip, copy);
            if (newIndex >= 0)
                selectedImportedRegions.push_back ({ item.clipIndex, newIndex });
        }

        selectedImportedRegion = selectedImportedRegions.empty() ? ArrangementAudioSelection {} : selectedImportedRegions.front();
        refreshAfterImportedAudioEdit ("pasted audio regions");
    }

    void duplicateSelectedImportedAudioRegions()
    {
        auto selections = importedRegionSelectionForEdit();
        if (selections.empty())
            return;

        pushUndoSnapshot ("duplicate audio regions");
        selectedImportedRegions.clear();

        for (const auto& selection : selections)
        {
            if (! importedRegionExists (selection))
                continue;

            auto& clip = importedLaneAudioClips[static_cast<size_t> (selection.clipIndex)];
            auto copy = clip.regions[static_cast<size_t> (selection.regionIndex)];
            copy.startSeconds = regionEndSeconds (copy);
            clip.regions.push_back (copy);
            sanitiseImportedClipRegions (clip);

            const auto newIndex = findMatchingImportedRegionIndex (clip, copy);
            if (newIndex >= 0)
                selectedImportedRegions.push_back ({ selection.clipIndex, newIndex });
        }

        selectedImportedRegion = selectedImportedRegions.empty() ? ArrangementAudioSelection {} : selectedImportedRegions.front();
        refreshAfterImportedAudioEdit ("duplicated audio regions");
    }

    void refreshAfterImportedAudioEdit (const juce::String& message)
    {
        if (importedLaneAudioClips.empty())
        {
            arrangementPlusMode = false;
            running = false;
            scriptRunning = false;
            selectedImportedRegion = {};
            selectedImportedRegions.clear();
            audioCallback.clearImportedAudioPlayback();
        }
        else
        {
            arrangementPlusMode = true;
            pruneImportedRegionSelection();
        }

        if (arrangementPlusMode)
        {
            static_cast<void> (audioCallback.loadImportedAudioClips (importedLaneAudioClips, &topLevelStates, false, &masterEffectSlots));
            syncImportedPlaybackMix();
        }

        markProjectDirty();
        syncArrangementTimelineView();
        syncMixerView();
        syncViewButtons();
        statusLabel.setText (message, juce::dontSendNotification);
    }

    ImportedLaneAudioClip* selectedInspectorClip()
    {
        if (! importedRegionExists (selectedImportedRegion))
            return nullptr;

        return &importedLaneAudioClips[static_cast<size_t> (selectedImportedRegion.clipIndex)];
    }

    ImportedAudioRegion* selectedInspectorRegion()
    {
        auto* clip = selectedInspectorClip();
        if (clip == nullptr)
            return nullptr;

        return &clip->regions[static_cast<size_t> (selectedImportedRegion.regionIndex)];
    }

    void syncRegionInspectorControls()
    {
        juce::ScopedValueSetter<bool> guard (suppressRegionInspectorCallbacks, true);
        const auto hasRegion = arrangementPlusMode && importedRegionExists (selectedImportedRegion);
        auto* clip = hasRegion ? selectedInspectorClip() : nullptr;
        auto* region = hasRegion ? selectedInspectorRegion() : nullptr;

        regionInspectorLabel.setText (hasRegion && clip != nullptr
                                        ? "region S" + juce::String (clip->stateIndex + 1)
                                             + " T" + juce::String (clip->trackIndex + 1)
                                             + " L" + juce::String (clip->laneIndex + 1)
                                        : "region",
                                      juce::dontSendNotification);

        auto setEditorEnabled = [hasRegion] (juce::TextEditor& editor)
        {
            editor.setEnabled (hasRegion);
            if (! hasRegion && ! editor.hasKeyboardFocus (true))
                editor.setText ({}, juce::dontSendNotification);
        };

        setEditorEnabled (regionStartEditor);
        setEditorEnabled (regionLengthEditor);
        setEditorEnabled (regionSourceEditor);
        setEditorEnabled (regionGainEditor);
        setEditorEnabled (regionFadeInEditor);
        setEditorEnabled (regionFadeOutEditor);
        regionFadeInCurveBox.setEnabled (hasRegion);
        regionFadeOutCurveBox.setEnabled (hasRegion);

        if (region == nullptr)
        {
            regionFadeInCurveBox.setSelectedId (0, juce::dontSendNotification);
            regionFadeOutCurveBox.setSelectedId (0, juce::dontSendNotification);
            return;
        }

        auto setEditorText = [] (juce::TextEditor& editor, const juce::String& text)
        {
            if (! editor.hasKeyboardFocus (true))
                editor.setText (text, juce::dontSendNotification);
        };

        setEditorText (regionStartEditor, juce::String (region->startSeconds, 2));
        setEditorText (regionLengthEditor, juce::String (region->lengthSeconds, 2));
        setEditorText (regionSourceEditor, juce::String (region->sourceStartSeconds, 2));
        setEditorText (regionGainEditor, juce::String (region->gain, 2));
        setEditorText (regionFadeInEditor, juce::String (region->fadeInSeconds, 2));
        setEditorText (regionFadeOutEditor, juce::String (region->fadeOutSeconds, 2));
        regionFadeInCurveBox.setSelectedId (juce::jlimit (0, 3, region->fadeInCurve) + 1, juce::dontSendNotification);
        regionFadeOutCurveBox.setSelectedId (juce::jlimit (0, 3, region->fadeOutCurve) + 1, juce::dontSendNotification);
    }

    double regionInspectorEditorValue (const juce::TextEditor& editor, double fallback) const
    {
        const auto text = editor.getText().trim();
        if (text.isEmpty())
            return fallback;

        return juce::jmax (0.0, text.getDoubleValue());
    }

    bool beginRegionInspectorEdit (const char* undoLabel)
    {
        if (suppressRegionInspectorCallbacks || ! importedRegionExists (selectedImportedRegion))
            return false;

        pushUndoSnapshot (undoLabel);
        return true;
    }

    void reselectEditedInspectorRegion (int clipIndex, const ImportedAudioRegion& editedRegion)
    {
        if (clipIndex < 0 || clipIndex >= static_cast<int> (importedLaneAudioClips.size()))
            return;

        const auto newIndex = findMatchingImportedRegionIndex (importedLaneAudioClips[static_cast<size_t> (clipIndex)], editedRegion);
        if (newIndex >= 0)
        {
            selectedImportedRegion = { clipIndex, newIndex };
            selectedImportedRegions = { selectedImportedRegion };
        }
    }

    void applyRegionInspectorStartEdit()
    {
        auto* region = selectedInspectorRegion();
        if (region == nullptr)
            return;

        const auto nextStart = regionInspectorEditorValue (regionStartEditor, region->startSeconds);
        if (std::abs (region->startSeconds - nextStart) < 0.0001 || ! beginRegionInspectorEdit ("edit region start"))
            return;

        const auto clipIndex = selectedImportedRegion.clipIndex;
        region->startSeconds = nextStart;
        const auto editedRegion = *region;
        sanitiseImportedClipRegions (importedLaneAudioClips[static_cast<size_t> (clipIndex)]);
        reselectEditedInspectorRegion (clipIndex, editedRegion);
        refreshAfterImportedAudioEdit ("edited region start");
    }

    void applyRegionInspectorLengthEdit()
    {
        auto* clip = selectedInspectorClip();
        auto* region = selectedInspectorRegion();
        if (clip == nullptr || region == nullptr)
            return;

        const auto nextLength = juce::jlimit (minImportedRegionLengthSeconds,
                                             juce::jmax (minImportedRegionLengthSeconds, clip->lengthSeconds - region->sourceStartSeconds),
                                             regionInspectorEditorValue (regionLengthEditor, region->lengthSeconds));
        if (std::abs (region->lengthSeconds - nextLength) < 0.0001 || ! beginRegionInspectorEdit ("edit region length"))
            return;

        const auto clipIndex = selectedImportedRegion.clipIndex;
        region->lengthSeconds = nextLength;
        region->fadeInSeconds = juce::jlimit (0.0, region->lengthSeconds, region->fadeInSeconds);
        region->fadeOutSeconds = juce::jlimit (0.0, region->lengthSeconds - region->fadeInSeconds, region->fadeOutSeconds);
        const auto editedRegion = *region;
        sanitiseImportedClipRegions (*clip);
        reselectEditedInspectorRegion (clipIndex, editedRegion);
        refreshAfterImportedAudioEdit ("edited region length");
    }

    void applyRegionInspectorSourceEdit()
    {
        auto* clip = selectedInspectorClip();
        auto* region = selectedInspectorRegion();
        if (clip == nullptr || region == nullptr)
            return;

        const auto nextSource = juce::jlimit (0.0, clip->lengthSeconds, regionInspectorEditorValue (regionSourceEditor, region->sourceStartSeconds));
        if (std::abs (region->sourceStartSeconds - nextSource) < 0.0001 || ! beginRegionInspectorEdit ("edit region source"))
            return;

        const auto clipIndex = selectedImportedRegion.clipIndex;
        region->sourceStartSeconds = nextSource;
        region->lengthSeconds = juce::jlimit (minImportedRegionLengthSeconds,
                                              juce::jmax (minImportedRegionLengthSeconds, clip->lengthSeconds - region->sourceStartSeconds),
                                              region->lengthSeconds);
        const auto editedRegion = *region;
        sanitiseImportedClipRegions (*clip);
        reselectEditedInspectorRegion (clipIndex, editedRegion);
        refreshAfterImportedAudioEdit ("edited region source");
    }

    void applyRegionInspectorGainEdit()
    {
        auto* region = selectedInspectorRegion();
        if (region == nullptr)
            return;

        const auto nextGain = juce::jlimit (0.0f, 2.0f, static_cast<float> (regionInspectorEditorValue (regionGainEditor, region->gain)));
        if (std::abs (region->gain - nextGain) < 0.001f || ! beginRegionInspectorEdit ("edit region gain"))
            return;

        setImportedAudioRegionGain (selectedImportedRegion.clipIndex, selectedImportedRegion.regionIndex, nextGain);
    }

    void applyRegionInspectorFadeInEdit()
    {
        auto* region = selectedInspectorRegion();
        if (region == nullptr)
            return;

        const auto nextFade = juce::jlimit (0.0, region->lengthSeconds, regionInspectorEditorValue (regionFadeInEditor, region->fadeInSeconds));
        if (std::abs (region->fadeInSeconds - nextFade) < 0.0001 || ! beginRegionInspectorEdit ("edit fade in"))
            return;

        setImportedAudioRegionFades (selectedImportedRegion.clipIndex,
                                     selectedImportedRegion.regionIndex,
                                     nextFade,
                                     region->fadeOutSeconds);
    }

    void applyRegionInspectorFadeOutEdit()
    {
        auto* region = selectedInspectorRegion();
        if (region == nullptr)
            return;

        const auto nextFade = juce::jlimit (0.0, region->lengthSeconds, regionInspectorEditorValue (regionFadeOutEditor, region->fadeOutSeconds));
        if (std::abs (region->fadeOutSeconds - nextFade) < 0.0001 || ! beginRegionInspectorEdit ("edit fade out"))
            return;

        setImportedAudioRegionFades (selectedImportedRegion.clipIndex,
                                     selectedImportedRegion.regionIndex,
                                     region->fadeInSeconds,
                                     nextFade);
    }

    void applyRegionInspectorFadeCurveEdit (bool fadeOut)
    {
        if (suppressRegionInspectorCallbacks || ! importedRegionExists (selectedImportedRegion))
            return;

        const auto selectedId = fadeOut ? regionFadeOutCurveBox.getSelectedId() : regionFadeInCurveBox.getSelectedId();
        if (selectedId <= 0)
            return;

        auto& region = importedLaneAudioClips[static_cast<size_t> (selectedImportedRegion.clipIndex)]
                          .regions[static_cast<size_t> (selectedImportedRegion.regionIndex)];
        auto& curve = fadeOut ? region.fadeOutCurve : region.fadeInCurve;
        const auto nextCurve = juce::jlimit (0, 3, selectedId - 1);
        if (curve == nextCurve)
            return;

        pushUndoSnapshot ("edit fade curve");
        curve = nextCurve;
        refreshAfterImportedAudioEdit (fadeOut ? "edited fade out curve" : "edited fade in curve");
    }

    Wf::StateSpec* getTrack (int stateIndex, int trackIndex)
    {
        if (! isTopLevelStatePopulated (stateIndex))
            return nullptr;

        auto& tracks = *topLevelStates[static_cast<size_t> (stateIndex)];
        if (trackIndex < 0 || trackIndex >= static_cast<int> (tracks.size()))
            return nullptr;

        return &tracks[static_cast<size_t> (trackIndex)];
    }

    static juce::File pluginScanDirectory()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
            .getChildFile ("ChucK-ME");
    }

    static juce::File rememberedPluginScanFile()
    {
        return pluginScanDirectory().getChildFile ("au-vst3-plugin-scan.xml");
    }

    static juce::File pluginScanDeadMansPedalFile()
    {
        return pluginScanDirectory().getChildFile ("plugin-scan-dead-mans-pedal.txt");
    }

    static bool isScannableEffectFormat (juce::AudioPluginFormat& format)
    {
        const auto name = format.getName();
        return name.equalsIgnoreCase ("VST3")
            || name.equalsIgnoreCase ("AudioUnit");
    }

    static bool isUsableScannedEffect (const juce::PluginDescription& description)
    {
        return ! description.isInstrument
            && description.numInputChannels > 0
            && description.numOutputChannels > 0
            && (description.pluginFormatName.equalsIgnoreCase ("VST3")
                || description.pluginFormatName.equalsIgnoreCase ("AudioUnit"));
    }

    juce::Array<juce::PluginDescription> getScannedEffectPlugins() const
    {
        juce::Array<juce::PluginDescription> plugins;

        for (const auto& description : knownPluginList.getTypes())
            if (isUsableScannedEffect (description))
                plugins.add (description);

        return plugins;
    }

    void loadRememberedPluginScan()
    {
        knownPluginList.clear();

        const auto file = rememberedPluginScanFile();
        if (! file.existsAsFile())
            return;

        if (auto xml = juce::parseXML (file))
        {
            knownPluginList.recreateFromXml (*xml);
            knownPluginList.sort (juce::KnownPluginList::sortAlphabetically, true);
        }
    }

    void saveRememberedPluginScan()
    {
        auto xml = knownPluginList.createXml();
        if (xml == nullptr)
            return;

        auto directory = pluginScanDirectory();
        if (! directory.createDirectory())
            return;

        static_cast<void> (rememberedPluginScanFile().replaceWithText (xml->toString(), false, false, "\n"));
    }

    struct PluginScanResult
    {
        juce::KnownPluginList scannedList;
        juce::StringArray failedFiles;
    };

    void scanOrRescanEffectPlugins()
    {
        if (pluginScanInProgress)
            return;

        if (pluginScanThread.joinable())
            pluginScanThread.join();

        if (running)
            stopMainTransport();

        pluginScanAbortRequested.store (false, std::memory_order_release);
        pluginScanInProgress = true;
        menuItemsChanged();
        statusLabel.setText ("scanning AU/VST3 plugins...", juce::dontSendNotification);

        const juce::Component::SafePointer<MainComponent> safeThis (this);
        auto* abortRequested = &pluginScanAbortRequested;
        pluginScanThread = std::thread ([safeThis, abortRequested]
        {
            performEffectPluginScan (safeThis, abortRequested);
        });
    }

    static void performEffectPluginScan (juce::Component::SafePointer<MainComponent> safeThis,
                                         const std::atomic<bool>* abortRequested)
    {
        auto directory = pluginScanDirectory();
        static_cast<void> (directory.createDirectory());
        const auto deadMansPedal = pluginScanDeadMansPedalFile();
        static_cast<void> (deadMansPedal.deleteFile());

        juce::AudioPluginFormatManager scanFormatManager;
        juce::addDefaultFormatsToManager (scanFormatManager);

        auto result = std::make_shared<PluginScanResult>();

        for (auto* format : scanFormatManager.getFormats())
        {
            if (format == nullptr || ! isScannableEffectFormat (*format))
                continue;

            if (abortRequested != nullptr && abortRequested->load (std::memory_order_acquire))
                break;

            juce::PluginDirectoryScanner scanner (result->scannedList,
                                                  *format,
                                                  format->getDefaultLocationsToSearch(),
                                                  true,
                                                  deadMansPedal,
                                                  false);

            juce::String currentPlugin;
            while (scanner.scanNextFile (false, currentPlugin))
            {
                if (abortRequested != nullptr && abortRequested->load (std::memory_order_acquire))
                    break;

                const juce::String pluginName (currentPlugin);
                juce::MessageManager::callAsync ([safeThis, pluginName]
                {
                    if (safeThis != nullptr && safeThis->pluginScanInProgress)
                        safeThis->statusLabel.setText ("scanning " + pluginName, juce::dontSendNotification);
                });
            }

            result->failedFiles.addArray (scanner.getFailedFiles());
        }

        juce::MessageManager::callAsync ([safeThis, result]
        {
            if (safeThis != nullptr)
                safeThis->finishEffectPluginScan (result);
        });
    }

    void finishEffectPluginScan (const std::shared_ptr<PluginScanResult>& result)
    {
        if (result == nullptr)
        {
            pluginScanInProgress = false;
            menuItemsChanged();
            return;
        }

        knownPluginList.clear();
        for (const auto& description : result->scannedList.getTypes())
            if (isUsableScannedEffect (description))
                knownPluginList.addType (description);

        knownPluginList.sort (juce::KnownPluginList::sortAlphabetically, true);
        saveRememberedPluginScan();

        const auto count = getScannedEffectPlugins().size();
        statusLabel.setText ("scanned " + juce::String (count) + " AU/VST3 effect"
                                + (count == 1 ? juce::String() : "s")
                                + (result->failedFiles.isEmpty() ? juce::String()
                                                                 : " (" + juce::String (result->failedFiles.size()) + " failed)"),
                             juce::dontSendNotification);
        pluginScanInProgress = false;
        menuItemsChanged();
    }

    void showEffectSlotMenu (int stateIndex, int trackIndex, int slotIndex)
    {
        if (! arrangementPlusMode)
            return;

        auto* slotToEdit = getEffectSlotSpec (stateIndex, trackIndex, slotIndex);
        if (slotToEdit == nullptr)
            return;

        auto& slot = *slotToEdit;
        juce::PopupMenu menu;
        const auto hasScannedPlugins = ! getScannedEffectPlugins().isEmpty();

        if (slot.pluginName.isNotEmpty())
        {
            menu.addItem (1, slot.active ? "Bypass " + slot.pluginName : "Activate " + slot.pluginName);
            menu.addItem (2, "Replace...", hasScannedPlugins);
            menu.addItem (3, "Clear");
        }
        else if (hasScannedPlugins)
        {
            menu.addItem (2, "Load AU/VST3...");
        }
        else
        {
            menu.addItem (4, "No scanned AU/VST3 effects", false);
            menu.addItem (5, "Use File > Scan AU/VST3 Plugins", false);
        }

        menu.showMenuAsync (juce::PopupMenu::Options(),
                            [this, stateIndex, trackIndex, slotIndex] (int result)
                            {
                                if (result == 1)
                                    toggleEffectSlot (stateIndex, trackIndex, slotIndex);
                                else if (result == 2)
                                    chooseEffectPluginForSlot (stateIndex, trackIndex, slotIndex);
                                else if (result == 3)
                                    clearEffectSlot (stateIndex, trackIndex, slotIndex);
                            });
    }

    void toggleEffectSlot (int stateIndex, int trackIndex, int slotIndex)
    {
        if (auto* slot = getEffectSlotSpec (stateIndex, trackIndex, slotIndex))
        {
            if (slot->pluginName.isNotEmpty())
            {
                pushUndoSnapshot ("toggle effect slot");
                slot->active = ! slot->active;
                refreshAfterEffectSlotChange (stateIndex, trackIndex);
            }
        }
    }

    void clearEffectSlot (int stateIndex, int trackIndex, int slotIndex)
    {
        auto* slot = getEffectSlotSpec (stateIndex, trackIndex, slotIndex);
        if (slot == nullptr || slot->pluginName.isEmpty())
            return;

        pushUndoSnapshot ("clear effect slot");
        *slot = {};
        refreshAfterEffectSlotChange (stateIndex, trackIndex);
    }

    void chooseEffectPluginForSlot (int stateIndex, int trackIndex, int slotIndex)
    {
        auto plugins = getScannedEffectPlugins();
        if (plugins.isEmpty())
        {
            statusLabel.setText ("no scanned AU/VST3 effects - use File > Scan AU/VST3 Plugins", juce::dontSendNotification);
            return;
        }

        auto* slot = getEffectSlotSpec (stateIndex, trackIndex, slotIndex);
        if (slot == nullptr)
            return;

        const auto currentIdentifier = slot->pluginIdentifier;
        juce::PopupMenu pluginMenu;
        juce::KnownPluginList::addToMenu (pluginMenu,
                                          plugins,
                                          juce::KnownPluginList::sortByManufacturer,
                                          currentIdentifier);

        pluginMenu.showMenuAsync (juce::PopupMenu::Options(),
                                  [this, plugins, stateIndex, trackIndex, slotIndex] (int result)
                                  {
                                      const auto pluginIndex = juce::KnownPluginList::getIndexChosenByMenu (plugins, result);
                                      if (pluginIndex < 0 || pluginIndex >= plugins.size())
                                          return;

                                      applyScannedEffectPluginToSlot (stateIndex,
                                                                      trackIndex,
                                                                      slotIndex,
                                                                      plugins.getReference (pluginIndex));
                                  });
    }

    void applyScannedEffectPluginToSlot (int stateIndex,
                                         int trackIndex,
                                         int slotIndex,
                                         const juce::PluginDescription& description)
    {
        auto* slot = getEffectSlotSpec (stateIndex, trackIndex, slotIndex);
        if (slot == nullptr)
            return;

        pushUndoSnapshot ("load effect slot");
        slot->active = true;
        slot->pluginName = description.name.isNotEmpty() ? description.name : description.descriptiveName;
        slot->pluginFormatName = description.pluginFormatName;
        slot->pluginFileOrIdentifier = description.fileOrIdentifier;
        slot->pluginIdentifier = description.createIdentifierString();

        refreshAfterEffectSlotChange (stateIndex, trackIndex);
    }

    Wf::TrackEffectSlotSpec* getEffectSlotSpec (int stateIndex, int trackIndex, int slotIndex)
    {
        if (slotIndex < 0 || slotIndex >= maxTrackEffectSlots)
            return nullptr;

        if (stateIndex < 0 || trackIndex < 0)
            return &masterEffectSlots[static_cast<size_t> (slotIndex)];

        auto* track = getTrack (stateIndex, trackIndex);
        if (track == nullptr)
            return nullptr;

        return &track->effectSlots[static_cast<size_t> (slotIndex)];
    }

    void refreshAfterEffectSlotChange (int stateIndex, int trackIndex)
    {
        if (arrangementPlusMode && ! importedLaneAudioClips.empty())
        {
            static_cast<void> (audioCallback.loadImportedAudioClips (importedLaneAudioClips, &topLevelStates, false, &masterEffectSlots));
            syncImportedPlaybackMix();
        }
        else if (stateIndex == performingTopLevelState && trackIndex == performingTrackIndex)
        {
            loadSelectedContentForCurrentState();
        }

        markProjectDirty();
        refreshLabels();
    }

    void selectViewedTopLevelState (int index)
    {
        const auto nextState = juce::jlimit (0, maxTopLevelStates - 1, index);

        if (viewedTopLevelState == nextState)
        {
            syncViewedStateControls();
            refreshLabels();
            return;
        }

        viewedTopLevelState = nextState;
        if (isTopLevelStatePopulated (viewedTopLevelState))
            selectedState = juce::jlimit (0, getViewedTrackCount() - 1, selectedState);

        syncViewedStateControls();
        refreshLabels();
    }

    void setPerformingTopLevelState (int index, bool restartFromFirstTrack = false)
    {
        const auto nextState = juce::jlimit (0, maxTopLevelStates - 1, index);
        if (! isTopLevelStatePopulated (nextState))
            return;

        if (performingTopLevelState == nextState && ! restartFromFirstTrack)
        {
            refreshLabels();
            return;
        }

        performingTopLevelState = nextState;
        performingTrackIndex = restartFromFirstTrack
            ? 0
            : juce::jlimit (0, getPerformingTrackCount() - 1, performingTrackIndex);
        trackElapsedBars = 0.0;
        nextBarTransitionCheck = 1.0;
        orbitPhase = 0.0f;

        if (viewedTopLevelState == performingTopLevelState)
            selectedState = performingTrackIndex;

        loadSelectedContentForCurrentState();
        refreshLabels();
    }

    int firstEmptyTopLevelState() const
    {
        for (int i = 0; i < maxTopLevelStates; ++i)
            if (! isTopLevelStatePopulated (i))
                return i;

        return -1;
    }

    int firstPopulatedTopLevelState() const
    {
        for (int i = 0; i < maxTopLevelStates; ++i)
            if (isTopLevelStatePopulated (i))
                return i;

        return -1;
    }

    void createTopLevelState()
    {
        auto target = viewedTopLevelState;

        if (isTopLevelStatePopulated (target))
            target = firstEmptyTopLevelState();

        if (target < 0 || target >= maxTopLevelStates)
            return;

        pushUndoSnapshot ("new state");
        auto created = Wf::makeDefaultStates();

        for (int i = 0; i < static_cast<int> (created.size()); ++i)
            created[static_cast<size_t> (i)].name = trackDisplayName ("Track " + juce::String (i + 1));

        topLevelStates[static_cast<size_t> (target)] = std::move (created);
        topLevelTemposBpm[static_cast<size_t> (target)] = 88.0f;
        topLevelTimeSigNumerators[static_cast<size_t> (target)] = 4;
        topLevelTimeSigDenominators[static_cast<size_t> (target)] = 4;
        viewedTopLevelState = target;
        selectedState = 0;
        selectedLane = 0;
        markProjectDirty();
        selectViewedTopLevelState (target);
    }

    void duplicateViewedTopLevelState()
    {
        const auto target = firstEmptyTopLevelState();
        if (target < 0 || ! isTopLevelStatePopulated (viewedTopLevelState))
            return;

        pushUndoSnapshot ("duplicate state");
        const auto source = static_cast<size_t> (viewedTopLevelState);
        topLevelStates[static_cast<size_t> (target)] = topLevelStates[source];
        topLevelTemposBpm[static_cast<size_t> (target)] = topLevelTemposBpm[source];
        topLevelTimeSigNumerators[static_cast<size_t> (target)] = topLevelTimeSigNumerators[source];
        topLevelTimeSigDenominators[static_cast<size_t> (target)] = topLevelTimeSigDenominators[source];
        markProjectDirty();
        selectViewedTopLevelState (target);
    }

    void deleteViewedTopLevelState()
    {
        const auto target = viewedTopLevelState;
        if (! isTopLevelStatePopulated (target))
            return;

        pushUndoSnapshot ("delete state");
        const auto deletingPerformingState = target == performingTopLevelState;
        if (deletingPerformingState)
        {
            scriptRunning = false;
            running = false;
            syncTransportButtons();
            applyCurrentAudioControls();
        }

        topLevelStates[static_cast<size_t> (target)].reset();
        topLevelTemposBpm[static_cast<size_t> (target)] = 88.0f;
        topLevelTimeSigNumerators[static_cast<size_t> (target)] = 4;
        topLevelTimeSigDenominators[static_cast<size_t> (target)] = 4;
        selectedState = 0;
        selectedLane = 0;

        if (deletingPerformingState)
        {
            const auto replacement = firstPopulatedTopLevelState();
            performingTopLevelState = replacement >= 0 ? replacement : target;
            performingTrackIndex = 0;
            trackElapsedBars = 0.0;
            nextBarTransitionCheck = 1.0;
            orbitPhase = 0.0f;
        }

        refreshLabels();
        markProjectDirty();
        grabKeyboardFocus();
    }

    bool isInlineTextEditorFocused() const
    {
        return globalScriptEditor.hasKeyboardFocus (true)
            || laneCodeEditor.hasKeyboardFocus (true)
            || trackNameEditor.hasKeyboardFocus (true)
            || trackDurationEditor.hasKeyboardFocus (true)
            || laneNameEditor.hasKeyboardFocus (true)
            || laneTempoEditor.hasKeyboardFocus (true)
            || laneDurationEditor.hasKeyboardFocus (true)
            || laneCountEditor.hasKeyboardFocus (true)
            || stateTrackCountEditor.hasKeyboardFocus (true)
            || orbitCanvas.isEditingTransitionProbability();
    }

    std::vector<GlobalScriptStep> parseGlobalScript() const
    {
        std::vector<GlobalScriptStep> parsed;
        const auto text = stripGlobalScriptComments (globalScriptEditor.getText().toStdString());
        const std::regex tokenRegex (
            R"((?:playState\s*\(\s*(1[0-6]|[1-9])\s*,\s*(\d+(?:\.\d+)?)\s*(?:,\s*(?:tempo\s*=\s*)?(\d+(?:\.\d+)?))?\s*(?:,\s*(?:(?:timeSig|meter)\s*=\s*)?(\d{1,2})\s*(?:/|,)\s*(2|4|8|16))?\s*\))|(?:state\s*\(\s*(1[0-6]|[1-9])\s*\))|(?:(\d+(?:\.\d+)?)\s*::\s*bar)|(?:(?:tempo|bpm)\s*\(\s*(\d+(?:\.\d+)?)\s*\))|(?:(?:timeSig|meter)\s*\(\s*(\d{1,2})\s*,\s*(2|4|8|16)\s*\)))",
            std::regex_constants::icase);
        int pendingStateIndex = -1;
        std::optional<float> pendingTempoBpm;
        std::optional<int> pendingTimeSigNumerator;
        std::optional<int> pendingTimeSigDenominator;

        auto addStep = [&] (int stateIndex,
                            double bars,
                            std::optional<float> stepTempoBpm,
                            std::optional<int> stepTimeSigNumerator,
                            std::optional<int> stepTimeSigDenominator)
        {
            if (! isTopLevelStatePopulated (stateIndex) || bars <= 0.0)
                return;

            parsed.push_back ({ stateIndex,
                                bars,
                                stepTempoBpm.has_value() ? stepTempoBpm : pendingTempoBpm,
                                stepTimeSigNumerator.has_value() ? stepTimeSigNumerator : pendingTimeSigNumerator,
                                stepTimeSigDenominator.has_value() ? stepTimeSigDenominator : pendingTimeSigDenominator });
        };

        for (std::sregex_iterator it (text.begin(), text.end(), tokenRegex), end; it != end; ++it)
        {
            if ((*it)[1].matched)
            {
                pendingStateIndex = std::stoi ((*it)[1].str()) - 1;
                const auto bars = std::stod ((*it)[2].str());
                const auto stepTempoBpm = (*it)[3].matched
                    ? std::optional<float> (sanitizeScriptTempo (std::stof ((*it)[3].str())))
                    : std::optional<float>();
                const auto stepNumerator = (*it)[4].matched
                    ? std::optional<int> (sanitizeScriptTimeSigNumerator (std::stoi ((*it)[4].str())))
                    : std::optional<int>();
                const auto stepDenominator = (*it)[5].matched
                    ? std::optional<int> (sanitizeScriptTimeSigDenominator (std::stoi ((*it)[5].str())))
                    : std::optional<int>();

                addStep (pendingStateIndex, bars, stepTempoBpm, stepNumerator, stepDenominator);
                pendingStateIndex = -1;
                continue;
            }

            if ((*it)[6].matched)
            {
                pendingStateIndex = std::stoi ((*it)[6].str()) - 1;
                continue;
            }

            if ((*it)[7].matched && pendingStateIndex >= 0)
            {
                const auto bars = std::stod ((*it)[7].str());
                addStep (pendingStateIndex, bars, {}, {}, {});
                pendingStateIndex = -1;
                continue;
            }

            if ((*it)[8].matched)
            {
                pendingTempoBpm = sanitizeScriptTempo (std::stof ((*it)[8].str()));
                continue;
            }

            if ((*it)[9].matched && (*it)[10].matched)
            {
                pendingTimeSigNumerator = sanitizeScriptTimeSigNumerator (std::stoi ((*it)[9].str()));
                pendingTimeSigDenominator = sanitizeScriptTimeSigDenominator (std::stoi ((*it)[10].str()));
            }
        }

        return parsed;
    }

    static std::string stripGlobalScriptComments (const std::string& text)
    {
        std::string stripped;
        stripped.reserve (text.size());

        size_t lineStart = 0;
        while (lineStart < text.size())
        {
            const auto lineEnd = text.find_first_of ("\r\n", lineStart);
            const auto contentEnd = lineEnd == std::string::npos ? text.size() : lineEnd;
            const auto commentStart = text.find ("//", lineStart);
            const auto copyEnd = commentStart != std::string::npos && commentStart < contentEnd
                ? commentStart
                : contentEnd;

            stripped.append (text, lineStart, copyEnd - lineStart);

            if (lineEnd == std::string::npos)
                break;

            stripped += text[lineEnd];
            lineStart = lineEnd + 1;

            if (text[lineEnd] == '\r' && lineStart < text.size() && text[lineStart] == '\n')
            {
                stripped += text[lineStart];
                ++lineStart;
            }
        }

        return stripped;
    }

    static float sanitizeScriptTempo (float tempoBpm)
    {
        return juce::jlimit (30.0f, 220.0f, tempoBpm);
    }

    static int sanitizeScriptTimeSigNumerator (int numerator)
    {
        return juce::jlimit (1, 16, numerator);
    }

    static int sanitizeScriptTimeSigDenominator (int denominator)
    {
        if (denominator == 2 || denominator == 4 || denominator == 8 || denominator == 16)
            return denominator;

        return 4;
    }

    bool isUsingImportedAudioPlayback() const
    {
        return arrangementPlusMode && ! importedLaneAudioClips.empty();
    }

    void toggleMainTransport()
    {
        if (running)
            stopMainTransport();
        else
            playFromBeginning();
    }

    void playFromBeginning()
    {
        globalScriptSteps = parseGlobalScript();
        scriptStepIndex = 0;
        scriptStepElapsedBars = 0.0;
        scriptShouldStopAtEnd = juce::String (stripGlobalScriptComments (globalScriptEditor.getText().toStdString())).containsIgnoreCase ("stop");
        running = true;
        syncTransportButtons();

        if (isUsingImportedAudioPlayback())
        {
            scriptRunning = false;
            static_cast<void> (audioCallback.loadImportedAudioClips (importedLaneAudioClips, &topLevelStates, true, &masterEffectSlots));
            syncImportedPlaybackMix();
            audioCallback.startImportedAudioPlayback (getCurrentMasterGain());
            applyCurrentAudioControls();
            refreshLabels();
            return;
        }

        audioCallback.useChucKPlayback();

        if (globalScriptSteps.empty())
        {
            scriptRunning = false;
            setPerformingTopLevelState (performingTopLevelState, true);
        }
        else
        {
            scriptRunning = true;
            applyGlobalScriptStep (globalScriptSteps.front());
        }

        applyCurrentAudioControls();
        refreshLabels();
    }

    void stopMainTransport()
    {
        scriptRunning = false;
        running = false;
        syncTransportButtons();
        if (isUsingImportedAudioPlayback())
        {
            audioCallback.stopImportedAudioPlayback();
            arrangementEditPlayheadSeconds = 0.0;
            syncArrangementTimelineView();
        }
        else
            applyCurrentAudioControls();
        refreshLabels();
    }

    void syncTransportButtons()
    {
        const auto text = isUsingImportedAudioPlayback()
            ? (running ? "Stop+" : "Play+")
            : (running ? "Stop" : "Play");
        runScriptButton.setButtonText (text);
    }

    void selectState (int index)
    {
        const auto* viewedTracks = getViewedTracks();
        if (viewedTracks == nullptr || viewedTracks->empty())
            return;

        selectedState = (index + static_cast<int> (viewedTracks->size())) % static_cast<int> (viewedTracks->size());
        orbitPhase = 0.0f;

        if (running && viewedTopLevelState == performingTopLevelState)
        {
            performingTrackIndex = selectedState;
            trackElapsedBars = 0.0;
            nextBarTransitionCheck = 1.0;
            loadSelectedContentForCurrentState();
        }

        refreshLabels();
    }

    void openTrackFocusView (int index)
    {
        const auto* viewedTracks = getViewedTracks();
        if (viewedTracks == nullptr || viewedTracks->empty())
            return;

        selectState (index);
        focusedTrackIndex = selectedState;
        setMainView (MainView::track);
        trackFocusCanvas.grabKeyboardFocus();
    }

    void selectFocusedLane (int index)
    {
        const auto* viewedTracks = getViewedTracks();
        if (viewedTracks == nullptr || viewedTracks->empty())
            return;

        focusedTrackIndex = juce::jlimit (0, static_cast<int> (viewedTracks->size()) - 1, focusedTrackIndex);
        selectedState = focusedTrackIndex;
        selectLane (index);
    }

    void pickState()
    {
        const auto* viewedTracks = getViewedTracks();
        if (viewedTracks == nullptr || viewedTracks->empty())
            return;

        std::uniform_int_distribution<int> distribution (0, static_cast<int> (viewedTracks->size()) - 1);
        selectState (distribution (random));
    }

    void setTransitionProbability (int trackIndex, std::optional<int> probability)
    {
        auto* viewedTracks = getViewedTracks();
        if (viewedTracks == nullptr || trackIndex < 0 || trackIndex >= static_cast<int> (viewedTracks->size()))
            return;

        auto& track = (*viewedTracks)[static_cast<size_t> (trackIndex)];
        if (track.transitionProbabilityPercent == probability)
            return;

        pushUndoSnapshot ("change transition probability");
        track.transitionProbabilityPercent = probability;

        if (viewedTopLevelState == performingTopLevelState && trackIndex == performingTrackIndex)
            nextBarTransitionCheck = std::floor (trackElapsedBars) + 1.0;

        markProjectDirty();
        refreshLabels();
    }

    void selectLane (int index)
    {
        const auto* viewedTracks = getViewedTracks();
        if (viewedTracks == nullptr || viewedTracks->empty())
            return;

        const auto& track = (*viewedTracks)[static_cast<size_t> (selectedState)];
        if (track.lanes.empty())
            return;

        selectedLane = juce::jlimit (0, static_cast<int> (track.lanes.size()) - 1, index);
        refreshLabels();
    }

    void refreshLabels()
    {
        syncViewedStateControls();

        for (int i = 0; i < maxTopLevelStates; ++i)
        {
            auto& button = stateButtons[static_cast<size_t> (i)];
            if (isTopLevelStatePopulated (i))
                styleButton (button, stateAccentForIndex (i));
            else
                styleEmptyStateButton (button);

            button.setEnabled (true);
            button.setToggleState (viewedTopLevelState == i, juce::dontSendNotification);
        }

        const auto hasEmptyStateSlot = firstEmptyTopLevelState() >= 0;
        newStateButton.setEnabled (hasEmptyStateSlot);
        duplicateStateButton.setEnabled (hasEmptyStateSlot && isTopLevelStatePopulated (viewedTopLevelState));
        deleteStateButton.setEnabled (isTopLevelStatePopulated (viewedTopLevelState));

        const auto* viewedTracks = getViewedTracks();
        if (viewedTracks == nullptr || viewedTracks->empty())
        {
            selectedLabel.setText ("Empty state", juce::dontSendNotification);

            for (auto& button : laneButtons)
            {
                button.setButtonText ("");
                button.setToggleState (false, juce::dontSendNotification);
                button.setEnabled (false);
                button.setVisible (false);
            }

            for (auto& button : laneSoloButtons)
            {
                button.setToggleState (false, juce::dontSendNotification);
                button.setEnabled (false);
                button.setVisible (false);
            }

            for (auto& button : laneMuteButtons)
            {
                button.setToggleState (false, juce::dontSendNotification);
                button.setEnabled (false);
                button.setVisible (false);
            }

            syncEditControls (nullptr, nullptr);
            updateLaneCodeHeader ("lane code", mutedInk().withAlpha (0.22f));
            updateLaneCodeMetadata (nullptr);
            laneCodeRunButton.setEnabled (false);
            setLaneCodeEditorText ("// Click New to create this state.");
            orbitCanvas.setState (nullptr, 0, orbitPhase, running);
            syncOverallView();
            syncTrackFocusCanvas (nullptr);
            syncMixerView();
            syncArrangementTimelineView();
            syncViewVisibility();
            syncViewButtons();
            return;
        }

        selectedState = juce::jlimit (0, static_cast<int> (viewedTracks->size()) - 1, selectedState);
        const auto& state = (*viewedTracks)[static_cast<size_t> (selectedState)];
        selectedLabel.setText (trackDisplayName (state.name), juce::dontSendNotification);
        selectedLane = juce::jlimit (0, juce::jmax (0, static_cast<int> (state.lanes.size()) - 1), selectedLane);

        for (int i = 0; i < static_cast<int> (laneButtons.size()); ++i)
        {
            auto& button = laneButtons[static_cast<size_t> (i)];
            auto& soloButton = laneSoloButtons[static_cast<size_t> (i)];
            auto& muteButton = laneMuteButtons[static_cast<size_t> (i)];

            if (i < static_cast<int> (state.lanes.size()))
            {
                const auto& lane = state.lanes[static_cast<size_t> (i)];

                button.setButtonText (juce::String (i + 1) + "  " + lane.name);
                button.setEnabled (true);
                button.setVisible (true);
                button.setToggleState (selectedLane == i, juce::dontSendNotification);

                soloButton.setEnabled (true);
                soloButton.setVisible (true);
                soloButton.setToggleState (lane.solo, juce::dontSendNotification);

                muteButton.setEnabled (true);
                muteButton.setVisible (true);
                muteButton.setToggleState (lane.muted, juce::dontSendNotification);
            }
            else
            {
                button.setButtonText ("");
                button.setEnabled (false);
                button.setVisible (false);
                button.setToggleState (false, juce::dontSendNotification);

                soloButton.setEnabled (false);
                soloButton.setVisible (false);
                soloButton.setToggleState (false, juce::dontSendNotification);

                muteButton.setEnabled (false);
                muteButton.setVisible (false);
                muteButton.setToggleState (false, juce::dontSendNotification);
            }
        }

        syncEditControls (&state, state.lanes.empty() ? nullptr : &state.lanes[static_cast<size_t> (selectedLane)]);
        updateLaneCode();
        orbitCanvas.setState (viewedTracks, selectedState, orbitPhase, running);
        syncOverallView();
        syncTrackFocusCanvas (viewedTracks);
        syncMixerView();
        syncArrangementTimelineView();
        syncViewVisibility();
        syncViewButtons();
    }

    void updateLaneCode()
    {
        if (laneCodeDirty)
            return;

        const auto* viewedTracks = getViewedTracks();
        if (viewedTracks == nullptr || viewedTracks->empty())
            return;

        const auto& state = (*viewedTracks)[static_cast<size_t> (selectedState)];
        if (state.lanes.empty())
        {
            updateLaneCodeHeader ("lane code", mutedInk().withAlpha (0.22f));
            updateLaneCodeMetadata (nullptr);
            laneCodeRunButton.setEnabled (false);
            setLaneCodeEditorText ("// This track has no lanes.");
            return;
        }

        const auto laneIndex = static_cast<size_t> (juce::jlimit (0, static_cast<int> (state.lanes.size()) - 1, selectedLane));
        updateLaneCodeMetadata (&state.lanes[laneIndex]);
        setLaneCodeEditorText (makeLaneCode (state.lanes[laneIndex], selectedLane));
        laneCodeViewedTopLevelState = viewedTopLevelState;
        laneCodeTrackIndex = selectedState;
        laneCodeLaneIndex = selectedLane;
        laneCodeDirty = false;
        laneCodeRunButton.setEnabled (false);
        laneCodeLastValidatedText = laneCodeEditor.getText();
        updateLaneCodeHeader ("lane code", mutedInk().withAlpha (0.22f));
    }

    void syncEditControls (const Wf::StateSpec* track, const Wf::LaneSpec* lane)
    {
        juce::ScopedValueSetter<bool> guard (suppressEditCallbacks, true);

        const auto hasTrack = track != nullptr;
        const auto hasLane = lane != nullptr;
        const auto probabilityControlsDuration = hasTrack && track->transitionProbabilityPercent.has_value();

        trackNameEditor.setEnabled (hasTrack);
        trackDurationEditor.setEnabled (hasTrack && ! probabilityControlsDuration);
        laneNameEditor.setEnabled (hasLane);
        laneTempoEditor.setEnabled (hasLane);
        laneDurationEditor.setEnabled (hasLane);
        laneCountEditor.setEnabled (hasTrack);
        laneCountLabel.setAlpha (hasTrack ? 1.0f : 0.34f);
        muteLaneButton.setEnabled (hasLane);
        soloLaneButton.setEnabled (hasLane);
        duplicateLaneButton.setEnabled (hasLane && track != nullptr && static_cast<int> (track->lanes.size()) < maxTrackLanes);
        deleteLaneButton.setEnabled (hasLane && track != nullptr && track->lanes.size() > 1);
        updateLaneCodeMetadata (lane);

        if (! trackNameEditor.hasKeyboardFocus (true))
            trackNameEditor.setText (hasTrack ? trackDisplayName (track->name) : juce::String(), juce::dontSendNotification);

        if (! trackDurationEditor.hasKeyboardFocus (true))
        {
            if (hasTrack && track->duration.has_value())
                trackDurationEditor.setText (formatTrackDuration (*track->duration), juce::dontSendNotification);
            else
                trackDurationEditor.setText ({}, juce::dontSendNotification);
        }

        if (! laneNameEditor.hasKeyboardFocus (true))
            laneNameEditor.setText (hasLane ? lane->name : juce::String(), juce::dontSendNotification);

        if (! laneTempoEditor.hasKeyboardFocus (true))
            laneTempoEditor.setText (hasLane && lane->tempoBpm.has_value()
                                         ? juce::String (*lane->tempoBpm, 1)
                                         : juce::String(),
                                     juce::dontSendNotification);

        if (! laneDurationEditor.hasKeyboardFocus (true))
        {
            if (hasLane && lane->duration.has_value())
                laneDurationEditor.setText (formatTrackDuration (*lane->duration), juce::dontSendNotification);
            else
                laneDurationEditor.setText ({}, juce::dontSendNotification);
        }

        if (! laneCountEditor.hasKeyboardFocus (true))
            laneCountEditor.setText (hasTrack ? juce::String (static_cast<int> (track->lanes.size())) : juce::String(), juce::dontSendNotification);

        if (hasLane)
        {
            muteLaneButton.setToggleState (lane->muted, juce::dontSendNotification);
            soloLaneButton.setToggleState (lane->solo, juce::dontSendNotification);
        }
        else
        {
            muteLaneButton.setToggleState (false, juce::dontSendNotification);
            soloLaneButton.setToggleState (false, juce::dontSendNotification);
        }
    }

    void applyTrackNameEdit()
    {
        if (suppressEditCallbacks)
            return;

        if (auto* track = getSelectedViewedTrack())
        {
            const auto nextName = trackNameEditor.getText().trim().isNotEmpty()
                ? trackDisplayName (trackNameEditor.getText().trim())
                : trackDisplayName ("Track");
            if (track->name == nextName)
                return;

            beginUndoSnapshotForTextEdit (trackNameEditor, "edit track name");
            track->name = nextName;
            refreshAfterStructureEdit (false);
        }
    }

    static bool durationsEqual (const std::optional<Wf::TrackDurationSpec>& a,
                                const std::optional<Wf::TrackDurationSpec>& b)
    {
        if (a.has_value() != b.has_value())
            return false;

        return ! a.has_value() || (a->bars == b->bars && a->beats == b->beats);
    }

    void applyTrackDurationEdit()
    {
        if (suppressEditCallbacks)
            return;

        if (auto* track = getSelectedViewedTrack())
        {
            if (track->transitionProbabilityPercent.has_value())
                return;

            const auto parsed = parseTrackDuration (trackDurationEditor.getText());
            if (durationsEqual (track->duration, parsed))
                return;

            beginUndoSnapshotForTextEdit (trackDurationEditor, "edit track duration");
            track->duration = parsed;
            refreshAfterStructureEdit (false);
        }
    }

    static juce::String formatTrackDuration (const Wf::TrackDurationSpec& duration)
    {
        return juce::String (duration.bars) + "." + juce::String (duration.beats);
    }

    static std::optional<Wf::TrackDurationSpec> parseTrackDuration (juce::String text)
    {
        text = text.trim();
        if (text.isEmpty())
            return {};

        const auto dot = text.indexOfChar ('.');
        const auto barsText = dot >= 0 ? text.substring (0, dot).trim() : text;
        const auto beatsText = dot >= 0 ? text.substring (dot + 1).trim() : juce::String();
        const auto bars = barsText.isEmpty() ? 0 : juce::jmax (0, barsText.getIntValue());
        const auto beats = beatsText.isEmpty() ? 0 : juce::jmax (0, beatsText.getIntValue());

        if (bars == 0 && beats == 0)
            return {};

        return Wf::TrackDurationSpec { juce::jmin (128, bars), juce::jmin (1024, beats) };
    }

    void applyLaneNameEdit()
    {
        if (suppressEditCallbacks)
            return;

        if (auto* lane = getSelectedViewedLane())
        {
            const auto nextName = laneNameEditor.getText().trim().isNotEmpty() ? laneNameEditor.getText().trim() : "Lane";
            if (lane->name == nextName)
                return;

            beginUndoSnapshotForTextEdit (laneNameEditor, "edit lane name");
            lane->name = nextName;
            refreshAfterStructureEdit (false);
        }
    }

    void applyLaneTempoEdit (bool reloadAudioIfNeeded)
    {
        if (suppressEditCallbacks)
            return;

        if (auto* lane = getSelectedViewedLane())
        {
            const auto text = laneTempoEditor.getText().trim();
            std::optional<float> nextTempo;
            if (text.isEmpty())
                nextTempo.reset();
            else
                nextTempo = juce::jlimit (30.0f, 220.0f, static_cast<float> (text.getDoubleValue()));

            const auto unchanged = lane->tempoBpm.has_value() == nextTempo.has_value()
                                && (! lane->tempoBpm.has_value() || std::abs (*lane->tempoBpm - *nextTempo) < 0.001f);
            if (unchanged)
                return;

            beginUndoSnapshotForTextEdit (laneTempoEditor, "edit lane bpm");
            lane->tempoBpm = nextTempo;
            refreshAfterStructureEdit (reloadAudioIfNeeded);
        }
    }

    void applyLaneDurationEdit (bool reloadAudioIfNeeded)
    {
        if (suppressEditCallbacks)
            return;

        if (auto* lane = getSelectedViewedLane())
        {
            const auto parsed = parseTrackDuration (laneDurationEditor.getText());
            if (durationsEqual (lane->duration, parsed))
                return;

            beginUndoSnapshotForTextEdit (laneDurationEditor, "edit lane duration");
            lane->duration = parsed;
            refreshAfterStructureEdit (reloadAudioIfNeeded);
        }
    }

    void toggleSelectedLaneMute()
    {
        if (auto* lane = getSelectedViewedLane())
        {
            pushUndoSnapshot ("mute lane");
            lane->muted = ! lane->muted;
            refreshAfterStructureEdit (true);
        }
    }

    void toggleSelectedLaneSolo()
    {
        if (auto* lane = getSelectedViewedLane())
        {
            pushUndoSnapshot ("solo lane");
            lane->solo = ! lane->solo;
            refreshAfterStructureEdit (true);
        }
    }

    void toggleLaneMute (int index)
    {
        auto* track = getSelectedViewedTrack();
        if (track == nullptr || index < 0 || index >= static_cast<int> (track->lanes.size()))
            return;

        selectedLane = index;
        auto& lane = track->lanes[static_cast<size_t> (index)];
        pushUndoSnapshot ("mute lane");
        lane.muted = ! lane.muted;
        refreshAfterStructureEdit (true);
    }

    void toggleLaneSolo (int index)
    {
        auto* track = getSelectedViewedTrack();
        if (track == nullptr || index < 0 || index >= static_cast<int> (track->lanes.size()))
            return;

        selectedLane = index;
        auto& lane = track->lanes[static_cast<size_t> (index)];
        pushUndoSnapshot ("solo lane");
        lane.solo = ! lane.solo;
        refreshAfterStructureEdit (true);
    }

    void setLanePhaseOffset (int index, float phaseOffset)
    {
        auto* track = getSelectedViewedTrack();
        if (track == nullptr || index < 0 || index >= static_cast<int> (track->lanes.size()))
            return;

        selectedLane = index;
        auto& lane = track->lanes[static_cast<size_t> (index)];
        const auto nextOffset = juce::jlimit (0.0f, 0.999f, phaseOffset);
        if (std::abs (lane.phaseOffsetBars - nextOffset) < 0.0005f)
            return;

        lane.phaseOffsetBars = nextOffset;
        refreshAfterStructureEdit (true);
    }

    void duplicateSelectedLane()
    {
        auto* track = getSelectedViewedTrack();
        if (track == nullptr || selectedLane < 0 || selectedLane >= static_cast<int> (track->lanes.size()) || static_cast<int> (track->lanes.size()) >= maxTrackLanes)
            return;

        pushUndoSnapshot ("duplicate lane");
        auto copy = track->lanes[static_cast<size_t> (selectedLane)];
        copy.name = copy.name + " copy";
        track->lanes.insert (track->lanes.begin() + selectedLane + 1, copy);
        ++selectedLane;
        refreshAfterStructureEdit (true);
    }

    void deleteSelectedLane()
    {
        auto* track = getSelectedViewedTrack();
        if (track == nullptr || track->lanes.size() <= 1 || selectedLane < 0 || selectedLane >= static_cast<int> (track->lanes.size()))
            return;

        pushUndoSnapshot ("delete lane");
        track->lanes.erase (track->lanes.begin() + selectedLane);
        selectedLane = juce::jlimit (0, static_cast<int> (track->lanes.size()) - 1, selectedLane);
        refreshAfterStructureEdit (true);
    }

    void refreshAfterStructureEdit (bool reloadAudioIfNeeded)
    {
        if (reloadAudioIfNeeded && viewedTopLevelState == performingTopLevelState && selectedState == performingTrackIndex)
            loadSelectedContentForCurrentState();

        markProjectDirty();
        refreshLabels();
    }

    void setLaneCodeEditorText (const juce::String& text)
    {
        juce::ScopedValueSetter<bool> guard (suppressLaneCodeCallbacks, true);
        laneCodeEditor.setText (text, juce::dontSendNotification);
        laneCodeEditor.refreshBaseTextColour();
    }

    void updateLaneCodeHeader (const juce::String& text, juce::Colour outline)
    {
        laneCodeHeader.setText (text, juce::dontSendNotification);
        laneCodeEditor.setColour (juce::TextEditor::outlineColourId, outline);
    }

    void updateLaneCodeMetadata (const Wf::LaneSpec* lane)
    {
        if (lane == nullptr)
        {
            laneCodeMetadataLabel.setText ("No lane selected", juce::dontSendNotification);
            return;
        }

        laneCodeMetadataLabel.setText (makeLaneMetadataText (*lane), juce::dontSendNotification);
    }

    static juce::String makeLaneMetadataText (const Wf::LaneSpec& lane)
    {
        juce::String text;
        text << lane.name
             << "  |  base " << Wf::chuckFloat (lane.baseHz) << " Hz"
             << "  |  vol " << Wf::chuckFloat (lane.volume)
             << "  |  pulse " << lane.pulseTicks << "/" << lane.openTicks
             << "  |  bpm " << (lane.tempoBpm.has_value() ? juce::String (*lane.tempoBpm, 1) : juce::String ("track"))
             << "  |  dur " << (lane.duration.has_value() ? formatTrackDuration (*lane.duration) : juce::String ("track"))
             << "  |  phase " << juce::String (lane.phaseOffsetBars, 3)
             << "  |  host: tick didTick stepPhase laneActive intensity bright orbit";
        return text;
    }

    static juce::String makeLaneCode (const Wf::LaneSpec& lane, int laneIndex)
    {
        juce::String code;
        code << laneDeclarationMarker << "\n";
        if (lane.customDeclarationCode.has_value())
            code << *lane.customDeclarationCode << "\n";
        else
            Wf::appendLaneDeclaration (code, lane, laneIndex);

        code << "\n" << laneControlMarker << "\n";
        if (lane.customControlCode.has_value())
            code << *lane.customControlCode << "\n";
        else
            Wf::appendLaneControl (code, lane, laneIndex);

        return code;
    }

    struct ParsedLaneCode
    {
        juce::String declaration;
        juce::String control;
    };

    static std::optional<ParsedLaneCode> parseLaneCode (juce::String text)
    {
        const auto declarationMarkerText = juce::String (laneDeclarationMarker);
        const auto controlMarkerText = juce::String (laneControlMarker);
        const auto declarationMarkerIndex = text.indexOf (declarationMarkerText);
        const auto controlMarkerIndex = text.indexOf (controlMarkerText);

        if (declarationMarkerIndex < 0 || controlMarkerIndex < 0 || controlMarkerIndex <= declarationMarkerIndex)
            return {};

        const auto declarationMarkerEnd = declarationMarkerIndex + declarationMarkerText.length();
        const auto controlMarkerEnd = controlMarkerIndex + controlMarkerText.length();
        const auto declarationLineEnd = text.substring (declarationMarkerEnd).indexOfChar ('\n');
        const auto controlLineEnd = text.substring (controlMarkerEnd).indexOfChar ('\n');

        if (declarationLineEnd < 0)
            return {};

        const auto declarationStart = declarationMarkerEnd + declarationLineEnd + 1;
        const auto controlStart = controlLineEnd < 0 ? text.length() : controlMarkerEnd + controlLineEnd + 1;
        auto declaration = text.substring (declarationStart, controlMarkerIndex).trim();
        auto control = text.substring (controlStart).trim();

        if (declaration.isEmpty() || control.isEmpty())
            return {};

        return ParsedLaneCode { declaration, control };
    }

    static bool validateLaneProgram (const Wf::StateSpec& candidateTrack, juce::String& error)
    {
        EmbeddedChucKEngine validator;
        if (! validator.prepare (48000.0, 256, 0, 2))
        {
            error = validator.getLastError();
            return false;
        }

        if (! validator.loadProgram (Wf::buildStateProgram (candidateTrack), Wf::makeWfParameterBindings()))
        {
            error = validator.getLastError();
            return false;
        }

        return true;
    }

    void markLaneCodeEdited()
    {
        if (suppressLaneCodeCallbacks)
            return;

        laneCodeDirty = true;
        laneCodeRunButton.setEnabled (true);
        updateLaneCodeHeader ("lane code - edited", amber().withAlpha (0.62f));
        refreshProjectDirtyIndicator();
    }

    void applyLaneCodeEdit()
    {
        const auto text = laneCodeEditor.getText();
        if (! laneCodeDirty && text == laneCodeLastValidatedText)
            return;

        laneCodeLastValidatedText = text;

        auto* track = getTrack (laneCodeViewedTopLevelState, laneCodeTrackIndex);
        if (track == nullptr || laneCodeLaneIndex < 0 || laneCodeLaneIndex >= static_cast<int> (track->lanes.size()))
            return;

        const auto parsed = parseLaneCode (text);
        if (! parsed.has_value())
        {
            laneCodeDirty = true;
            laneCodeRunButton.setEnabled (true);
            updateLaneCodeHeader ("lane code - invalid", coral().withAlpha (0.74f));
            return;
        }

        auto candidateTrack = *track;
        auto& candidateLane = candidateTrack.lanes[static_cast<size_t> (laneCodeLaneIndex)];
        candidateLane.customDeclarationCode = parsed->declaration;
        candidateLane.customControlCode = parsed->control;

        juce::String validationError;
        if (! validateLaneProgram (candidateTrack, validationError))
        {
            laneCodeDirty = true;
            laneCodeRunButton.setEnabled (true);
            updateLaneCodeHeader ("lane code - invalid", coral().withAlpha (0.74f));
            return;
        }

        auto& lane = track->lanes[static_cast<size_t> (laneCodeLaneIndex)];
        const auto declarationUnchanged = lane.customDeclarationCode.has_value() && *lane.customDeclarationCode == parsed->declaration;
        const auto controlUnchanged = lane.customControlCode.has_value() && *lane.customControlCode == parsed->control;
        if (declarationUnchanged && controlUnchanged)
        {
            laneCodeDirty = false;
            laneCodeRunButton.setEnabled (false);
            updateLaneCodeHeader ("lane code - live", green().withAlpha (0.58f));
            refreshProjectDirtyIndicator();
            return;
        }

        pushUndoSnapshot ("run lane code");
        lane.customDeclarationCode = parsed->declaration;
        lane.customControlCode = parsed->control;
        laneCodeDirty = false;
        laneCodeRunButton.setEnabled (false);
        updateLaneCodeHeader ("lane code - live", green().withAlpha (0.58f));
        markProjectDirty();
        syncMixerView();

        if (laneCodeViewedTopLevelState == performingTopLevelState && laneCodeTrackIndex == performingTrackIndex)
            loadSelectedContentForCurrentState();

        refreshLabels();
    }

    void applyStateTrackCountEdit()
    {
        if (suppressStateControlCallbacks)
            return;

        auto* viewedTracks = getViewedTracks();
        if (viewedTracks == nullptr)
            return;

        const auto digits = stateTrackCountEditor.getText().retainCharacters ("0123456789").trim();
        if (digits.isEmpty())
            return;

        const auto requestedCount = digits.getIntValue();
        const auto clampedCount = juce::jlimit (1, maxStateTracks, requestedCount);

        if (clampedCount != requestedCount)
            stateTrackCountEditor.setText (juce::String (clampedCount), juce::dontSendNotification);

        resizeViewedStateTracks (clampedCount);
    }

    void applyLaneCountEdit()
    {
        if (suppressEditCallbacks)
            return;

        auto* track = getSelectedViewedTrack();
        if (track == nullptr)
            return;

        const auto digits = laneCountEditor.getText().retainCharacters ("0123456789").trim();
        if (digits.isEmpty())
            return;

        const auto requestedCount = digits.getIntValue();
        const auto clampedCount = juce::jlimit (1, maxTrackLanes, requestedCount);

        if (clampedCount != requestedCount)
            laneCountEditor.setText (juce::String (clampedCount), juce::dontSendNotification);

        resizeSelectedTrackLanes (clampedCount);
    }

    void resizeViewedStateTracks (int requestedCount)
    {
        auto* viewedTracks = getViewedTracks();
        if (viewedTracks == nullptr)
            return;

        const auto targetCount = juce::jlimit (1, maxStateTracks, requestedCount);
        const auto previousCount = static_cast<int> (viewedTracks->size());
        if (targetCount == previousCount)
            return;

        pushUndoSnapshot ("change track count");
        if (targetCount > previousCount)
        {
            const auto defaults = Wf::makeDefaultStates();

            for (int trackIndex = previousCount; trackIndex < targetCount; ++trackIndex)
            {
                auto track = defaults.empty()
                    ? Wf::StateSpec { "Track " + juce::String (trackIndex + 1), 88.0, {}, Wf::TrackDurationSpec { 1, 0 }, {} }
                    : defaults[static_cast<size_t> (trackIndex % static_cast<int> (defaults.size()))];

                track.name = trackDisplayName ("Track " + juce::String (trackIndex + 1));
                track.duration = Wf::TrackDurationSpec { 1, 0 };
                track.transitionProbabilityPercent.reset();
                viewedTracks->push_back (std::move (track));
            }
        }
        else
        {
            viewedTracks->resize (static_cast<size_t> (targetCount));
        }

        selectedState = juce::jlimit (0, targetCount - 1, selectedState);

        const auto editingPerformingState = viewedTopLevelState == performingTopLevelState;
        const auto previousPerformingTrack = performingTrackIndex;

        if (editingPerformingState)
            performingTrackIndex = juce::jlimit (0, targetCount - 1, performingTrackIndex);

        if (editingPerformingState && performingTrackIndex != previousPerformingTrack)
        {
            selectedState = performingTrackIndex;
            trackElapsedBars = 0.0;
            nextBarTransitionCheck = 1.0;
            orbitPhase = 0.0f;
            loadSelectedContentForCurrentState();
        }

        markProjectDirty();
        refreshLabels();
    }

    void resizeSelectedTrackLanes (int requestedCount)
    {
        auto* track = getSelectedViewedTrack();
        if (track == nullptr)
            return;

        const auto targetCount = juce::jlimit (1, maxTrackLanes, requestedCount);
        const auto previousCount = static_cast<int> (track->lanes.size());
        if (targetCount == previousCount)
            return;

        pushUndoSnapshot ("change lane count");
        if (targetCount > previousCount)
        {
            const auto defaults = Wf::makeDefaultStates();
            const auto defaultTrack = defaults.empty()
                ? Wf::StateSpec {}
                : defaults[static_cast<size_t> (selectedState % static_cast<int> (defaults.size()))];

            for (int laneIndex = previousCount; laneIndex < targetCount; ++laneIndex)
            {
                auto lane = ! defaultTrack.lanes.empty()
                    ? defaultTrack.lanes[static_cast<size_t> (laneIndex % static_cast<int> (defaultTrack.lanes.size()))]
                    : Wf::LaneSpec { "Lane " + juce::String (laneIndex + 1), "arp", 220.0f, 0.18f, 4, 2 };

                lane.name = "Lane " + juce::String (laneIndex + 1);
                lane.muted = false;
                lane.solo = false;
                track->lanes.push_back (std::move (lane));
            }
        }
        else
        {
            track->lanes.resize (static_cast<size_t> (targetCount));
        }

        selectedLane = juce::jlimit (0, targetCount - 1, selectedLane);
        refreshAfterStructureEdit (true);
        resized();
        repaint();
    }

    void syncViewedStateControls()
    {
        juce::ScopedValueSetter<bool> guard (suppressStateControlCallbacks, true);
        const auto index = static_cast<size_t> (juce::jlimit (0, maxTopLevelStates - 1, viewedTopLevelState));
        const auto tempoBpm = topLevelTemposBpm[index];
        const auto numerator = topLevelTimeSigNumerators[index];
        const auto denominator = topLevelTimeSigDenominators[index];
        const auto populated = isTopLevelStatePopulated (viewedTopLevelState);
        const auto* viewedTracks = getViewedTracks();
        const auto trackCount = viewedTracks == nullptr ? 0 : static_cast<int> (viewedTracks->size());

        stateSettingsLabel.setText ("State " + juce::String (viewedTopLevelState + 1) + (populated ? " settings" : " empty"), juce::dontSendNotification);
        stateTempoLabel.setText ("Tempo  " + juce::String (tempoBpm, 1) + " bpm", juce::dontSendNotification);
        stateTimeSigLabel.setText ("Time signature  " + juce::String (numerator) + "/" + juce::String (denominator), juce::dontSendNotification);
        stateTrackCountEditor.setEnabled (populated);
        stateTrackCountLabel.setAlpha (populated ? 1.0f : 0.34f);
        if (! stateTrackCountEditor.hasKeyboardFocus (true))
            stateTrackCountEditor.setText (populated ? juce::String (trackCount) : juce::String(), juce::dontSendNotification);

        stateTempoSlider.setValue (tempoBpm, juce::dontSendNotification);
        timeSigNumeratorBox.setSelectedId (numerator, juce::dontSendNotification);
        timeSigDenominatorBox.setSelectedId (denominator, juce::dontSendNotification);
    }

    void timerCallback() override
    {
        const auto now = juce::Time::getMillisecondCounterHiRes();
        const auto deltaSeconds = lastTimerMs > 0.0 ? juce::jlimit (0.0, 0.1, (now - lastTimerMs) * 0.001) : 0.0;
        lastTimerMs = now;

        const auto rate = defaultPlaybackRate;
        const auto tempoBpm = getPerformingTempoBpm();

        if (running && isUsingImportedAudioPlayback())
        {
            if (! audioCallback.isImportedAudioPlaying())
            {
                running = false;
                syncTransportButtons();
            }
        }
        else if (running)
        {
            trackElapsedBars += deltaSeconds * (tempoBpm / 60.0) * static_cast<double> (rate) / getPerformingQuarterNotesPerBar();
            advanceGlobalScript (deltaSeconds, tempoBpm, rate);

            if (running)
            {
                if (const auto probabilityPercent = getPerformingTrackTransitionProbabilityPercent())
                {
                    auto didTransition = false;

                    while (running && ! didTransition && trackElapsedBars >= nextBarTransitionCheck)
                    {
                        if (shouldTakeProbabilisticTransition (*probabilityPercent))
                        {
                            advancePerformingTrack();
                            didTransition = true;
                        }
                        else
                        {
                            nextBarTransitionCheck += 1.0;
                        }
                    }

                    if (running && ! didTransition)
                        orbitPhase = static_cast<float> (std::fmod (trackElapsedBars, 1.0));
                }
                else if (const auto durationBars = getPerformingTrackDurationBars())
                {
                    orbitPhase = static_cast<float> (juce::jlimit (0.0, 1.0, trackElapsedBars / *durationBars));

                    if (trackElapsedBars >= *durationBars)
                        advancePerformingTrack();
                }
                else
                {
                    orbitPhase = static_cast<float> (std::fmod (trackElapsedBars, 1.0));
                }
            }
        }

        applyCurrentAudioControls();

        statusLabel.setText (juce::String (running ? "running  " : "stopped  ") + audioCallback.diagnostics(), juce::dontSendNotification);
        if (const auto* viewedTracks = getViewedTracks())
        {
            orbitCanvas.setState (viewedTracks, selectedState, orbitPhase, running);
            syncOverallView();
            syncTrackFocusCanvas (viewedTracks);
            syncArrangementTimelineView();
        }
    }

    void advancePerformingTrack()
    {
        const auto* performingTracks = getPerformingTracks();
        if (performingTracks == nullptr || performingTracks->empty())
            return;

        performingTrackIndex = (performingTrackIndex + 1) % static_cast<int> (performingTracks->size());
        trackElapsedBars = 0.0;
        nextBarTransitionCheck = 1.0;
        orbitPhase = 0.0f;

        if (viewedTopLevelState == performingTopLevelState)
            selectedState = performingTrackIndex;

        loadSelectedContentForCurrentState();
        refreshLabels();
    }

    void advanceGlobalScript (double deltaSeconds, double tempoBpm, float rate)
    {
        if (! scriptRunning || scriptStepIndex >= globalScriptSteps.size())
            return;

        scriptStepElapsedBars += deltaSeconds * (tempoBpm / 60.0) * static_cast<double> (rate) / getPerformingQuarterNotesPerBar();

        if (scriptStepElapsedBars < globalScriptSteps[scriptStepIndex].bars)
            return;

        ++scriptStepIndex;
        scriptStepElapsedBars = 0.0;

        if (scriptStepIndex >= globalScriptSteps.size())
        {
            scriptRunning = false;

            if (scriptShouldStopAtEnd)
            {
                stopMainTransport();
            }

            return;
        }

        applyGlobalScriptStep (globalScriptSteps[scriptStepIndex]);
    }

    float getPerformingTempoBpm() const
    {
        if (scriptRunning && scriptStepIndex < globalScriptSteps.size() && globalScriptSteps[scriptStepIndex].tempoBpm.has_value())
            return *globalScriptSteps[scriptStepIndex].tempoBpm;

        return topLevelTemposBpm[static_cast<size_t> (juce::jlimit (0, maxTopLevelStates - 1, performingTopLevelState))];
    }

    double getPerformingQuarterNotesPerBar() const
    {
        const auto index = static_cast<size_t> (juce::jlimit (0, maxTopLevelStates - 1, performingTopLevelState));
        const auto numerator = static_cast<double> (scriptRunning
                                                        && scriptStepIndex < globalScriptSteps.size()
                                                        && globalScriptSteps[scriptStepIndex].timeSigNumerator.has_value()
                                                    ? *globalScriptSteps[scriptStepIndex].timeSigNumerator
                                                    : topLevelTimeSigNumerators[index]);
        const auto denominator = static_cast<double> (juce::jmax (1, scriptRunning
                                                                      && scriptStepIndex < globalScriptSteps.size()
                                                                      && globalScriptSteps[scriptStepIndex].timeSigDenominator.has_value()
                                                                  ? *globalScriptSteps[scriptStepIndex].timeSigDenominator
                                                                  : topLevelTimeSigDenominators[index]));
        return numerator * 4.0 / denominator;
    }

    double getPerformingBeatsPerBar() const
    {
        const auto index = static_cast<size_t> (juce::jlimit (0, maxTopLevelStates - 1, performingTopLevelState));
        return static_cast<double> (scriptRunning
                                        && scriptStepIndex < globalScriptSteps.size()
                                        && globalScriptSteps[scriptStepIndex].timeSigNumerator.has_value()
                                    ? *globalScriptSteps[scriptStepIndex].timeSigNumerator
                                    : topLevelTimeSigNumerators[index]);
    }

    std::optional<double> getPerformingTrackDurationBars() const
    {
        const auto* performingTracks = getPerformingTracks();
        if (performingTracks == nullptr || performingTracks->empty())
            return {};

        const auto index = static_cast<size_t> (juce::jlimit (0, static_cast<int> (performingTracks->size()) - 1, performingTrackIndex));
        if (! (*performingTracks)[index].duration.has_value())
            return {};

        const auto duration = *(*performingTracks)[index].duration;
        const auto beatsPerBar = getPerformingBeatsPerBar();
        const auto totalBars = static_cast<double> (duration.bars)
                             + (static_cast<double> (duration.beats) / beatsPerBar);

        if (totalBars <= 0.0)
            return {};

        return juce::jmax (0.25, totalBars);
    }

    std::optional<int> getPerformingTrackTransitionProbabilityPercent() const
    {
        const auto* performingTracks = getPerformingTracks();
        if (performingTracks == nullptr || performingTracks->empty())
            return {};

        const auto index = static_cast<size_t> (juce::jlimit (0, static_cast<int> (performingTracks->size()) - 1, performingTrackIndex));
        return (*performingTracks)[index].transitionProbabilityPercent;
    }

    bool shouldTakeProbabilisticTransition (int probabilityPercent)
    {
        if (probabilityPercent <= 0)
            return false;

        if (probabilityPercent >= 100)
            return true;

        std::uniform_int_distribution<int> distribution (1, 100);
        return distribution (random) <= probabilityPercent;
    }

    void applyGlobalScriptStep (const GlobalScriptStep& step)
    {
        setPerformingTopLevelState (step.stateIndex, true);
    }

    bool isTopLevelStatePopulated (int index) const
    {
        return index >= 0
            && index < maxTopLevelStates
            && topLevelStates[static_cast<size_t> (index)].has_value()
            && ! topLevelStates[static_cast<size_t> (index)]->empty();
    }

    const std::vector<Wf::StateSpec>* getViewedTracks() const
    {
        if (! isTopLevelStatePopulated (viewedTopLevelState))
            return nullptr;

        return &*topLevelStates[static_cast<size_t> (viewedTopLevelState)];
    }

    std::vector<Wf::StateSpec>* getViewedTracks()
    {
        if (! isTopLevelStatePopulated (viewedTopLevelState))
            return nullptr;

        return &*topLevelStates[static_cast<size_t> (viewedTopLevelState)];
    }

    Wf::StateSpec* getSelectedViewedTrack()
    {
        auto* viewedTracks = getViewedTracks();
        if (viewedTracks == nullptr || viewedTracks->empty())
            return nullptr;

        selectedState = juce::jlimit (0, static_cast<int> (viewedTracks->size()) - 1, selectedState);
        return &(*viewedTracks)[static_cast<size_t> (selectedState)];
    }

    Wf::LaneSpec* getSelectedViewedLane()
    {
        auto* track = getSelectedViewedTrack();
        if (track == nullptr || track->lanes.empty())
            return nullptr;

        selectedLane = juce::jlimit (0, static_cast<int> (track->lanes.size()) - 1, selectedLane);
        return &track->lanes[static_cast<size_t> (selectedLane)];
    }

    const std::vector<Wf::StateSpec>* getPerformingTracks() const
    {
        if (! isTopLevelStatePopulated (performingTopLevelState))
            return nullptr;

        return &*topLevelStates[static_cast<size_t> (performingTopLevelState)];
    }

    int getViewedTrackCount() const
    {
        if (const auto* viewedTracks = getViewedTracks())
            return static_cast<int> (viewedTracks->size());

        return 1;
    }

    int getPerformingTrackCount() const
    {
        if (const auto* performingTracks = getPerformingTracks())
            return static_cast<int> (performingTracks->size());

        return 1;
    }

    float getCurrentMasterGain() const
    {
        return running ? static_cast<float> (gainSlider.getValue()) : 0.0f;
    }

    float getCurrentTempoHz() const
    {
        if (getPerformingTracks() == nullptr)
            return 1.0f;

        return static_cast<float> ((getPerformingTempoBpm() / 60.0) * defaultPlaybackRate);
    }

    void loadSelectedContentForCurrentState()
    {
        if (isUsingImportedAudioPlayback())
            return;

        const auto* performingTracks = getPerformingTracks();
        if (performingTracks == nullptr || performingTracks->empty())
            return;

        performingTrackIndex = juce::jlimit (0, static_cast<int> (performingTracks->size()) - 1, performingTrackIndex);
        auto audioTrack = (*performingTracks)[static_cast<size_t> (performingTrackIndex)];
        audioTrack.clockBeatsPerBar = static_cast<int> (getPerformingBeatsPerBar());
        audioTrack.clockQuarterNotesPerBar = getPerformingQuarterNotesPerBar();
        static_cast<void> (audioCallback.loadStateWithControls (audioTrack,
                                                                getCurrentMasterGain(),
                                                                getCurrentTempoHz(),
                                                                defaultIntensity,
                                                                defaultBrightness,
                                                                orbitPhase));
    }

    void applyCurrentAudioControls()
    {
        if (isUsingImportedAudioPlayback())
        {
            audioCallback.setImportedMasterGain (getCurrentMasterGain());
            return;
        }

        if (getPerformingTracks() == nullptr)
            return;

        audioCallback.setControls (getCurrentMasterGain(),
                                   getCurrentTempoHz(),
                                   defaultIntensity,
                                   defaultBrightness,
                                   orbitPhase);
    }

    MinimalLookAndFeel minimalLookAndFeel;
    WfAudioCallback audioCallback;
    juce::AudioDeviceManager audioDeviceManager;
    juce::AudioPluginFormatManager pluginFormatManager;
    juce::KnownPluginList knownPluginList;
    std::thread pluginScanThread;
    std::atomic<bool> pluginScanAbortRequested { false };
    std::unique_ptr<juce::FileChooser> renderChooser;
    std::unique_ptr<juce::FileChooser> projectChooser;
    juce::File currentProjectFile;
    juce::File loadingProjectFile;
    std::array<std::optional<std::vector<Wf::StateSpec>>, maxTopLevelStates> topLevelStates;
    std::array<Wf::TrackEffectSlotSpec, maxTrackEffectSlots> masterEffectSlots {};
    std::vector<ImportedLaneAudioClip> importedLaneAudioClips;
    std::mt19937 random { 0x5eed1234u };

    juce::Label titleLabel;
    juce::Label statusLabel;
    juce::Label volumeLabel;
    juce::Label stateSettingsLabel;
    juce::Label stateTempoLabel;
    juce::Label stateTimeSigLabel;
    juce::Label stateTrackCountLabel;
    juce::Label stateCodeHeader;
    juce::Label laneCodeHeader;
    juce::Label trackSectionLabel;
    juce::Label trackNameLabel;
    juce::Label trackDurationLabel;
    juce::Label laneSectionLabel;
    juce::Label laneNameLabel;
    juce::Label laneTempoLabel;
    juce::Label laneDurationLabel;
    juce::Label laneCountLabel;
    juce::Label selectedLabel;
    juce::Label laneHeader;
    juce::Component laneListContent;
    juce::Viewport laneListViewport;
    std::array<juce::TextButton, maxTrackLanes> laneButtons;
    std::array<juce::TextButton, maxTrackLanes> laneSoloButtons;
    std::array<juce::TextButton, maxTrackLanes> laneMuteButtons;

    OrbitCanvas orbitCanvas;
    OverallCanvas overallCanvas;
    TrackFocusCanvas trackFocusCanvas;
    TrackFocusDivider trackFocusDivider;
    CodeViewDivider codeViewDivider;
    MixerCanvas mixerCanvas;
    juce::Viewport mixerViewport;
    ArrangementTimelineCanvas arrangementTimelineCanvas;
    juce::Viewport arrangementTimelineViewport;
    std::array<juce::TextButton, maxTopLevelStates> stateButtons;
    juce::TextButton runScriptButton;
    juce::TextButton newStateButton;
    juce::TextButton duplicateStateButton;
    juce::TextButton deleteStateButton;
    juce::TextButton overallButton;
    juce::TextButton arrangementButton;
    juce::TextButton codeViewButton;
    juce::TextButton timelineViewButton;
    juce::TextButton mixerViewButton;
    juce::TextButton removeRenderedAudioButton;
    juce::TextButton arrangementPointerToolButton;
    juce::TextButton arrangementScissorsToolButton;
    juce::TextButton arrangementFadeToolButton;
    juce::TextButton splitImportedRegionButton;
    juce::TextButton deleteImportedRegionButton;
    juce::TextButton muteLaneButton;
    juce::TextButton soloLaneButton;
    juce::TextButton duplicateLaneButton;
    juce::TextButton deleteLaneButton;
    juce::TextButton laneCodeRunButton;
    juce::Slider gainSlider;
    juce::Slider stateTempoSlider;
    juce::Label arrangementHorizontalZoomLabel;
    juce::Slider arrangementHorizontalZoomSlider;
    juce::Label arrangementVerticalZoomLabel;
    juce::Slider arrangementVerticalZoomSlider;
    juce::Label regionInspectorLabel;
    juce::Label regionStartLabel;
    juce::Label regionLengthLabel;
    juce::Label regionSourceLabel;
    juce::Label regionGainLabel;
    juce::Label regionFadeInLabel;
    juce::Label regionFadeOutLabel;
    juce::Label regionFadeInCurveLabel;
    juce::Label regionFadeOutCurveLabel;
    juce::Label laneCodeMetadataLabel;
    juce::ComboBox timeSigNumeratorBox;
    juce::ComboBox timeSigDenominatorBox;
    juce::ComboBox regionFadeInCurveBox;
    juce::ComboBox regionFadeOutCurveBox;
    CodeTextEditor globalScriptEditor;
    CodeTextEditor laneCodeEditor;
    juce::TextEditor trackNameEditor;
    juce::TextEditor trackDurationEditor;
    juce::TextEditor laneNameEditor;
    juce::TextEditor laneTempoEditor;
    juce::TextEditor laneDurationEditor;
    juce::TextEditor laneCountEditor;
    juce::TextEditor stateTrackCountEditor;
    juce::TextEditor regionStartEditor;
    juce::TextEditor regionLengthEditor;
    juce::TextEditor regionSourceEditor;
    juce::TextEditor regionGainEditor;
    juce::TextEditor regionFadeInEditor;
    juce::TextEditor regionFadeOutEditor;
    std::array<float, maxTopLevelStates> topLevelTemposBpm = defaultTopLevelTempos();
    std::array<int, maxTopLevelStates> topLevelTimeSigNumerators = defaultTopLevelTimeSigNumerators();
    std::array<int, maxTopLevelStates> topLevelTimeSigDenominators = defaultTopLevelTimeSigDenominators();

    int viewedTopLevelState = 0;
    int performingTopLevelState = 0;
    int selectedState = 0;
    int focusedTrackIndex = 0;
    int performingTrackIndex = 0;
    int selectedLane = 0;
    int trackFocusCodePaneWidthPx = defaultTrackFocusCodePaneWidth;
    int trackFocusCodePaneDragStartWidth = defaultTrackFocusCodePaneWidth;
    std::vector<GlobalScriptStep> globalScriptSteps;
    std::vector<ProjectUndoSnapshot> undoStack;
    std::vector<ProjectUndoSnapshot> redoStack;
    ObjectClipboardKind objectClipboardKind = ObjectClipboardKind::none;
    std::optional<std::vector<Wf::StateSpec>> copiedTopLevelState;
    std::optional<Wf::StateSpec> copiedTrack;
    std::optional<Wf::LaneSpec> copiedLane;
    std::vector<ImportedRegionClipboardItem> copiedImportedRegions;
    juce::TextEditor* activeUndoTextEditor = nullptr;
    size_t scriptStepIndex = 0;
    double scriptStepElapsedBars = 0.0;
    bool scriptRunning = false;
    bool scriptShouldStopAtEnd = false;
    bool arrangementPlusMode = false;
    ArrangementAudioTool arrangementAudioTool = ArrangementAudioTool::pointer;
    ArrangementAudioSelection selectedImportedRegion;
    std::vector<ArrangementAudioSelection> selectedImportedRegions;
    double arrangementEditPlayheadSeconds = 0.0;
    double arrangementHorizontalZoom = 1.0;
    double arrangementVerticalZoom = 1.0;
    bool suppressStateControlCallbacks = false;
    bool suppressEditCallbacks = false;
    bool suppressLaneCodeCallbacks = false;
    bool suppressRegionInspectorCallbacks = false;
    bool suppressProjectDirty = false;
    bool suppressProjectUndo = false;
    bool importedFadeDragUndoStarted = false;
    bool importedTrimDragUndoStarted = false;
    bool importedGainDragUndoStarted = false;
    bool laneCodeDirty = false;
    bool projectDirty = false;
    bool pluginScanInProgress = false;
    MainView mainView = MainView::arrangement;
    juce::String laneCodeLastValidatedText;
    int laneCodeViewedTopLevelState = -1;
    int laneCodeTrackIndex = -1;
    int laneCodeLaneIndex = -1;
    int codeViewSplitDragStartHeightPx = 0;
    int missingImportedAudioOnLastLoad = 0;
    float codeViewStatePaneRatio = 1.0f / 3.0f;
    float orbitPhase = 0.0f;
    double trackElapsedBars = 0.0;
    double nextBarTransitionCheck = 1.0;
    bool running = false;
    static constexpr double minImportedRegionLengthSeconds = 0.025;
    double lastTimerMs = 0.0;
};

class WfApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "ChucK-ME"; }
    const juce::String getApplicationVersion() override { return "0.1.11"; }
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
        if (mainWindow != nullptr)
        {
            mainWindow->requestCloseWithPrompt();
            return;
        }

        quit();
    }

private:
    class MainWindow final : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (juce::String name)
            : DocumentWindow (std::move (name), juce::Colour (0xff101813), DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            mainComponent = new MainComponent();
            setContentOwned (mainComponent, true);
            juce::MenuBarModel::setMacMainMenu (mainComponent);
            centreWithSize (getWidth(), getHeight());
            setResizable (true, true);
            setVisible (true);
        }

        ~MainWindow() override
        {
            juce::MenuBarModel::setMacMainMenu (nullptr);
        }

        void closeButtonPressed() override
        {
            requestCloseWithPrompt();
        }

        void requestCloseWithPrompt()
        {
            if (mainComponent == nullptr)
            {
                juce::JUCEApplication::getInstance()->quit();
                return;
            }

            mainComponent->confirmQuitThen ([] { juce::JUCEApplication::getInstance()->quit(); });
        }

    private:
        MainComponent* mainComponent = nullptr;
    };

    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION (WfApplication)
