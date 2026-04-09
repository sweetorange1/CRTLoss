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

    // 显示波形的“预设”选择（由界面设置，宿主保存工程时需要持久化）
    int getDisplayPresetIndex() const noexcept { return displayPresetIndex.load(std::memory_order_relaxed); }
    void setDisplayPresetIndex(int newIndex) noexcept
    {
        // 目前预设数量固定为 12（0..11）
        displayPresetIndex.store(juce::jlimit(0, 11, newIndex), std::memory_order_relaxed);
    }

    // 前置增益（dB）：默认 +4dB，范围 -5..+24
    static constexpr float kPreGainDbMin = -5.0f;
    static constexpr float kPreGainDbMax = 24.0f;

    float getPreGainDb() const noexcept { return preGainDb.load(std::memory_order_relaxed); }
    void setPreGainDb(float db) noexcept
    {
        preGainDb.store(juce::jlimit(kPreGainDbMin, kPreGainDbMax, db), std::memory_order_relaxed);
    }
    void addPreGainDb(float deltaDb) noexcept { setPreGainDb(getPreGainDb() + deltaDb); }

    // 额外限制器（前置增益之后）：阈值为线性幅度（0..1），严格将信号限制在 [-th, +th]
    static constexpr float kLimiterThresholdMin = 0.0f;
    static constexpr float kLimiterThresholdMax = 1.0f;

    float getLimiterThreshold() const noexcept { return limiterThreshold.load(std::memory_order_relaxed); }
    void setLimiterThreshold(float th) noexcept
    {
        limiterThreshold.store(juce::jlimit(kLimiterThresholdMin, kLimiterThresholdMax, th), std::memory_order_relaxed);
    }

    bool bypassed = false;

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

    juce::SpinLock oscilloscopeLock;
    std::array<float, oscilloscopeBufferSize> oscilloscopeBuffer {};
    int oscilloscopeWritePos = 0;

    std::atomic<int> displayPresetIndex { 0 };

    std::atomic<float> preGainDb { 4.0f };
    std::atomic<float> limiterThreshold { 1.0f };

    std::array<LossPreset, 12> lossPresets {};
    std::array<uint8_t, kLossBandCount> lossMask {};
    std::array<int, kLossBandCount> activeLossBands {};
    std::array<int, kLossBandCount> droppedLossBands {};
    int activeLossBandCount = 0;
    int droppedLossBandCount = 0;

    std::array<juce::IIRFilter, kLossBandCount> lossBandNotchL {};
    std::array<juce::IIRFilter, kLossBandCount> lossBandNotchR {};

    double currentSampleRateForLoss = 0.0;
    double lossTimeSeconds = 0.0;
    double nextLossRetriggerSeconds = 0.0;
    int lastLossPresetIndex = -1;
    juce::Random lossRandom;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LDSJvstAudioProcessor)
};
