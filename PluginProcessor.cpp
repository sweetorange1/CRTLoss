#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <algorithm>
#include <cmath>
#include <limits>

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
    lossMaskSnapshot.fill(1);
    lossBandWet.fill(1.0f);
    activeLossBandCount = 0;
    lossRandom.setSeedRandomly();

    strictFft = std::make_unique<juce::dsp::FFT>(kStrictFftOrder);

    for (int ch = 0; ch < 2; ++ch)
    {
        strictInputFifo[(size_t) ch].assign((size_t) kStrictFftSize, 0.0f);
        strictOutputFifo[(size_t) ch].assign((size_t) kStrictFftSize, 0.0f);
        strictOutputOverlap[(size_t) ch].assign((size_t) kStrictFftSize, 0.0f);
        strictProcessTemp[(size_t) ch].assign((size_t) (2 * kStrictFftSize), 0.0f);
    }

    for (int n = 0; n < kStrictFftSize; ++n)
        strictWindow[(size_t) n] = 0.5f - 0.5f * std::cos(2.0f * juce::MathConstants<float>::pi * (float) n / (float) (kStrictFftSize - 1));

    resetStrictBandCutState();
}

LDSJvstAudioProcessor::~LDSJvstAudioProcessor()
{
    isPrepared.store(false, std::memory_order_release);
    isShuttingDown.store(true, std::memory_order_release);
    nextLossRetriggerSeconds = std::numeric_limits<double>::infinity();
}

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
    // lossPresets[1].retriggerSeconds = 0.18;
    lossPresets[1].retriggerSeconds = 1;

    // 2: 高频保留概率更高
    for (int i = 0; i < kLossBandCount; ++i)
    {
        const float t = (float) i / (float) (kLossBandCount - 1);
        lossPresets[2].probabilities[(size_t) i] = juce::jlimit(0.05f, 0.95f, 0.15f + 0.75f * t);
    }
    // lossPresets[2].retriggerSeconds = 0.12;
    lossPresets[2].retriggerSeconds = 1;


    // 3: 中频优先（钟形）
    //    时序说明：属于 BPM 阵列预设；重触发间隔使用 16 分音符（60/BPM*0.25）。
    //    单起点（中频附近）+ 固定窗口宽度，按低->高纯顺序推进；不叠加随机频段。

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
    //    时序说明：属于 BPM 阵列预设；重触发间隔使用三连音网格（60/BPM/3）。
    //    三起点并行阵列（低/中/高）+ 固定窗口，纯顺序推进；不叠加随机频段。

    for (int i = 0; i < kLossBandCount; ++i)
    {
        const float t = (float) i / (float) (kLossBandCount - 1);
        const float v = 0.5f + 0.38f * std::sin(2.0f * juce::MathConstants<float>::pi * (t * 3.0f + 0.1f));
        lossPresets[6].probabilities[(size_t) i] = juce::jlimit(0.05f, 0.95f, v);
    }
    lossPresets[6].retriggerSeconds = 0.07;

    // 7: 稀疏窄峰
    //    时序说明：属于 BPM 阵列预设；重触发间隔使用 32 分音符（60/BPM*0.125）。
    //    双起点 + 窄窗口 + 更快推进步长（每次2格），形成高速扫频感；不叠加随机频段。

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
    //     时序说明：属于 BPM 阵列预设；重触发间隔使用 8 分音符（60/BPM*0.5）。
    //     双起点（从低频区域起步）+ 较宽窗口，纯顺序推进，听感更稳更厚；不叠加随机频段。

    for (int i = 0; i < kLossBandCount; ++i)
    {
        const float t = (float) i / (float) (kLossBandCount - 1);
        lossPresets[10].probabilities[(size_t) i] = (t < 0.35f ? 0.85f : 0.20f + 0.50f * t);
    }
    lossPresets[10].retriggerSeconds = 0.11;

    // 11: 高频稳定+低频随机
    //     时序说明：属于 BPM 阵列预设；重触发间隔使用附点 8 分音符（60/BPM*0.75）。
    //     双起点（从高频区域起步）+ 反向推进，时值更长，扫动更“呼吸化”；不叠加随机频段。

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

bool LDSJvstAudioProcessor::isBpmSequencedPreset(int presetIndex) noexcept
{
    return presetIndex == 3 || presetIndex == 6 || presetIndex == 7 || presetIndex == 10 || presetIndex == 11;
}

LDSJvstAudioProcessor::SequencedPresetProfile LDSJvstAudioProcessor::getSequencedPresetProfile(int presetIndex) noexcept
{
    switch (presetIndex)
    {
        case 3:  return { 1, 100, 50, 4, 1, false }; // 单起点，中频起步，平稳上行
        case 6:  return { 3, 33, 6,  3, 1, false };  // 三起点，低-中-高并行阵列
        case 7:  return { 2, 50, 8,  2, 2, false };  // 双起点，窄窗快速扫动
        case 10: return { 2, 50, 0,  5, 1, false };  // 双起点，低频起步，较宽窗口
        case 11: return { 2, 50, 80, 4, 1, true  };  // 双起点，高频起步，反向推进
        default: return {};
    }
}

double LDSJvstAudioProcessor::getSequencedRetriggerSeconds(int presetIndex, double bpm) const noexcept
{
    const double safeBpm = juce::jlimit(40.0, 260.0, bpm > 0.0 ? bpm : 120.0);
    const double beatSec = 60.0 / safeBpm;

    switch (presetIndex)
    {
        case 3:  return juce::jlimit(0.02, 0.50, beatSec * 0.25);      // 16分音符
        case 6:  return juce::jlimit(0.02, 0.50, beatSec / 3.0);       // 三连音颗粒感
        case 7:  return juce::jlimit(0.02, 0.50, beatSec * 0.125);     // 32分音符
        case 10: return juce::jlimit(0.02, 0.80, beatSec * 0.50);      // 8分音符
        case 11: return juce::jlimit(0.02, 0.80, beatSec * 0.75);      // 附点8分音符感
        default: return 0.10;
    }
}

double LDSJvstAudioProcessor::getRandomPresetRetriggerSeconds(double bpm) const noexcept
{
    const double safeBpm = juce::jlimit(40.0, 260.0, bpm > 0.0 ? bpm : 120.0);
    const double beatSec = 60.0 / safeBpm;
    const double perBeat = (double) juce::jlimit(kRandomRetriggerPerBeatMin,
                                                  kRandomRetriggerPerBeatMax,
                                                  getRandomRetriggerPerBeat());
    return juce::jlimit(0.01, 1.20, beatSec / perBeat);
}

void LDSJvstAudioProcessor::ensureLossFilters(double sampleRate)

{
    if (sampleRate <= 0.0)
        return;

    const float desiredQ = juce::jlimit(kLossNotchQMin, kLossNotchQMax, getLossNotchQ());
    const int algorithmMode = juce::jlimit(kLossAlgorithmLegacy,
                                           kLossAlgorithmUniformBandwidth,
                                           getLossAlgorithmMode());

    const bool sampleRateUnchanged = (sampleRate == currentSampleRateForLoss);
    const bool qUnchanged = (std::abs((float) currentLossNotchQForFilters - desiredQ) < 0.0001f);
    const bool modeUnchanged = (algorithmMode == currentLossAlgorithmModeForFilters);

    if (sampleRateUnchanged && qUnchanged && modeUnchanged)
        return;

    currentSampleRateForLoss = sampleRate;
    currentLossNotchQForFilters = desiredQ;
    currentLossAlgorithmModeForFilters = algorithmMode;

    const double tau = juce::jmax(0.0001, (double) kLossMaskSmoothingTimeSeconds);
    lossMaskSmoothCoeff = (float) std::exp(-1.0 / (sampleRate * tau));

    // 频段定义：沿用现有 20Hz~20kHz 的对数划分（100 段）
    // Uniform Bandwidth 模式下，不再使用全段统一Q；
    // 而是按每个频段上下边界推导“该频段对应的Q”，使每段陷波覆盖宽度更贴近该段带宽。
    constexpr float lo = 20.0f;
    constexpr float hi = 20000.0f;
    const float ratio = std::pow(hi / lo, 1.0f / (float) (kLossBandCount - 1));

    const float nyquistSafe = (float) (sampleRate * 0.45);

    for (int i = 0; i < kLossBandCount; ++i)

    {
        auto& fl = lossBandNotchL[(size_t) i];
        auto& fr = lossBandNotchR[(size_t) i];

        const float center = getLossBandCenterHz(i);
        const float f = juce::jlimit(20.0f, juce::jmax(20.0f, nyquistSafe), center);

        float qForBand = desiredQ;

        if (algorithmMode == kLossAlgorithmUniformBandwidth)
        {
            const float fLo = center / std::sqrt(ratio);
            const float fHi = center * std::sqrt(ratio);
            const float bandWidthHz = juce::jmax(1.0f, fHi - fLo);
            const float derivedQ = juce::jmax(0.01f, center / bandWidthHz);

            // 用现有 Notch Q 范围约束，保证参数行为与稳定性一致
            qForBand = juce::jlimit(kLossNotchQMin, kLossNotchQMax, derivedQ);
        }

        fl.reset();
        fr.reset();
        fl.setCoefficients(juce::IIRCoefficients::makeNotchFilter(sampleRate, f, qForBand));
        fr.setCoefficients(juce::IIRCoefficients::makeNotchFilter(sampleRate, f, qForBand));

    }
}

void LDSJvstAudioProcessor::retriggerLossMask(double nowSeconds)
{
    const int preset = juce::jlimit(0, 11, getDisplayPresetIndex());
    const auto& lp = lossPresets[(size_t) preset];

    double hostBpm = 120.0;
    if (auto* playHead = getPlayHead())
    {
        juce::AudioPlayHead::CurrentPositionInfo pos;
        if (playHead->getCurrentPosition(pos) && pos.bpm > 0.0)
            hostBpm = pos.bpm;
    }

    activeLossBandCount = 0;

    if (isBpmSequencedPreset(preset))
    {
        const auto profile = getSequencedPresetProfile(preset);

        for (int i = 0; i < kLossBandCount; ++i)
            lossMask[(size_t) i] = 0;

        const int anchorCount = juce::jlimit(1, 8, profile.anchorCount);
        const int spacing = juce::jmax(1, profile.anchorSpacing);

        for (int a = 0; a < anchorCount; ++a)
        {
            const int anchorBase = (profile.baseStartBand + a * spacing) % kLossBandCount;
            const int dir = profile.reverse ? -1 : 1;
            const int centerBand = (anchorBase + dir * sequenceHeadBand + kLossBandCount * 8) % kLossBandCount;

            for (int i = 0; i < kLossBandCount; ++i)
            {
                const int d1 = std::abs(i - centerBand);
                const int d2 = kLossBandCount - d1;
                const int dist = juce::jmin(d1, d2);

                if (dist <= profile.halfWindow)
                    lossMask[(size_t) i] = 1;
            }
        }

        for (int i = 0; i < kLossBandCount; ++i)
        {
            if (lossMask[(size_t) i] != 0)
                activeLossBands[(size_t) activeLossBandCount++] = i;
        }

        const int step = juce::jmax(1, profile.stepPerTick);
        sequenceHeadBand = (sequenceHeadBand + step) % kLossBandCount;

        nextLossRetriggerSeconds = nowSeconds + getSequencedRetriggerSeconds(preset, hostBpm);
    }

    else
    {
        for (int i = 0; i < kLossBandCount; ++i)
        {
            const float p = juce::jlimit(0.0f, 1.0f, lp.probabilities[(size_t) i]);
            const bool keep = lossRandom.nextFloat() < p;
            lossMask[(size_t) i] = keep ? (uint8_t) 1 : (uint8_t) 0;

            if (keep)
                activeLossBands[(size_t) activeLossBandCount++] = i;
        }

        nextLossRetriggerSeconds = nowSeconds + getRandomPresetRetriggerSeconds(hostBpm);
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

    {
        const juce::SpinLock::ScopedTryLockType sl(lossMaskSnapshotLock);
        if (sl.isLocked())
            lossMaskSnapshot = lossMask;
    }

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
    isShuttingDown.store(false, std::memory_order_release);
    isPrepared.store(true, std::memory_order_release);
    sequenceHeadBand = 0;

    const juce::SpinLock::ScopedLockType sl(oscilloscopeLock);
    std::fill(oscilloscopeBuffer.begin(), oscilloscopeBuffer.end(), 0.0f);
    oscilloscopeWritePos = 0;

    ensureLossFilters(sampleRate);
    resetStrictBandCutState();
    strictBandCutLastEnabled = isStrictBandCutEnabled();

    lossTimeSeconds = 0.0;
    nextLossRetriggerSeconds = 0.0;
    lastLossPresetIndex = juce::jlimit(0, 11, getDisplayPresetIndex());
    activeLossBandCount = 0;
    droppedLossBandCount = 0;
    lossMask.fill(1);
    lossBandWet.fill(1.0f);

    {
        const juce::SpinLock::ScopedTryLockType maskLock(lossMaskSnapshotLock);
        if (maskLock.isLocked())
            lossMaskSnapshot = lossMask;
    }

    retriggerLossMask(lossTimeSeconds);

}

void LDSJvstAudioProcessor::releaseResources()
{
    isPrepared.store(false, std::memory_order_release);
    isShuttingDown.store(true, std::memory_order_release);
    nextLossRetriggerSeconds = std::numeric_limits<double>::infinity();
    activeLossBandCount = 0;
    droppedLossBandCount = 0;

    lossMask.fill(1);
    lossBandWet.fill(0.0f);
    resetStrictBandCutState();

    const juce::SpinLock::ScopedTryLockType maskLock(lossMaskSnapshotLock);
    if (maskLock.isLocked())
        lossMaskSnapshot = lossMask;
}

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

void LDSJvstAudioProcessor::getLossMaskSnapshot(juce::Array<uint8_t>& dest)
{
    dest.resize(kLossBandCount);

    if (isShuttingDown.load(std::memory_order_acquire))
    {
        for (int i = 0; i < kLossBandCount; ++i)
            dest.set(i, 1);
        return;
    }

    const juce::SpinLock::ScopedTryLockType sl(lossMaskSnapshotLock);
    if (! sl.isLocked())
    {
        for (int i = 0; i < kLossBandCount; ++i)
            dest.set(i, 1);
        return;
    }

    for (int i = 0; i < kLossBandCount; ++i)
        dest.set(i, lossMaskSnapshot[(size_t) i]);
}

void LDSJvstAudioProcessor::resetStrictBandCutState()
{
    for (int ch = 0; ch < 2; ++ch)
    {
        if (! strictInputFifo[(size_t) ch].empty())
            std::fill(strictInputFifo[(size_t) ch].begin(), strictInputFifo[(size_t) ch].end(), 0.0f);

        if (! strictOutputFifo[(size_t) ch].empty())
            std::fill(strictOutputFifo[(size_t) ch].begin(), strictOutputFifo[(size_t) ch].end(), 0.0f);

        if (! strictOutputOverlap[(size_t) ch].empty())
            std::fill(strictOutputOverlap[(size_t) ch].begin(), strictOutputOverlap[(size_t) ch].end(), 0.0f);
    }
}

void LDSJvstAudioProcessor::processStrictBandCut(juce::AudioBuffer<float>& buffer, int numChannels)
{
    if (strictFft == nullptr)
        return;

    const int channels = juce::jlimit(0, 2, numChannels);
    if (channels <= 0)
        return;

    const int numSamples = buffer.getNumSamples();
    if (numSamples <= 0)
        return;

    constexpr float lo = 20.0f;
    constexpr float hi = 20000.0f;
    const float ratio = std::pow(hi / lo, 1.0f / (float) (kLossBandCount - 1));

    std::array<uint8_t, kLossBandCount> localMask {};
    for (int b = 0; b < kLossBandCount; ++b)
        localMask[(size_t) b] = lossMask[(size_t) b];

    const double sr = juce::jmax(1.0, getSampleRate());
    const float nyquist = (float) (sr * 0.5);
    const float maxBandHz = juce::jmax(lo, nyquist * 0.999f);

    std::vector<float> binGain((size_t) (kStrictFftSize / 2 + 1), 1.0f);

    for (int b = 0; b < kLossBandCount; ++b)
    {
        if (localMask[(size_t) b] != 0)
            continue;

        const float center = getLossBandCenterHz(b);
        const float fLo = juce::jlimit(lo, maxBandHz, center / std::sqrt(ratio));
        const float fHi = juce::jlimit(lo, maxBandHz, center * std::sqrt(ratio));

        const int k0 = juce::jlimit(0, kStrictFftSize / 2,
                                    (int) std::floor((double) fLo * (double) kStrictFftSize / sr));
        const int k1 = juce::jlimit(0, kStrictFftSize / 2,
                                    (int) std::ceil((double) fHi * (double) kStrictFftSize / sr));

        for (int k = k0; k <= k1; ++k)
            binGain[(size_t) k] = 0.0f;
    }

    for (int ch = 0; ch < channels; ++ch)
    {
        auto* x = buffer.getWritePointer(ch);
        auto& inFifo = strictInputFifo[(size_t) ch];
        auto& outFifo = strictOutputFifo[(size_t) ch];
        auto& overlap = strictOutputOverlap[(size_t) ch];
        auto& fftData = strictProcessTemp[(size_t) ch];

        if ((int) inFifo.size() != kStrictFftSize ||
            (int) outFifo.size() != kStrictFftSize ||
            (int) overlap.size() != kStrictFftSize ||
            (int) fftData.size() != 2 * kStrictFftSize)
            continue;

        int writePos = 0;
        while (writePos < numSamples)
        {
            const int chunk = juce::jmin(kStrictHopSize, numSamples - writePos);

            std::move(inFifo.begin() + chunk, inFifo.end(), inFifo.begin());
            std::copy(x + writePos, x + writePos + chunk, inFifo.end() - chunk);

            std::move(outFifo.begin() + chunk, outFifo.end(), outFifo.begin());
            std::fill(outFifo.end() - chunk, outFifo.end(), 0.0f);

            std::fill(fftData.begin(), fftData.end(), 0.0f);
            for (int n = 0; n < kStrictFftSize; ++n)
                fftData[(size_t) n] = inFifo[(size_t) n] * strictWindow[(size_t) n];

            strictFft->performRealOnlyForwardTransform(fftData.data());

            fftData[0] *= binGain[0];
            fftData[1] *= binGain[(size_t) (kStrictFftSize / 2)];

            for (int k = 1; k < kStrictFftSize / 2; ++k)
            {
                const float g = binGain[(size_t) k];
                fftData[(size_t) (2 * k)] *= g;
                fftData[(size_t) (2 * k + 1)] *= g;
            }

            strictFft->performRealOnlyInverseTransform(fftData.data());

            const float norm = 2.0f / (3.0f * (float) kStrictFftSize);
            for (int n = 0; n < kStrictFftSize; ++n)
            {
                const float yw = fftData[(size_t) n] * strictWindow[(size_t) n] * norm;
                outFifo[(size_t) n] += yw + overlap[(size_t) n];
            }

            std::fill(overlap.begin(), overlap.end(), 0.0f);
            for (int n = 0; n < (kStrictFftSize - kStrictHopSize); ++n)
                overlap[(size_t) n] = outFifo[(size_t) (n + kStrictHopSize)];

            std::copy(outFifo.begin(), outFifo.begin() + chunk, x + writePos);
            writePos += chunk;
        }
    }
}

void LDSJvstAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)

{
    juce::ScopedNoDenormals noDenormals;

    if (isShuttingDown.load(std::memory_order_acquire) || (! isPrepared.load(std::memory_order_acquire)))
        return;

    const auto totalNumInputChannels  = getTotalNumInputChannels();
    const auto totalNumOutputChannels = getTotalNumOutputChannels();

    // 清理多余输出通道（例如输入是mono而输出是stereo等情况）
    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear(i, 0, buffer.getNumSamples());

    // ============================================================
    // Hard Bypass：电视机电源键语义，直接透传原信号，不进入任何后续处理
    // ============================================================
    if (bypassed.load(std::memory_order_acquire))
        return;

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

    const bool strictEnabled = isStrictBandCutEnabled();
    if (strictEnabled != strictBandCutLastEnabled)
    {
        resetStrictBandCutState();
        strictBandCutLastEnabled = strictEnabled;
    }

    if (strictEnabled)
    {
        processStrictBandCut(buffer, totalNumOutputChannels);
    }
    else
    {
        auto* left  = (totalNumOutputChannels > 0) ? buffer.getWritePointer(0) : nullptr;
        auto* right = (totalNumOutputChannels > 1) ? buffer.getWritePointer(1) : nullptr;

        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            // 先统一更新本 sample 的频段 wet（与声道无关），再分别处理左右声道
            for (int b = 0; b < kLossBandCount; ++b)
            {
                const float targetWet = (lossMask[(size_t) b] == 0) ? 1.0f : 0.0f;
                const float prevWet = lossBandWet[(size_t) b];
                const float wet = targetWet + (prevWet - targetWet) * lossMaskSmoothCoeff;
                lossBandWet[(size_t) b] = wet;
            }

            if (left != nullptr)
            {
                float x = left[i];
                for (int b = 0; b < kLossBandCount; ++b)
                {
                    const float wet = lossBandWet[(size_t) b];
                    const float y = lossBandNotchL[(size_t) b].processSingleSampleRaw(x);
                    x = x + wet * (y - x);
                }
                left[i] = x;
            }

            if (right != nullptr)
            {
                float x = right[i];
                for (int b = 0; b < kLossBandCount; ++b)
                {
                    const float wet = lossBandWet[(size_t) b];
                    const float y = lossBandNotchR[(size_t) b].processSingleSampleRaw(x);
                    x = x + wet * (y - x);
                }
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

    state.setProperty("bypassed", bypassed.load(std::memory_order_relaxed) ? 1 : 0, nullptr);

    state.setProperty("preGainDb", (double) getPreGainDb(), nullptr);
    state.setProperty("randomRetriggerPerBeat", (double) getRandomRetriggerPerBeat(), nullptr);
    state.setProperty("limiterThreshold", (double) getLimiterThreshold(), nullptr);
    state.setProperty("lossNotchQ", (double) getLossNotchQ(), nullptr);
    state.setProperty("lossAlgorithmMode", getLossAlgorithmMode(), nullptr);
    state.setProperty("strictBandCutEnabled", isStrictBandCutEnabled() ? 1 : 0, nullptr);

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
    bypassed.store(((int) state.getProperty("bypassed", 0)) != 0, std::memory_order_relaxed);

    setPreGainDb((float) (double) state.getProperty("preGainDb", 10.0));
    setRandomRetriggerPerBeat((float) (double) state.getProperty("randomRetriggerPerBeat", 4.0));
    setLimiterThreshold((float) (double) state.getProperty("limiterThreshold", 1.0));
    setLossNotchQ((float) (double) state.getProperty("lossNotchQ", 11.0));
    setLossAlgorithmMode((int) state.getProperty("lossAlgorithmMode", kLossAlgorithmLegacy));
    setStrictBandCutEnabled(((int) state.getProperty("strictBandCutEnabled", 0)) != 0);

}

// 插件入口实现
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new LDSJvstAudioProcessor();
}