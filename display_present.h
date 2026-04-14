#pragma once

#include <JuceHeader.h>
#include <array>
#include <cmath>

namespace display_present
{
    // ==============================
    // 背景渲染参数（原有）
    // ==============================
    enum class BackgroundKind : juce::uint8
    {
        legacySolid = 0,
        digitalGrid,
        amberVignette,
        rainbowInterference,
        dotMask,
        radarSweep,
        phosphorBloom,
        oceanBlobs,
        mirrorCross,
        barcode,
        glitchStatic,
        neonStarfield,
        minimalVignette,
        greenTerminal,
    };

    struct BackgroundParams
    {
        BackgroundKind kind = BackgroundKind::legacySolid;

        juce::Colour baseA = juce::Colours::black;
        juce::Colour baseB = juce::Colours::black;

        // 通用叠加参数（不同算法会选择性使用）
        float vignetteAlpha = 0.0f;      // 暗角强度，0..1
        float noiseAlpha = 0.0f;         // 颗粒噪点透明度，0..1
        int noiseDownsample = 6;         // 噪点分辨率降采样，2..12

        float gridAlpha = 0.0f;          // 网格线透明度，0..1
        int gridStepPx = 32;             // 网格间距（像素）

        float scanlineAlpha = 0.0f;      // 扫描线透明度，0..1
        int scanlineStepPx = 2;          // 扫描线步进（像素）

        float glassWarp = 0.0f;          // 玻璃弧形扭曲强度，0..~0.25

        // 动态参数
        float motionSpeed = 1.0f;        // 背景动画速度倍率
        float accentAlpha = 0.10f;       // 背景强调色透明度
    };

    struct ScanlineWarpParams
    {
        // 扭曲主参数
        float maxOffsetPx = 0.0f;        // 单行最大左右偏移（像素）
        int windowRadius = 0;            // 行方向平滑窗口半径
        float updateProb = 0.0f;         // 每帧更新概率
        float temporalSmooth = 0.0f;     // 时域平滑系数

        // 额外可调：整屏同步撕裂概率、行漂移正弦
        float globalKickProbBase = 0.0025f;
        float globalKickProbPerPreset = 0.0004f;

        float driftAmp = 0.25f;
        float driftFreqBase = 0.45f;
        float driftFreqPerPreset = 0.07f;
        float driftYMul = 0.012f;

        // 原始噪声的“块状相关”控制
        int blockBase = 2;
        int blockPresetMod = 6;
    };

    // ==============================
    // OSD（频道 / TV-ST-SAP）布局参数
    // ==============================
    struct RelativeRect
    {
        // 相对屏幕区域尺寸（0~1）
        float x = 0.0f;
        float y = 0.0f;
        float w = 1.0f;
        float h = 1.0f;
    };

    struct ChannelOsdParams
    {
        RelativeRect rect { 468.0f / 643.0f, 68.0f / 529.0f, 152.0f / 643.0f, 44.0f / 529.0f };
        float fontMin = 31.0f;
        float fontMax = 52.0f;
        float fontHeightScale = 0.78f;
        float shadowOffsetX = 1.0f;
        float shadowOffsetY = 1.0f;
        float shadowAlpha = 0.72f;
        float textAlpha = 0.98f;
    };

    struct ModeOsdParams
    {
        RelativeRect rect { 20.0f / 643.0f, 68.0f / 529.0f, 256.0f / 643.0f, 72.0f / 529.0f };
        float fontMin = 24.0f;
        float fontMax = 34.0f;
        float fontHeightScale = 0.33f;
        float line1Split = 0.52f;
        float line2Split = 0.24f;
        float shadowOffsetX = 1.0f;
        float shadowOffsetY = 1.0f;
        float shadowAlpha = 0.70f;
        float textAlpha = 0.96f;
    };

    struct OSDParams
    {
        ChannelOsdParams channel;
        ModeOsdParams mode;
    };

    // ==============================
    // 频段随机 / 时序参数
    // ==============================
    enum class LossProbabilityModel : juce::uint8
    {
        constant = 0,      // 固定概率
        linear,            // 线性：a + b*t
        gaussianMid,       // 中心钟形：base + amp * exp(-((t-c)/sigma)^2)
        comb,              // 梳状：按周期窗口切换高/低概率
        dualPeak,          // 双峰
        sine,              // 正弦分布
        sparsePeaks,       // 稀疏窄峰
        piecewiseLowHigh,  // 分段（低频段固定，高频段线性）
        piecewiseHighLow,  // 分段（高频段固定，低频段线性）
    };

    struct LossDistributionParams
    {
        LossProbabilityModel model = LossProbabilityModel::constant;

        // 通用参数（按 model 解释）
        float a = 0.50f;
        float b = 0.0f;
        float c = 0.5f;
        float sigma = 0.22f;

        // comb 参数
        int combPeriod = 6;
        int combKeepWidth = 3;
        float combKeepP = 0.82f;
        float combDropP = 0.18f;

        // dualPeak 参数
        float peak1Center = 0.20f;
        float peak1Sigma = 0.12f;
        float peak1Amp = 0.42f;
        float peak2Center = 0.78f;
        float peak2Sigma = 0.16f;
        float peak2Amp = 0.50f;

        // sine 参数
        float sineBase = 0.50f;
        float sineAmp = 0.38f;
        float sineCycles = 3.0f;
        float sinePhase = 0.1f;

        // sparsePeaks 参数
        std::array<int, 8> sparseCenters { 8, 19, 31, 46, 59, 73, 88, 96 };
        int sparseCenterCount = 8;
        int sparseNeighborWidth = 1;
        float sparseCenterP = 0.92f;
        float sparseNeighborP = 0.55f;
        float sparseBaseP = 0.10f;

        // piecewise 参数
        float splitT = 0.35f;
        float lowFixedP = 0.85f;
        float highBase = 0.20f;
        float highSlope = 0.50f;
        float highFixedP = 0.86f;
        float lowBase = 0.18f;
        float lowSlope = 0.55f;

        // 统一概率钳位
        float minClamp = 0.05f;
        float maxClamp = 0.95f;
    };

    struct LossRandomParams
    {
        // 随机预设下，重触发周期 = beatSec / (randomRetriggerPerBeat * perPresetScale)
        float perPresetScale = 1.0f;
        float minRetriggerSeconds = 0.01f;
        float maxRetriggerSeconds = 1.20f;
    };

    struct LossSequenceParams
    {
        bool enabled = false;

        // 阵列参数
        int anchorCount = 1;
        int anchorSpacing = 50;
        int baseStartBand = 0;
        int halfWindow = 3;
        int stepPerTick = 1;
        bool reverse = false;

        // 时值参数（BPM 同步）
        // retriggerSeconds = clamp(beatSec * bpmDivision, minSec, maxSec)
        float bpmDivision = 0.25f;
        float minRetriggerSeconds = 0.02f;
        float maxRetriggerSeconds = 0.50f;
    };

    // ==============================
    // 每个预设的默认高低切参数
    // ==============================
    struct CutPresetParams
    {
        // 与处理器常量一致：0=HardMask，1=HPF/LPF
        int cutMode = 0;

        // 仅当 cutMode=1 时有效：12/24/48
        int cutSlopeDbPerOct = 12;

        // 高低切频率（Hz）
        float lowCutHz = 20.0f;
        float highCutHz = 20000.0f;

        // 切频道时是否自动应用这一套高低切
        bool applyOnChannelEnter = true;
    };

    struct DisplayPresetParams
    {
        BackgroundParams bg;
        ScanlineWarpParams warp;

        // 频道 / TV-ST-SAP OSD 配置
        OSDParams osd;

        // 频段随机/时序配置
        LossDistributionParams lossDistribution;
        LossRandomParams lossRandom;
        LossSequenceParams lossSequence;

        // 每个预设的高低切默认值
        CutPresetParams cutPreset;

        // 衍生预设波形主色（0..11 固定预设可忽略，12..9999 会写入）
        float waveHue = 0.33f;
        float waveSat = 0.90f;
        float waveVal = 1.0f;
    };

    inline constexpr int kPresetCount = 12;

    // ==============================
    // 预设配置表（频道 0..11）
    // ==============================
    inline const std::array<DisplayPresetParams, (size_t) kPresetCount> kPresets =
    {
        // 0
        DisplayPresetParams{
            BackgroundParams{ BackgroundKind::legacySolid, juce::Colours::black.withAlpha(0.06f), juce::Colours::black, 0.15f, 0.00f, 6, 0.00f, 32, 0.10f, 2, 0.0f, 1.0f, 0.10f },
            ScanlineWarpParams{ 8.0f, 10, 0.70f, 0.70f },
            OSDParams{},
            LossDistributionParams{ LossProbabilityModel::constant, 0.40f, 0.0f },
            LossRandomParams{ 1.0f, 0.01f, 1.20f },
            LossSequenceParams{},
            CutPresetParams{ 0, 12, 20.0f, 20000.0f, true }
        },

        // 1
        DisplayPresetParams{
            BackgroundParams{ BackgroundKind::digitalGrid, juce::Colour::fromRGB(0x05, 0x0E, 0x1A), juce::Colour::fromRGB(0x00, 0x1A, 0x2C), 0.18f, 0.00f, 8, 0.06f, 28, 0.05f, 2, 0.12f, 0.8f, 0.10f },
            ScanlineWarpParams{ 2.0f, 6, 0.25f, 0.88f },
            OSDParams{},
            LossDistributionParams{ LossProbabilityModel::linear, 0.88f, -0.68f },
            LossRandomParams{ 1.0f, 0.01f, 1.20f },
            LossSequenceParams{},
            CutPresetParams{ 0, 12, 20.0f, 20000.0f, true }
        },

        // 2
        DisplayPresetParams{
            BackgroundParams{ BackgroundKind::amberVignette, juce::Colour::fromRGB(0x10, 0x06, 0x00), juce::Colour::fromRGB(0x06, 0x02, 0x00), 0.62f, 0.05f, 4, 0.00f, 32, 0.18f, 2, 0.0f, 0.55f, 0.12f },
            ScanlineWarpParams{ 4.0f, 12, 0.45f, 0.82f },
            OSDParams{},
            LossDistributionParams{ LossProbabilityModel::linear, 0.15f, 0.75f },
            LossRandomParams{ 1.0f, 0.01f, 1.20f },
            LossSequenceParams{},
            CutPresetParams{ 0, 12, 20.0f, 20000.0f, true }
        },

        // 3
        DisplayPresetParams{
            BackgroundParams{ BackgroundKind::rainbowInterference, juce::Colours::black.withAlpha(0.35f), juce::Colours::black, 0.10f, 0.00f, 6, 0.00f, 32, 0.00f, 2, 0.0f, 1.10f, 0.12f },
            ScanlineWarpParams{ 3.0f, 5, 0.55f, 0.78f },
            OSDParams{},
            LossDistributionParams{ LossProbabilityModel::gaussianMid, 0.18f, 0.72f, 0.5f, 0.22f },
            LossRandomParams{ 1.0f, 0.01f, 1.20f },
            LossSequenceParams{ true, 1, 100, 50, 4, 1, false, 0.25f, 0.02f, 0.50f },
            CutPresetParams{ 1, 12, 55.0f, 18000.0f, true }
        },

        // 4
        DisplayPresetParams{
            BackgroundParams{ BackgroundKind::radarSweep, juce::Colour::fromRGB(0x00, 0x05, 0x10), juce::Colour::fromRGB(0x00, 0x00, 0x02), 0.42f, 0.00f, 8, 0.055f, 32, 0.00f, 2, 0.0f, 0.90f, 0.14f },
            ScanlineWarpParams{ 5.0f, 4, 0.60f, 0.75f },
            OSDParams{},
            LossDistributionParams{ LossProbabilityModel::comb, 0.0f, 0.0f, 0.0f, 0.0f, 6, 3, 0.82f, 0.18f },
            LossRandomParams{ 1.0f, 0.01f, 1.20f },
            LossSequenceParams{},
            CutPresetParams{ 1, 24, 80.0f, 16000.0f, true }
        },

        // 5
        DisplayPresetParams{
            BackgroundParams{ BackgroundKind::phosphorBloom, juce::Colour::fromRGB(0x00, 0x05, 0x0A), juce::Colour::fromRGB(0x00, 0x10, 0x12), 0.28f, 0.00f, 10, 0.00f, 32, 0.20f, 2, 0.0f, 0.75f, 0.14f },
            ScanlineWarpParams{ 2.8f, 8, 0.35f, 0.88f },
            OSDParams{},
            LossDistributionParams{
                LossProbabilityModel::dualPeak,
                0.10f, 0.0f, 0.0f, 0.0f,
                6, 3, 0.82f, 0.18f,
                0.20f, 0.12f, 0.42f,
                0.78f, 0.16f, 0.50f
            },
            LossRandomParams{ 1.0f, 0.01f, 1.20f },
            LossSequenceParams{},
            CutPresetParams{ 1, 24, 35.0f, 19000.0f, true }
        },

        // 6
        DisplayPresetParams{
            BackgroundParams{ BackgroundKind::mirrorCross, juce::Colour::fromRGB(0x00, 0x08, 0x04), juce::Colour::fromRGB(0x00, 0x12, 0x08), 0.20f, 0.00f, 10, 0.04f, 40, 0.16f, 2, 0.0f, 0.85f, 0.10f },
            ScanlineWarpParams{ 6.0f, 7, 0.65f, 0.72f },
            OSDParams{},
            LossDistributionParams{
                LossProbabilityModel::sine,
                0.0f, 0.0f, 0.0f, 0.0f,
                6, 3, 0.82f, 0.18f,
                0.20f, 0.12f, 0.42f,
                0.78f, 0.16f, 0.50f,
                0.50f, 0.38f, 3.0f, 0.1f
            },
            LossRandomParams{ 1.0f, 0.01f, 1.20f },
            LossSequenceParams{ true, 3, 33, 6, 3, 1, false, 1.0f / 3.0f, 0.02f, 0.50f },
            CutPresetParams{ 1, 24, 30.0f, 14000.0f, true }
        },

        // 7
        DisplayPresetParams{
            BackgroundParams{ BackgroundKind::barcode, juce::Colour::fromRGB(0x08, 0x02, 0x10), juce::Colour::fromRGB(0x02, 0x00, 0x04), 0.22f, 0.00f, 9, 0.00f, 32, 0.00f, 2, 0.0f, 0.95f, 0.12f },
            ScanlineWarpParams{ 3.5f, 8, 0.40f, 0.86f },
            OSDParams{},
            LossDistributionParams{ LossProbabilityModel::sparsePeaks },
            LossRandomParams{ 1.0f, 0.01f, 1.20f },
            LossSequenceParams{ true, 2, 50, 8, 2, 2, false, 0.125f, 0.02f, 0.50f },
            CutPresetParams{ 1, 48, 70.0f, 12500.0f, true }
        },

        // 8
        DisplayPresetParams{
            BackgroundParams{ BackgroundKind::glitchStatic, juce::Colours::black.withAlpha(0.18f), juce::Colours::black, 0.18f, 0.00f, 7, 0.00f, 32, 0.00f, 2, 0.0f, 1.20f, 0.14f },
            ScanlineWarpParams{ 10.0f, 3, 0.85f, 0.65f },
            OSDParams{},
            LossDistributionParams{ LossProbabilityModel::constant, 0.72f, 0.0f },
            LossRandomParams{ 1.0f, 0.01f, 1.20f },
            LossSequenceParams{},
            CutPresetParams{ 0, 12, 20.0f, 20000.0f, true }
        },

        // 9
        DisplayPresetParams{
            BackgroundParams{ BackgroundKind::neonStarfield, juce::Colour::fromRGB(0x05, 0x00, 0x08), juce::Colour::fromRGB(0x00, 0x00, 0x00), 0.30f, 0.00f, 10, 0.00f, 32, 0.00f, 2, 0.0f, 0.70f, 0.12f },
            ScanlineWarpParams{ 4.5f, 11, 0.35f, 0.88f },
            OSDParams{},
            LossDistributionParams{ LossProbabilityModel::constant, 0.26f, 0.0f },
            LossRandomParams{ 1.0f, 0.01f, 1.20f },
            LossSequenceParams{},
            CutPresetParams{ 0, 12, 20.0f, 20000.0f, true }
        },

        // 10
        DisplayPresetParams{
            BackgroundParams{ BackgroundKind::minimalVignette, juce::Colours::black.withAlpha(0.15f), juce::Colours::black, 0.22f, 0.00f, 12, 0.00f, 32, 0.00f, 2, 0.0f, 0.50f, 0.08f },
            ScanlineWarpParams{ 1.5f, 6, 0.18f, 0.92f },
            OSDParams{},
            LossDistributionParams{
                LossProbabilityModel::piecewiseLowHigh,
                0.0f, 0.0f, 0.0f, 0.0f,
                6, 3, 0.82f, 0.18f,
                0.20f, 0.12f, 0.42f,
                0.78f, 0.16f, 0.50f,
                0.50f, 0.38f, 3.0f, 0.1f,
                { 8, 19, 31, 46, 59, 73, 88, 96 }, 8, 1, 0.92f, 0.55f, 0.10f,
                0.35f, 0.85f, 0.20f, 0.50f
            },
            LossRandomParams{ 1.0f, 0.01f, 1.20f },
            LossSequenceParams{ true, 2, 50, 0, 5, 1, false, 0.50f, 0.02f, 0.80f },
            CutPresetParams{ 1, 12, 45.0f, 9000.0f, true }
        },

        // 11
        DisplayPresetParams{
            BackgroundParams{ BackgroundKind::greenTerminal, juce::Colour::fromRGB(0x00, 0x10, 0x08), juce::Colour::fromRGB(0x00, 0x06, 0x03), 0.20f, 0.00f, 9, 0.00f, 32, 0.18f, 2, 0.0f, 0.90f, 0.12f },
            ScanlineWarpParams{ 5.5f, 9, 0.55f, 0.80f },
            OSDParams{},
            LossDistributionParams{
                LossProbabilityModel::piecewiseHighLow,
                0.0f, 0.0f, 0.0f, 0.0f,
                6, 3, 0.82f, 0.18f,
                0.20f, 0.12f, 0.42f,
                0.78f, 0.16f, 0.50f,
                0.50f, 0.38f, 3.0f, 0.1f,
                { 8, 19, 31, 46, 59, 73, 88, 96 }, 8, 1, 0.92f, 0.55f, 0.10f,
                0.65f, 0.85f, 0.20f, 0.50f,
                0.86f, 0.18f, 0.55f
            },
            LossRandomParams{ 1.0f, 0.01f, 1.20f },
            LossSequenceParams{ true, 2, 50, 80, 4, 1, true, 0.75f, 0.02f, 0.80f },
            CutPresetParams{ 1, 24, 180.0f, 12000.0f, true }
        },
    };

    inline constexpr int kDerivedChannelMin = kPresetCount;
    inline constexpr int kDerivedChannelMax = 9999;

    inline const DisplayPresetParams& getPresetParams(int presetIndex) noexcept
    {
        const int idx = juce::jlimit(0, kPresetCount - 1, presetIndex);
        return kPresets[(size_t) idx];
    }

    inline juce::uint64 splitmix64(juce::uint64 x) noexcept
    {
        x += 0x9E3779B97F4A7C15ULL;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
        return x ^ (x >> 31);
    }

    inline float rand01(juce::uint64& state) noexcept
    {
        state = splitmix64(state);
        constexpr double invMaxU53 = 1.0 / (double) ((juce::uint64) 1 << 53);
        const juce::uint64 v = state >> 11;
        return (float) (v * invMaxU53);
    }

    inline int randInt(juce::uint64& state, int lo, int hiInclusive) noexcept
    {
        const int hi = juce::jmax(lo, hiInclusive);
        const float t = rand01(state);
        return lo + (int) std::floor(t * (float) (hi - lo + 1));
    }

    inline float randRange(juce::uint64& state, float lo, float hi) noexcept
    {
        return juce::jmap(rand01(state), lo, hi);
    }

    inline float logRandHz(juce::uint64& state, float hzMin, float hzMax) noexcept
    {
        const float lo = std::log(juce::jmax(1.0f, hzMin));
        const float hi = std::log(juce::jmax(hzMin + 1.0f, hzMax));
        return std::exp(randRange(state, lo, hi));
    }

    inline DisplayPresetParams derivePresetParamsFromChannelId(int channelId) noexcept
    {
        const int cid = juce::jlimit(kDerivedChannelMin, kDerivedChannelMax, channelId);
        const int baseIndex = cid % kPresetCount;

        DisplayPresetParams out = kPresets[(size_t) baseIndex];

        juce::uint64 s = splitmix64((juce::uint64) cid * 0xD6E8FEB86659FD93ULL + 0xA24BAED4963EE407ULL);

        static constexpr std::array<BackgroundKind, 12> kCommonBgKinds {
            BackgroundKind::legacySolid,
            BackgroundKind::digitalGrid,
            BackgroundKind::amberVignette,
            BackgroundKind::rainbowInterference,
            BackgroundKind::dotMask,
            BackgroundKind::radarSweep,
            BackgroundKind::phosphorBloom,
            BackgroundKind::oceanBlobs,
            BackgroundKind::mirrorCross,
            BackgroundKind::barcode,
            BackgroundKind::minimalVignette,
            BackgroundKind::greenTerminal
        };

        const float noiseStyleRoll = rand01(s);
        if (noiseStyleRoll < 0.05f)
            out.bg.kind = BackgroundKind::glitchStatic;
        else if (noiseStyleRoll < 0.10f)
            out.bg.kind = BackgroundKind::neonStarfield;
        else
            out.bg.kind = kCommonBgKinds[(size_t) randInt(s, 0, (int) kCommonBgKinds.size() - 1)];

        const float hueA = rand01(s);
        const float hueB = std::fmod(hueA + randRange(s, 0.12f, 0.58f), 1.0f);
        const float satA = randRange(s, 0.18f, 0.90f);
        const float satB = randRange(s, 0.12f, 0.75f);
        const float valA = randRange(s, 0.04f, 0.25f);
        const float valB = randRange(s, 0.00f, 0.16f);
        out.bg.baseA = juce::Colour::fromHSV(hueA, satA, valA, 1.0f);
        out.bg.baseB = juce::Colour::fromHSV(hueB, satB, valB, 1.0f);

        out.waveHue = std::fmod(hueA + randRange(s, -0.06f, 0.06f) + 1.0f, 1.0f);
        out.waveSat = randRange(s, 0.70f, 0.98f);
        out.waveVal = randRange(s, 0.86f, 1.00f);

        out.bg.vignetteAlpha = randRange(s, 0.10f, 0.64f);
        out.bg.noiseAlpha = randRange(s, 0.00f, 0.06f);
        if (out.bg.kind == BackgroundKind::glitchStatic)
            out.bg.noiseAlpha = randRange(s, 0.02f, 0.09f);
        else if (out.bg.kind == BackgroundKind::neonStarfield)
            out.bg.noiseAlpha = randRange(s, 0.01f, 0.05f);
        out.bg.noiseDownsample = randInt(s, 5, 12);
        out.bg.gridAlpha = randRange(s, 0.00f, 0.08f);

        out.bg.gridStepPx = randInt(s, 20, 48);
        out.bg.scanlineAlpha = randRange(s, 0.00f, 0.24f);
        out.bg.scanlineStepPx = randInt(s, 2, 3);
        out.bg.glassWarp = randRange(s, 0.00f, 0.18f);
        out.bg.motionSpeed = randRange(s, 0.50f, 1.35f);
        out.bg.accentAlpha = randRange(s, 0.06f, 0.16f);

        out.warp.maxOffsetPx = randRange(s, 1.2f, 10.5f);
        out.warp.windowRadius = randInt(s, 3, 12);
        out.warp.updateProb = randRange(s, 0.20f, 0.88f);
        out.warp.temporalSmooth = randRange(s, 0.64f, 0.94f);
        out.warp.globalKickProbBase = randRange(s, 0.0012f, 0.0050f);
        out.warp.globalKickProbPerPreset = randRange(s, 0.0002f, 0.0012f);
        out.warp.driftAmp = randRange(s, 0.10f, 0.48f);
        out.warp.driftFreqBase = randRange(s, 0.26f, 0.92f);
        out.warp.driftFreqPerPreset = randRange(s, 0.03f, 0.12f);
        out.warp.driftYMul = randRange(s, 0.008f, 0.018f);
        out.warp.blockBase = randInt(s, 1, 5);
        out.warp.blockPresetMod = randInt(s, 3, 10);

        out.osd.channel.shadowAlpha = randRange(s, 0.52f, 0.82f);
        out.osd.channel.textAlpha = randRange(s, 0.90f, 1.00f);
        out.osd.mode.shadowAlpha = randRange(s, 0.50f, 0.80f);
        out.osd.mode.textAlpha = randRange(s, 0.88f, 0.99f);

        const int modelId = randInt(s, 0, (int) LossProbabilityModel::piecewiseHighLow);
        out.lossDistribution.model = static_cast<LossProbabilityModel>(modelId);
        out.lossDistribution.a = randRange(s, 0.10f, 0.88f);
        out.lossDistribution.b = randRange(s, -0.80f, 0.80f);
        out.lossDistribution.c = randRange(s, 0.20f, 0.80f);
        out.lossDistribution.sigma = randRange(s, 0.08f, 0.34f);

        out.lossDistribution.combPeriod = randInt(s, 3, 12);
        out.lossDistribution.combKeepWidth = randInt(s, 1, out.lossDistribution.combPeriod - 1);
        out.lossDistribution.combKeepP = randRange(s, 0.64f, 0.95f);
        out.lossDistribution.combDropP = randRange(s, 0.05f, 0.36f);

        out.lossDistribution.peak1Center = randRange(s, 0.10f, 0.35f);
        out.lossDistribution.peak1Sigma = randRange(s, 0.06f, 0.18f);
        out.lossDistribution.peak1Amp = randRange(s, 0.22f, 0.55f);
        out.lossDistribution.peak2Center = randRange(s, 0.62f, 0.90f);
        out.lossDistribution.peak2Sigma = randRange(s, 0.10f, 0.24f);
        out.lossDistribution.peak2Amp = randRange(s, 0.24f, 0.58f);

        out.lossDistribution.sineBase = randRange(s, 0.30f, 0.70f);
        out.lossDistribution.sineAmp = randRange(s, 0.15f, 0.45f);
        out.lossDistribution.sineCycles = randRange(s, 1.0f, 5.0f);
        out.lossDistribution.sinePhase = randRange(s, 0.0f, 1.0f);

        out.lossDistribution.sparseCenterCount = randInt(s, 4, 8);
        out.lossDistribution.sparseNeighborWidth = randInt(s, 0, 2);
        out.lossDistribution.sparseCenterP = randRange(s, 0.72f, 0.98f);
        out.lossDistribution.sparseNeighborP = randRange(s, 0.34f, 0.72f);
        out.lossDistribution.sparseBaseP = randRange(s, 0.04f, 0.22f);
        for (int i = 0; i < out.lossDistribution.sparseCenterCount; ++i)
            out.lossDistribution.sparseCenters[(size_t) i] = randInt(s, 0, 99);

        out.lossDistribution.splitT = randRange(s, 0.22f, 0.78f);
        out.lossDistribution.lowFixedP = randRange(s, 0.62f, 0.95f);
        out.lossDistribution.highBase = randRange(s, 0.05f, 0.40f);
        out.lossDistribution.highSlope = randRange(s, 0.20f, 0.75f);
        out.lossDistribution.highFixedP = randRange(s, 0.62f, 0.95f);
        out.lossDistribution.lowBase = randRange(s, 0.05f, 0.40f);
        out.lossDistribution.lowSlope = randRange(s, 0.20f, 0.75f);

        out.lossDistribution.minClamp = randRange(s, 0.03f, 0.16f);
        out.lossDistribution.maxClamp = randRange(s, 0.84f, 0.97f);

        out.lossRandom.perPresetScale = 0.55f + (float) (cid - kDerivedChannelMin) * (1.90f / (float) (kDerivedChannelMax - kDerivedChannelMin));
        out.lossRandom.minRetriggerSeconds = randRange(s, 0.01f, 0.06f);
        out.lossRandom.maxRetriggerSeconds = randRange(s, 0.45f, 1.20f);
        if (out.lossRandom.maxRetriggerSeconds <= out.lossRandom.minRetriggerSeconds)
            out.lossRandom.maxRetriggerSeconds = out.lossRandom.minRetriggerSeconds + 0.20f;

        out.lossSequence.enabled = rand01(s) < 0.45f;
        out.lossSequence.anchorCount = randInt(s, 1, 4);
        out.lossSequence.anchorSpacing = randInt(s, 18, 55);
        out.lossSequence.baseStartBand = randInt(s, 0, 99);
        out.lossSequence.halfWindow = randInt(s, 1, 7);
        out.lossSequence.stepPerTick = randInt(s, 1, 3);
        out.lossSequence.reverse = rand01(s) < 0.50f;
        static constexpr std::array<float, 8> kBpmDivisions { 0.125f, 0.16666667f, 0.25f, 0.33333334f, 0.5f, 0.75f, 1.0f, 1.5f };
        out.lossSequence.bpmDivision = kBpmDivisions[(size_t) randInt(s, 0, (int) kBpmDivisions.size() - 1)];
        out.lossSequence.minRetriggerSeconds = randRange(s, 0.015f, 0.060f);
        out.lossSequence.maxRetriggerSeconds = randRange(s, 0.30f, 0.95f);
        if (out.lossSequence.maxRetriggerSeconds <= out.lossSequence.minRetriggerSeconds)
            out.lossSequence.maxRetriggerSeconds = out.lossSequence.minRetriggerSeconds + 0.25f;

        out.cutPreset.cutMode = (rand01(s) < 0.35f) ? 0 : 1;
        static constexpr std::array<int, 3> kSlopes { 12, 24, 48 };
        out.cutPreset.cutSlopeDbPerOct = kSlopes[(size_t) randInt(s, 0, (int) kSlopes.size() - 1)];
        if (out.cutPreset.cutMode == 0)
        {
            out.cutPreset.lowCutHz = 20.0f;
            out.cutPreset.highCutHz = 20000.0f;
        }
        else
        {
            float lowHz = logRandHz(s, 20.0f, 900.0f);
            float highHz = logRandHz(s, 3500.0f, 20000.0f);
            if (highHz <= lowHz + 500.0f)
                highHz = juce::jlimit(1500.0f, 20000.0f, lowHz + randRange(s, 900.0f, 6000.0f));
            out.cutPreset.lowCutHz = juce::jlimit(20.0f, 19500.0f, lowHz);
            out.cutPreset.highCutHz = juce::jlimit(out.cutPreset.lowCutHz + 100.0f, 20000.0f, highHz);
        }
        out.cutPreset.applyOnChannelEnter = true;

        return out;
    }

    inline DisplayPresetParams getPresetParamsForChannel(int channelId) noexcept
    {
        const int cid = juce::jlimit(0, kDerivedChannelMax, channelId);
        if (cid < kPresetCount)
            return kPresets[(size_t) cid];
        return derivePresetParamsFromChannelId(cid);
    }

    // 根据频段分布配置计算某个频段“保留概率”(0~1)
    inline float getLossKeepProbability(const LossDistributionParams& cfg, int bandIndex, int bandCount) noexcept
    {
        const int safeBands = juce::jmax(2, bandCount);
        const int bi = juce::jlimit(0, safeBands - 1, bandIndex);
        const float t = (float) bi / (float) (safeBands - 1);

        float p = cfg.a;

        switch (cfg.model)
        {
            case LossProbabilityModel::constant:
                p = cfg.a;
                break;

            case LossProbabilityModel::linear:
                p = cfg.a + cfg.b * t;
                break;

            case LossProbabilityModel::gaussianMid:
            {
                const float d = (t - cfg.c) / juce::jmax(0.001f, cfg.sigma);
                p = cfg.a + cfg.b * std::exp(-(d * d));
                break;
            }

            case LossProbabilityModel::comb:
            {
                const int period = juce::jmax(1, cfg.combPeriod);
                const int keepW = juce::jlimit(0, period, cfg.combKeepWidth);
                p = ((bi % period) < keepW) ? cfg.combKeepP : cfg.combDropP;
                break;
            }

            case LossProbabilityModel::dualPeak:
            {
                const float d1 = (t - cfg.peak1Center) / juce::jmax(0.001f, cfg.peak1Sigma);
                const float d2 = (t - cfg.peak2Center) / juce::jmax(0.001f, cfg.peak2Sigma);
                p = cfg.a + cfg.peak1Amp * std::exp(-(d1 * d1)) + cfg.peak2Amp * std::exp(-(d2 * d2));
                break;
            }

            case LossProbabilityModel::sine:
            {
                p = cfg.sineBase + cfg.sineAmp * std::sin(2.0f * juce::MathConstants<float>::pi * (t * cfg.sineCycles + cfg.sinePhase));
                break;
            }

            case LossProbabilityModel::sparsePeaks:
            {
                p = cfg.sparseBaseP;
                const int count = juce::jlimit(0, (int) cfg.sparseCenters.size(), cfg.sparseCenterCount);
                const int n = juce::jmax(0, cfg.sparseNeighborWidth);
                for (int i = 0; i < count; ++i)
                {
                    const int c = juce::jlimit(0, safeBands - 1, cfg.sparseCenters[(size_t) i]);
                    for (int k = -n; k <= n; ++k)
                    {
                        const int idx = c + k;
                        if (idx == bi)
                        {
                            p = (k == 0) ? cfg.sparseCenterP : cfg.sparseNeighborP;
                            break;
                        }
                    }
                }
                break;
            }

            case LossProbabilityModel::piecewiseLowHigh:
                p = (t < cfg.splitT) ? cfg.lowFixedP : (cfg.highBase + cfg.highSlope * t);
                break;

            case LossProbabilityModel::piecewiseHighLow:
                p = (t > cfg.splitT) ? cfg.highFixedP : (cfg.lowBase + cfg.lowSlope * (1.0f - t));
                break;

            default:
                p = cfg.a;
                break;
        }

        return juce::jlimit(cfg.minClamp, cfg.maxClamp, p);
    }

    // 兼容旧调用：0..11 固定预设
    inline float getLossKeepProbability(int presetIndex, int bandIndex, int bandCount) noexcept
    {
        return getLossKeepProbability(getPresetParams(presetIndex).lossDistribution, bandIndex, bandCount);
    }

    // 新调用：按频道 ID（0..9999）计算保留概率；>=12 使用衍生预设
    inline float getLossKeepProbabilityForChannel(int channelId, int bandIndex, int bandCount) noexcept
    {
        const auto cfg = getPresetParamsForChannel(channelId);
        return getLossKeepProbability(cfg.lossDistribution, bandIndex, bandCount);
    }
}
