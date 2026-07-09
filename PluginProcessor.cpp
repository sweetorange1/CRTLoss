#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "display_present.h"
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
    lossMask.fill(1);

    lossMaskSnapshot.fill(1);
    lossBandWet.fill(1.0f);
    activeLossBandCount = 0;
    lossRandom.setSeedRandomly();


}

LDSJvstAudioProcessor::~LDSJvstAudioProcessor()
{
    isPrepared.store(false, std::memory_order_release);
    isShuttingDown.store(true, std::memory_order_release);
    nextLossRetriggerSeconds = std::numeric_limits<double>::infinity();
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
    const auto cfg = display_present::getPresetParamsForChannel(presetIndex);
    return cfg.lossSequence.enabled;
}

LDSJvstAudioProcessor::SequencedPresetProfile LDSJvstAudioProcessor::getSequencedPresetProfile(int presetIndex) noexcept
{
    const auto cfg = display_present::getPresetParamsForChannel(presetIndex);
    const auto& seq = cfg.lossSequence;
    if (! seq.enabled)
        return {};

    return {
        juce::jlimit(1, 8, seq.anchorCount),
        juce::jmax(1, seq.anchorSpacing),
        juce::jlimit(0, kLossBandCount - 1, seq.baseStartBand),
        juce::jmax(0, seq.halfWindow),
        juce::jmax(1, seq.stepPerTick),
        seq.reverse
    };
}


double LDSJvstAudioProcessor::getSequencedRetriggerSeconds(int presetIndex, double bpm) const noexcept
{
    const double safeBpm = juce::jlimit(40.0, 260.0, bpm > 0.0 ? bpm : 120.0);
    const double beatSec = 60.0 / safeBpm;

    const auto cfg = display_present::getPresetParamsForChannel(presetIndex);
    const auto& seq = cfg.lossSequence;
    if (! seq.enabled)
        return 0.10;

    return juce::jlimit((double) seq.minRetriggerSeconds,
                        (double) seq.maxRetriggerSeconds,
                        beatSec * (double) seq.bpmDivision);
}


double LDSJvstAudioProcessor::getRandomPresetRetriggerSeconds(double bpm) const noexcept
{
    const double safeBpm = juce::jlimit(40.0, 260.0, bpm > 0.0 ? bpm : 120.0);
    const double beatSec = 60.0 / safeBpm;
    const double perBeat = (double) juce::jlimit(kRandomRetriggerPerBeatMin,
                                                  kRandomRetriggerPerBeatMax,
                                                  getRandomRetriggerPerBeat());

    const int channelId = juce::jlimit(LDSJvstAudioProcessor::kDisplayChannelMin,
                                       LDSJvstAudioProcessor::kDisplayChannelMax,
                                       getDisplayPresetIndex());
    const auto cfg = display_present::getPresetParamsForChannel(channelId);
    const auto& rnd = cfg.lossRandom;
    const double scale = juce::jmax(0.0001, (double) rnd.perPresetScale);

    return juce::jlimit((double) rnd.minRetriggerSeconds,
                        (double) rnd.maxRetriggerSeconds,
                        beatSec / (perBeat * scale));
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

// ============================================================================
// FFT-Mask 算法（Loss Algorithm Mode == kLossAlgorithmFftMask）
// STFT + Hann + 75% overlap + OLA。
// 参考 SpectrumTag 的 STFT 处理管线。
// ============================================================================

void LDSJvstAudioProcessor::rebuildStft(double sampleRate, int numChannels)
{
    juce::ignoreUnused(sampleRate);

    const int N = juce::jmax(64, stftSize);
    const int hop = juce::jmax(1, N / 4);
    const int numBins = N / 2 + 1;

    stftSize = N;
    stftHop  = hop;
    stftStates.clear();
    stftStates.resize((size_t) juce::jmax(1, numChannels));

    std::vector<float> hann((size_t) N);
    for (int n = 0; n < N; ++n)
        hann[(size_t) n] = 0.5f * (1.0f - std::cos(juce::MathConstants<float>::twoPi
                                                    * (float) n / (float) juce::jmax(1, N - 1)));

    const int order = (int) std::round(std::log2((double) N));

    for (int ch = 0; ch < (int) stftStates.size(); ++ch)
    {
        auto& st = stftStates[(size_t) ch];
        st.fft = std::make_unique<juce::dsp::FFT>(order);
        st.window = hann;
        st.fftWork.assign((size_t) (2 * N), 0.0f);
        st.inputRing.assign((size_t) N, 0.0f);
        st.dryDelayRing.assign((size_t) N, 0.0f);
        st.outputRing.assign((size_t) N, 0.0f);
        st.olaNormRing.assign((size_t) N, 0.0f);
        st.smoothedGains.assign((size_t) numBins, 1.0f);
        st.outFifo.assign((size_t) (N * 2), 0.0f);
        st.inputPos = 0;
        st.accumCount = 0;
        st.frameCount = 0;
        st.dryDelayWritePos = 0;
        st.outFifoWrite = 0;
        st.outFifoRead = 0;
        st.outFifoCount = 0;
    }
}

void LDSJvstAudioProcessor::resetStftState() noexcept
{
    for (auto& st : stftStates)
    {
        std::fill(st.inputRing.begin(),    st.inputRing.end(),    0.0f);
        std::fill(st.dryDelayRing.begin(), st.dryDelayRing.end(), 0.0f);
        std::fill(st.outputRing.begin(),   st.outputRing.end(),   0.0f);
        std::fill(st.olaNormRing.begin(),  st.olaNormRing.end(),  0.0f);
        std::fill(st.outFifo.begin(),      st.outFifo.end(),      0.0f);
        std::fill(st.smoothedGains.begin(),st.smoothedGains.end(),1.0f);
        st.inputPos = 0;
        st.accumCount = 0;
        st.frameCount = 0;
        st.dryDelayWritePos = 0;
        st.outFifoWrite = 0;
        st.outFifoRead = 0;
        st.outFifoCount = 0;
    }
}

// 单帧 STFT 处理：从 inputRing 取最新 N 个采样加窗 → FFT → 逐 bin 乘增益（相位保留）
// → IFFT → 加合成窗 OLA 到 outputRing → 用 ∑w² 归一化后写入输出 FIFO。
void LDSJvstAudioProcessor::processStftFrame(int ch,
                                             const std::vector<float>& targetBinGains,
                                             float gainSmoothAlpha)
{
    if (ch < 0 || ch >= (int) stftStates.size())
        return;

    auto& st = stftStates[(size_t) ch];
    if (st.fft == nullptr)
        return;

    const int N = stftSize;
    const int hop = stftHop;
    const int numBins = N / 2 + 1;

    // Step 1) 加窗取 N 个最新采样。inputPos 指向"下一个写入位置"，因此最老采样从 inputPos 开始（环形）
    const int frameStart = st.inputPos;
    for (int n = 0; n < N; ++n)
    {
        const int idx = (frameStart + n) % N;
        st.fftWork[(size_t) n] = st.inputRing[(size_t) idx] * st.window[(size_t) n];
    }
    std::fill(st.fftWork.begin() + N, st.fftWork.end(), 0.0f);

    // Step 2) 实数 FFT
    st.fft->performRealOnlyForwardTransform(st.fftWork.data());

    // Step 3) 逐 bin 应用平滑增益（保留相位）
    // JUCE real-only 打包：bin0=fftWork[0]，binN/2=fftWork[1]，bin1..N/2-1=fftWork[2k]/[2k+1]
    {
        const float target = targetBinGains[0];
        float& smooth = st.smoothedGains[0];
        smooth += (target - smooth) * gainSmoothAlpha;
        st.fftWork[0] *= smooth;
    }
    for (int k = 1; k < numBins - 1; ++k)
    {
        const float target = targetBinGains[(size_t) k];
        float& smooth = st.smoothedGains[(size_t) k];
        smooth += (target - smooth) * gainSmoothAlpha;
        st.fftWork[(size_t) (2 * k)]     *= smooth;
        st.fftWork[(size_t) (2 * k + 1)] *= smooth;
    }
    {
        const int kNyq = numBins - 1;
        const float target = targetBinGains[(size_t) kNyq];
        float& smooth = st.smoothedGains[(size_t) kNyq];
        smooth += (target - smooth) * gainSmoothAlpha;
        st.fftWork[1] *= smooth;
    }

    // Step 4) 实数 IFFT（JUCE 内部会除以 N）
    st.fft->performRealOnlyInverseTransform(st.fftWork.data());

    // Step 5) 加合成窗 OLA 累加，同时累积 w² 用于逐样本归一化
    for (int n = 0; n < N; ++n)
    {
        const int outIdx = (frameStart + n) % N;
        const float w = st.window[(size_t) n];
        st.outputRing[(size_t) outIdx]  += st.fftWork[(size_t) n] * w;
        st.olaNormRing[(size_t) outIdx] += w * w;
    }

    // Step 6) 预热完成后每帧推 hop 个稳态归一化样本到 FIFO
    ++st.frameCount;
    if (st.frameCount >= N / hop)
    {
        const int fifoStart = frameStart;
        constexpr float kNormEps = 1.0e-8f;
        for (int i = 0; i < hop; ++i)
        {
            const int pos = (fifoStart + i) % N;
            const float norm = st.olaNormRing[(size_t) pos];
            const float y = (norm > kNormEps)
                ? (st.outputRing[(size_t) pos] / norm)
                : st.outputRing[(size_t) pos];

            st.outFifo[(size_t) st.outFifoWrite] = y;
            st.outputRing[(size_t) pos]   = 0.0f;
            st.olaNormRing[(size_t) pos]  = 0.0f;
            st.outFifoWrite = (st.outFifoWrite + 1) % (int) st.outFifo.size();
            ++st.outFifoCount;
        }
    }
}

// 将频段掩码映射到 FFT bin 后驱动 STFT。lossMaskInvertedNow 已在调用方按业务语义解析。
void LDSJvstAudioProcessor::processFftLossMode(juce::AudioBuffer<float>& buffer,
                                               bool lossMaskInvertedNow)
{
    if (stftStates.empty())
        return;

    const int numCh = juce::jmin(buffer.getNumChannels(), (int) stftStates.size());
    const int numSamps = buffer.getNumSamples();
    if (numCh <= 0 || numSamps <= 0)
        return;

    const double sr = juce::jmax(1.0, getSampleRate());
    const int N = stftSize;
    const int hop = stftHop;
    const int numBins = N / 2 + 1;

    // 增益平滑：10ms 时间常数，与 SpectrumTag 一致
    const float frameSeconds = (float) hop / (float) sr;
    const float smoothAlpha  = 1.0f - std::exp(- frameSeconds / 0.010f);

    // 干湿延迟对齐：N - hop = 3N/4
    const int dryDelaySamples = juce::jlimit(0, N - 1, N - hop);

    std::vector<float> targetBinGains((size_t) numBins, 1.0f);

    // ------------------------------------------------------------------
    //  预先根据 lossBandWet[100] 计算 targetBinGains[numBins]：
    //    对每个 FFT bin，取其中心频率 hz = k * sr / N，
    //    通过对数映射到 100 个频段中的某一个，读取 lossBandWet[band] 作为丢失强度。
    //    lossMaskInverted 已经在 processBlock 中融合进 lossBandWet 的目标值。
    //    目标增益 g = 1 - wet。
    //  低于 20Hz 或高于 20kHz 的 bin 不做处理（保持 unity）。
    //  低于最低段中心的低频保护 & 高于最高段中心的高频保护同理处理为 unity。
    // ------------------------------------------------------------------
    juce::ignoreUnused(lossMaskInvertedNow); // 反转已通过 lossBandWet 表达
    constexpr float kBandLo = 20.0f;
    constexpr float kBandHi = 20000.0f;
    const float logRatio = std::log(kBandHi / kBandLo);

    targetBinGains[0] = 1.0f; // DC 恒 unity
    for (int k = 1; k < numBins; ++k)
    {
        const float hz = (float) k * (float) sr / (float) N;
        if (hz < kBandLo || hz > kBandHi)
        {
            targetBinGains[(size_t) k] = 1.0f;
            continue;
        }

        // 对数映射：与 getLossBandCenterHz 相反的映射
        const float t = std::log(hz / kBandLo) / logRatio; // 0..1
        int band = (int) std::round(t * (float) (kLossBandCount - 1));
        band = juce::jlimit(0, kLossBandCount - 1, band);

        const float wet = lossBandWet[(size_t) band];
        const float g = 1.0f - juce::jlimit(0.0f, 1.0f, wet);
        targetBinGains[(size_t) k] = g;
    }

    // ------------------------------------------------------------------
    //  Sample-major 主循环：每个采样点分通道写入 inputRing / dryDelayRing，
    //  累计到 hop 后触发一帧 STFT。同时输出侧从 FIFO 取一个采样写回 buffer；
    //  FIFO 未就绪（预热期）则以延迟对齐的 dry 样本兜底，保证零 pop。
    // ------------------------------------------------------------------
    for (int n = 0; n < numSamps; ++n)
    {
        bool frameReady = false;

        for (int ch = 0; ch < numCh; ++ch)
        {
            auto& st = stftStates[(size_t) ch];
            const float inSample = buffer.getReadPointer(ch)[n];

            st.inputRing[(size_t) st.inputPos] = inSample;
            st.inputPos = (st.inputPos + 1) % N;

            const int dryWritePos = st.dryDelayWritePos;
            st.dryDelayRing[(size_t) dryWritePos] = inSample;
            st.dryDelayWritePos = (dryWritePos + 1) % N;

            ++st.accumCount;
            if (st.accumCount >= hop)
                frameReady = true;
        }

        if (frameReady)
        {
            for (int ch = 0; ch < numCh; ++ch)
            {
                auto& st = stftStates[(size_t) ch];
                st.accumCount -= hop;
                processStftFrame(ch, targetBinGains, smoothAlpha);
            }
        }

        // 输出：FIFO 就绪则取 wet，否则取延迟 dry（预热期）。
        for (int ch = 0; ch < numCh; ++ch)
        {
            auto& st = stftStates[(size_t) ch];

            const int dryReadPos = (st.dryDelayWritePos + N - dryDelaySamples) % N;
            const float delayedDry = st.dryDelayRing[(size_t) dryReadPos];

            float out = delayedDry;
            if (st.outFifoCount > 0)
            {
                out = st.outFifo[(size_t) st.outFifoRead];
                st.outFifoRead = (st.outFifoRead + 1) % (int) st.outFifo.size();
                --st.outFifoCount;
            }

            buffer.getWritePointer(ch)[n] = out;
        }
    }
}

void LDSJvstAudioProcessor::ensureCutFilters(double sampleRate)
{
    if (sampleRate <= 0.0)
        return;

    const float lowHz = juce::jlimit(kLowCutHzMin, kLowCutHzMax, getLowCutHz());
    const float highHz = juce::jlimit(kHighCutHzMin, kHighCutHzMax, getHighCutHz());
    const int slope = getCutSlopeDbPerOct();

    const bool sampleRateUnchanged = (sampleRate == currentSampleRateForCutFilter);
    const bool lowUnchanged = (std::abs(lowHz - currentLowCutHzForCutFilter) < 0.0001f);
    const bool highUnchanged = (std::abs(highHz - currentHighCutHzForCutFilter) < 0.0001f);
    const bool slopeUnchanged = (slope == currentSlopeForCutFilter);

    if (sampleRateUnchanged && lowUnchanged && highUnchanged && slopeUnchanged)
        return;

    const bool shouldResetFilterState = !sampleRateUnchanged;

    currentSampleRateForCutFilter = sampleRate;
    currentLowCutHzForCutFilter = lowHz;
    currentHighCutHzForCutFilter = highHz;
    currentSlopeForCutFilter = slope;

    const int stages = juce::jlimit(1, kCutFilterMaxStages, slope / 12);

    for (int i = 0; i < stages; ++i)
    {
        if (shouldResetFilterState)
        {
            cutHighPassL[(size_t) i].reset();
            cutHighPassR[(size_t) i].reset();
            cutLowPassL[(size_t) i].reset();
            cutLowPassR[(size_t) i].reset();
        }

        cutHighPassL[(size_t) i].setCoefficients(juce::IIRCoefficients::makeHighPass(sampleRate, lowHz));
        cutHighPassR[(size_t) i].setCoefficients(juce::IIRCoefficients::makeHighPass(sampleRate, lowHz));
        cutLowPassL[(size_t) i].setCoefficients(juce::IIRCoefficients::makeLowPass(sampleRate, highHz));
        cutLowPassR[(size_t) i].setCoefficients(juce::IIRCoefficients::makeLowPass(sampleRate, highHz));
    }

}

void LDSJvstAudioProcessor::processHpfLpfCut(juce::AudioBuffer<float>& buffer) noexcept
{
    const int slope = getCutSlopeDbPerOct();
    const int stages = juce::jlimit(1, kCutFilterMaxStages, slope / 12);

    auto* left  = (buffer.getNumChannels() > 0) ? buffer.getWritePointer(0) : nullptr;
    auto* right = (buffer.getNumChannels() > 1) ? buffer.getWritePointer(1) : nullptr;

    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        if (left != nullptr)
        {
            float x = left[i];
            for (int s = 0; s < stages; ++s)
            {
                x = cutHighPassL[(size_t) s].processSingleSampleRaw(x);
                x = cutLowPassL[(size_t) s].processSingleSampleRaw(x);
            }
            left[i] = x;
        }

        if (right != nullptr)
        {
            float x = right[i];
            for (int s = 0; s < stages; ++s)
            {
                x = cutHighPassR[(size_t) s].processSingleSampleRaw(x);
                x = cutLowPassR[(size_t) s].processSingleSampleRaw(x);
            }
            right[i] = x;
        }
    }
}

void LDSJvstAudioProcessor::applyCutMaskToLossMask() noexcept
{
    const float lowHz = juce::jlimit(kLowCutHzMin, kLowCutHzMax, getLowCutHz());
    const float highHz = juce::jlimit(kHighCutHzMin, kHighCutHzMax, getHighCutHz());

    activeLossBandCount = 0;
    droppedLossBandCount = 0;

    for (int i = 0; i < kLossBandCount; ++i)
    {
        const float f = getLossBandCenterHz(i);
        const bool inCutRegion = (f < lowHz) || (f > highHz);

        if (inCutRegion)
            lossMask[(size_t) i] = 0;

        if (lossMask[(size_t) i] != 0)
            activeLossBands[(size_t) activeLossBandCount++] = i;
        else
            droppedLossBands[(size_t) droppedLossBandCount++] = i;
    }

    // 若高低切设置导致无可用频段，则保留一个最接近lowCutHz的频段作为兜底
    if (activeLossBandCount <= 0)
    {
        int best = 0;
        float bestDist = std::abs(getLossBandCenterHz(0) - lowHz);
        for (int i = 1; i < kLossBandCount; ++i)
        {
            const float d = std::abs(getLossBandCenterHz(i) - lowHz);
            if (d < bestDist)
            {
                best = i;
                bestDist = d;
            }
        }

        lossMask[(size_t) best] = 1;
        activeLossBands[0] = best;
        activeLossBandCount = 1;

        droppedLossBandCount = 0;
        for (int i = 0; i < kLossBandCount; ++i)
            if (lossMask[(size_t) i] == 0)
                droppedLossBands[(size_t) droppedLossBandCount++] = i;
    }

    const juce::SpinLock::ScopedTryLockType sl(lossMaskSnapshotLock);
    if (sl.isLocked())
        lossMaskSnapshot = lossMask;
}

void LDSJvstAudioProcessor::retriggerLossMask(double nowSeconds)
{
    const int channelId = juce::jlimit(LDSJvstAudioProcessor::kDisplayChannelMin,
                                       LDSJvstAudioProcessor::kDisplayChannelMax,
                                       getDisplayPresetIndex());

    const float lowHz = juce::jlimit(kLowCutHzMin, kLowCutHzMax, getLowCutHz());
    const float highHz = juce::jlimit(kHighCutHzMin, kHighCutHzMax, getHighCutHz());
    const auto isBandAllowedByCut = [lowHz, highHz](int band) noexcept
    {
        const float f = getLossBandCenterHz(band);
        return (f >= lowHz) && (f <= highHz);
    };

    double hostBpm = 120.0;
    if (auto* playHead = getPlayHead())
    {
        juce::AudioPlayHead::CurrentPositionInfo pos;
        if (playHead->getCurrentPosition(pos) && pos.bpm > 0.0)
            hostBpm = pos.bpm;
    }

    activeLossBandCount = 0;

    if (isBpmSequencedPreset(channelId))
    {
        const auto profile = getSequencedPresetProfile(channelId);

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

                if (dist <= profile.halfWindow && isBandAllowedByCut(i))
                    lossMask[(size_t) i] = 1;
            }
        }

        const int step = juce::jmax(1, profile.stepPerTick);
        sequenceHeadBand = (sequenceHeadBand + step) % kLossBandCount;

        nextLossRetriggerSeconds = nowSeconds + getSequencedRetriggerSeconds(channelId, hostBpm);
    }
    else
    {
        for (int i = 0; i < kLossBandCount; ++i)
        {
            if (! isBandAllowedByCut(i))
            {
                lossMask[(size_t) i] = 0;
                continue;
            }

            const float p = juce::jlimit(0.0f, 1.0f,
                                         display_present::getLossKeepProbabilityForChannel(channelId, i, kLossBandCount));
            const bool keep = lossRandom.nextFloat() < p;
            lossMask[(size_t) i] = keep ? (uint8_t) 1 : (uint8_t) 0;

            if (keep)
                activeLossBands[(size_t) activeLossBandCount++] = i;
        }

        nextLossRetriggerSeconds = nowSeconds + getRandomPresetRetriggerSeconds(hostBpm);
    }

    applyCutMaskToLossMask();
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

void LDSJvstAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    isShuttingDown.store(false, std::memory_order_release);

    const juce::SpinLock::ScopedLockType processingLock(processingStateLock);

    isPrepared.store(true, std::memory_order_release);
    sequenceHeadBand = 0;

    const juce::SpinLock::ScopedLockType sl(oscilloscopeLock);
    std::fill(oscilloscopeBuffer.begin(), oscilloscopeBuffer.end(), 0.0f);
    oscilloscopeWritePos = 0;

    ensureLossFilters(sampleRate);

    lossTimeSeconds = 0.0;
    nextLossRetriggerSeconds = 0.0;
    lastLossPresetIndex = juce::jlimit(LDSJvstAudioProcessor::kDisplayChannelMin,
                                      LDSJvstAudioProcessor::kDisplayChannelMax,
                                      getDisplayPresetIndex());

    currentLowCutHzForMask = getLowCutHz();
    currentHighCutHzForMask = getHighCutHz();
    currentCutModeForMask = getCutMode();
    currentLossMaskInvertedForMask = isLossMaskInverted() ? 1 : 0;
    currentSampleRateForCutFilter = 0.0;

    currentLowCutHzForCutFilter = -1.0f;
    currentHighCutHzForCutFilter = -1.0f;
    currentSlopeForCutFilter = -1;
    ensureCutFilters(sampleRate);
    activeLossBandCount = 0;

    droppedLossBandCount = 0;
    lossMask.fill(1);
    lossBandWet.fill(1.0f);

    cutCrossfadeSamplesRemaining = 0;
    lastLowCutHzForCrossfade = -1.0f;
    lastHighCutHzForCrossfade = -1.0f;
    lastSlopeForCrossfade = -1;
    cutDryBuffer.setSize(juce::jmax(1, getTotalNumOutputChannels()),
                         juce::jmax(1, samplesPerBlock),
                         false, false, true);
    cutDryBuffer.clear();

    {
        const juce::SpinLock::ScopedTryLockType maskLock(lossMaskSnapshotLock);
        if (maskLock.isLocked())
            lossMaskSnapshot = lossMask;
    }

    retriggerLossMask(lossTimeSeconds);

    // FFT-Mask 分支的 STFT/OLA 状态：按当前通道数重建，并按算法模式上报插件延迟。
    // 只有在 FFT 模式下才上报非零延迟，避免 IIR 分支强行引入 3N/4 采样的 latency。
    const int fftNumCh = juce::jmax(getTotalNumInputChannels(), getTotalNumOutputChannels());
    rebuildStft(sampleRate, juce::jmax(1, fftNumCh));
    resetStftState();

    const int algoNow = getLossAlgorithmMode();
    const int desiredLatency = (algoNow == kLossAlgorithmFftMask) ? juce::jmax(0, stftSize - stftHop) : 0;
    setLatencySamples(desiredLatency);
    stftLatencyReported = desiredLatency;
    lastStftAlgorithmMode = algoNow;

}

void LDSJvstAudioProcessor::releaseResources()
{
    isPrepared.store(false, std::memory_order_release);
    isShuttingDown.store(true, std::memory_order_release);

    // 宿主卸载阶段不阻塞等待音频线程，避免与 processBlock 内部路径互相等待导致卡死。
    const juce::SpinLock::ScopedTryLockType processingLock(processingStateLock);
    if (! processingLock.isLocked())
        return;

    nextLossRetriggerSeconds = std::numeric_limits<double>::infinity();
    activeLossBandCount = 0;
    droppedLossBandCount = 0;
    currentSampleRateForCutFilter = 0.0;
    currentLowCutHzForCutFilter = -1.0f;
    currentHighCutHzForCutFilter = -1.0f;
    currentSlopeForCutFilter = -1;
    lastLowCutHzForCrossfade = -1.0f;
    lastHighCutHzForCrossfade = -1.0f;
    lastSlopeForCrossfade = -1;
    currentLossMaskInvertedForMask = isLossMaskInverted() ? 1 : 0;

    lossMask.fill(1);

    lossBandWet.fill(0.0f);

    // FFT-Mask 状态清理：避免宿主复位/挂起阶段 stftStates 引用悬空内存
    for (auto& st : stftStates)
    {
        std::fill(st.inputRing.begin(),    st.inputRing.end(),    0.0f);
        std::fill(st.dryDelayRing.begin(), st.dryDelayRing.end(), 0.0f);
        std::fill(st.outputRing.begin(),   st.outputRing.end(),   0.0f);
        std::fill(st.olaNormRing.begin(),  st.olaNormRing.end(),  0.0f);
        std::fill(st.outFifo.begin(),      st.outFifo.end(),      0.0f);
        std::fill(st.smoothedGains.begin(),st.smoothedGains.end(),1.0f);
        st.inputPos = 0;
        st.accumCount = 0;
        st.frameCount = 0;
        st.dryDelayWritePos = 0;
        st.outFifoWrite = 0;
        st.outFifoRead = 0;
        st.outFifoCount = 0;
    }
    lastStftAlgorithmMode = -1;

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

    const bool inverted = isLossMaskInverted();
    const uint8_t fallbackBit = inverted ? (uint8_t) 0 : (uint8_t) 1;

    if (isShuttingDown.load(std::memory_order_acquire))
    {
        for (int i = 0; i < kLossBandCount; ++i)
            dest.set(i, fallbackBit);
        return;
    }

    const juce::SpinLock::ScopedTryLockType sl(lossMaskSnapshotLock);
    if (! sl.isLocked())
    {
        for (int i = 0; i < kLossBandCount; ++i)
            dest.set(i, fallbackBit);
        return;
    }

    for (int i = 0; i < kLossBandCount; ++i)
    {
        const uint8_t raw = lossMaskSnapshot[(size_t) i];
        const uint8_t effective = inverted ? (uint8_t) ((raw == 0) ? 1 : 0) : raw;
        dest.set(i, effective);
    }
}


void LDSJvstAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)

{
    juce::ScopedNoDenormals noDenormals;

    if (isShuttingDown.load(std::memory_order_acquire) || (! isPrepared.load(std::memory_order_acquire)))
        return;

    const juce::SpinLock::ScopedLockType processingLock(processingStateLock);
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
    // 3) 高低切（可切换）
    //    - HardMask：不在音频链路中额外滤波
    //    - HPF/LPF：使用可变斜率的高通/低通级联滤波链（含过渡交叉淡化）
    // ============================================================
    if (getCutMode() == kCutModeHpfLpf)
    {
        const float lowHzNow = getLowCutHz();
        const float highHzNow = getHighCutHz();
        const int slopeNow = getCutSlopeDbPerOct();
        const bool cutDragNow = isCutDragActive();

        const bool cutFilterParamsChanged = (std::abs(lowHzNow - currentLowCutHzForCutFilter) > 0.0001f)
                                          || (std::abs(highHzNow - currentHighCutHzForCutFilter) > 0.0001f)
                                          || (slopeNow != currentSlopeForCutFilter)
                                          || (getSampleRate() != currentSampleRateForCutFilter);

        const bool hasPrevCrossfadeAnchor = (lastSlopeForCrossfade >= 0)
                                         && (lastLowCutHzForCrossfade > 0.0f)
                                         && (lastHighCutHzForCrossfade > 0.0f);

        // 拖拽时会产生大量细小参数更新；对“是否触发交叉淡化”加阈值，
        // 避免每次微调都重启淡化而带来电流感/拉链噪声。
        constexpr float kCutCrossfadeRetuneHzEpsilon = 18.0f;
        const bool cutParamsChangedForCrossfade = (! hasPrevCrossfadeAnchor)
                                               || (std::abs(lowHzNow - lastLowCutHzForCrossfade) > kCutCrossfadeRetuneHzEpsilon)
                                               || (std::abs(highHzNow - lastHighCutHzForCrossfade) > kCutCrossfadeRetuneHzEpsilon)
                                               || (slopeNow != lastSlopeForCrossfade);

        if (cutFilterParamsChanged && ! cutDragNow && cutParamsChangedForCrossfade && cutCrossfadeSamplesRemaining <= 0)
            cutCrossfadeSamplesRemaining = kCutCrossfadeSamples;

        if (cutFilterParamsChanged)
        {
            lastLowCutHzForCrossfade = lowHzNow;
            lastHighCutHzForCrossfade = highHzNow;
            lastSlopeForCrossfade = slopeNow;
        }

        const int numChannels = buffer.getNumChannels();
        const int numSamples = buffer.getNumSamples();
        if (cutDryBuffer.getNumChannels() < numChannels || cutDryBuffer.getNumSamples() < numSamples)
            cutDryBuffer.setSize(numChannels, numSamples, false, false, true);

        for (int ch = 0; ch < numChannels; ++ch)
            cutDryBuffer.copyFrom(ch, 0, buffer, ch, 0, numSamples);

        ensureCutFilters(getSampleRate());
        processHpfLpfCut(buffer);

        if (cutCrossfadeSamplesRemaining > 0)
        {
            const int totalFadeSamples = kCutCrossfadeSamples;
            const int fadeSamples = juce::jmin(numSamples, cutCrossfadeSamplesRemaining);
            const int processedSamplesBeforeThisBlock = totalFadeSamples - cutCrossfadeSamplesRemaining;

            for (int ch = 0; ch < numChannels; ++ch)
            {
                auto* wet = buffer.getWritePointer(ch);
                const auto* dry = cutDryBuffer.getReadPointer(ch);

                for (int i = 0; i < fadeSamples; ++i)
                {
                    const int fadeIndex = processedSamplesBeforeThisBlock + i + 1;
                    const float t = (float) fadeIndex / (float) juce::jmax(1, totalFadeSamples);
                    wet[i] = dry[i] + (wet[i] - dry[i]) * juce::jlimit(0.0f, 1.0f, t);
                }
            }

            cutCrossfadeSamplesRemaining -= fadeSamples;
        }
    }
    else
    {
        cutCrossfadeSamplesRemaining = 0;
        lastLowCutHzForCrossfade = -1.0f;
        lastHighCutHzForCrossfade = -1.0f;
        lastSlopeForCrossfade = -1;
    }

    // ============================================================
    // 4) 频带随机丢失：20Hz~20kHz 划分为 100 段，按当前预设概率矩阵 + 触发周期重随机
    //    实现方式：对“未命中保留”的频段施加窄带 Notch，从而模拟该段频率信息丢失
    // ============================================================

    ensureLossFilters(getSampleRate());

    const int currentPreset = juce::jlimit(LDSJvstAudioProcessor::kDisplayChannelMin,
                                           LDSJvstAudioProcessor::kDisplayChannelMax,
                                           getDisplayPresetIndex());

    if (currentPreset != lastLossPresetIndex)
    {
        if (! isLossMaskFrozen())
        {
            lastLossPresetIndex = currentPreset;
            retriggerLossMask(lossTimeSeconds);
        }
    }

    const double blockSeconds = (double) buffer.getNumSamples() / juce::jmax(1.0, getSampleRate());
    lossTimeSeconds += blockSeconds;

    if (! isLossMaskFrozen() && (lossTimeSeconds >= nextLossRetriggerSeconds))
        retriggerLossMask(lossTimeSeconds);

    const float lowCutNow = getLowCutHz();
    const float highCutNow = getHighCutHz();
    const int cutModeNow = getCutMode();
    const int lossMaskInvertedNowForMask = isLossMaskInverted() ? 1 : 0;

    const bool cutParamsChanged = (std::abs(lowCutNow - currentLowCutHzForMask) > 0.0001f) ||
                                  (std::abs(highCutNow - currentHighCutHzForMask) > 0.0001f) ||
                                  (cutModeNow != currentCutModeForMask) ||
                                  (lossMaskInvertedNowForMask != currentLossMaskInvertedForMask);

    if (cutParamsChanged)
    {
        applyCutMaskToLossMask();

        currentLowCutHzForMask = lowCutNow;
        currentHighCutHzForMask = highCutNow;
        currentCutModeForMask = cutModeNow;
        currentLossMaskInvertedForMask = lossMaskInvertedNowForMask;
    }

    const bool lossMaskInvertedNow = (lossMaskInvertedNowForMask != 0);
    const int algorithmModeNow = juce::jlimit(kLossAlgorithmLegacy,
                                              kLossAlgorithmFftMask,
                                              getLossAlgorithmMode());

    // 算法模式变化时重置 STFT 状态，避免残留数据造成一次性 pop
    if (algorithmModeNow != lastStftAlgorithmMode)
    {
        if (algorithmModeNow == kLossAlgorithmFftMask)
        {
            const int desiredLatency = juce::jmax(0, stftSize - stftHop);
            if (desiredLatency != stftLatencyReported)
            {
                setLatencySamples(desiredLatency);
                stftLatencyReported = desiredLatency;
            }
            resetStftState();
        }
        else
        {
            if (stftLatencyReported != 0)
            {
                setLatencySamples(0);
                stftLatencyReported = 0;
            }
        }
        lastStftAlgorithmMode = algorithmModeNow;
    }

    // 无论使用哪种算法，都要按 10ms 时间常数平滑 lossBandWet（wet=1 表示丢失）
    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        for (int b = 0; b < kLossBandCount; ++b)
        {
            const bool droppedByMask = (lossMask[(size_t) b] == 0);
            const bool effectiveDropped = lossMaskInvertedNow ? (! droppedByMask) : droppedByMask;
            const float targetWet = effectiveDropped ? 1.0f : 0.0f;
            const float prevWet = lossBandWet[(size_t) b];
            const float wet = targetWet + (prevWet - targetWet) * lossMaskSmoothCoeff;
            lossBandWet[(size_t) b] = wet;
        }
        // 注：为保持与 IIR 分支一致，wet 每 sample 都推进一次（哪怕 FFT 分支在 processFftLossMode
        // 中只在 hop 边界读取一次，也需要让 wet 收敛到当前 mask 目标）。
    }

    if (algorithmModeNow == kLossAlgorithmFftMask)
    {
        // FFT 分支：直接在 STFT/OLA 中完成频段掩码，跳过 IIR notch
        processFftLossMode(buffer, lossMaskInvertedNow);
    }
    else
    {
        // 传统 IIR notch 分支（Legacy / Uniform Bandwidth）
        auto* left  = (totalNumOutputChannels > 0) ? buffer.getWritePointer(0) : nullptr;
        auto* right = (totalNumOutputChannels > 1) ? buffer.getWritePointer(1) : nullptr;

        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
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
    // 5) 最终 0dB 硬削波：防止电平过高
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
    state.setProperty("lossMaskInverted", isLossMaskInverted() ? 1 : 0, nullptr);
    state.setProperty("lowCutHz", (double) getLowCutHz(), nullptr);
    state.setProperty("highCutHz", (double) getHighCutHz(), nullptr);
    state.setProperty("cutMode", getCutMode(), nullptr);
    state.setProperty("cutSlopeDbPerOct", getCutSlopeDbPerOct(), nullptr);

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

    // 某些宿主会在销毁/重建边界调用状态恢复；这里避免阻塞等待音频线程。
    const juce::SpinLock::ScopedTryLockType processingLock(processingStateLock);
    if (! processingLock.isLocked())
        return;

    setDisplayPresetIndex((int) state.getProperty("preset", 0));
    bypassed.store(((int) state.getProperty("bypassed", 0)) != 0, std::memory_order_relaxed);

    setPreGainDb((float) (double) state.getProperty("preGainDb", 10.0));
    setRandomRetriggerPerBeat((float) (double) state.getProperty("randomRetriggerPerBeat", 4.0));
    setLimiterThreshold((float) (double) state.getProperty("limiterThreshold", 1.0));
    setLossNotchQ((float) (double) state.getProperty("lossNotchQ", 11.0));
    setLossAlgorithmMode((int) state.getProperty("lossAlgorithmMode", kLossAlgorithmLegacy));
    setLossMaskInverted(((int) state.getProperty("lossMaskInverted", 0)) != 0);

    const float restoredLowCutHz = (float) (double) state.getProperty("lowCutHz", (double) kLowCutHzMin);
    const float restoredHighCutHz = (float) (double) state.getProperty("highCutHz", (double) kHighCutHzMax);
    setLowCutHz(restoredLowCutHz);
    setHighCutHz(restoredHighCutHz);
    setCutMode((int) state.getProperty("cutMode", kCutModeHardMask));
    setCutSlopeDbPerOct((int) state.getProperty("cutSlopeDbPerOct", kCutSlope12dB));
    currentLowCutHzForMask = getLowCutHz();
    currentHighCutHzForMask = getHighCutHz();
    currentCutModeForMask = getCutMode();
    currentLossMaskInvertedForMask = isLossMaskInverted() ? 1 : 0;
    currentSampleRateForCutFilter = 0.0;
    currentLowCutHzForCutFilter = -1.0f;
    currentHighCutHzForCutFilter = -1.0f;
    currentSlopeForCutFilter = -1;
    lastLowCutHzForCrossfade = -1.0f;
    lastHighCutHzForCrossfade = -1.0f;
    lastSlopeForCrossfade = -1;

}

// 插件入口实现
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new LDSJvstAudioProcessor();
}