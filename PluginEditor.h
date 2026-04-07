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

    // bypass 触发区域（按基准尺寸布局，resized() 里会按比例缩放）
    static constexpr int bypassX = 600;
    static constexpr int bypassY = 433;
    static constexpr int bypassW = 65;
    static constexpr int bypassH = 37;

    // 遥控器（按基准尺寸布局，超出编辑器边界部分自动裁剪）
    static constexpr int remoteX = 232;
    static constexpr int remoteY = 471;
    static constexpr int remoteW = 168;
    static constexpr int remoteH = 488;
    static constexpr int remoteLift = 342; // 拿出来时向上平移

    // 遥控器上的 bypass 按钮（相对遥控器图片左上角的偏移，按基准尺寸）
    // 用户给的基准坐标：左上角(305,416) 大小 68*10（此坐标对应“遥控器被拿出来”后的界面位置）
    static constexpr int remoteBypassOffsetX = 73;  // 305 - remoteX
    static constexpr int remoteBypassOffsetY = 287; // 416 - (remoteY - remoteLift)
    static constexpr int remoteBypassW = 68;
    static constexpr int remoteBypassH = 10;

    juce::ComponentBoundsConstrainer resizeConstrainer;
    juce::Image tvImage;
    juce::Image bypassImage;
    juce::Image remoteImage;

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

    class BypassHitArea final : public juce::Component
    {
    public:
        explicit BypassHitArea (LDSJvstAudioProcessorEditor& ownerEditor)
            : owner (ownerEditor)
        {
            setMouseCursor(juce::MouseCursor::PointingHandCursor);
        }

        void paint (juce::Graphics& g) override;
        void mouseUp (const juce::MouseEvent&) override;

    private:
        LDSJvstAudioProcessorEditor& owner;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BypassHitArea)
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
    BypassHitArea bypassHitArea { *this };
    TvOverlayComponent tvOverlay { tvImage };
    juce::OwnedArray<IndicatorLight> presetLights;

    class RemoteControlOverlay final : public juce::Component
                                      , private juce::Timer
    {
    public:
        explicit RemoteControlOverlay (LDSJvstAudioProcessorEditor& ownerEditor)
            : owner (ownerEditor)
        {
            setInterceptsMouseClicks(false, false);
            owner.addMouseListener(this, true);
        }

        ~RemoteControlOverlay() override
        {
            owner.removeMouseListener(this);
        }

        void paint (juce::Graphics& g) override;

        void beginAnimation() { startTimerHz(60); }

    private:
        void timerCallback() override;
        void mouseDown (const juce::MouseEvent& e) override;
        void mouseUp (const juce::MouseEvent& e) override;

        int remotePressedButtonIndex = -1;
        double remotePressedStartSeconds = 0.0;
        static constexpr double remotePressedDurationSeconds = 0.11;

        LDSJvstAudioProcessorEditor& owner;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RemoteControlOverlay)
    };

    RemoteControlOverlay remoteOverlay { *this };
    bool remotePulledOut = false; // 目标状态：true=拿出来，false=收回
    void setRemotePulledOut (bool shouldBePulledOut);

    // 遥控器动画（0=原位置，1=完全上移 remoteLift）
    float remotePullAmount = 0.0f;
    bool remotePullAnimating = false;
    float remotePullAnimFrom = 0.0f;
    float remotePullAnimTo = 0.0f;
    double remotePullAnimStartSeconds = 0.0;
    static constexpr double remotePullAnimDurationSeconds = 0.18;
    float getRemotePullAmount() const noexcept { return remotePullAmount; }

    // bypass 动画状态（电视开关机风格）
    bool bypassTransitionActive = false;
    bool bypassTransitionToOn = false;
    double bypassTransitionStartSeconds = 0.0;
    static constexpr double bypassTransitionDurationSeconds = 0.22; // 更快一点：越短越“啪”

    void toggleBypassFromUI();
    bool isBypassedOrTransitioningToBypass() const noexcept;
    float getBypassTransitionT() noexcept;

    // 预设切换动画状态（类似换台/调谐的过渡）
    bool presetTransitionActive = false;
    int presetTransitionFrom = 0;
    int presetTransitionTo = 0;
    double presetTransitionStartSeconds = 0.0;
    static constexpr double presetTransitionDurationSeconds = 0.26;

    float getPresetTransitionT() noexcept;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LDSJvstAudioProcessorEditor)
};