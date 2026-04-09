#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>

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
    int getDisplayPresetIndex() const noexcept { return displayPresetIndex; }
    void setDisplayPresetIndex(int newIndex) noexcept
    {
        // 目前预设数量固定为 12（0..11）
        displayPresetIndex = juce::jlimit(0, 11, newIndex);
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

    bool bypassed = false;

private:
    static constexpr int oscilloscopeBufferSize = 2048;

    void pushSamplesToOscilloscope(const float* samples, int numSamples);

    juce::SpinLock oscilloscopeLock;
    std::array<float, oscilloscopeBufferSize> oscilloscopeBuffer {};
    int oscilloscopeWritePos = 0;

    int displayPresetIndex = 0;

    std::atomic<float> preGainDb { 4.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LDSJvstAudioProcessor)
};
