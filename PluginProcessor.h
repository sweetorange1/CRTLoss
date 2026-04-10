#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <vector>

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

    // 显示波形的“预设”选择（由界面设置，宿主保存工程时需要持久化）
    int getDisplayPresetIndex() const noexcept { return displayPresetIndex.load(std::memory_order_relaxed); }
    void setDisplayPresetIndex(int newIndex) noexcept
    {
        // 目前预设数量固定为 12（0..11）
        displayPresetIndex.store(juce::jlimit(0, 11, newIndex), std::memory_order_relaxed);
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

    std::atomic<bool> bypassed { false };

    bool isShuttingDownNow() const noexcept
    {
        return isShuttingDown.load(std::memory_order_acquire);
    }

private:
    static constexpr int oscilloscopeBufferSize = 2048;
    static constexpr int kLossBandCount = 100;

    struct LossPreset
    {
        std::array<float, kLossBandCount> probabilities {};
        double retriggerSeconds = 0.50;
    };

    void pushSamplesToOscilloscope(const float* samples, int numSamples);
    void initLossPresets();
    void ensureLossFilters(double sampleRate);
    void retriggerLossMask(double nowSeconds);
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

    std::array<LossPreset, 12> lossPresets {};
    std::array<uint8_t, kLossBandCount> lossMask {};
    std::array<int, kLossBandCount> activeLossBands {};
    std::array<int, kLossBandCount> droppedLossBands {};
    int activeLossBandCount = 0;
    int droppedLossBandCount = 0;

    std::array<juce::IIRFilter, kLossBandCount> lossBandNotchL {};
    std::array<juce::IIRFilter, kLossBandCount> lossBandNotchR {};

    std::array<float, kLossBandCount> lossBandWet {};
    static constexpr float kLossMaskSmoothingTimeSeconds = 0.010f;
    float lossMaskSmoothCoeff = 1.0f;

    double currentSampleRateForLoss = 0.0;
    double currentLossNotchQForFilters = -1.0;
    double lossTimeSeconds = 0.0;

    double nextLossRetriggerSeconds = 0.0;
    int lastLossPresetIndex = -1;
    juce::Random lossRandom;

    int sequenceHeadBand = 0;
    std::atomic<bool> isPrepared { false };
    std::atomic<bool> isShuttingDown { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LDSJvstAudioProcessor)
};
