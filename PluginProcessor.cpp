#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <algorithm>
#include <cmath>

LDSJvstAudioProcessor::LDSJvstAudioProcessor()
    : juce::AudioProcessor (BusesProperties()
#if ! JucePlugin_IsMidiEffect
#if ! JucePlugin_IsSynth
        .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
#endif
        .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
#endif
    )
{
    initLossPresets();
    lossMask.fill(1);
    activeLossBandCount = 0;
    lossRandom.setSeedRandomly();
}

LDSJvstAudioProcessor::~LDSJvstAudioProcessor() {}

void LDSJvstAudioProcessor::initLossPresets()
{
    for (auto& preset : lossPresets)
    {
        preset.probabilities.fill(0.50f);
        preset.retriggerSeconds = 0.50;
    }

    // 0: 全频段均匀 50%，0.5s 重触发（你描述的示例）
    lossPresets[0].probabilities.fill(0.50f);
    lossPresets[0].retriggerSeconds = 0.01;

    // 1: 低频保留概率更高
    for (int i = 0; i < kLossBandCount; ++i)
    {
        const float t = (float) i / (float) (kLossBandCount - 1);
        lossPresets[1].probabilities[(size_t) i] = juce::jlimit(0.05f, 0.95f, 0.88f - 0.68f * t);
    }
    lossPresets[1].retriggerSeconds = 0.18;

    // 2: 高频保留概率更高
    for (int i = 0; i < kLossBandCount; ++i)
    {
        const float t = (float) i / (float) (kLossBandCount - 1);
        lossPresets[2].probabilities[(size_t) i] = juce::jlimit(0.05f, 0.95f, 0.15f + 0.75f * t);
    }
    lossPresets[2].retriggerSeconds = 0.12;

    // 3: 中频优先（钟形）
    for (int i = 0; i < kLossBandCount; ++i)
    {
        const float t = (float) i / (float) (kLossBandCount - 1);
        const float d = (t - 0.5f) / 0.22f;
        lossPresets[3].probabilities[(size_t) i] = juce::jlimit(0.05f, 0.95f, 0.18f + 0.72f * std::exp(-(d * d)));
    }
    lossPresets[3].retriggerSeconds = 0.08;

    // 4: 梳状分布
    for (int i = 0; i < kLossBandCount; ++i)
    {
        const bool comb = ((i % 6) < 3);
        lossPresets[4].probabilities[(size_t) i] = comb ? 0.82f : 0.18f;
    }
    lossPresets[4].retriggerSeconds = 0.09;

    // 5: 低频+中高频双峰
    for (int i = 0; i < kLossBandCount; ++i)
    {
        const float t = (float) i / (float) (kLossBandCount - 1);
        const float d1 = (t - 0.20f) / 0.12f;
        const float d2 = (t - 0.78f) / 0.16f;
        const float v = 0.10f + 0.42f * std::exp(-(d1 * d1)) + 0.50f * std::exp(-(d2 * d2));
        lossPresets[5].probabilities[(size_t) i] = juce::jlimit(0.05f, 0.95f, v);
    }
    lossPresets[5].retriggerSeconds = 0.14;

    // 6: 周期波动分布
    for (int i = 0; i < kLossBandCount; ++i)
    {
        const float t = (float) i / (float) (kLossBandCount - 1);
        const float v = 0.5f + 0.38f * std::sin(2.0f * juce::MathConstants<float>::pi * (t * 3.0f + 0.1f));
        lossPresets[6].probabilities[(size_t) i] = juce::jlimit(0.05f, 0.95f, v);
    }
    lossPresets[6].retriggerSeconds = 0.07;

    // 7: 稀疏窄峰
    lossPresets[7].probabilities.fill(0.10f);
    for (int c : { 8, 19, 31, 46, 59, 73, 88, 96 })
        for (int k = -1; k <= 1; ++k)
            if (const int idx = c + k; idx >= 0 && idx < kLossBandCount)
                lossPresets[7].probabilities[(size_t) idx] = (k == 0 ? 0.92f : 0.55f);
    lossPresets[7].retriggerSeconds = 0.05;

    // 8: 稳定轻丢失
    lossPresets[8].probabilities.fill(0.72f);
    lossPresets[8].retriggerSeconds = 0.30;

    // 9: 激进丢失
    lossPresets[9].probabilities.fill(0.26f);
    lossPresets[9].retriggerSeconds = 0.06;

    // 10: 低频稳定+高频随机
    for (int i = 0; i < kLossBandCount; ++i)
    {
        const float t = (float) i / (float) (kLossBandCount - 1);
        lossPresets[10].probabilities[(size_t) i] = (t < 0.35f ? 0.85f : 0.20f + 0.50f * t);
    }
    lossPresets[10].retriggerSeconds = 0.11;

    // 11: 高频稳定+低频随机
    for (int i = 0; i < kLossBandCount; ++i)
    {
        const float t = (float) i / (float) (kLossBandCount - 1);
        lossPresets[11].probabilities[(size_t) i] = (t > 0.65f ? 0.86f : 0.18f + 0.55f * (1.0f - t));
    }
    lossPresets[11].retriggerSeconds = 0.10;
}

float LDSJvstAudioProcessor::getLossBandCenterHz(int bandIndex) noexcept
{
    const float lo = 20.0f;
    const float hi = 20000.0f;
    const float n = (float) juce::jlimit(0, kLossBandCount - 1, bandIndex);
    const float t = n / (float) (kLossBandCount - 1);
    return lo * std::pow(hi / lo, t);
}

void LDSJvstAudioProcessor::ensureLossFilters(double sampleRate)
{
    if (sampleRate <= 0.0)
        return;

    if (sampleRate == currentSampleRateForLoss)
        return;

    currentSampleRateForLoss = sampleRate;

    for (int i = 0; i < kLossBandCount; ++i)
    {
        auto& fl = lossBandNotchL[(size_t) i];
        auto& fr = lossBandNotchR[(size_t) i];

        const float nyquistSafe = (float) (sampleRate * 0.45);
        const float f = juce::jlimit(20.0f, juce::jmax(20.0f, nyquistSafe), getLossBandCenterHz(i));
        const float q = 5.5f;

        fl.reset();
        fr.reset();
        fl.setCoefficients(juce::IIRCoefficients::makeNotchFilter(sampleRate, f, q));
        fr.setCoefficients(juce::IIRCoefficients::makeNotchFilter(sampleRate, f, q));

    }
}

void LDSJvstAudioProcessor::retriggerLossMask(double nowSeconds)
{
    const int preset = juce::jlimit(0, 11, getDisplayPresetIndex());
    const auto& lp = lossPresets[(size_t) preset];

    activeLossBandCount = 0;

    for (int i = 0; i < kLossBandCount; ++i)
    {
        const float p = juce::jlimit(0.0f, 1.0f, lp.probabilities[(size_t) i]);
        const bool keep = lossRandom.nextFloat() < p;
        lossMask[(size_t) i] = keep ? (uint8_t) 1 : (uint8_t) 0;

        if (keep)
            activeLossBands[(size_t) activeLossBandCount++] = i;
    }

    // 兜底：至少保留一个频段，避免完全静音
    if (activeLossBandCount <= 0)
    {
        const int idx = lossRandom.nextInt(kLossBandCount);
        lossMask[(size_t) idx] = 1;
        activeLossBands[0] = idx;
        activeLossBandCount = 1;
    }

    droppedLossBandCount = 0;
    for (int i = 0; i < kLossBandCount; ++i)
    {
        if (lossMask[(size_t) i] == 0)
            droppedLossBands[(size_t) droppedLossBandCount++] = i;
    }

    nextLossRetriggerSeconds = nowSeconds + juce::jmax(0.01, lp.retriggerSeconds);
}

bool LDSJvstAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const

{
#if JucePlugin_IsMidiEffect
    juce::ignoreUnused (layouts);
    return true;
#else
    // 必须有输出
    if (layouts.getMainOutputChannelSet() == juce::AudioChannelSet::disabled())
        return false;

#if ! JucePlugin_IsSynth
    // 作为效果器：输入/输出声道数必须一致（支持 mono/stereo）
    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;
#endif

    const auto out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo();
#endif
}

void LDSJvstAudioProcessor::prepareToPlay(double sampleRate, int)
{
    const juce::SpinLock::ScopedLockType sl(oscilloscopeLock);
    std::fill(oscilloscopeBuffer.begin(), oscilloscopeBuffer.end(), 0.0f);
    oscilloscopeWritePos = 0;

    ensureLossFilters(sampleRate);

    lossTimeSeconds = 0.0;
    nextLossRetriggerSeconds = 0.0;
    lastLossPresetIndex = juce::jlimit(0, 11, getDisplayPresetIndex());
    activeLossBandCount = 0;
    droppedLossBandCount = 0;
    lossMask.fill(1);

    retriggerLossMask(lossTimeSeconds);

}

void LDSJvstAudioProcessor::releaseResources() {}

void LDSJvstAudioProcessor::pushSamplesToOscilloscope(const float* samples, int numSamples)
{
    if (samples == nullptr || numSamples <= 0)
        return;

    const juce::SpinLock::ScopedTryLockType sl(oscilloscopeLock);
    if (! sl.isLocked())
        return;

    for (int i = 0; i < numSamples; ++i)
    {
        oscilloscopeBuffer[(size_t) oscilloscopeWritePos] = samples[i];
        oscilloscopeWritePos = (oscilloscopeWritePos + 1) % oscilloscopeBufferSize;
    }
}

void LDSJvstAudioProcessor::getOscilloscopeSnapshot(juce::Array<float>& dest)
{
    dest.resize(oscilloscopeBufferSize);

    const juce::SpinLock::ScopedTryLockType sl(oscilloscopeLock);
    if (! sl.isLocked())
        return;

    // 以 writePos 作为“最新数据之后的位置”，从旧到新拷贝
    for (int i = 0; i < oscilloscopeBufferSize; ++i)
    {
        const int idx = (oscilloscopeWritePos + i) % oscilloscopeBufferSize;
        dest.set(i, oscilloscopeBuffer[(size_t) idx]);
    }
}

void LDSJvstAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const auto totalNumInputChannels  = getTotalNumInputChannels();
    const auto totalNumOutputChannels = getTotalNumOutputChannels();

    // 清理多余输出通道（例如输入是mono而输出是stereo等情况）
    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear(i, 0, buffer.getNumSamples());

    // ============================================================
    // 1) 前置增益（dB）
    // ============================================================
    const float db = getPreGainDb();
    const float g = juce::Decibels::decibelsToGain(db);

    if (g != 1.0f)
        buffer.applyGain(g);

    // ============================================================
    // 2) 额外限制器（前置增益之后）：严格限制在 [-th, +th]
    // ============================================================
    const float th = getLimiterThreshold();
    if (th < 1.0f)
    {
        for (int ch = 0; ch < totalNumOutputChannels; ++ch)
        {
            auto* d = buffer.getWritePointer(ch);
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                d[i] = juce::jlimit(-th, th, d[i]);
        }
    }

    // ============================================================
    // 3) 频带随机丢失：20Hz~20kHz 划分为 100 段，按当前预设概率矩阵 + 触发周期重随机
    //    实现方式：对“未命中保留”的频段施加窄带 Notch，从而模拟该段频率信息丢失
    // ============================================================

    ensureLossFilters(getSampleRate());

    const int currentPreset = juce::jlimit(0, 11, getDisplayPresetIndex());
    if (currentPreset != lastLossPresetIndex)
    {
        lastLossPresetIndex = currentPreset;
        retriggerLossMask(lossTimeSeconds);
    }

    const double blockSeconds = (double) buffer.getNumSamples() / juce::jmax(1.0, getSampleRate());
    lossTimeSeconds += blockSeconds;

    if (lossTimeSeconds >= nextLossRetriggerSeconds)
        retriggerLossMask(lossTimeSeconds);

    if (droppedLossBandCount > 0)
    {
        if (totalNumOutputChannels > 0)
        {
            auto* left = buffer.getWritePointer(0);
            for (int i = 0; i < buffer.getNumSamples(); ++i)
            {
                float x = left[i];
                for (int n = 0; n < droppedLossBandCount; ++n)
                    x = lossBandNotchL[(size_t) droppedLossBands[(size_t) n]].processSingleSampleRaw(x);
                left[i] = x;
            }
        }

        if (totalNumOutputChannels > 1)
        {
            auto* right = buffer.getWritePointer(1);
            for (int i = 0; i < buffer.getNumSamples(); ++i)
            {
                float x = right[i];
                for (int n = 0; n < droppedLossBandCount; ++n)
                    x = lossBandNotchR[(size_t) droppedLossBands[(size_t) n]].processSingleSampleRaw(x);
                right[i] = x;
            }
        }
    }

    // ============================================================
    // 4) 最终 0dB 硬削波：防止电平过高
    // ============================================================

    for (int ch = 0; ch < totalNumOutputChannels; ++ch)
    {
        auto* d = buffer.getWritePointer(ch);
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            d[i] = juce::jlimit(-1.0f, 1.0f, d[i]);
    }

    // 示例波形：抓取主输出的第0通道（反映最终输出）
    if (totalNumOutputChannels > 0)
        pushSamplesToOscilloscope(buffer.getReadPointer(0), buffer.getNumSamples());

}

juce::AudioProcessorEditor* LDSJvstAudioProcessor::createEditor() { return new LDSJvstAudioProcessorEditor(*this); }
bool LDSJvstAudioProcessor::hasEditor() const { return true; }

const juce::String LDSJvstAudioProcessor::getName() const { return "LDSJvst"; }
bool LDSJvstAudioProcessor::acceptsMidi() const { return false; }
bool LDSJvstAudioProcessor::producesMidi() const { return false; }
bool LDSJvstAudioProcessor::isMidiEffect() const { return false; }
double LDSJvstAudioProcessor::getTailLengthSeconds() const { return 0.0; }

int LDSJvstAudioProcessor::getNumPrograms() { return 1; }
int LDSJvstAudioProcessor::getCurrentProgram() { return 0; }
void LDSJvstAudioProcessor::setCurrentProgram(int) {}
const juce::String LDSJvstAudioProcessor::getProgramName(int) { return {}; }
void LDSJvstAudioProcessor::changeProgramName(int, const juce::String&) {}

void LDSJvstAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    juce::ValueTree state("LDSJvstState");
    state.setProperty("version", 1, nullptr);
    state.setProperty("preset", getDisplayPresetIndex(), nullptr);

    state.setProperty("bypassed", bypassed ? 1 : 0, nullptr);
    state.setProperty("preGainDb", (double) getPreGainDb(), nullptr);
    state.setProperty("limiterThreshold", (double) getLimiterThreshold(), nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary(*xml, destData);
}

void LDSJvstAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes <= 0)
        return;

    std::unique_ptr<juce::XmlElement> xmlState(getXmlFromBinary(data, sizeInBytes));
    if (xmlState == nullptr)
        return;

    const juce::ValueTree state = juce::ValueTree::fromXml(*xmlState);
    if (! state.isValid())
        return;

    // 兼容未来可能的类型调整
    if (! state.hasType("LDSJvstState"))
        return;

    setDisplayPresetIndex((int) state.getProperty("preset", 0));
    bypassed = ((int) state.getProperty("bypassed", 0)) != 0;

    setPreGainDb((float) (double) state.getProperty("preGainDb", 4.0));
    setLimiterThreshold((float) (double) state.getProperty("limiterThreshold", 1.0));
}

// 插件入口实现
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new LDSJvstAudioProcessor();
}