#pragma once

#include <JuceHeader.h>

#include <array>

namespace display_present
{
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
        float vignetteAlpha = 0.0f;      // 0..1
        float noiseAlpha = 0.0f;         // 0..1
        int noiseDownsample = 6;         // 2..12

        float gridAlpha = 0.0f;          // 0..1
        int gridStepPx = 32;

        float scanlineAlpha = 0.0f;      // 0..1
        int scanlineStepPx = 2;

        // 玻璃弧形（中心放大、四周缩紧）的网格扭曲强度（仅部分背景使用）
        float glassWarp = 0.0f;          // 0..~0.25

        // 动态参数
        float motionSpeed = 1.0f;
        float accentAlpha = 0.10f;
    };

    struct ScanlineWarpParams
    {
        // 扭曲主参数
        float maxOffsetPx = 0.0f;
        int windowRadius = 0;
        float updateProb = 0.0f;
        float temporalSmooth = 0.0f;

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

    struct DisplayPresetParams
    {
        BackgroundParams bg;
        ScanlineWarpParams warp;
    };

    inline constexpr int kPresetCount = 12;

    inline const std::array<DisplayPresetParams, (size_t) kPresetCount> kPresets =
    {
        // 0: 老电视像素雪花（雪花由渲染层单独画，这里背景仅做暗底）
        DisplayPresetParams{
            BackgroundParams{ BackgroundKind::legacySolid, juce::Colours::black.withAlpha(0.06f), juce::Colours::black, 0.15f, 0.00f, 6, 0.00f, 32, 0.10f, 2, 0.0f, 1.0f, 0.10f },
            ScanlineWarpParams{ 8.0f, 10, 0.70f, 0.70f }
        },

        // 1: 冷色数字示波器（网格 + 冷色渐变；去掉轻噪，网格带玻璃弧形扭曲）
        DisplayPresetParams{
            BackgroundParams{ BackgroundKind::digitalGrid, juce::Colour::fromRGB(0x05, 0x0E, 0x1A), juce::Colour::fromRGB(0x00, 0x1A, 0x2C), 0.18f, 0.00f, 8, 0.06f, 28, 0.05f, 2, 0.12f, 0.8f, 0.10f },
            ScanlineWarpParams{ 2.0f, 6, 0.25f, 0.88f }
        },

        // 2: 琥珀 CRT（暗角更强、噪点更细密）
        DisplayPresetParams{
            BackgroundParams{ BackgroundKind::amberVignette, juce::Colour::fromRGB(0x10, 0x06, 0x00), juce::Colour::fromRGB(0x06, 0x02, 0x00), 0.62f, 0.05f, 4, 0.00f, 32, 0.18f, 2, 0.0f, 0.55f, 0.12f },
            ScanlineWarpParams{ 4.0f, 12, 0.45f, 0.82f }
        },

        // 3: 彩虹干扰（彩色条纹干扰，不依赖噪点）
        DisplayPresetParams{
            BackgroundParams{ BackgroundKind::rainbowInterference, juce::Colours::black.withAlpha(0.35f), juce::Colours::black, 0.10f, 0.00f, 6, 0.00f, 32, 0.00f, 2, 0.0f, 1.10f, 0.12f },
            ScanlineWarpParams{ 3.0f, 5, 0.55f, 0.78f }
        },

        // 4: 全新预设（紫外雷达扫掠：同心环 + 扫掠扇形）
        DisplayPresetParams{
            BackgroundParams{ BackgroundKind::radarSweep, juce::Colour::fromRGB(0x00, 0x05, 0x10), juce::Colour::fromRGB(0x00, 0x00, 0x02), 0.42f, 0.00f, 8, 0.055f, 32, 0.00f, 2, 0.0f, 0.90f, 0.14f },
            ScanlineWarpParams{ 5.0f, 4, 0.60f, 0.75f }
        },

        // 5: 全新预设（冷色磷光 CRT：中心辉光 + 强扫描线，去噪点）
        DisplayPresetParams{
            BackgroundParams{ BackgroundKind::phosphorBloom, juce::Colour::fromRGB(0x00, 0x05, 0x0A), juce::Colour::fromRGB(0x00, 0x10, 0x12), 0.28f, 0.00f, 10, 0.00f, 32, 0.20f, 2, 0.0f, 0.75f, 0.14f },
            ScanlineWarpParams{ 2.8f, 8, 0.35f, 0.88f }
        },

        // 6: 镜像（中心十字 + 对称渐变 + 强扫描线）
        DisplayPresetParams{
            BackgroundParams{ BackgroundKind::mirrorCross, juce::Colour::fromRGB(0x00, 0x08, 0x04), juce::Colour::fromRGB(0x00, 0x12, 0x08), 0.20f, 0.00f, 10, 0.04f, 40, 0.16f, 2, 0.0f, 0.85f, 0.10f },
            ScanlineWarpParams{ 6.0f, 7, 0.65f, 0.72f }
        },

        // 7: 条形码（竖条纹底 + 紫色基调）
        DisplayPresetParams{
            BackgroundParams{ BackgroundKind::barcode, juce::Colour::fromRGB(0x08, 0x02, 0x10), juce::Colour::fromRGB(0x02, 0x00, 0x04), 0.22f, 0.00f, 9, 0.00f, 32, 0.00f, 2, 0.0f, 0.95f, 0.12f },
            ScanlineWarpParams{ 3.5f, 8, 0.40f, 0.86f }
        },

        // 8: 故障静电（灰噪 + 撕裂条）—— 先去掉噪点以免挡波形
        DisplayPresetParams{
            BackgroundParams{ BackgroundKind::glitchStatic, juce::Colours::black.withAlpha(0.18f), juce::Colours::black, 0.18f, 0.00f, 7, 0.00f, 32, 0.00f, 2, 0.0f, 1.20f, 0.14f },
            ScanlineWarpParams{ 10.0f, 3, 0.85f, 0.65f }
        },

        // 9: 霓虹星空（稀疏星点 + 紫色暗角）—— 去噪点
        DisplayPresetParams{
            BackgroundParams{ BackgroundKind::neonStarfield, juce::Colour::fromRGB(0x05, 0x00, 0x08), juce::Colour::fromRGB(0x00, 0x00, 0x00), 0.30f, 0.00f, 10, 0.00f, 32, 0.00f, 2, 0.0f, 0.70f, 0.12f },
            ScanlineWarpParams{ 4.5f, 11, 0.35f, 0.88f }
        },

        // 10: 极简（几乎纯黑 + 轻暗角）—— 去噪点
        DisplayPresetParams{
            BackgroundParams{ BackgroundKind::minimalVignette, juce::Colours::black.withAlpha(0.15f), juce::Colours::black, 0.22f, 0.00f, 12, 0.00f, 32, 0.00f, 2, 0.0f, 0.50f, 0.08f },
            ScanlineWarpParams{ 1.5f, 6, 0.18f, 0.92f }
        },

        // 11: 绿屏终端（扫描线）—— 去噪点
        DisplayPresetParams{
            BackgroundParams{ BackgroundKind::greenTerminal, juce::Colour::fromRGB(0x00, 0x10, 0x08), juce::Colour::fromRGB(0x00, 0x06, 0x03), 0.20f, 0.00f, 9, 0.00f, 32, 0.18f, 2, 0.0f, 0.90f, 0.12f },
            ScanlineWarpParams{ 5.5f, 9, 0.55f, 0.80f }
        },
    };

    inline const DisplayPresetParams& getPresetParams(int presetIndex) noexcept
    {
        const int idx = juce::jlimit(0, kPresetCount - 1, presetIndex);
        return kPresets[(size_t) idx];
    }
}
