#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include <vector>
#include <deque>

class LDSJvstAudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    LDSJvstAudioProcessorEditor(LDSJvstAudioProcessor&);
    ~LDSJvstAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void paintOverChildren(juce::Graphics&) override;
    void resized() override;

private:
    static constexpr int baseEditorWidth  = 1000;
    static constexpr int baseEditorHeight = 750;

    // 电视机屏幕显示区域（按基准尺寸布局，resized() 里会按比例缩放）
    // 正确位置：左上角(130,135)，大小 606*466
    static constexpr int screenX = 130;
    static constexpr int screenY = 135;
    static constexpr int screenW = 606;
    static constexpr int screenH = 466;

    static constexpr float editorAspectRatio = (float) baseEditorWidth / (float) baseEditorHeight;

    static constexpr int presetCount = 12;
    static constexpr int presetLightX = 829;
    static constexpr int presetLightY = 157;
    static constexpr int presetLightSize = 17;
    static constexpr int presetLightGap = 10;

    // bypass 触发区域（按基准尺寸布局，resized() 里会按比例缩放）
    static constexpr int bypassX = 857;
    static constexpr int bypassY = 619;
    static constexpr int bypassW = 93;
    static constexpr int bypassH = 53;

    // 遥控器（按基准尺寸布局，超出编辑器边界部分自动裁剪）
    static constexpr int remoteX = 782;
    static constexpr int remoteY = 684;
    static constexpr int remoteW = 168;
    static constexpr int remoteH = 488;
    static constexpr int remoteLift = 415; // 拿出来时向上平移（684 -> 269）

    // 遥控器上的 bypass 按钮（相对遥控器图片左上角的偏移，按基准尺寸）
    // 用户给的基准坐标：左上角(305,416) 大小 68*10（此坐标对应“遥控器被拿出来”后的界面位置）
    static constexpr int remoteBypassOffsetX = 73;  // 305 - remoteX
    static constexpr int remoteBypassOffsetY = 287; // 416 - (remoteY - remoteLift)
    static constexpr int remoteBypassW = 68;
    static constexpr int remoteBypassH = 10;

    // 临时 Q 输入框（底部区域，按基准尺寸）
    static constexpr int tempQLabelX = 145;
    static constexpr int tempQLabelY = 708;
    static constexpr int tempQLabelW = 130;
    static constexpr int tempQLabelH = 24;
    static constexpr int tempQInputX = 280;
    static constexpr int tempQInputY = 706;
    static constexpr int tempQInputW = 120;
    static constexpr int tempQInputH = 28;

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

            if (owner.processor.bypassed.load(std::memory_order_acquire))
            {
                g.setColour(juce::Colours::black.withAlpha(0.75f));
                g.fillEllipse(b);
                g.setColour(juce::Colours::black.withAlpha(0.35f));
                g.drawEllipse(b, 1.0f);
                return;
            }

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

        void shutdownForEditorTeardown() { stopTimer(); }

        void paint(juce::Graphics&) override;

    private:
        void timerCallback() override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp   (const juce::MouseEvent&) override;

        LDSJvstAudioProcessorEditor& owner;
        LDSJvstAudioProcessor& processor;
        juce::Array<float> samples;
        juce::Array<uint8_t> lossMaskSnapshotUI;

        static constexpr int kBandGridCount = LDSJvstAudioProcessor::kLossBandCountForUI;

        // 限制器线拖拽
        bool limiterDragActive = false;
        float limiterDragStartThreshold = 1.0f;

        // 频段灯带高低切三角拖拽
        int cutHandleDragMode = 0; // 0=none, 1=lowCut, 2=highCut

        // 屏幕离屏缓冲：用于做“按行 remap”的电视机行同步噪声扭曲
        juce::Image screenBase;
        juce::Image screenWarp;
        int screenBufferW = 0;
        int screenBufferH = 0;

        // 扫描行噪声（每行一个偏移），带窗口平均 + 时间平滑
        std::vector<float> scanlineNoiseRaw;
        std::vector<float> scanlineNoiseSmoothed;
        std::vector<float> scanlineOffsetPx;

        // 预设 3：视觉暂留（把上一帧内容衰减并向右平移，形成拖影）
        juce::Image preset3TrailA;
        juce::Image preset3TrailB;
        bool preset3TrailFlip = false;
        double preset3TrailLastSec = 0.0;

        struct Preset3TrailItem
        {
            juce::Path path;
            double tSec = 0.0;
        };
        std::deque<Preset3TrailItem> preset3Trail;

        // 预设 8 / 10：同款拖影（时间窗内逐渐变黑），但不移动
        double preset8TrailLastSec = 0.0;
        std::deque<Preset3TrailItem> preset8Trail;
        double preset10TrailLastSec = 0.0;
        std::deque<Preset3TrailItem> preset10Trail;

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
            setInterceptsMouseClicks(true, true);
        }

        ~RemoteControlOverlay() override = default;

        void shutdownForEditorTeardown()
        {
            stopTimer();
            volumeRepeatActive = false;
            volumeRepeatDir = 0;
        }

        bool hitTest (int x, int y) override

        {
            // 遥控器“拿出来/动画中”时：作为一个 modal 覆盖层，阻止底下的按钮/指示灯被误触。
            if (owner.remotePulledOut || owner.getRemotePullAmount() > 0.001f)
                return true;

            // 收回状态：只在遥控器区域拦截点击（用于点一下把遥控器拿出来）。
            const float scale = (float) getWidth() / (float) baseEditorWidth;
            const float rx = remoteX * scale;
            const float ry = (remoteY - remoteLift * owner.getRemotePullAmount()) * scale;
            const float rw = remoteW * scale;
            const float rh = remoteH * scale;
            return juce::Rectangle<float>(rx, ry, rw, rh).contains((float) x, (float) y);
        }

        void paint (juce::Graphics& g) override;

        void beginAnimation() { startTimerHz(60); }

    private:
        void timerCallback() override;
        void mouseDown (const juce::MouseEvent& e) override;
        void mouseUp (const juce::MouseEvent& e) override;

        int remotePressedButtonIndex = -1;

        // VOL+/VOL- 按住自动连发（模拟电视机按住持续调节）
        bool volumeRepeatActive = false;
        int volumeRepeatDir = 0; // +1 = VOL+，-1 = VOL-
        double volumeRepeatPressSeconds = 0.0;
        double volumeRepeatLastStepSeconds = 0.0;
        static constexpr double volumeRepeatInitialDelaySeconds = 0.32;
        static constexpr double volumeRepeatIntervalSeconds = 0.075;

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

    // 前置增益（遥控器 VOL+/VOL-）+ 电视屏幕 OSD 控制条
    void nudgePreGainDbFromUI (float deltaDb);
    void toggleLossAlgorithmFromUI(); // 遥控器 ST/SAP：在两种频带丢失算法间切换
    void cycleCutModeOrSlopeFromTV(); // 遥控器 TV：硬裁剪 <-> HPF/LPF，且在HPF/LPF内轮换12/24/48dB
    float getVolumeOsdT() noexcept; // 0..1（时间进度），0 表示不显示
    float getModeOsdT() noexcept;   // 0..1（TV/ST-SAP 状态时间进度），0 表示不显示

    void pushChannelDigitFromRemote (int digit); // 遥控器 0-9：频道输入（最多4位）
    juce::String getChannelOsdText() noexcept;   // 屏幕频道数字文案（固定4位）
    void triggerChannelJumpNow();                // 将当前输入提交为频道数字显示

    // 临时测试控件：输入 Notch Q。
    // 若要直接屏蔽该控件，把这里改成 false 即可。
    static constexpr bool kEnableTempQInput = false;
    void refreshTempNotchQInputText();
    void applyTempNotchQFromInput();

    juce::Label tempNotchQLabel;
    juce::TextEditor tempNotchQInput;

    bool volumeOsdActive = false;
    double volumeOsdStartSeconds = 0.0;
    static constexpr double volumeOsdDurationSeconds = 3.0;

    bool modeOsdActive = false;
    double modeOsdStartSeconds = 0.0;
    static constexpr double modeOsdDurationSeconds = 3.0;

    // 遥控器频道输入（仅屏幕显示，不关联实际音频/预设逻辑）
    juce::String pendingChannelDigits;
    double pendingChannelLastInputSeconds = 0.0;
    bool channelOsdActive = false;
    juce::String channelDisplayText;
    double channelOsdStartSeconds = 0.0;

    static constexpr int kMaxChannelDigits = 4;
    static constexpr double channelInputTimeoutSeconds = 3.0;
    static constexpr double channelInputOsdDurationSeconds = 1.2;
    static constexpr double presetChannelOsdDurationSeconds = 3.0;
    double channelOsdCurrentDurationSeconds = channelInputOsdDurationSeconds;

    bool editorShuttingDown = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LDSJvstAudioProcessorEditor)
};