#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"

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
}

LDSJvstAudioProcessor::~LDSJvstAudioProcessor() {}

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

void LDSJvstAudioProcessor::prepareToPlay(double, int)
{
    const juce::SpinLock::ScopedLockType sl(oscilloscopeLock);
    std::fill(oscilloscopeBuffer.begin(), oscilloscopeBuffer.end(), 0.0f);
    oscilloscopeWritePos = 0;
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
    // 3) 硬削波 Hard Clip：保证输出不超过 0dBFS（[-1, +1]）
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
    state.setProperty("preset", displayPresetIndex, nullptr);
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