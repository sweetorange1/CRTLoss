#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

class LDSJvstAudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    LDSJvstAudioProcessorEditor(LDSJvstAudioProcessor&);
    ~LDSJvstAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    static constexpr int baseEditorWidth  = 700;
    static constexpr int baseEditorHeight = 525;

    static constexpr int screenX = 75;
    static constexpr int screenY = 84;
    static constexpr int screenW = 450;
    static constexpr int screenH = 370;

    static constexpr float editorAspectRatio = (float) baseEditorWidth / (float) baseEditorHeight;

    static constexpr int presetCount = 12;
    static constexpr int presetLightX = 580;
    static constexpr int presetLightY = 110;
    static constexpr int presetLightSize = 12;
    static constexpr int presetLightGap = 7;

    juce::ComponentBoundsConstrainer resizeConstrainer;
    juce::Image tvImage;

    int selectedPresetIndex = 0;
    void setSelectedPresetIndex (int newIndex);
    int getSelectedPresetIndex() const noexcept { return selectedPresetIndex; }

    class IndicatorLight final : public juce::Component
    {
    public:
        IndicatorLight (LDSJvstAudioProcessorEditor& ownerEditor, int presetIdx)
            : owner (ownerEditor), index (presetIdx)
        {
            setMouseCursor(juce::MouseCursor::PointingHandCursor);
        }

        void paint (juce::Graphics& g) override
        {
            auto b = getLocalBounds().toFloat().reduced(1.0f);

            const bool isOn = (owner.getSelectedPresetIndex() == index);

            g.setColour(juce::Colours::black.withAlpha(0.55f));
            g.fillEllipse(b);

            g.setColour(isOn ? juce::Colours::lime.withAlpha(0.95f)
                             : juce::Colours::lime.darker(0.8f).withAlpha(0.35f));
            g.fillEllipse(b.reduced(1.0f));

            g.setColour(juce::Colours::white.withAlpha(isOn ? 0.25f : 0.08f));
            g.drawEllipse(b, 1.0f);
        }

        void mouseUp (const juce::MouseEvent&) override
        {
            owner.setSelectedPresetIndex(index);
        }

    private:
        LDSJvstAudioProcessorEditor& owner;
        const int index;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(IndicatorLight)
    };

    class TvOverlayComponent final : public juce::Component
    {
    public:
        explicit TvOverlayComponent (const juce::Image& img)
            : image (img)
        {
            setInterceptsMouseClicks(false, false);
        }

        void paint (juce::Graphics& g) override
        {
            if (! image.isValid())
                return;

            g.drawImageWithin(image,
                              0, 0, getWidth(), getHeight(),
                              juce::RectanglePlacement::stretchToFit,
                              false);
        }

    private:
        const juce::Image& image;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TvOverlayComponent)
    };

    class OscilloscopeComponent final : public juce::Component,
                                        private juce::Timer
    {
    public:
        OscilloscopeComponent(LDSJvstAudioProcessorEditor& ownerEditor, LDSJvstAudioProcessor&);

        void paint(juce::Graphics&) override;

    private:
        void timerCallback() override;

        LDSJvstAudioProcessorEditor& owner;
        LDSJvstAudioProcessor& processor;
        juce::Array<float> samples;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OscilloscopeComponent)
    };

    LDSJvstAudioProcessor& processor;
    OscilloscopeComponent oscilloscope;
    juce::TextButton bypassButton { "Bypass" };
    TvOverlayComponent tvOverlay { tvImage };
    juce::OwnedArray<IndicatorLight> presetLights;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LDSJvstAudioProcessorEditor)
};