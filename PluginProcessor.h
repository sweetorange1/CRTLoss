#pragma once

#include <JuceHeader.h>
#include "display_present.h"
#include <array>
#include <atomic>
#include <vector>
#include <memory>

class LDSJvstAudioProcessor : public juce::AudioProcessor
{
public:
    LDSJvstAudioProcessor();
    ~LDSJvstAudioProcessor() override;

    // AudioProcessor overrides
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;
    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int index, const juce::String& newName) override;

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // 提供给界面线程使用：获取示波器数据快照（按时间从旧到新排列）
    void getOscilloscopeSnapshot(juce::Array<float>& dest);

    // 提供给界面线程使用：获取当前 100 频段保留/丢失掩码快照（1=允许通过，0=被滤掉）
    void getLossMaskSnapshot(juce::Array<uint8_t>& dest);

    static constexpr int kLossBandCountForUI = 100;

    // 显示频道（由界面设置，宿主保存工程时需要持久化）
    // 0..11 使用固定预设；12..9999 使用由频道ID确定的衍生预设
    static constexpr int kDisplayChannelMin = 0;
    static constexpr int kDisplayChannelMax = display_present::kDerivedChannelMax;

    int getDisplayPresetIndex() const noexcept { return displayPresetIndex.load(std::memory_order_relaxed); }
    void setDisplayPresetIndex(int newIndex) noexcept
    {
        displayPresetIndex.store(juce::jlimit(kDisplayChannelMin, kDisplayChannelMax, newIndex), std::memory_order_relaxed);
    }

    // 前置增益（dB）：默认 +10dB，范围 -5..+24
    static constexpr float kPreGainDbMin = -5.0f;
    static constexpr float kPreGainDbMax = 24.0f;

    float getPreGainDb() const noexcept { return preGainDb.load(std::memory_order_relaxed); }
    void setPreGainDb(float db) noexcept
    {
        preGainDb.store(juce::jlimit(kPreGainDbMin, kPreGainDbMax, db), std::memory_order_relaxed);
    }
    void addPreGainDb(float deltaDb) noexcept { setPreGainDb(getPreGainDb() + deltaDb); }

    // 随机预设重随机速度：每拍触发次数（BPM 同步）
    static constexpr float kRandomRetriggerPerBeatMin = 0.25f;
    static constexpr float kRandomRetriggerPerBeatMax = 16.0f;

    float getRandomRetriggerPerBeat() const noexcept { return randomRetriggerPerBeat.load(std::memory_order_relaxed); }
    void setRandomRetriggerPerBeat(float v) noexcept
    {
        randomRetriggerPerBeat.store(juce::jlimit(kRandomRetriggerPerBeatMin, kRandomRetriggerPerBeatMax, v), std::memory_order_relaxed);
    }

    // 额外限制器（前置增益之后）：阈值为线性幅度（0..1），严格将信号限制在 [-th, +th]
    static constexpr float kLimiterThresholdMin = 0.0f;
    static constexpr float kLimiterThresholdMax = 1.0f;

    float getLimiterThreshold() const noexcept { return limiterThreshold.load(std::memory_order_relaxed); }
    void setLimiterThreshold(float th) noexcept
    {
        limiterThreshold.store(juce::jlimit(kLimiterThresholdMin, kLimiterThresholdMax, th), std::memory_order_relaxed);
    }

    // 频带丢失 Notch Q：用于测试不同陷波带宽（Q越大，带宽越窄）
    static constexpr float kLossNotchQMin = 0.1f;
    static constexpr float kLossNotchQMax = 30.0f;

    float getLossNotchQ() const noexcept { return lossNotchQ.load(std::memory_order_relaxed); }
    void setLossNotchQ(float q) noexcept
    {
        lossNotchQ.store(juce::jlimit(kLossNotchQMin, kLossNotchQMax, q), std::memory_order_relaxed);
    }

    // 频带丢失算法模式：
    // 0 = Legacy（当前旧算法，固定Q）
    // 1 = Uniform Bandwidth（按频带边界推导Q，使每段带宽更精确统一）
    static constexpr int kLossAlgorithmLegacy = 0;
    static constexpr int kLossAlgorithmUniformBandwidth = 1;

    int getLossAlgorithmMode() const noexcept { return lossAlgorithmMode.load(std::memory_order_relaxed); }
    void setLossAlgorithmMode(int mode) noexcept
    {
        lossAlgorithmMode.store(juce::jlimit(kLossAlgorithmLegacy, kLossAlgorithmUniformBandwidth, mode),
                                std::memory_order_relaxed);
    }
    void toggleLossAlgorithmMode() noexcept
    {
        setLossAlgorithmMode(getLossAlgorithmMode() == kLossAlgorithmLegacy
                                 ? kLossAlgorithmUniformBandwidth
                                 : kLossAlgorithmLegacy);
    }

    // 频带丢失掩码反转（MUTE）：true 时将“保留/丢失”含义翻转
    bool isLossMaskInverted() const noexcept
    {
        return lossMaskInverted.load(std::memory_order_acquire);
    }

    void setLossMaskInverted(bool inverted) noexcept
    {
        lossMaskInverted.store(inverted, std::memory_order_release);
    }

    void toggleLossMaskInverted() noexcept
    {
        setLossMaskInverted(! isLossMaskInverted());
    }

    // Sleep 冻结：true 时保持当前频带丢失掩码，不再进行下一次重随机/时序推进
    bool isLossMaskFrozen() const noexcept
    {
        return lossMaskFrozen.load(std::memory_order_acquire);
    }

    void setLossMaskFrozen(bool frozen) noexcept
    {
        lossMaskFrozen.store(frozen, std::memory_order_release);
    }

    void toggleLossMaskFrozen() noexcept
    {
        setLossMaskFrozen(! isLossMaskFrozen());
    }

    // 频段丢失调度前的高低切（Hz）
    static constexpr float kLowCutHzMin = 20.0f;
    static constexpr float kLowCutHzMax = 20000.0f;
    static constexpr float kHighCutHzMin = 20.0f;
    static constexpr float kHighCutHzMax = 20000.0f;

    // 高低切算法：
    // 0 = Hard Mask（当前频段掩码硬裁剪）
    // 1 = HPF/LPF（音频链路中的高通/低通滤波链）
    static constexpr int kCutModeHardMask = 0;
    static constexpr int kCutModeHpfLpf = 1;

    // HPF/LPF 斜率（内部DSP仍以 dB/oct 处理）
    static constexpr int kCutSlope12dB = 12;
    static constexpr int kCutSlope24dB = 24;
    static constexpr int kCutSlope48dB = 48;

    // HPF/LPF 手柄可视化角度（用于界面显示与配置认知）
    static constexpr int kCutAngle12Deg = 45;
    static constexpr int kCutAngle24Deg = 60;
    static constexpr int kCutAngle48Deg = 75;

    static int cutSlopeDbPerOctToAngleDeg(int slopeDbPerOct) noexcept
    {
        if (slopeDbPerOct >= kCutSlope48dB)
            return kCutAngle48Deg;
        if (slopeDbPerOct >= kCutSlope24dB)
            return kCutAngle24Deg;
        return kCutAngle12Deg;
    }

    float getLowCutHz() const noexcept { return lowCutHz.load(std::memory_order_relaxed); }
    float getHighCutHz() const noexcept { return highCutHz.load(std::memory_order_relaxed); }
    int getCutMode() const noexcept { return cutMode.load(std::memory_order_relaxed); }
    int getCutSlopeDbPerOct() const noexcept { return cutSlopeDbPerOct.load(std::memory_order_relaxed); }
    int getCutSlopeAngleDeg() const noexcept { return cutSlopeDbPerOctToAngleDeg(getCutSlopeDbPerOct()); }

    void setLowCutHz(float hz) noexcept
    {
        const float hi = getHighCutHz();
        const float clamped = juce::jlimit(kLowCutHzMin, juce::jmin(kLowCutHzMax, hi), hz);
        lowCutHz.store(clamped, std::memory_order_relaxed);
    }

    void setHighCutHz(float hz) noexcept
    {
        const float lo = getLowCutHz();
        const float clamped = juce::jlimit(juce::jmax(kHighCutHzMin, lo), kHighCutHzMax, hz);
        highCutHz.store(clamped, std::memory_order_relaxed);
    }

    void setCutMode(int mode) noexcept
    {
        cutMode.store(juce::jlimit(kCutModeHardMask, kCutModeHpfLpf, mode), std::memory_order_relaxed);
    }

    void setCutSlopeDbPerOct(int slope) noexcept
    {
        int normalized = kCutSlope12dB;
        if (slope >= kCutSlope48dB)
            normalized = kCutSlope48dB;
        else if (slope >= kCutSlope24dB)
            normalized = kCutSlope24dB;
        cutSlopeDbPerOct.store(normalized, std::memory_order_relaxed);
    }

    void setCutDragActive(bool active) noexcept
    {
        cutDragActive.store(active, std::memory_order_release);
    }

    bool isCutDragActive() const noexcept
    {
        return cutDragActive.load(std::memory_order_acquire);
    }

    void cycleCutModeOrSlopeFromTV() noexcept
    {
        const int mode = getCutMode();
        const int slope = getCutSlopeDbPerOct();

        if (mode == kCutModeHardMask)
        {
            setCutMode(kCutModeHpfLpf);
            setCutSlopeDbPerOct(kCutSlope12dB);
            return;
        }

        if (slope == kCutSlope12dB)
            setCutSlopeDbPerOct(kCutSlope24dB);
        else if (slope == kCutSlope24dB)
            setCutSlopeDbPerOct(kCutSlope48dB);
        else
            setCutMode(kCutModeHardMask);
    }

    std::atomic<bool> bypassed { false };

    bool isShuttingDownNow() const noexcept
    {
        return isShuttingDown.load(std::memory_order_acquire);
    }

private:
    static constexpr int oscilloscopeBufferSize = 2048;
    static constexpr int kLossBandCount = 100;

    void pushSamplesToOscilloscope(const float* samples, int numSamples);

    void ensureLossFilters(double sampleRate);
    void retriggerLossMask(double nowSeconds);
    void applyCutMaskToLossMask() noexcept;
    void ensureCutFilters(double sampleRate);
    void processHpfLpfCut(juce::AudioBuffer<float>& buffer) noexcept;
    static float getLossBandCenterHz(int bandIndex) noexcept;
    static bool isBpmSequencedPreset(int presetIndex) noexcept;

    struct SequencedPresetProfile
    {
        int anchorCount = 1;
        int anchorSpacing = 50;
        int baseStartBand = 0;
        int halfWindow = 3;
        int stepPerTick = 1;
        bool reverse = false;
    };

    static SequencedPresetProfile getSequencedPresetProfile(int presetIndex) noexcept;
    double getSequencedRetriggerSeconds(int presetIndex, double bpm) const noexcept;
    double getRandomPresetRetriggerSeconds(double bpm) const noexcept;

    // 保护音频线程中的非原子缓存状态（如 current* / last* / lossMask 等）与状态恢复/释放资源阶段的并发访问
    juce::SpinLock processingStateLock;

    juce::SpinLock oscilloscopeLock;
    std::array<float, oscilloscopeBufferSize> oscilloscopeBuffer {};

    int oscilloscopeWritePos = 0;

    juce::SpinLock lossMaskSnapshotLock;
    std::array<uint8_t, kLossBandCount> lossMaskSnapshot {};

    std::atomic<int> displayPresetIndex { 0 };

    std::atomic<float> preGainDb { 10.0f };
    std::atomic<float> randomRetriggerPerBeat { 4.0f };
    std::atomic<float> limiterThreshold { 1.0f };
    std::atomic<float> lossNotchQ { 6.0f };
    std::atomic<int> lossAlgorithmMode { kLossAlgorithmLegacy };
    std::atomic<bool> lossMaskFrozen { false };
    std::atomic<bool> lossMaskInverted { false };
    std::atomic<float> lowCutHz { kLowCutHzMin };

    std::atomic<float> highCutHz { kHighCutHzMax };
    std::atomic<int> cutMode { kCutModeHardMask };
    std::atomic<int> cutSlopeDbPerOct { kCutSlope12dB };
    std::atomic<bool> cutDragActive { false };

    std::array<uint8_t, kLossBandCount> lossMask {};

    std::array<int, kLossBandCount> activeLossBands {};
    std::array<int, kLossBandCount> droppedLossBands {};
    int activeLossBandCount = 0;
    int droppedLossBandCount = 0;

    std::array<juce::IIRFilter, kLossBandCount> lossBandNotchL {};
    std::array<juce::IIRFilter, kLossBandCount> lossBandNotchR {};

    static constexpr int kCutFilterMaxStages = 4;
    std::array<juce::IIRFilter, kCutFilterMaxStages> cutHighPassL {};
    std::array<juce::IIRFilter, kCutFilterMaxStages> cutHighPassR {};
    std::array<juce::IIRFilter, kCutFilterMaxStages> cutLowPassL {};
    std::array<juce::IIRFilter, kCutFilterMaxStages> cutLowPassR {};

    std::array<float, kLossBandCount> lossBandWet {};
    static constexpr float kLossMaskSmoothingTimeSeconds = 0.010f;
    float lossMaskSmoothCoeff = 1.0f;

    double currentSampleRateForLoss = 0.0;

    double currentLossNotchQForFilters = -1.0;
    int currentLossAlgorithmModeForFilters = -1;
    float currentLowCutHzForMask = -1.0f;
    float currentHighCutHzForMask = -1.0f;
    int currentCutModeForMask = -1;
    int currentLossMaskInvertedForMask = 0;
    double currentSampleRateForCutFilter = 0.0;

    float currentLowCutHzForCutFilter = -1.0f;
    float currentHighCutHzForCutFilter = -1.0f;
    int currentSlopeForCutFilter = -1;

    juce::AudioBuffer<float> cutDryBuffer;
    int cutCrossfadeSamplesRemaining = 0;
    static constexpr int kCutCrossfadeSamples = 1024;

    float lastLowCutHzForCrossfade = -1.0f;
    float lastHighCutHzForCrossfade = -1.0f;
    int lastSlopeForCrossfade = -1;

    double lossTimeSeconds = 0.0;

    double nextLossRetriggerSeconds = 0.0;
    int lastLossPresetIndex = -1;
    juce::Random lossRandom;

    int sequenceHeadBand = 0;
    std::atomic<bool> isPrepared { false };
    std::atomic<bool> isShuttingDown { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LDSJvstAudioProcessor)
};
