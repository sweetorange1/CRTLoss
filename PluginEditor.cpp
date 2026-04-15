#include "PluginEditor.h"
#include <JuceHeader.h>
#include "BinaryData.h"
#include "display_present.h"
#include <cstring>

namespace
{
    static constexpr auto kPluginUiVersionText = "v1.1.10";
}

// --- BypassHitArea ---
void LDSJvstAudioProcessorEditor::BypassHitArea::paint (juce::Graphics& g)
{
    // 触发区本身保持透明；如果 bypass 开启，则用图片覆盖该区域
    if (owner.processor.bypassed.load(std::memory_order_acquire) && owner.bypassImage.isValid())

    {
        g.drawImageWithin(owner.bypassImage,
                          0, 0, getWidth(), getHeight(),
                          juce::RectanglePlacement::stretchToFit,
                          false);
    }
}

void LDSJvstAudioProcessorEditor::BypassHitArea::mouseUp (const juce::MouseEvent&)
{
    owner.toggleBypassFromUI();
}

LDSJvstAudioProcessorEditor::OscilloscopeComponent::OscilloscopeComponent(LDSJvstAudioProcessorEditor& o,
                                                                         LDSJvstAudioProcessor& p)
    : owner(o), processor(p)
{
    setInterceptsMouseClicks(true, false);
    startTimerHz(30);
}

void LDSJvstAudioProcessorEditor::OscilloscopeComponent::timerCallback()
{
    if (owner.editorShuttingDown || processor.isShuttingDownNow())
    {
        stopTimer();
        return;
    }

    processor.getOscilloscopeSnapshot(samples);
    processor.getLossMaskSnapshot(lossMaskSnapshotUI);
    repaint();
}

void LDSJvstAudioProcessorEditor::OscilloscopeComponent::mouseDown (const juce::MouseEvent& e)
{
    processor.setCutDragActive(false);

    const auto b = getLocalBounds().toFloat();

    const int bands = kBandGridCount;
    if (bands > 0)
    {

        const float marginX = juce::jmax(8.0f, b.getWidth() * 0.03f);
        const float gridAreaW = juce::jmax(50.0f, b.getWidth() - marginX * 2.0f);
        const float gap = juce::jmax(0.0f, juce::jmin(1.0f, b.getWidth() * 0.0012f));
        const float cellW = juce::jmax(1.0f, (gridAreaW - gap * (float) (bands - 1)) / (float) bands);
        const float cellH = juce::jmax(3.0f, b.getHeight() * 0.020f);

        const float x0 = b.getX() + (b.getWidth() - (cellW * (float) bands + gap * (float) (bands - 1))) * 0.5f;
        const float x1 = x0 + cellW * (float) bands + gap * (float) (bands - 1);
        const float y0 = b.getBottom() - cellH - juce::jmax(2.0f, b.getHeight() * 0.018f);

        const float logMin = std::log(LDSJvstAudioProcessor::kLowCutHzMin);
        const float logMax = std::log(LDSJvstAudioProcessor::kHighCutHzMax);
        const float logSpan = juce::jmax(0.0001f, logMax - logMin);

        auto hzToX = [&](float hz)
        {
            const float h = juce::jlimit(LDSJvstAudioProcessor::kLowCutHzMin,
                                         LDSJvstAudioProcessor::kHighCutHzMax,
                                         hz);
            const float t = (std::log(h) - logMin) / logSpan;
            return x0 + juce::jlimit(0.0f, 1.0f, t) * (x1 - x0);
        };

        auto xToHz = [&](float x)
        {
            const float t = juce::jlimit(0.0f, 1.0f, (x - x0) / juce::jmax(1.0f, x1 - x0));
            return std::exp(logMin + t * logSpan);
        };

        const float lowX = hzToX(processor.getLowCutHz());
        const float highX = hzToX(processor.getHighCutHz());
        const float triW = juce::jmax(9.0f, cellW * 2.3f);
        const float triH = juce::jmax(6.0f, cellH * 2.2f);

        const auto lowHit = juce::Rectangle<float>(lowX - triW * 0.6f, y0 - triH - 4.0f, triW * 1.2f, triH + 8.0f);
        const auto highHit = juce::Rectangle<float>(highX - triW * 0.6f, y0 - triH - 4.0f, triW * 1.2f, triH + 8.0f);

        if (lowHit.contains(e.position))
        {
            cutHandleDragMode = 1;
            processor.setCutDragActive(true);
            processor.setLowCutHz(xToHz(e.position.x));
            repaint();
            return;
        }

        if (highHit.contains(e.position))
        {
            cutHandleDragMode = 2;
            processor.setCutDragActive(true);
            processor.setHighCutHz(xToHz(e.position.x));
            repaint();
            return;
        }

        const auto stripHit = juce::Rectangle<float>(x0, y0 - triH, x1 - x0, cellH + triH + 6.0f);
        if (stripHit.contains(e.position))
        {
            const float dxLow = std::abs(e.position.x - lowX);
            const float dxHigh = std::abs(e.position.x - highX);
            cutHandleDragMode = (dxLow <= dxHigh) ? 1 : 2;
            processor.setCutDragActive(true);

            if (cutHandleDragMode == 1)
                processor.setLowCutHz(xToHz(e.position.x));
            else
                processor.setHighCutHz(xToHz(e.position.x));

            repaint();
            return;
        }

    }

    const float midY = b.getCentreY();
    const float scaleY = b.getHeight() * 0.40f;

    // 允许用户点击显示区域任意位置来设置限制器阈值：
    // 鼠标离中心越远，阈值越大；两根线保持上下对称。
    const float dist = std::abs(e.position.y - midY);
    const float th = (scaleY > 1.0f) ? juce::jlimit(0.0f, 1.0f, dist / scaleY) : 1.0f;

    processor.setLimiterThreshold(th);

    // 立即进入拖拽（不需要精准点中线）
    limiterDragActive = true;
    limiterDragStartThreshold = th;
}

void LDSJvstAudioProcessorEditor::OscilloscopeComponent::mouseDrag (const juce::MouseEvent& e)
{
    if (cutHandleDragMode != 0)
    {
        const auto b = getLocalBounds().toFloat();
        const int bands = kBandGridCount;
        if (bands > 0)
        {
            const float marginX = juce::jmax(8.0f, b.getWidth() * 0.03f);
            const float gridAreaW = juce::jmax(50.0f, b.getWidth() - marginX * 2.0f);
            const float gap = juce::jmax(0.0f, juce::jmin(1.0f, b.getWidth() * 0.0012f));
            const float cellW = juce::jmax(1.0f, (gridAreaW - gap * (float) (bands - 1)) / (float) bands);

            const float x0 = b.getX() + (b.getWidth() - (cellW * (float) bands + gap * (float) (bands - 1))) * 0.5f;
            const float x1 = x0 + cellW * (float) bands + gap * (float) (bands - 1);

            const float logMin = std::log(LDSJvstAudioProcessor::kLowCutHzMin);
            const float logMax = std::log(LDSJvstAudioProcessor::kHighCutHzMax);
            const float logSpan = juce::jmax(0.0001f, logMax - logMin);

            const float t = juce::jlimit(0.0f, 1.0f, (e.position.x - x0) / juce::jmax(1.0f, x1 - x0));
            const float hz = std::exp(logMin + t * logSpan);

            if (cutHandleDragMode == 1)
                processor.setLowCutHz(hz);
            else
                processor.setHighCutHz(hz);

            repaint();
        }
        return;
    }

    if (! limiterDragActive)
        return;

    const auto b = getLocalBounds().toFloat();

    const float midY = b.getCentreY();
    const float scaleY = b.getHeight() * 0.40f;

    // 鼠标离中心越远，阈值越大；以当前拖拽位置为准（两根线对称）
    const float dist = std::abs(e.position.y - midY);
    const float th = (scaleY > 1.0f) ? juce::jlimit(0.0f, 1.0f, dist / scaleY) : 1.0f;

    processor.setLimiterThreshold(th);
}

void LDSJvstAudioProcessorEditor::OscilloscopeComponent::mouseUp (const juce::MouseEvent&)
{
    limiterDragActive = false;
    cutHandleDragMode = 0;
    processor.setCutDragActive(false);
}

void LDSJvstAudioProcessorEditor::OscilloscopeComponent::paint(juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();

    const int preset = owner.getSelectedPresetIndex();
    const bool derivedChannel = (preset >= display_present::kPresetCount);
    const int stylePreset = derivedChannel ? (1 + ((preset * 7 + 5) % (display_present::kPresetCount - 1))) : preset;

    const auto presetParams = display_present::getPresetParamsForChannel(preset);

    const auto getWaveBaseColour = [&]() -> juce::Colour
    {
        if (derivedChannel)
            return juce::Colour::fromHSV(presetParams.waveHue,
                                          juce::jlimit(0.45f, 1.0f, presetParams.waveSat),
                                          juce::jlimit(0.55f, 1.0f, presetParams.waveVal),
                                          1.0f);

        switch (preset)
        {
            case 0:  return juce::Colour::fromRGB(0x39, 0xFF, 0x14);
            case 1:  return juce::Colour::fromRGB(0x3A, 0xE6, 0xFF);
            case 2:  return juce::Colour::fromRGB(0xFF, 0xB0, 0x30);
            case 4:  return juce::Colour::fromRGB(0x5A, 0xFF, 0xE5);
            case 5:  return juce::Colour::fromRGB(0x5A, 0xFF, 0xE5);
            case 6:  return juce::Colour::fromRGB(0x7C, 0xFF, 0x6B);
            case 7:  return juce::Colour::fromRGB(0xFF, 0x4D, 0xFF);
            case 8:  return juce::Colour::fromRGB(0xFF, 0x66, 0x33);
            case 9:  return juce::Colour::fromRGB(0xB7, 0x4D, 0xFF);
            case 10: return juce::Colours::white;
            case 11: return juce::Colour::fromRGB(0x00, 0xFF, 0x66);
            case 3:
            default: return juce::Colours::white;
        }
    };

    const auto waveBaseColour = getWaveBaseColour();
    const auto waveGlowColour = waveBaseColour.withMultipliedBrightness(1.06f);
    const auto osdAccentColour = waveBaseColour.withMultipliedSaturation(0.92f).withMultipliedBrightness(0.96f);
    const float corner = 6.0f;

    // 先裁剪到圆角矩形，避免各种背景绘制溢出
    juce::Path clip;
    clip.addRoundedRectangle(b, corner);
    const juce::Graphics::ScopedSaveState clipState(g);
    g.reduceClipRegion(clip);

    // bypass：电视关机语义，稳态时全黑；过渡时保留开关机动画
    const bool bypassActive = owner.isBypassedOrTransitioningToBypass();

    if (bypassActive && ! owner.bypassTransitionActive)
    {
        g.setColour(juce::Colours::black);
        g.fillRect(b);
        return;
    }

    if (samples.isEmpty() && ! owner.bypassTransitionActive)
        return;

    // ============================================================
    // 1) 离屏渲染基础画面（背景 + 波形 + glitch + 动画），再做“按行 remap”
    //    这样才能实现真实的“行同步噪声导致每一行左右偏移”。
    // ============================================================

    const int W = juce::jmax(2, (int) std::ceil(b.getWidth()));
    const int H = juce::jmax(2, (int) std::ceil(b.getHeight()));

    if (screenBase.getWidth() != W || screenBase.getHeight() != H)
    {
        screenBase = juce::Image(juce::Image::ARGB, W, H, true);
        screenWarp = juce::Image(juce::Image::ARGB, W, H, true);

        screenBufferW = W;
        screenBufferH = H;

        scanlineNoiseRaw.assign((size_t) H, 0.0f);
        scanlineNoiseSmoothed.assign((size_t) H, 0.0f);
        scanlineOffsetPx.assign((size_t) H, 0.0f);
    }

    // 离屏上使用 (0,0,W,H) 坐标；最终再画回到 b
    const juce::Rectangle<float> sb(0.0f, 0.0f, (float) W, (float) H);

    // === 预设背景（算法化）===
    // 要点：每个预设的背景“算法差异”尽量大，同时所有可调参数集中在 display_present.h
    auto drawBackground = [&](juce::Graphics& gg)
    {
        const auto presetParams = display_present::getPresetParamsForChannel(preset);
        const auto& bg = presetParams.bg;

        const float seconds = (float) (juce::Time::getMillisecondCounterHiRes() * 0.001);
        juce::Random rng ((int) (juce::Time::getMillisecondCounter() ^ (preset * 0xA341316C)));

        // 1) 基础底色/渐变
        {
            juce::ColourGradient grad(bg.baseA, sb.getX(), sb.getY(), bg.baseB, sb.getRight(), sb.getBottom(), false);
            gg.setGradientFill(grad);
            gg.fillRect(sb);
        }

        auto applyVignette = [&](float alpha)
        {
            if (alpha <= 0.0f)
                return;

            juce::ColourGradient vg(juce::Colours::transparentBlack, sb.getCentreX(), sb.getCentreY(),
                                    juce::Colours::black.withAlpha(alpha), sb.getCentreX(), sb.getBottom(), true);
            vg.addColour(0.65, juce::Colours::transparentBlack);
            gg.setGradientFill(vg);
            gg.fillRect(sb);
        };

        auto applyNoise = [&](float alpha, int downsample)
        {
            if (alpha <= 0.0f)
                return;

            downsample = juce::jlimit(2, 12, downsample);
            const int lw = juce::jmax(2, (int) (sb.getWidth()  / (float) downsample));
            const int lh = juce::jmax(2, (int) (sb.getHeight() / (float) downsample));

            juce::Image noiseImg(juce::Image::ARGB, lw, lh, true);
            {
                juce::Image::BitmapData bd(noiseImg, juce::Image::BitmapData::readWrite);
                juce::Random nrng ((int) (juce::Time::getMillisecondCounter() ^ (preset * 0xC2B2AE35)));

                for (int y = 0; y < lh; ++y)
                {
                    auto* line = reinterpret_cast<juce::PixelARGB*> (bd.getLinePointer(y));
                    for (int x = 0; x < lw; ++x)
                    {
                        const auto v = (juce::uint8) (nrng.nextInt(256));
                        line[x].setARGB((juce::uint8) (juce::jlimit(0, 255, (int) std::round(alpha * 255.0f))), v, v, v);
                    }
                }
            }

            gg.drawImage(noiseImg,
                         sb.getX(), sb.getY(), sb.getWidth(), sb.getHeight(),
                         0, 0, lw, lh,
                         false);
        };

        auto applyGrid = [&](float alpha, int stepPx)
        {
            if (alpha <= 0.0f)
                return;

            stepPx = juce::jlimit(12, 72, stepPx);
            gg.setColour(juce::Colours::white.withAlpha(alpha));

            for (float x = sb.getX(); x <= sb.getRight(); x += (float) stepPx)
                gg.drawLine(x, sb.getY(), x, sb.getBottom(), 1.0f);
            for (float y = sb.getY(); y <= sb.getBottom(); y += (float) stepPx)
                gg.drawLine(sb.getX(), y, sb.getRight(), y, 1.0f);
        };

        auto applyScanlines = [&](float alpha, int stepPx)
        {
            if (alpha <= 0.0f)
                return;

            stepPx = juce::jlimit(1, 6, stepPx);
            gg.setColour(juce::Colours::black.withAlpha(alpha));
            for (float y = sb.getY(); y <= sb.getBottom(); y += (float) stepPx)
                gg.drawLine(sb.getX(), y, sb.getRight(), y, 1.0f);
        };

        // 2) 根据背景算法类型叠加“特征纹理”
        switch (bg.kind)
        {
            case display_present::BackgroundKind::digitalGrid:
            {
                // 预设 1：这里会带“玻璃弧形”扭曲：中心略放大，四周缩紧（只扭曲网格，不扭曲底色）
                const float gw = juce::jlimit(0.0f, 0.25f, bg.glassWarp);
                if (gw > 0.0f)
                {
                    // 用“径向非线性映射”来弯曲网格线：x/y 都会随着半径变化，线条就会自然弯起来
                    // 这里用轻微的 pincushion（k<0）来实现“中心放大、四周缩紧”的感觉
                    const float cx = sb.getCentreX();
                    const float cy = sb.getCentreY();
                    const float rx = sb.getWidth() * 0.5f;
                    const float ry = sb.getHeight() * 0.5f;

                    const float k = -gw; // 负号：边缘向中心收拢

                    auto warpPoint = [&](float x, float y)
                    {
                        const float nx = (x - cx) / rx;
                        const float ny = (y - cy) / ry;
                        const float r2 = nx * nx + ny * ny;

                        // r2 越大，收缩越明显；系数做轻微放大以便肉眼可见
                        const float f = 1.0f + (0.85f * k) * r2;
                        return juce::Point<float>(cx + nx * rx * f, cy + ny * ry * f);
                    };

                    const int stepPx = juce::jlimit(12, 72, bg.gridStepPx);
                    gg.setColour(juce::Colours::white.withAlpha(bg.gridAlpha));

                    // 扩展绘制范围：扭曲后四角容易“缩进去”，所以我们把网格线在边界外多画几条
                    const float margin = (float) (stepPx * 3);

                    // 画竖线：x 固定、沿 y 采样，得到一条弯曲的 Path
                    for (float x = sb.getX() - margin; x <= sb.getRight() + margin; x += (float) stepPx)
                    {
                        juce::Path p;
                        const float y0 = sb.getY() - margin;
                        const float y1 = sb.getBottom() + margin;
                        const float dy = 7.0f;

                        auto pt0 = warpPoint(x, y0);
                        p.startNewSubPath(pt0.x, pt0.y);
                        for (float y = y0 + dy; y <= y1 + 0.5f; y += dy)
                        {
                            auto pt = warpPoint(x, juce::jmin(y, y1));
                            p.lineTo(pt.x, pt.y);
                        }
                        gg.strokePath(p, juce::PathStrokeType(1.0f));
                    }

                    // 画横线：y 固定、沿 x 采样
                    for (float y = sb.getY() - margin; y <= sb.getBottom() + margin; y += (float) stepPx)
                    {
                        juce::Path p;
                        const float x0 = sb.getX() - margin;
                        const float x1 = sb.getRight() + margin;
                        const float dx = 7.0f;

                        auto pt0 = warpPoint(x0, y);
                        p.startNewSubPath(pt0.x, pt0.y);
                        for (float x = x0 + dx; x <= x1 + 0.5f; x += dx)
                        {
                            auto pt = warpPoint(juce::jmin(x, x1), y);
                            p.lineTo(pt.x, pt.y);
                        }
                        gg.strokePath(p, juce::PathStrokeType(1.0f));
                    }

                    // 轻微的边缘加深，让“弧形玻璃”更明显
                    applyVignette(bg.vignetteAlpha);
                }

                else
                {
                    applyGrid(bg.gridAlpha, bg.gridStepPx);
                    applyVignette(bg.vignetteAlpha);
                }

                // 注意：preset1 已在参数表中把 noiseAlpha 设为 0，所以这里不会有轻噪
                applyNoise(bg.noiseAlpha, bg.noiseDownsample);
                break;
            }
            case display_present::BackgroundKind::amberVignette:
            {
                // 琥珀辉光：中心偏亮、边缘偏暗
                juce::ColourGradient glow(juce::Colour::fromRGB(0xFF, 0xB0, 0x30).withAlpha(bg.accentAlpha),
                                          sb.getCentreX(), sb.getCentreY(),
                                          juce::Colours::transparentBlack,
                                          sb.getCentreX(), sb.getBottom(), true);
                glow.addColour(0.25, juce::Colour::fromRGB(0xFF, 0xB0, 0x30).withAlpha(bg.accentAlpha * 0.6f));
                gg.setGradientFill(glow);
                gg.fillRect(sb);

                applyScanlines(bg.scanlineAlpha, bg.scanlineStepPx);
                applyNoise(bg.noiseAlpha, bg.noiseDownsample);
                applyVignette(bg.vignetteAlpha);
                break;
            }
            case display_present::BackgroundKind::radarSweep:
            {
                // 同心环 + 扫掠扇形（紫外雷达风）
                const float cx = sb.getCentreX();
                const float cy = sb.getCentreY();
                const float rMax = 0.52f * juce::jmin(sb.getWidth(), sb.getHeight());

                gg.setColour(juce::Colours::white.withAlpha(bg.gridAlpha));
                const int rings = 5;
                for (int i = 1; i <= rings; ++i)
                {
                    const float rr = rMax * ((float) i / (float) rings);
                    gg.drawEllipse(cx - rr, cy - rr, rr * 2.0f, rr * 2.0f, 1.0f);
                }

                // 十字准星
                gg.setColour(juce::Colours::white.withAlpha(bg.gridAlpha * 0.85f));
                gg.drawLine(cx, sb.getY(), cx, sb.getBottom(), 1.0f);
                gg.drawLine(sb.getX(), cy, sb.getRight(), cy, 1.0f);

                // 扫掠扇形（渐隐）
                const float ang = std::fmod(seconds * (0.9f + 0.8f * bg.motionSpeed), juce::MathConstants<float>::twoPi);
                const float sweepWidth = 0.65f;
                juce::Path sweep;
                sweep.addPieSegment(juce::Rectangle<float>(cx - rMax, cy - rMax, rMax * 2.0f, rMax * 2.0f),
                                    ang - sweepWidth * 0.5f, ang + sweepWidth * 0.5f, 0.0f);

                gg.setColour(juce::Colour::fromRGB(0xA0, 0x50, 0xFF).withAlpha(bg.accentAlpha));
                gg.fillPath(sweep);

                applyNoise(bg.noiseAlpha, bg.noiseDownsample);
                applyVignette(bg.vignetteAlpha);
                break;
            }
            case display_present::BackgroundKind::phosphorBloom:
            {
                // 冷色磷光 CRT：中心辉光 + 强扫描线（不依赖噪点）
                juce::Colour core = juce::Colour::fromRGB(0x5A, 0xFF, 0xE5).withAlpha(bg.accentAlpha);
                juce::Colour edge = juce::Colours::transparentBlack;

                juce::ColourGradient bloom(core, sb.getCentreX(), sb.getCentreY(), edge, sb.getCentreX(), sb.getBottom(), true);
                bloom.addColour(0.25, core.withAlpha(bg.accentAlpha * 0.55f));
                bloom.addColour(0.55, core.withAlpha(bg.accentAlpha * 0.25f));
                gg.setGradientFill(bloom);
                gg.fillRect(sb);

                // 稍微再加一层偏横向的辉光，模拟“屏幕玻璃散射”
                juce::ColourGradient bloom2(core.withAlpha(bg.accentAlpha * 0.55f), sb.getX(), sb.getCentreY(),
                                            edge, sb.getRight(), sb.getCentreY(), false);
                gg.setGradientFill(bloom2);
                gg.fillRect(sb);

                applyScanlines(bg.scanlineAlpha, bg.scanlineStepPx);
                applyVignette(bg.vignetteAlpha);
                break;
            }
            case display_present::BackgroundKind::rainbowInterference:

            {
                // 彩色干扰条：多条窄的横向渐变条，随时间漂移
                const int bands = 18;
                for (int i = 0; i < bands; ++i)
                {
                    const float t = (float) i / (float) bands;
                    const float hue = std::fmod(t + seconds * 0.08f * bg.motionSpeed, 1.0f);
                    const float y = sb.getY() + (t * sb.getHeight());
                    const float h = 4.0f + 10.0f * (0.5f + 0.5f * std::sin(seconds * (0.7f + 0.12f * i)));

                    juce::ColourGradient cg(juce::Colour::fromHSV(hue, 0.95f, 1.0f, bg.accentAlpha), sb.getX(), y,
                                            juce::Colours::transparentBlack, sb.getRight(), y + h,
                                            false);
                    gg.setGradientFill(cg);
                    gg.fillRect(sb.getX(), y, sb.getWidth(), h);
                }

                applyVignette(bg.vignetteAlpha);
                break;
            }

            case display_present::BackgroundKind::dotMask:
            {
                // 点阵遮罩：用 tiled fill 做“点阵/子像素”感（比逐像素更快）
                const int step = juce::jlimit(2, 6, 4);
                if ((! dotMaskTile.isValid())
                    || dotMaskTile.getWidth() != step * 2
                    || dotMaskTile.getHeight() != step * 2)
                {
                    dotMaskTile = juce::Image(juce::Image::ARGB, step * 2, step * 2, true);
                    juce::Graphics tg(dotMaskTile);
                    tg.fillAll(juce::Colours::transparentBlack);
                    tg.setColour(juce::Colours::white.withAlpha(0.06f));
                    tg.fillEllipse(0.0f, 0.0f, (float) step, (float) step);
                    tg.fillEllipse((float) step, (float) step, (float) step, (float) step);
                }

                gg.setTiledImageFill(dotMaskTile, (int) std::fmod(seconds * 12.0f * bg.motionSpeed, (float) (step * 2)),
                                     (int) std::fmod(seconds * 6.0f * bg.motionSpeed, (float) (step * 2)),
                                     1.0f);
                gg.setOpacity(0.35f);
                gg.fillRect(sb);
                gg.setOpacity(1.0f);

                applyNoise(bg.noiseAlpha, bg.noiseDownsample);
                applyVignette(bg.vignetteAlpha);
                break;
            }
            case display_present::BackgroundKind::oceanBlobs:
            {
                // 柔和流动光斑：几枚大的径向渐变 blob
                const int blobs = 5;
                for (int i = 0; i < blobs; ++i)
                {
                    const float ph = seconds * (0.25f + 0.07f * (float) i) * bg.motionSpeed;
                    const float x = sb.getX() + (0.15f + 0.70f * (0.5f + 0.5f * std::sin(ph + 1.7f * i))) * sb.getWidth();
                    const float y = sb.getY() + (0.15f + 0.70f * (0.5f + 0.5f * std::cos(ph * 0.9f + 2.3f * i))) * sb.getHeight();
                    const float r = 40.0f + 120.0f * (0.5f + 0.5f * std::sin(ph * 1.3f));

                    juce::Colour c = juce::Colour::fromRGB(0x00, 0xFF, 0xC6).withAlpha(bg.accentAlpha * 0.75f);
                    juce::ColourGradient cg(c, x, y, juce::Colours::transparentBlack, x + r, y + r, true);
                    cg.addColour(0.35, c.withAlpha(bg.accentAlpha * 0.35f));
                    gg.setGradientFill(cg);
                    gg.fillEllipse(x - r, y - r, r * 2.0f, r * 2.0f);
                }

                applyVignette(bg.vignetteAlpha);
                applyNoise(bg.noiseAlpha, bg.noiseDownsample);
                break;
            }
            case display_present::BackgroundKind::mirrorCross:
            {
                // 中心十字+轻网格
                gg.setColour(juce::Colours::white.withAlpha(bg.gridAlpha));
                gg.drawLine(sb.getCentreX(), sb.getY(), sb.getCentreX(), sb.getBottom(), 1.0f);
                gg.drawLine(sb.getX(), sb.getCentreY(), sb.getRight(), sb.getCentreY(), 1.0f);

                applyGrid(bg.gridAlpha * 0.6f, bg.gridStepPx);
                applyScanlines(bg.scanlineAlpha, bg.scanlineStepPx);

                // 一根缓慢移动的扫描线（替代“闪烁矩形光斑”的视觉干扰）
                const float y = sb.getY() + std::fmod(seconds * (0.18f + 0.10f * bg.motionSpeed), 1.0f) * sb.getHeight();
                juce::ColourGradient sl(juce::Colours::transparentWhite, sb.getX(), y - 10.0f,
                                        juce::Colours::white.withAlpha(bg.accentAlpha * 0.9f), sb.getX(), y,
                                        false);
                sl.addColour(0.75, juce::Colours::transparentWhite);
                gg.setGradientFill(sl);
                gg.fillRect(sb.getX(), y - 10.0f, sb.getWidth(), 20.0f);

                applyVignette(bg.vignetteAlpha);
                break;
            }

            case display_present::BackgroundKind::barcode:
            {
                // 竖条纹：随机条宽/间隔（轻微动画）
                const float base = 0.10f + 0.06f * (0.5f + 0.5f * std::sin(seconds * 0.7f * bg.motionSpeed));
                for (float x = sb.getX(); x < sb.getRight();)
                {
                    const float w = 1.0f + (float) (1 + (rng.nextInt(6)));
                    const float gap = (float) (rng.nextInt(5));
                    gg.setColour(juce::Colours::white.withAlpha(base * (rng.nextBool() ? 0.45f : 1.0f)));
                    gg.fillRect(x, sb.getY(), w, sb.getHeight());
                    x += w + gap;
                }

                applyVignette(bg.vignetteAlpha);
                applyNoise(bg.noiseAlpha, bg.noiseDownsample);
                break;
            }
            case display_present::BackgroundKind::glitchStatic:
            {
                // 静电：更粗的噪点 + 偶发的亮线
                applyNoise(bg.noiseAlpha, juce::jlimit(2, 12, bg.noiseDownsample));

                const int lines = 2 + (int) std::round(6.0f * (0.5f + 0.5f * std::sin(seconds * 1.3f * bg.motionSpeed)));
                gg.setColour(juce::Colours::white.withAlpha(bg.accentAlpha));
                for (int i = 0; i < lines; ++i)
                {
                    const float y = sb.getY() + rng.nextFloat() * sb.getHeight();
                    gg.drawLine(sb.getX(), y, sb.getRight(), y, 1.0f);
                }

                applyVignette(bg.vignetteAlpha);
                break;
            }
            case display_present::BackgroundKind::neonStarfield:
            {
                // 星点：属于“噪点类装饰”，用 bg.noiseAlpha 作为开关/强度
                const float starK = juce::jlimit(0.0f, 1.0f, bg.noiseAlpha / 0.03f);
                if (starK > 0.0f)
                {
                    const int stars = 20 + (int) std::round(60.0f * starK);
                    gg.setColour(juce::Colours::white.withAlpha(0.04f * starK));
                    for (int i = 0; i < stars; ++i)
                    {
                        const float x = sb.getX() + rng.nextFloat() * sb.getWidth();
                        const float y = sb.getY() + rng.nextFloat() * sb.getHeight();
                        gg.fillRect(x, y, 1.0f, 1.0f);
                    }

                    const int meteors = (starK > 0.6f ? 2 : 1);
                    gg.setColour(juce::Colour::fromRGB(0xB7, 0x4D, 0xFF).withAlpha(bg.accentAlpha * starK));
                    for (int i = 0; i < meteors; ++i)
                    {
                        const float x = sb.getX() + rng.nextFloat() * sb.getWidth();
                        const float y = sb.getY() + rng.nextFloat() * sb.getHeight();
                        gg.drawLine(x, y, x + (20.0f + 40.0f * rng.nextFloat()), y + (8.0f + 18.0f * rng.nextFloat()), 1.0f);
                    }
                }

                applyVignette(bg.vignetteAlpha);
                break;
            }

            case display_present::BackgroundKind::minimalVignette:
            {
                applyNoise(bg.noiseAlpha, bg.noiseDownsample);
                applyVignette(bg.vignetteAlpha);
                break;
            }
            case display_present::BackgroundKind::greenTerminal:
            {
                // 终端列：低透明度的字符列（用细竖条模拟）
                gg.setColour(juce::Colour::fromRGB(0x00, 0xFF, 0x66).withAlpha(bg.accentAlpha));
                for (float x = sb.getX(); x < sb.getRight(); x += 14.0f)
                {
                    const float h = sb.getHeight() * (0.15f + 0.85f * rng.nextFloat());
                    const float y0 = sb.getY() + rng.nextFloat() * (sb.getHeight() - h);
                    gg.fillRect(x, y0, 2.0f, h);
                }

                applyScanlines(bg.scanlineAlpha, bg.scanlineStepPx);
                applyNoise(bg.noiseAlpha, bg.noiseDownsample);
                applyVignette(bg.vignetteAlpha);
                break;
            }
            case display_present::BackgroundKind::legacySolid:
            default:
            {
                applyVignette(bg.vignetteAlpha);
                break;
            }
        }
    };

    auto drawGrid = [&](juce::Graphics& gg, juce::Colour c, int stepPx)
    {
        gg.setColour(c);
        for (float x = sb.getX(); x <= sb.getRight(); x += (float) stepPx)
            gg.drawLine(x, sb.getY(), x, sb.getBottom(), 1.0f);
        for (float y = sb.getY(); y <= sb.getBottom(); y += (float) stepPx)
            gg.drawLine(sb.getX(), y, sb.getRight(), y, 1.0f);
    };

    auto drawScanlines = [&](juce::Graphics& gg, juce::Colour c, int stepPx)
    {
        gg.setColour(c);
        for (float y = sb.getY(); y <= sb.getBottom(); y += (float) stepPx)
            gg.drawLine(sb.getX(), y, sb.getRight(), y, 1.0f);
    };

    // === 生成波形 Path（离屏坐标）===
    const float midY = sb.getCentreY();
    const float scaleY = sb.getHeight() * 0.40f;

    juce::Path waveform;

    const int n = samples.size();
    const float dx = (n > 1 ? (sb.getWidth() / (float) (n - 1)) : 0.0f);

    auto sampleToPoint = [&](int i)
    {
        const float x = sb.getX() + dx * (float) i;
        const float s = juce::jlimit(-1.0f, 1.0f, samples.getUnchecked(i));
        const float y = midY - s * scaleY;
        return juce::Point<float>(x, y);
    };

    if (! bypassActive)
    {
        for (int i = 0; i < n; ++i)
        {
            const auto p = sampleToPoint(i);
            if (i == 0) waveform.startNewSubPath(p.x, p.y);
            else        waveform.lineTo(p.x, p.y);
        }
    }

    // === 先在 screenBase 上画完整画面 ===
    {
        juce::Graphics gg(screenBase);
        gg.setImageResamplingQuality(juce::Graphics::lowResamplingQuality);
        gg.fillAll(juce::Colours::transparentBlack);

        drawBackground(gg);

        // 预设 0：像素电视雪花（这块仍然用原来的逐像素生成方式）
        if (stylePreset == 0 && ! bypassActive)

        {
            const int downsample = 2;
            const int lw = juce::jmax(2, (int) (sb.getWidth()  / (float) downsample));
            const int lh = juce::jmax(2, (int) (sb.getHeight() / (float) downsample));

            juce::Image low (juce::Image::ARGB, lw, lh, true);
            {
                juce::Image::BitmapData bd (low, juce::Image::BitmapData::readWrite);
                juce::Random rng ((int) juce::Time::getMillisecondCounter());

                for (int y = 0; y < lh; ++y)
                {
                    auto* line = reinterpret_cast<juce::PixelARGB*> (bd.getLinePointer(y));
                    const bool scanline = ((y & 1) == 0);

                    for (int x = 0; x < lw; ++x)
                    {
                        juce::uint8 v;
                        if (rng.nextFloat() < 0.09f)
                            v = (juce::uint8) rng.nextInt(48);
                        else
                            v = (juce::uint8) (172 + rng.nextInt(84));

                        v = (juce::uint8) ((v / 8) * 8);
                        if (scanline)
                            v = (juce::uint8) juce::jlimit(0, 255, (int) (v * 0.94f));

                        line[x].setARGB((juce::uint8) 255, v, v, v);
                    }
                }
            }

            gg.drawImage(low,
                         sb.getX(), sb.getY(), sb.getWidth(), sb.getHeight(),
                         0, 0, lw, lh,
                         false);

            // 给预设0的雪花底图加一层黑色滤镜，压低亮度以和其他预设统一
            gg.setColour(juce::Colours::black.withAlpha(0.24f));
            gg.fillRect(sb);

            const auto neonGreen = waveGlowColour;

            const float pixelStep = 3.0f;
            auto qx = [pixelStep, x0 = sb.getX()](float x) { return x0 + std::round((x - x0) / pixelStep) * pixelStep; };
            auto qy = [pixelStep, y0 = sb.getY()](float y) { return y0 + std::round((y - y0) / pixelStep) * pixelStep; };

            juce::Path pixelWave;
            for (int i = 0; i < n; ++i)
            {
                const auto p = sampleToPoint(i);
                const float x = qx(p.x);
                const float y = qy(p.y);
                if (i == 0) pixelWave.startNewSubPath(x, y);
                else        pixelWave.lineTo(x, y);
            }

            gg.setColour(juce::Colours::black.withAlpha(0.7f));
            gg.strokePath(pixelWave, juce::PathStrokeType(4.4f, juce::PathStrokeType::mitered, juce::PathStrokeType::butt));

            gg.setColour(neonGreen.withAlpha(1.0f));
            gg.strokePath(pixelWave, juce::PathStrokeType(3.0f, juce::PathStrokeType::mitered, juce::PathStrokeType::butt));
        }
        else if (! bypassActive)
        {
            switch (stylePreset)
            {
                case 1:
                {
                    // 网格由背景算法（带玻璃弧形扭曲）负责，这里只画波形，避免“正方形格子 + 扭曲格子”叠在一起
                    const auto c = waveGlowColour;

                    gg.setColour(c.withAlpha(0.18f));
                    gg.strokePath(waveform, juce::PathStrokeType(6.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

                    gg.setColour(c.withAlpha(0.95f));
                    gg.strokePath(waveform, juce::PathStrokeType(2.3f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                    break;
                }

                case 2:
                {
                    drawScanlines(gg, juce::Colours::black.withAlpha(0.14f), 2);
                    const auto amber = waveGlowColour;

                    juce::Path trail = waveform;
                    trail.applyTransform(juce::AffineTransform::translation(0.0f, 1.0f));
                    gg.setColour(amber.withAlpha(0.14f));
                    gg.strokePath(trail, juce::PathStrokeType(5.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

                    trail = waveform;
                    trail.applyTransform(juce::AffineTransform::translation(0.0f, -1.0f));
                    gg.setColour(amber.withAlpha(0.10f));
                    gg.strokePath(trail, juce::PathStrokeType(5.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

                    gg.setColour(amber.withAlpha(1.0f));
                    gg.strokePath(waveform, juce::PathStrokeType(2.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                    break;
                }
                case 3:
                {
                    // 视觉暂留（按“黑屏衰减”的理解）：保留过去 ~0.5s 的若干条波形快照，
                    // 让它们随着时间变暗（趋近黑色）并向右轻微偏移；不做图像反馈，因此不会出现“无限模糊拖尾”。

                    const double nowSec = juce::Time::getMillisecondCounterHiRes() * 0.001;

                    // 若暂停太久就清空，避免残影被“冻结”误判为不消失
                    if (preset3TrailLastSec > 0.0 && (nowSec - preset3TrailLastSec) > 0.25)
                        preset3Trail.clear();
                    preset3TrailLastSec = nowSec;

                    constexpr double trailSec = 0.20;
                    constexpr int maxItems = 40; // 60fps 下 0.5s 大约 30 帧，留一点余量

                    preset3Trail.push_back(Preset3TrailItem{ waveform, nowSec });
                    while ((int) preset3Trail.size() > maxItems)
                        preset3Trail.pop_front();

                    while (! preset3Trail.empty() && (nowSec - preset3Trail.front().tSec) > trailSec)
                        preset3Trail.pop_front();

                    // 先画旧的，新的覆盖在上面
                    const auto baseTrail = waveGlowColour;

                    for (const auto& it : preset3Trail)
                    {
                        const float age = (float) (nowSec - it.tSec);
                        const float u = juce::jlimit(0.0f, 1.0f, age / (float) trailSec);

                        // u 越大越旧：变暗（趋近黑）+ 透明度快速下降
                        const float brightnessMul = 1.0f - 0.90f * u;
                        const float alpha = 0.45f * std::pow(1.0f - u, 2.2f);

                        const int shiftPx = (int) std::round(24.0f * u);

                        juce::Path p = it.path;
                        p.applyTransform(juce::AffineTransform::translation((float) shiftPx, 0.0f));

                        gg.setColour(baseTrail.withMultipliedBrightness(brightnessMul).withAlpha(alpha));
                        gg.strokePath(p, juce::PathStrokeType(2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                    }

                    // 当前帧：轻微辉光 + 彩色主线（更清晰）
                    for (int pass = 0; pass < 2; ++pass)
                    {
                        const float a = (pass == 0 ? 0.06f : 0.90f);
                        const float w = (pass == 0 ? 3.6f : 2.1f);

                        for (int i = 1; i < n; ++i)
                        {
                            const float tt = (float) i / (float) (n - 1);
                            auto col = juce::Colour::fromHSV(tt, 0.85f, 1.0f, a);
                            gg.setColour(col);

                            const auto p0 = sampleToPoint(i - 1);
                            const auto p1 = sampleToPoint(i);
                            gg.drawLine(p0.x, p0.y, p1.x, p1.y, w);
                        }
                    }

                    break;
                }

                case 4:
                {
                    // 预设 4：全新风格（与雷达背景协调的“霓虹向量示波”）
                    const auto neonA = waveGlowColour.withMultipliedBrightness(0.86f);
                    const auto neonB = waveGlowColour;

                    juce::Path glow = waveform;
                    glow.applyTransform(juce::AffineTransform::translation(0.0f, 0.6f));

                    gg.setColour(juce::Colours::black.withAlpha(0.55f));
                    gg.strokePath(glow, juce::PathStrokeType(7.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

                    gg.setColour(neonA.withAlpha(0.14f));
                    gg.strokePath(waveform, juce::PathStrokeType(10.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

                    gg.setColour(neonB.withAlpha(0.85f));
                    gg.strokePath(waveform, juce::PathStrokeType(2.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

                    // 采样点高光（少量）
                    const int step = juce::jmax(1, n / 110);
                    gg.setColour(neonB.withAlpha(0.20f));
                    for (int i = 0; i < n; i += step)
                    {
                        const auto p = sampleToPoint(i);
                        gg.fillEllipse(p.x - 1.4f, p.y - 1.4f, 2.8f, 2.8f);
                    }

                    break;
                }

                case 5:
                {
                    // 冷色磷光：更像 CRT 的“发光磷粉”，让波形清晰但有柔和辉光
                    const auto c = waveGlowColour;

                    // 压暗底阴影（让亮线更立体）
                    juce::Path shadow = waveform;
                    shadow.applyTransform(juce::AffineTransform::translation(1.2f, 1.0f));
                    gg.setColour(juce::Colours::black.withAlpha(0.55f));
                    gg.strokePath(shadow, juce::PathStrokeType(6.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

                    // 外辉光
                    gg.setColour(c.withAlpha(0.14f));
                    gg.strokePath(waveform, juce::PathStrokeType(9.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

                    // 内辉光
                    gg.setColour(c.withAlpha(0.28f));
                    gg.strokePath(waveform, juce::PathStrokeType(5.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

                    // 主线
                    gg.setColour(c.withAlpha(0.96f));
                    gg.strokePath(waveform, juce::PathStrokeType(2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                    break;
                }

                case 6:
                {
                    drawGrid(gg, juce::Colours::white.withAlpha(0.03f), 40);
                    auto c = waveGlowColour;

                    juce::Path mirror = waveform;
                    mirror.applyTransform(juce::AffineTransform::scale(1.0f, -1.0f, 0.0f, midY));

                    gg.setColour(juce::Colours::black.withAlpha(0.55f));
                    gg.strokePath(mirror, juce::PathStrokeType(3.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

                    gg.setColour(c.withAlpha(0.95f));
                    gg.strokePath(waveform, juce::PathStrokeType(2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                    gg.strokePath(mirror,  juce::PathStrokeType(2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                    break;
                }
                case 7:
                {
                    const auto mag = waveGlowColour;

                    const int step = juce::jmax(1, n / 180);
                    for (int i = 0; i < n; i += step)
                    {
                        const auto p = sampleToPoint(i);
                        gg.setColour(mag.withAlpha(0.75f));
                        gg.drawLine(p.x, midY, p.x, p.y, 2.0f);
                    }

                    gg.setColour(mag.withAlpha(0.95f));
                    gg.strokePath(waveform, juce::PathStrokeType(1.6f, juce::PathStrokeType::mitered, juce::PathStrokeType::butt));
                    break;
                }
                case 8:
                {
                    // 预设 8：加入 0.5s 拖影（逐渐变黑消失），但不做右移
                    const double nowSec = juce::Time::getMillisecondCounterHiRes() * 0.001;
                    if (preset8TrailLastSec > 0.0 && (nowSec - preset8TrailLastSec) > 0.25)
                        preset8Trail.clear();
                    preset8TrailLastSec = nowSec;

                    constexpr double trailSec = 0.32;
                    constexpr int maxItems = 40;

                    juce::Random rng ((int) juce::Time::getMillisecondCounter());
                    const auto c = waveGlowColour;

                    // 当前帧：保留原来的“抖动”风格，但改成 Path 以便写入拖影队列
                    juce::Path jitterPath;
                    if (n > 0)
                    {
                        const auto p0 = sampleToPoint(0);
                        jitterPath.startNewSubPath(p0.x, p0.y);
                        for (int i = 1; i < n; ++i)
                        {
                            const auto p1 = sampleToPoint(i);
                            const float jx = (rng.nextFloat() - 0.5f) * 1.6f;
                            const float jy = (rng.nextFloat() - 0.5f) * 1.2f;
                            jitterPath.lineTo(p1.x + jx, p1.y + jy);
                        }
                    }

                    preset8Trail.push_back(Preset3TrailItem{ jitterPath, nowSec });
                    while ((int) preset8Trail.size() > maxItems)
                        preset8Trail.pop_front();
                    while (! preset8Trail.empty() && (nowSec - preset8Trail.front().tSec) > trailSec)
                        preset8Trail.pop_front();

                    // 先画旧的拖影，再画当前帧
                    for (const auto& it : preset8Trail)
                    {
                        const float age = (float) (nowSec - it.tSec);
                        const float u = juce::jlimit(0.0f, 1.0f, age / (float) trailSec);

                        const float brightnessMul = 1.0f - 0.92f * u;
                        const float alpha = 0.40f * std::pow(1.0f - u, 2.4f);

                        gg.setColour(c.withMultipliedBrightness(brightnessMul).withAlpha(alpha));
                        gg.strokePath(it.path, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                    }

                    const int dots = 120;
                    gg.setColour(juce::Colours::white.withAlpha(0.06f));
                    for (int i = 0; i < dots; ++i)
                    {
                        const float x = sb.getX() + rng.nextFloat() * sb.getWidth();
                        const float y = sb.getY() + rng.nextFloat() * sb.getHeight();
                        gg.fillRect(x, y, 1.0f, 1.0f);
                    }
                    break;
                }

                case 9:
                {
                    const auto purp = waveGlowColour;

                    juce::Path shadow = waveform;
                    shadow.applyTransform(juce::AffineTransform::translation(2.0f, 2.0f));

                    gg.setColour(juce::Colours::black.withAlpha(0.60f));
                    gg.strokePath(shadow, juce::PathStrokeType(5.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

                    gg.setColour(purp.withAlpha(0.20f));
                    gg.strokePath(waveform, juce::PathStrokeType(7.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

                    gg.setColour(purp.withAlpha(1.0f));
                    gg.strokePath(waveform, juce::PathStrokeType(2.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                    break;
                }
                case 10:
                {
                    // 预设 10：加入 0.5s 拖影（逐渐变黑消失），但不移动
                    const double nowSec = juce::Time::getMillisecondCounterHiRes() * 0.001;
                    if (preset10TrailLastSec > 0.0 && (nowSec - preset10TrailLastSec) > 0.25)
                        preset10Trail.clear();
                    preset10TrailLastSec = nowSec;

                    constexpr double trailSec = 1.8;
                    constexpr int maxItems = 40;

                    preset10Trail.push_back(Preset3TrailItem{ waveform, nowSec });
                    while ((int) preset10Trail.size() > maxItems)
                        preset10Trail.pop_front();
                    while (! preset10Trail.empty() && (nowSec - preset10Trail.front().tSec) > trailSec)
                        preset10Trail.pop_front();

                    for (const auto& it : preset10Trail)
                    {
                        const float age = (float) (nowSec - it.tSec);
                        const float u = juce::jlimit(0.0f, 1.0f, age / (float) trailSec);

                        const float brightnessMul = 1.0f - 0.95f * u;
                        const float alpha = 0.42f * std::pow(1.0f - u, 2.6f);

                        gg.setColour(juce::Colours::white.withMultipliedBrightness(brightnessMul).withAlpha(alpha));
                        gg.strokePath(it.path, juce::PathStrokeType(1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                    }

                    gg.setColour(juce::Colours::white.withAlpha(0.10f));
                    gg.drawLine(sb.getX(), midY, sb.getRight(), midY, 1.0f);

                    gg.setColour(juce::Colours::white.withAlpha(0.92f));
                    gg.strokePath(waveform, juce::PathStrokeType(1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                    break;
                }

                case 11:
                {
                    drawScanlines(gg, juce::Colours::black.withAlpha(0.18f), 2);
                    const auto green = juce::Colour::fromRGB(0x00, 0xFF, 0x66);

                    gg.setColour(green.withAlpha(0.15f));
                    gg.strokePath(waveform, juce::PathStrokeType(8.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

                    gg.setColour(green.withAlpha(1.0f));
                    gg.strokePath(waveform, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                    break;
                }
                default:
                {
                    gg.setColour(juce::Colours::white.withAlpha(0.08f));
                    gg.drawLine(sb.getX(), midY, sb.getRight(), midY, 1.0f);

                    gg.setColour(waveBaseColour.withAlpha(0.9f));

                    gg.strokePath(waveform, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                    break;
                }
            }
        }

        // 中心线（大多数预设都更像示波器）
        if (! bypassActive && stylePreset != 10)

        {
            gg.setColour(juce::Colours::white.withAlpha(0.06f));
            gg.drawLine(sb.getX(), midY, sb.getRight(), midY, 1.0f);
        }

        // TV Glitch：统一叠加层
        auto applyGlitch = [&](const juce::Path& wave, bool hasWave)
        {
            const int ms = (int) juce::Time::getMillisecondCounter();
            const float seconds = (float) (juce::Time::getMillisecondCounterHiRes() * 0.001);

            const int burstPeriod = 150 + stylePreset * 17;
            const int burstMod = 7 + (stylePreset % 5);
            const bool burst = (((ms / burstPeriod) % burstMod) == (stylePreset % burstMod));

            float amount = juce::jlimit(0.18f, 1.0f, 0.32f + 0.055f * (float) stylePreset + (burst ? 0.45f : 0.0f));

            juce::Random rng ((int) (ms ^ (preset * 0x9E3779B9) ^ (stylePreset * 0x45D9F3B)));

            auto rollBar = [&](float speed, juce::Colour c, float alpha)
            {
                const float phase = std::fmod(seconds * speed, 1.0f);
                const float y = sb.getY() + phase * sb.getHeight();
                const float h = 6.0f + 40.0f * amount;

                juce::ColourGradient grad(c.withAlpha(0.0f), sb.getX(), y - h,
                                          c.withAlpha(alpha), sb.getX(), y,
                                          false);
                grad.addColour(0.70, c.withAlpha(0.0f));
                gg.setGradientFill(grad);
                gg.fillRect(sb.getX(), y - h, sb.getWidth(), h * 2.0f);
            };

            auto tearStrips = [&](int count, float maxDx, bool colorful)
            {
                for (int i = 0; i < count; ++i)
                {
                    const float y = sb.getY() + rng.nextFloat() * sb.getHeight();
                    const float h = 1.5f + rng.nextFloat() * (8.0f + 22.0f * amount);
                    const float dx = (rng.nextFloat() - 0.5f) * maxDx;

                    gg.setColour(juce::Colours::black.withAlpha(0.05f * amount));
                    gg.fillRect(sb.getX(), y, sb.getWidth(), h);

                    if (colorful)
                        gg.setColour(juce::Colour::fromHSV(rng.nextFloat(), 0.95f, 1.0f, 0.12f * amount));
                    else
                        gg.setColour(juce::Colours::white.withAlpha(0.06f * amount));
                    gg.fillRect(sb.getX() + dx, y, sb.getWidth(), h);

                    gg.setColour(juce::Colours::white.withAlpha(0.04f * amount));
                    gg.drawLine(sb.getX(), y, sb.getRight(), y, 1.0f);
                }
            };

            auto macroBlocks = [&](int count, float bwMax, float bhMax, bool tinted)
            {
                for (int i = 0; i < count; ++i)
                {
                    const float bw = 3.0f + rng.nextFloat() * bwMax;
                    const float bh = 2.0f + rng.nextFloat() * bhMax;
                    const float x = sb.getX() + rng.nextFloat() * (sb.getWidth() - bw);
                    const float y = sb.getY() + rng.nextFloat() * (sb.getHeight() - bh);

                    const bool dark = (rng.nextFloat() < 0.45f);
                    if (dark)
                        gg.setColour(juce::Colours::black.withAlpha(0.10f * amount));
                    else if (tinted)
                        gg.setColour(juce::Colour::fromHSV(rng.nextFloat(), 0.85f, 1.0f, 0.09f * amount));
                    else
                        gg.setColour(juce::Colours::white.withAlpha(0.07f * amount));

                    gg.fillRect(x, y, bw, bh);

                    if (burst && rng.nextFloat() < 0.25f)
                    {
                        gg.setColour(juce::Colours::white.withAlpha(0.05f));
                        gg.drawRect(juce::Rectangle<float>(x, y, bw, bh), 1.0f);
                    }
                }
            };

            auto rgbSplitWave = [&](float off, float alpha)
            {
                if (! hasWave)
                    return;

                juce::Path pr = wave; pr.applyTransform(juce::AffineTransform::translation(+off, 0.0f));
                juce::Path pb = wave; pb.applyTransform(juce::AffineTransform::translation(-off, 0.0f));

                gg.setColour(juce::Colours::red.withAlpha(alpha));
                gg.strokePath(pr, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

                gg.setColour(juce::Colours::deepskyblue.withAlpha(alpha));
                gg.strokePath(pb, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            };

            auto grain = [&](int dots, juce::Colour c, float a)
            {
                gg.setColour(c.withAlpha(a));
                for (int i = 0; i < dots; ++i)
                {
                    const float x = sb.getX() + rng.nextFloat() * sb.getWidth();
                    const float y = sb.getY() + rng.nextFloat() * sb.getHeight();
                    gg.fillRect(x, y, 1.0f, 1.0f);
                }
            };

            switch (stylePreset)
            {
                case 1:

                {
                    rollBar(0.30f, juce::Colour::fromRGB(0x9A, 0xE6, 0xFF), 0.12f * amount);
                    tearStrips(2 + (burst ? 6 : 2), 40.0f + 120.0f * amount, true);
                    rgbSplitWave(0.8f + 3.8f * amount, 0.09f * amount);
                    grain(80 + (int) (220 * amount), juce::Colours::white, 0.03f * amount);
                    break;
                }
                case 2:
                {
                    const float y = sb.getBottom() - (8.0f + 18.0f * (0.5f + 0.5f * std::sin(seconds * 1.7f)));
                    gg.setColour(juce::Colours::white.withAlpha(0.12f * amount));
                    gg.fillRect(sb.getX(), y, sb.getWidth(), 2.0f + 4.0f * amount);

                    tearStrips(1 + (burst ? 7 : 3), 60.0f + 160.0f * amount, false);
                    rgbSplitWave(0.5f + 2.0f * amount, 0.06f * amount);
                    grain(120 + (int) (260 * amount), juce::Colours::white, 0.02f * amount);
                    break;
                }
                case 3:
                {
                    rollBar(0.18f, juce::Colour::fromHSV(std::fmod(seconds * 0.3f, 1.0f), 1.0f, 1.0f, 1.0f), 0.10f * amount);
                    tearStrips(3 + (burst ? 10 : 4), 90.0f + 220.0f * amount, true);
                    macroBlocks(8 + (int) (26 * amount), 60.0f + 90.0f * amount, 18.0f + 20.0f * amount, true);
                    if (burst) rgbSplitWave(1.5f + 6.0f * amount, 0.10f);
                    break;
                }
                case 4:
                {
                    macroBlocks(18 + (int) (55 * amount), 22.0f + 50.0f * amount, 10.0f + 22.0f * amount, false);
                    tearStrips(1 + (burst ? 6 : 1), 30.0f + 90.0f * amount, false);
                    if (burst) grain(400 + (int) (600 * amount), juce::Colours::white, 0.03f);
                    break;
                }
                case 5:
                {
                    rollBar(0.22f, juce::Colour::fromRGB(0x00, 0xFF, 0xC6), 0.10f * amount);
                    macroBlocks(10 + (int) (20 * amount), 90.0f + 130.0f * amount, 30.0f + 30.0f * amount, true);

                    if (hasWave)
                    {
                        for (int i = 0; i < (burst ? 6 : 3); ++i)
                        {
                            const float ox = (rng.nextFloat() - 0.5f) * (8.0f + 30.0f * amount);
                            const float oy = (rng.nextFloat() - 0.5f) * (6.0f + 24.0f * amount);
                            juce::Path ghost = wave;
                            ghost.applyTransform(juce::AffineTransform::translation(ox, oy));
                            gg.setColour(juce::Colours::white.withAlpha(0.04f * amount));
                            gg.strokePath(ghost, juce::PathStrokeType(6.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                        }
                    }

                    tearStrips(2 + (burst ? 8 : 2), 70.0f + 160.0f * amount, true);
                    break;
                }
                case 6:
                {
                    gg.setColour(juce::Colours::black.withAlpha(0.16f * amount));
                    for (float y = sb.getY(); y <= sb.getBottom(); y += 2.0f)
                        gg.drawLine(sb.getX(), y, sb.getRight(), y, 1.0f);

                    tearStrips(2 + (burst ? 5 : 2), 80.0f + 180.0f * amount, false);
                    rgbSplitWave(0.9f + 3.0f * amount, 0.07f * amount);
                    break;
                }
                case 7:
                {
                    // 降噪：macroBlocks 很容易盖住波形，这里改为更轻的撕裂条
                    if (burst)
                        tearStrips(1 + (int) (4 * amount), 220.0f, true);
                    else
                        tearStrips(1, 120.0f, true);
                    break;
                }
                case 8:
                {
                    // 降噪：去掉大量 grain，保留少量水平亮线
                    for (int i = 0; i < 2 + (int) (5 * amount); ++i)
                    {
                        const float y = sb.getY() + rng.nextFloat() * sb.getHeight();
                        const float x1 = sb.getX();
                        const float x2 = sb.getRight();
                        gg.setColour(juce::Colours::white.withAlpha(0.015f + 0.03f * amount));
                        gg.drawLine(x1, y, x2, y, 1.0f);
                    }
                    if (burst)
                        tearStrips(1, 120.0f, false);
                    break;
                }
                case 9:
                {
                    // 降噪：降低 CRT tile 覆盖强度，避免影响波形清晰度
                    // 注意：避免函数内 static 图像，规避部分宿主卸载阶段的静态析构卡死风险。
                    const int step = (int) juce::jlimit(2.0f, 6.0f, 6.0f - 3.0f * amount);
                    juce::Image tile (juce::Image::ARGB, step * 3, step, true);
                    {
                        juce::Image::BitmapData bd(tile, juce::Image::BitmapData::readWrite);
                        for (int y = 0; y < step; ++y)
                        {
                            bd.setPixelColour(0, y, juce::Colours::red);
                            bd.setPixelColour(1, y, juce::Colours::green);
                            bd.setPixelColour(2, y, juce::Colours::blue);
                        }
                    }

                    const int ox = (int) (sb.getX() + std::fmod(seconds * (18.0f + 35.0f * amount), (float) (step * 3)));
                    const int oy = (int) (sb.getY() + std::fmod(seconds * (6.0f + 10.0f * amount), (float) step));
                    gg.setTiledImageFill(tile, ox, oy, 0.008f * amount);
                    gg.fillRect(sb);

                    rollBar(0.16f, juce::Colour::fromRGB(0xB7, 0x4D, 0xFF), 0.05f * amount);
                    tearStrips(1 + (burst ? 5 : 1), 90.0f + 160.0f * amount, true);
                    rgbSplitWave(0.6f + 2.2f * amount, 0.05f * amount);
                    break;
                }
                case 10:
                {
                    // 降噪：去掉 macroBlocks，仅保留轻微撕裂/滚动条
                    if (burst)
                        rollBar(0.18f, juce::Colours::white, 0.08f);
                    tearStrips(1, 50.0f + 70.0f * amount, false);
                    break;
                }

                case 11:
                {
                    rollBar(0.12f, juce::Colour::fromRGB(0x00, 0xFF, 0x66), 0.10f * amount);
                    // 去掉闪烁矩形块（太干扰波形可读性），保留撕裂/色分离
                    tearStrips(2 + (burst ? 8 : 2), 80.0f + 200.0f * amount, false);
                    if (burst && hasWave)
                        rgbSplitWave(2.0f + 5.0f * amount, 0.08f);
                    break;
                }

                default:
                {
                    rollBar(0.22f, juce::Colours::white, 0.10f * amount);
                    tearStrips(2 + (int) (5 * amount), 80.0f + 150.0f * amount, true);
                    macroBlocks(10 + (int) (30 * amount), 60.0f + 100.0f * amount, 20.0f + 20.0f * amount, true);
                    rgbSplitWave(0.8f + 3.0f * amount, 0.06f * amount);
                    break;
                }
            }
        };

        if (! owner.processor.bypassed.load(std::memory_order_acquire))
            applyGlitch(waveform, ! bypassActive);

        // --- 预设切换动画 ---
        const float pt = owner.getPresetTransitionT();
        if (pt > 0.0f)
        {
            const float e = pt * pt * (3.0f - 2.0f * pt);
            const float inv = 1.0f - e;

            juce::Random rng ((int) juce::Time::getMillisecondCounter() ^ (preset * 0xBADC0DE));

            const float edgeBoost = juce::jlimit(0.0f, 1.0f, (std::abs(pt - 0.5f) * 2.0f));
            const float snowAmount = (0.12f + 0.28f * inv) * (0.35f + 0.65f * edgeBoost);
            gg.setColour(juce::Colours::white.withAlpha(snowAmount));
            const int snowDots = 450 + (int) (1200 * inv);
            for (int i = 0; i < snowDots; ++i)
            {
                const float x = sb.getX() + rng.nextFloat() * sb.getWidth();
                const float y = sb.getY() + rng.nextFloat() * sb.getHeight();
                gg.fillRect(x, y, 1.0f, 1.0f);
            }

            const float rollY = sb.getY() + std::fmod((float) (juce::Time::getMillisecondCounterHiRes() * 0.001 * 0.95), 1.0f) * sb.getHeight();
            juce::ColourGradient roll(juce::Colours::white.withAlpha(0.0f), sb.getX(), rollY - 40.0f,
                                      juce::Colours::white.withAlpha(0.12f * inv), sb.getX(), rollY,
                                      false);
            roll.addColour(0.60, juce::Colours::white.withAlpha(0.0f));
            gg.setGradientFill(roll);
            gg.fillRect(sb.getX(), rollY - 40.0f, sb.getWidth(), 80.0f);

            const float wipe = juce::jlimit(0.0f, 1.0f, (pt < 0.5f ? pt * 2.0f : (1.0f - pt) * 2.0f));
            const float barH = sb.getHeight() * (0.10f + 0.35f * wipe);
            gg.setColour(juce::Colours::black.withAlpha(0.35f * inv));
            gg.fillRect(sb.getX(), sb.getCentreY() - barH * 0.5f, sb.getWidth(), barH);

            const int blocks = 6 + (int) (22 * inv);
            for (int i = 0; i < blocks; ++i)
            {
                const float bw = 8.0f + rng.nextFloat() * (70.0f * inv);
                const float bh = 6.0f + rng.nextFloat() * (35.0f * inv);
                const float x = sb.getX() + rng.nextFloat() * (sb.getWidth() - bw);
                const float y = sb.getY() + rng.nextFloat() * (sb.getHeight() - bh);
                const bool dark = (rng.nextFloat() < 0.55f);
                gg.setColour((dark ? juce::Colours::black : juce::Colours::white).withAlpha(0.10f * inv));
                gg.fillRect(x, y, bw, bh);
            }
        }

        // --- 电视开关机动画 ---
        const float t = owner.getBypassTransitionT();
        if (t > 0.0f)
        {
            const bool toBypass = owner.bypassTransitionToOn;
            const float e = t * t * (3.0f - 2.0f * t);
            const float k = toBypass ? e : (1.0f - e);

            juce::Random rng ((int) juce::Time::getMillisecondCounter());
            const float flicker = 0.75f + 0.25f * rng.nextFloat();

            gg.setColour(juce::Colours::white.withAlpha(0.18f * (1.0f - k) * flicker));
            gg.drawLine(sb.getX(), sb.getCentreY(), sb.getRight(), sb.getCentreY(), 2.0f + 2.0f * (1.0f - k));

            const int strips = 1 + (int) std::round(6.0f * (1.0f - k));
            for (int i = 0; i < strips; ++i)
            {
                const float y = sb.getY() + rng.nextFloat() * sb.getHeight();
                const float hh = 1.5f + rng.nextFloat() * (10.0f + 22.0f * (1.0f - k));
                const float dx = (rng.nextFloat() - 0.5f) * (40.0f + 220.0f * (1.0f - k));
                gg.setColour(juce::Colours::black.withAlpha(0.08f * (1.0f - k)));
                gg.fillRect(sb.getX(), y, sb.getWidth(), hh);
                gg.setColour(juce::Colour::fromHSV(rng.nextFloat(), 0.95f, 1.0f, 0.08f * (1.0f - k)));
                gg.fillRect(sb.getX() + dx, y, sb.getWidth(), hh);
            }

            if (t < 0.12f)
            {
                const float flash = (1.0f - (t / 0.12f));
                gg.setColour(juce::Colours::white.withAlpha(0.10f * flash));
                gg.fillRect(sb);
            }

            gg.setColour(juce::Colours::black.withAlpha(0.55f * (1.0f - k)));
            gg.fillRect(sb);
        }

        // OSD 统一色调：始终与波形主色系一致
        const auto& presetParams = display_present::getPresetParamsForChannel(preset);
        const auto accent = osdAccentColour;

        // 频道 OSD：右上角，独立显示/消失（仅显示4位频道号）
        const juce::String channelText = owner.getChannelOsdText();
        if (channelText.isNotEmpty())
        {
            const auto& channelCfg = presetParams.osd.channel;

            const float cox = sb.getWidth()  * channelCfg.rect.x;
            const float coy = sb.getHeight() * channelCfg.rect.y;
            const float cow = sb.getWidth()  * channelCfg.rect.w;
            const float coh = sb.getHeight() * channelCfg.rect.h;

            const auto channelOuter = juce::Rectangle<float>(sb.getX() + cox, sb.getY() + coy, cow, coh);

            const float channelFontSize = juce::jlimit(channelCfg.fontMin,
                                                       channelCfg.fontMax,
                                                       channelOuter.getHeight() * channelCfg.fontHeightScale);
            gg.setFont(juce::Font(channelFontSize, juce::Font::bold));

            gg.setColour(juce::Colours::black.withAlpha(channelCfg.shadowAlpha));
            gg.drawText(channelText,
                        channelOuter.translated(channelCfg.shadowOffsetX, channelCfg.shadowOffsetY),
                        juce::Justification::centred,
                        true);

            gg.setColour(accent.withAlpha(channelCfg.textAlpha));
            gg.drawText(channelText, channelOuter, juce::Justification::centred, true);
        }

        // TV/ST-SAP/MUTE 状态 OSD：左上角，独立显示/消失
        const float modeT = owner.getModeOsdT();
        if (modeT > 0.0f)
        {
            const float fade = 1.0f - juce::jlimit(0.0f, 1.0f, (modeT - 0.80f) / 0.20f);

            const int algoMode = owner.processor.getLossAlgorithmMode();
            const juce::String algoText = (algoMode == LDSJvstAudioProcessor::kLossAlgorithmUniformBandwidth)
                                            ? "ST/SAP: UNIFORM BW"
                                            : "ST/SAP: LEGACY Q";

            const int cutMode = owner.processor.getCutMode();
            const int cutSlopeDb = owner.processor.getCutSlopeDbPerOct();
            const juce::String cutText = (cutMode == LDSJvstAudioProcessor::kCutModeHardMask)
                                           ? "TV: HARD MASK"
                                           : ("TV: HPF/LPF " + juce::String(cutSlopeDb) + "dB/oct");

            const juce::String muteText = owner.processor.isLossMaskInverted()
                                            ? "MUTE: INVERT ON"
                                            : "MUTE: INVERT OFF";

            const auto& modeCfg = presetParams.osd.mode;

            const float mox = sb.getWidth()  * modeCfg.rect.x;
            const float moy = sb.getHeight() * modeCfg.rect.y;
            const float mow = sb.getWidth()  * modeCfg.rect.w;
            const float moh = sb.getHeight() * modeCfg.rect.h;

            const auto modeOuter = juce::Rectangle<float>(sb.getX() + mox, sb.getY() + moy, mow, moh);

            const float lineFontSize = juce::jlimit(modeCfg.fontMin,
                                                    modeCfg.fontMax,
                                                    modeOuter.getHeight() * modeCfg.fontHeightScale);
            gg.setFont(juce::Font(lineFontSize, juce::Font::plain));

            auto line1 = modeOuter;
            line1.removeFromTop(modeOuter.getHeight() * modeCfg.line1Split);
            auto line2 = modeOuter;
            line2.removeFromTop(modeOuter.getHeight() * modeCfg.line2Split);
            auto line3 = modeOuter;
            line3.removeFromTop(modeOuter.getHeight() * 0.86f);

            gg.setColour(juce::Colours::black.withAlpha(modeCfg.shadowAlpha * fade));
            gg.drawText(cutText,
                        line1.translated(modeCfg.shadowOffsetX, modeCfg.shadowOffsetY),
                        juce::Justification::centredTop,
                        true);
            gg.drawText(algoText,
                        line2.translated(modeCfg.shadowOffsetX, modeCfg.shadowOffsetY),
                        juce::Justification::centredTop,
                        true);
            gg.drawText(muteText,
                        line3.translated(modeCfg.shadowOffsetX, modeCfg.shadowOffsetY),
                        juce::Justification::centredTop,
                        true);

            gg.setColour(accent.withAlpha(modeCfg.textAlpha * fade));
            gg.drawText(cutText, line1, juce::Justification::centredTop, true);
            gg.drawText(algoText, line2, juce::Justification::centredTop, true);
            gg.drawText(muteText, line3, juce::Justification::centredTop, true);

        }

        // 音量 OSD：屏幕下方，独立显示/消失
        const float osdT = owner.getVolumeOsdT();
        if (osdT > 0.0f)
        {
            const float fade = 1.0f - juce::jlimit(0.0f, 1.0f, (osdT - 0.80f) / 0.20f);

            const float minDb = LDSJvstAudioProcessor::kPreGainDbMin;
            const float maxDb = LDSJvstAudioProcessor::kPreGainDbMax;
            const float curDb = owner.processor.getPreGainDb();
            const float u = juce::jlimit(0.0f, 1.0f, (curDb - minDb) / (maxDb - minDb));

            const float ox = sb.getWidth()  * (96.0f  / 643.0f);
            const float oy = sb.getHeight() * (375.0f / 529.0f);
            const float ow = sb.getWidth()  * (460.0f / 643.0f);
            const float oh = sb.getHeight() * (43.0f  / 529.0f);

            const auto outer = juce::Rectangle<float>(sb.getX() + ox, sb.getY() + oy, ow, oh);
            const auto inner = outer.reduced(6.0f, 6.0f);

            gg.setColour(juce::Colours::black.withAlpha(0.55f * fade));
            gg.fillRoundedRectangle(outer, 6.0f);
            gg.setColour(accent.withAlpha(0.10f * fade));
            gg.fillRoundedRectangle(outer, 6.0f);

            gg.setColour(accent.withAlpha(0.28f * fade));
            gg.drawRoundedRectangle(outer, 6.0f, 1.0f);

            const int ticks = 10;
            gg.setColour(accent.withAlpha(0.12f * fade));
            for (int i = 1; i < ticks; ++i)
            {
                const float tx = inner.getX() + inner.getWidth() * ((float) i / (float) ticks);
                gg.drawLine(tx, inner.getY(), tx, inner.getBottom(), 1.0f);
            }

            auto fill = inner;
            fill.setWidth(inner.getWidth() * u);
            gg.setColour(accent.withAlpha(0.85f * fade));
            gg.fillRect(fill);

            const float fontSize = juce::jlimit(10.0f, 16.0f, outer.getHeight() * 0.45f);
            gg.setFont(juce::Font(fontSize, juce::Font::bold));

            const juce::String label = "VOL";
            const juce::String value = juce::String(curDb >= 0.0f ? "+" : "") + juce::String(curDb, 0) + "dB";

            gg.setColour(juce::Colours::black.withAlpha(0.65f * fade));
            gg.drawText(label + " " + value, outer.translated(1.0f, 1.0f), juce::Justification::centred, true);

            gg.setColour(accent.withAlpha(0.95f * fade));
            gg.drawText(label + " " + value, outer, juce::Justification::centred, true);
        }

        // 边框线（离屏也画一遍；最终 warp 后还能保持统一）
        gg.setColour(juce::Colours::white.withAlpha(0.10f));
        gg.drawRoundedRectangle(sb.reduced(0.5f), corner, 1.0f);
    }

    // ============================================================
    // 2) 生成“每一行的左右偏移”序列：随机噪声 -> 窗口平均 -> 时间平滑
    //    每个 preset 使用不同参数（强度/窗口/更新速率），让预设更有区别。
    // ============================================================

    const int ms = (int) juce::Time::getMillisecondCounter();
    juce::Random rng ((int) (ms ^ (preset * 0x6A09E667) ^ (stylePreset * 0x7F4A7C15)));

    const auto& warpParams = presetParams.warp;


    float maxOffsetPx = warpParams.maxOffsetPx;
    int windowRadius = warpParams.windowRadius;
    float updateProb = warpParams.updateProb;
    float temporalSmooth = warpParams.temporalSmooth;

    // bypass 时减弱扭曲（更像屏幕熄灭/稳定）
    if (bypassActive)
        maxOffsetPx *= 0.25f;

    // 偶尔整屏“同步撕裂”一下（更像老电视/录像带）
    const float globalKickProb = warpParams.globalKickProbBase + warpParams.globalKickProbPerPreset * (float) preset;
    const bool globalKick = (rng.nextFloat() < globalKickProb);
    const float globalKickDx = globalKick ? (rng.nextFloat() - 0.5f) * maxOffsetPx * 1.8f : 0.0f;

    // 1) 更新原始噪声（按概率更新，让画面更有“信号噪声的持续性”）
    const bool doUpdate = (rng.nextFloat() < updateProb) || (ms % 17 == 0) || globalKick;
    if (doUpdate)
    {
        // 让噪声在垂直方向上也有一点“块状相关性”，避免每行完全独立显得太电子
        const int block = juce::jlimit(1, 14,
                                       warpParams.blockBase + (stylePreset % warpParams.blockPresetMod)
                                       + windowRadius / 3);

        for (int y = 0; y < H;)
        {
            const float v = (rng.nextFloat() - 0.5f) * 2.0f; // [-1, +1]
            const int y2 = juce::jmin(H, y + block);
            for (int yy = y; yy < y2; ++yy)
                scanlineNoiseRaw[(size_t) yy] = v;
            y = y2;
        }
    }

    // 2) 窗口平均（box blur）
    for (int y = 0; y < H; ++y)
    {
        float acc = 0.0f;
        int cnt = 0;
        const int y0 = juce::jmax(0, y - windowRadius);
        const int y1 = juce::jmin(H - 1, y + windowRadius);
        for (int yy = y0; yy <= y1; ++yy)
        {
            acc += scanlineNoiseRaw[(size_t) yy];
            ++cnt;
        }
        const float avg = (cnt > 0 ? acc / (float) cnt : 0.0f);

        // 3) 时间平滑
        scanlineNoiseSmoothed[(size_t) y] = temporalSmooth * scanlineNoiseSmoothed[(size_t) y]
                                          + (1.0f - temporalSmooth) * avg;

        // 转换为像素偏移：加入一个轻微的正弦漂移，让“同步噪声”更像模拟信号
        const float seconds = (float) (juce::Time::getMillisecondCounterHiRes() * 0.001);
        const float drift = warpParams.driftAmp
                          * std::sin(seconds * (warpParams.driftFreqBase + warpParams.driftFreqPerPreset * (float) stylePreset)

                                    + (float) y * warpParams.driftYMul);
        scanlineOffsetPx[(size_t) y] = (scanlineNoiseSmoothed[(size_t) y] + drift) * maxOffsetPx + globalKickDx;

    }

    // ============================================================
    // 3) Remap：按行把 screenBase 采样到 screenWarp
    //    这里只做左右偏移（电视行同步噪声特征）。
    // ============================================================

    {
        juce::Image::BitmapData src(screenBase, juce::Image::BitmapData::readOnly);
        juce::Image::BitmapData dst(screenWarp, juce::Image::BitmapData::writeOnly);

        for (int y = 0; y < H; ++y)
        {
            const float dxRow = scanlineOffsetPx[(size_t) y];
            auto* out = reinterpret_cast<juce::PixelARGB*> (dst.getLinePointer(y));

            // source y 固定（只做横向 remap）
            const auto* in = reinterpret_cast<const juce::PixelARGB*> (src.getLinePointer(y));

            for (int x = 0; x < W; ++x)
            {
                const float sx = (float) x - dxRow;

                if (sx <= 0.0f)
                {
                    out[x] = in[0];
                    continue;
                }
                if (sx >= (float) (W - 1))
                {
                    out[x] = in[W - 1];
                    continue;
                }

                const int x0 = (int) sx;
                const int x1 = x0 + 1;
                const float tLerp = sx - (float) x0;

                const auto p0 = in[x0].getNativeARGB();
                const auto p1 = in[x1].getNativeARGB();

                const auto a0 = (int) ((p0 >> 24) & 0xFF);
                const auto r0 = (int) ((p0 >> 16) & 0xFF);
                const auto g0 = (int) ((p0 >>  8) & 0xFF);
                const auto b0 = (int) ((p0 >>  0) & 0xFF);

                const auto a1 = (int) ((p1 >> 24) & 0xFF);
                const auto r1 = (int) ((p1 >> 16) & 0xFF);
                const auto g1 = (int) ((p1 >>  8) & 0xFF);
                const auto b1 = (int) ((p1 >>  0) & 0xFF);

                auto lerp8 = [&](int v0, int v1)
                {
                    const float vv = (1.0f - tLerp) * (float) v0 + tLerp * (float) v1;
                    return (juce::uint8) juce::jlimit(0, 255, (int) std::round(vv));
                };

                out[x].setARGB(lerp8(a0, a1), lerp8(r0, r1), lerp8(g0, g1), lerp8(b0, b1));
            }
        }
    }

    // ============================================================
    // 4) 把扭曲后的图像画到最终屏幕区域 b
    // ============================================================
    g.setImageResamplingQuality(juce::Graphics::lowResamplingQuality);
    g.drawImage(screenWarp,
                b.getX(), b.getY(), b.getWidth(), b.getHeight(),
                0, 0, W, H,
                false);

    // ============================================================
    // 5) 限制器阈值线（UI）：两根上下对称的线
    //    目标：颜色/质感尽量贴合当前预设的“波形线”，但更弱化一些。
    //    注意：画在 remap 之后，让线条保持稳定且易于交互。
    // ============================================================
    {
        const float th = juce::jlimit(0.0f, 1.0f, processor.getLimiterThreshold());
        const float midY = b.getCentreY();
        const float scaleY = b.getHeight() * 0.40f;

        const float yTop = midY - th * scaleY;
        const float yBot = midY + th * scaleY;

        const auto& presetParams = display_present::getPresetParamsForChannel(preset);

        const float accent = presetParams.bg.accentAlpha;

        // 强度：比波形线明显更弱，但在不同预设亮度下保持可见
        const float mainA   = juce::jlimit(0.10f, 0.42f, 0.14f + 1.10f * accent);
        const float glowA   = juce::jlimit(0.04f, 0.20f, mainA * 0.38f);
        const float shadowA = juce::jlimit(0.05f, 0.25f, mainA * 0.55f);



        auto drawLineWithStyle = [&](float y, bool rainbow)
        {
            // 阴影（让线条更“贴”在屏幕上）
            g.setColour(juce::Colours::black.withAlpha(shadowA));
            g.drawLine(b.getX(), y + 1.0f, b.getRight(), y + 1.0f, 3.0f);

            if (rainbow)
            {
                // 渐变主线：匹配 preset 3 的彩虹波形
                juce::ColourGradient grad(juce::Colour::fromHSV(0.00f, 0.85f, 1.0f, mainA), b.getX(), y,
                                          juce::Colour::fromHSV(1.00f, 0.85f, 1.0f, mainA), b.getRight(), y,
                                          false);
                g.setGradientFill(grad);
                g.drawLine(b.getX(), y, b.getRight(), y, 2.0f);

                juce::ColourGradient glow(juce::Colour::fromHSV(0.00f, 0.85f, 1.0f, glowA), b.getX(), y,
                                          juce::Colour::fromHSV(1.00f, 0.85f, 1.0f, glowA), b.getRight(), y,
                                          false);
                g.setGradientFill(glow);
                g.drawLine(b.getX(), y, b.getRight(), y, 6.5f);
                return;
            }

            const auto base = waveBaseColour;

            // 外辉光（弱）
            g.setColour(base.withAlpha(glowA));
            g.drawLine(b.getX(), y, b.getRight(), y, 6.0f);

            // 主线（更细、更弱）
            g.setColour(base.withAlpha(mainA));
            g.drawLine(b.getX(), y, b.getRight(), y, 2.0f);
        };

        const bool rainbow = (stylePreset == 3);

        drawLineWithStyle(yTop, rainbow);
        drawLineWithStyle(yBot, rainbow);
    }

    // ============================================================
    // 6) 屏幕下方频段通过指示（100格）：左低频 -> 右高频
    //    亮 = 当前频段允许通过（mask=1），灭 = 当前频段被滤掉（mask=0）
    // ============================================================
    {
        const int bands = kBandGridCount;
        if (bands > 0)
        {
            if (lossMaskSnapshotUI.size() != bands)
            {
                lossMaskSnapshotUI.resize(bands);
                for (int i = 0; i < bands; ++i)
                    lossMaskSnapshotUI.set(i, (uint8_t) 1);
            }

            const float marginX = juce::jmax(8.0f, b.getWidth() * 0.03f);
            const float gridAreaW = juce::jmax(50.0f, b.getWidth() - marginX * 2.0f);

            const float gap = juce::jmax(0.0f, juce::jmin(1.0f, b.getWidth() * 0.0012f));
            const float cellW = juce::jmax(1.0f, (gridAreaW - gap * (float) (bands - 1)) / (float) bands);
            const float cellH = juce::jmax(3.0f, b.getHeight() * 0.020f);

            const float x0 = b.getX() + (b.getWidth() - (cellW * (float) bands + gap * (float) (bands - 1))) * 0.5f;
            const float y0 = b.getBottom() - cellH - juce::jmax(2.0f, b.getHeight() * 0.018f);

            const float accent = presetParams.bg.accentAlpha;

            const auto base = waveBaseColour;

            const auto litColour = base.withAlpha(juce::jlimit(0.65f, 0.98f, 0.68f + 1.2f * accent));
            const auto dimColour = juce::Colours::black.withAlpha(juce::jlimit(0.52f, 0.84f, 0.72f - 0.4f * accent));
            const auto borderColour = base.withAlpha(juce::jlimit(0.10f, 0.38f, 0.12f + 0.8f * accent));

            const float stripX0 = x0;
            const float stripX1 = x0 + cellW * (float) bands + gap * (float) (bands - 1);

            const float lowCutHz = processor.getLowCutHz();
            const float highCutHz = processor.getHighCutHz();

            const float logMin = std::log(LDSJvstAudioProcessor::kLowCutHzMin);
            const float logMax = std::log(LDSJvstAudioProcessor::kHighCutHzMax);
            const float logSpan = juce::jmax(0.0001f, logMax - logMin);

            auto hzToX = [&](float hz)
            {
                const float h = juce::jlimit(LDSJvstAudioProcessor::kLowCutHzMin,
                                             LDSJvstAudioProcessor::kHighCutHzMax,
                                             hz);
                const float t = (std::log(h) - logMin) / logSpan;
                return stripX0 + juce::jlimit(0.0f, 1.0f, t) * (stripX1 - stripX0);
            };

            const float lowCutX = hzToX(lowCutHz);
            const float highCutX = hzToX(highCutHz);

            const int cutMode = processor.getCutMode();
            const int cutSlope = processor.getCutSlopeDbPerOct();
            const int cutAngleDeg = processor.getCutSlopeAngleDeg();

            for (int i = 0; i < bands; ++i)
            {
                const float x = x0 + (float) i * (cellW + gap);
                const auto r = juce::Rectangle<float>(x, y0, cellW, cellH);
                const float cx = r.getCentreX();

                const bool inCutZone = (cx < lowCutX) || (cx > highCutX);
                const bool passByMask = lossMaskSnapshotUI[i] != 0;
                const bool pass = (! inCutZone) && passByMask;

                if (stylePreset == 3 && pass)

                {
                    const float t = (float) i / (float) juce::jmax(1, bands - 1);
                    g.setColour(juce::Colour::fromHSV(t, 0.88f, 1.0f, litColour.getFloatAlpha()));
                }
                else
                {
                    g.setColour(pass ? litColour : dimColour);
                }
                g.fillRect(r);

                g.setColour(borderColour);
                g.drawRect(r, 0.35f);
            }

            const float triW = juce::jmax(9.0f, cellW * 2.3f);
            const float triH = juce::jmax(6.0f, cellH * 2.2f);

            const bool rainbowHandle = (stylePreset == 3);

            const float rainbowTimePhase = std::fmod((float) (juce::Time::getMillisecondCounterHiRes() * 0.001 * 0.22), 1.0f);

            auto drawRainbowLine = [&](float x1, float y1, float x2, float y2, float width, float alpha)
            {
                constexpr int kSegments = 24;
                for (int s = 0; s < kSegments; ++s)
                {
                    const float t0 = (float) s / (float) kSegments;
                    const float t1 = (float) (s + 1) / (float) kSegments;
                    const float xa = x1 + (x2 - x1) * t0;
                    const float ya = y1 + (y2 - y1) * t0;
                    const float xb = x1 + (x2 - x1) * t1;
                    const float yb = y1 + (y2 - y1) * t1;
                    const float hue = std::fmod(t0 + rainbowTimePhase, 1.0f);
                    g.setColour(juce::Colour::fromHSV(hue, 0.88f, 1.0f, alpha));
                    g.drawLine(xa, ya, xb, yb, width);
                }
            };

            auto drawDownTriangle = [&](float cx, juce::Colour c)
            {
                juce::Path tri;
                const float topY = y0 - triH - 2.0f;
                tri.startNewSubPath(cx - triW * 0.5f, topY);
                tri.lineTo(cx + triW * 0.5f, topY);
                tri.lineTo(cx, y0 - 1.0f);
                tri.closeSubPath();

                g.setColour(juce::Colours::black.withAlpha(0.45f));
                g.fillPath(tri);

                if (rainbowHandle)
                {
                    const float hueA = std::fmod(rainbowTimePhase, 1.0f);
                    const float hueB = std::fmod(rainbowTimePhase + 0.34f, 1.0f);

                    juce::ColourGradient grad(juce::Colour::fromHSV(hueA, 0.88f, 1.0f, 0.32f), cx - triW * 0.5f, topY,
                                              juce::Colour::fromHSV(hueB, 0.88f, 1.0f, 0.32f), cx + triW * 0.5f, y0 - 1.0f,
                                              false);
                    g.setGradientFill(grad);
                    g.fillPath(tri);

                    juce::ColourGradient strokeGrad(juce::Colour::fromHSV(hueA, 0.88f, 1.0f, 0.92f), cx - triW * 0.5f, topY,
                                                    juce::Colour::fromHSV(hueB, 0.88f, 1.0f, 0.92f), cx + triW * 0.5f, y0 - 1.0f,
                                                    false);
                    g.setGradientFill(strokeGrad);
                    g.strokePath(tri, juce::PathStrokeType(1.1f));
                    return;
                }

                g.setColour(c.withAlpha(0.92f));
                g.strokePath(tri, juce::PathStrokeType(1.1f));
                g.setColour(c.withAlpha(0.30f));
                g.fillPath(tri);
            };

            auto drawSlopeLineHandle = [&](float cx, juce::Colour c, bool isLowCutHandle)
            {
                const float yLine = y0 - triH * 0.60f;
                const float clampedAngle = (float) juce::jlimit(0, 90, cutAngleDeg);
                const float len = triW * 1.90f;
                const float dx = len * std::cos(juce::degreesToRadians(clampedAngle));
                const float dy = len * std::sin(juce::degreesToRadians(clampedAngle));

                const float x0Line = isLowCutHandle ? (cx + dx * 0.5f) : (cx - dx * 0.5f);
                const float y0Line = yLine - dy * 0.5f;
                const float x1Line = isLowCutHandle ? (cx - dx * 0.5f) : (cx + dx * 0.5f);
                const float y1Line = yLine + dy * 0.5f;

                const float strokeW = (cutSlope >= LDSJvstAudioProcessor::kCutSlope48dB) ? 2.6f
                                    : (cutSlope >= LDSJvstAudioProcessor::kCutSlope24dB) ? 2.0f
                                    : 1.4f;

                if (rainbowHandle)
                {
                    drawRainbowLine(x0Line, y0Line, x1Line, y1Line, strokeW + 2.0f, 0.30f);
                    drawRainbowLine(x0Line, y0Line, x1Line, y1Line, strokeW, 0.98f);
                    return;
                }

                g.setColour(c.withAlpha(0.32f));
                g.drawLine(x0Line, y0Line, x1Line, y1Line, strokeW + 2.0f);

                g.setColour(c.withAlpha(0.98f));
                g.drawLine(x0Line, y0Line, x1Line, y1Line, strokeW);
            };

            const auto hardMaskHandleColour = base.withAlpha(0.95f);
            const auto hpfLpfHandleColour = base.brighter(0.10f).withAlpha(0.98f);

            if (cutMode == LDSJvstAudioProcessor::kCutModeHardMask)
            {
                drawDownTriangle(lowCutX, hardMaskHandleColour);
                drawDownTriangle(highCutX, hardMaskHandleColour);
            }
            else
            {
                // HPF/LPF 模式下仅显示斜线手柄（角度对应12/24/48 dB每倍频程）。
                drawSlopeLineHandle(lowCutX, hpfLpfHandleColour, true);
                drawSlopeLineHandle(highCutX, hpfLpfHandleColour, false);
            }

        }
    }
}

LDSJvstAudioProcessorEditor::LDSJvstAudioProcessorEditor(LDSJvstAudioProcessor& p)
    : juce::AudioProcessorEditor(&p), processor(p), oscilloscope(*this, p)
{
    tvImage = juce::ImageCache::getFromMemory(BinaryData::TV_png, BinaryData::TV_pngSize);
    bypassImage = juce::ImageCache::getFromMemory(BinaryData::BYPASS_png, BinaryData::BYPASS_pngSize);
    remoteImage = juce::ImageCache::getFromMemory(BinaryData::remote_control_png, BinaryData::remote_control_pngSize);

    int remoteLightSize = 0;
    const char* remoteLightNames[] = {
        "remote_control_light.png",
        "remote_control_light_png",
        "assets/remote_control_light.png"
    };
    for (const auto* name : remoteLightNames)
    {
        if (const auto* remoteLightData = BinaryData::getNamedResource(name, remoteLightSize))
        {
            remoteLightImage = juce::ImageCache::getFromMemory(remoteLightData, remoteLightSize);
            break;
        }
    }

    setResizable(true, true);

    resizeConstrainer.setFixedAspectRatio(editorAspectRatio);
    resizeConstrainer.setSizeLimits(320, 240, 1600, 1200);
    setConstrainer(&resizeConstrainer);

    setSize(1400, 1050);

    addAndMakeVisible(oscilloscope);
    addAndMakeVisible(bypassHitArea);
    addAndMakeVisible(tvOverlay);
    addAndMakeVisible(remoteOverlay);

    if constexpr (kEnableTempQInput)
    {
        tempNotchQLabel.setText("Notch Q (test)", juce::dontSendNotification);
        tempNotchQLabel.setJustificationType(juce::Justification::centredLeft);
        tempNotchQLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.9f));

        tempNotchQInput.setInputRestrictions(0, "0123456789.");
        tempNotchQInput.onReturnKey = [this] { applyTempNotchQFromInput(); };
        tempNotchQInput.onFocusLost = [this] { applyTempNotchQFromInput(); };

        addAndMakeVisible(tempNotchQLabel);
        addAndMakeVisible(tempNotchQInput);
        refreshTempNotchQInputText();
    }

    presetLights.ensureStorageAllocated(indicatorPresetCount);

    for (int i = 0; i < indicatorPresetCount; ++i)
    {
        auto* light = presetLights.add(new IndicatorLight(*this, i));
        addAndMakeVisible(light);
    }

    // 从宿主恢复的 state 里读取频道（如果没有则为默认 0）
    selectedChannelId = juce::jlimit(channelIdMin, channelIdMax, processor.getDisplayPresetIndex());
    previousChannelId = selectedChannelId;
    selectedPresetIndex = selectedChannelId % indicatorPresetCount;
    previousPresetIndex = selectedPresetIndex;

    if (resizableCorner != nullptr)
        resizableCorner->toFront(false);

    // 某些宿主在初次打开时不会立刻触发 resized()，这里强制布局一次，确保默认就能看到指示灯
    resized();

    // 让指示灯立刻反映当前预设
    for (auto* light : presetLights)
        if (light != nullptr)
            light->repaint();
}

LDSJvstAudioProcessorEditor::~LDSJvstAudioProcessorEditor()
{
    editorShuttingDown = true;

    oscilloscope.shutdownForEditorTeardown();
    remoteOverlay.shutdownForEditorTeardown();

    if constexpr (kEnableTempQInput)
    {
        tempNotchQInput.onReturnKey = nullptr;
        tempNotchQInput.onFocusLost = nullptr;
    }
}

void LDSJvstAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::black);
}

void LDSJvstAudioProcessorEditor::paintOverChildren(juce::Graphics& g)
{
    const auto area = getLocalBounds().toFloat();
    const float marginX = juce::jmax(5.0f, area.getWidth() * 0.008f);
    const float marginY = juce::jmax(4.0f, area.getHeight() * 0.006f);
    const float h = juce::jmax(9.0f, area.getHeight() * 0.016f);
    const float w = juce::jmax(52.0f, area.getWidth() * 0.075f);

    const auto tag = juce::Rectangle<float>(area.getRight() - marginX - w, area.getY() + marginY, w, h);

    g.setFont(juce::Font(juce::jmax(7.0f, h * 0.70f), juce::Font::plain));
    g.setColour(juce::Colours::black.withAlpha(0.50f));
    g.drawText(kPluginUiVersionText, tag, juce::Justification::centredRight, true);
}

void LDSJvstAudioProcessorEditor::toggleBypassFromUI()
{
    if (editorShuttingDown || processor.isShuttingDownNow())
        return;

    const bool next = ! processor.bypassed.load(std::memory_order_acquire);

    bypassTransitionActive = true;
    bypassTransitionToOn = next;
    bypassTransitionStartSeconds = juce::Time::getMillisecondCounterHiRes() * 0.001;

    processor.bypassed.store(next, std::memory_order_release);

    // 刷新触发区图片与屏幕
    bypassHitArea.repaint();
    oscilloscope.repaint();
    for (auto* light : presetLights)
        if (light != nullptr)
            light->repaint();
}

bool LDSJvstAudioProcessorEditor::isBypassedOrTransitioningToBypass() const noexcept
{
    // 目标是 bypass ON 时，波形不显示；动画开始时也先隐藏波形避免“残影”
    return processor.bypassed.load(std::memory_order_acquire) || (bypassTransitionActive && bypassTransitionToOn);
}

float LDSJvstAudioProcessorEditor::getBypassTransitionT() noexcept
{
    if (! bypassTransitionActive)
        return 0.0f;

    const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
    const double dt = now - bypassTransitionStartSeconds;
    const float t = (float) (dt / bypassTransitionDurationSeconds);
    if (t >= 1.0f)
    {
        // 动画结束：清理状态，避免一直处于 active
        bypassTransitionActive = false;
        return 0.0f;
    }
    return juce::jlimit(0.0f, 1.0f, t);
}

float LDSJvstAudioProcessorEditor::getPresetTransitionT() noexcept
{
    if (! presetTransitionActive)
        return 0.0f;

    const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
    const double dt = now - presetTransitionStartSeconds;
    const float t = (float) (dt / presetTransitionDurationSeconds);
    if (t >= 1.0f)
    {
        presetTransitionActive = false;
        return 0.0f;
    }
    return juce::jlimit(0.0f, 1.0f, t);
}

void LDSJvstAudioProcessorEditor::nudgePreGainDbFromUI (float deltaDb)
{
    if (editorShuttingDown || processor.isShuttingDownNow())
        return;

    processor.addPreGainDb(deltaDb);

    // 触发一次 OSD 显示
    volumeOsdActive = true;
    volumeOsdStartSeconds = juce::Time::getMillisecondCounterHiRes() * 0.001;

    oscilloscope.repaint();
}

void LDSJvstAudioProcessorEditor::toggleLossAlgorithmFromUI()
{
    if (editorShuttingDown || processor.isShuttingDownNow())
        return;

    processor.toggleLossAlgorithmMode();

    modeOsdActive = true;
    modeOsdStartSeconds = juce::Time::getMillisecondCounterHiRes() * 0.001;

    oscilloscope.repaint();
}

void LDSJvstAudioProcessorEditor::cycleCutModeOrSlopeFromTV()
{
    if (editorShuttingDown || processor.isShuttingDownNow())
        return;

    processor.cycleCutModeOrSlopeFromTV();

    modeOsdActive = true;
    modeOsdStartSeconds = juce::Time::getMillisecondCounterHiRes() * 0.001;

    oscilloscope.repaint();
}

void LDSJvstAudioProcessorEditor::toggleSleepFreezeFromUI()
{
    if (editorShuttingDown || processor.isShuttingDownNow())
        return;

    processor.toggleLossMaskFrozen();
    oscilloscope.repaint();
}

void LDSJvstAudioProcessorEditor::toggleLossMaskInvertFromUI()
{
    if (editorShuttingDown || processor.isShuttingDownNow())
        return;

    processor.toggleLossMaskInverted();

    modeOsdActive = true;
    modeOsdStartSeconds = juce::Time::getMillisecondCounterHiRes() * 0.001;

    oscilloscope.repaint();
}

void LDSJvstAudioProcessorEditor::recallPreviousChannelFromUI()
{
    if (editorShuttingDown || processor.isShuttingDownNow())
        return;

    const int cur = selectedChannelId;
    const int prev = juce::jlimit(channelIdMin, channelIdMax, previousChannelId);

    if (prev == cur)
        return;

    setSelectedPresetIndex(prev);
}


float LDSJvstAudioProcessorEditor::getVolumeOsdT() noexcept

{
    if (! volumeOsdActive)
        return 0.0f;

    const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
    const double dt = now - volumeOsdStartSeconds;
    const float t = (float) (dt / volumeOsdDurationSeconds);

    if (t >= 1.0f)
    {
        volumeOsdActive = false;
        return 0.0f;
    }

    return juce::jlimit(0.0f, 1.0f, t);
}

float LDSJvstAudioProcessorEditor::getModeOsdT() noexcept
{
    if (! modeOsdActive)
        return 0.0f;

    const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
    const double dt = now - modeOsdStartSeconds;
    const float t = (float) (dt / modeOsdDurationSeconds);

    if (t >= 1.0f)
    {
        modeOsdActive = false;
        return 0.0f;
    }

    return juce::jlimit(0.0f, 1.0f, t);
}

void LDSJvstAudioProcessorEditor::pushChannelDigitFromRemote (int digit)
{
    if (editorShuttingDown || processor.isShuttingDownNow())
        return;

    if (digit < 0 || digit > 9)
        return;

    const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;

    // 输入超时后重新开始一轮输入
    if ((! pendingChannelDigits.isEmpty())
        && (now - pendingChannelLastInputSeconds >= channelInputTimeoutSeconds))
    {
        pendingChannelDigits.clear();
    }

    channelOsdActive = false;

    if (pendingChannelDigits.length() >= kMaxChannelDigits)
        pendingChannelDigits.clear();

    pendingChannelDigits += juce::String(digit);
    pendingChannelLastInputSeconds = now;

    // 满4位立即“提交”频道
    if (pendingChannelDigits.length() >= kMaxChannelDigits)
        triggerChannelJumpNow();

    oscilloscope.repaint();
}

void LDSJvstAudioProcessorEditor::triggerChannelJumpNow()
{
    if (pendingChannelDigits.isEmpty())
        return;

    const juce::String submittedChannel = pendingChannelDigits;
    pendingChannelDigits.clear();

    const int channelIndex = submittedChannel.getIntValue();
    if (channelIndex >= channelIdMin && channelIndex <= channelIdMax)
    {
        setSelectedPresetIndex(channelIndex);
        return;
    }

    // 超出可用频道范围时，仅显示输入结果，不切换频道
    channelDisplayText = submittedChannel;

    channelOsdCurrentDurationSeconds = channelInputOsdDurationSeconds;
    channelOsdActive = true;
    channelOsdStartSeconds = juce::Time::getMillisecondCounterHiRes() * 0.001;
}

juce::String LDSJvstAudioProcessorEditor::getChannelOsdText() noexcept
{
    const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;

    if (! pendingChannelDigits.isEmpty())
    {
        const double idleSec = now - pendingChannelLastInputSeconds;

        // 3秒不输入：提交当前频道
        if (idleSec >= channelInputTimeoutSeconds)
            triggerChannelJumpNow();
        else
            return pendingChannelDigits.paddedLeft('-', kMaxChannelDigits);
    }

    if (channelOsdActive)
    {
        const double dt = now - channelOsdStartSeconds;
        if (dt >= channelOsdCurrentDurationSeconds)
        {
            channelOsdActive = false;
            return {};
        }

        return channelDisplayText.paddedLeft('-', kMaxChannelDigits);
    }

    return {};
}

void LDSJvstAudioProcessorEditor::refreshTempNotchQInputText()

{
    if constexpr (! kEnableTempQInput)
        return;

    const float q = processor.getLossNotchQ();
    tempNotchQInput.setText(juce::String(q, 3), juce::dontSendNotification);
}

void LDSJvstAudioProcessorEditor::applyTempNotchQFromInput()
{
    if constexpr (! kEnableTempQInput)
        return;

    if (editorShuttingDown || processor.isShuttingDownNow())
        return;

    const float parsed = tempNotchQInput.getText().getFloatValue();
    const float clamped = juce::jlimit(LDSJvstAudioProcessor::kLossNotchQMin,
                                       LDSJvstAudioProcessor::kLossNotchQMax,
                                       parsed);

    processor.setLossNotchQ(clamped);
    tempNotchQInput.setText(juce::String(clamped, 3), juce::dontSendNotification);
}

void LDSJvstAudioProcessorEditor::setSelectedPresetIndex (int newIndex)

{
    if (editorShuttingDown || processor.isShuttingDownNow())
        return;

    const int newChannelId = juce::jlimit(channelIdMin, channelIdMax, newIndex);
    const int newVisualIndex = newChannelId % indicatorPresetCount;

    // 频道号显示：点击/切换时都显示，3 秒后消失
    pendingChannelDigits.clear();
    channelDisplayText = juce::String(newChannelId);
    channelOsdCurrentDurationSeconds = presetChannelOsdDurationSeconds;
    channelOsdActive = true;
    channelOsdStartSeconds = juce::Time::getMillisecondCounterHiRes() * 0.001;

    if (selectedChannelId == newChannelId)
    {
        oscilloscope.repaint();
        return;
    }

    // 启动预设切换动画（灯号维度 0..11）
    presetTransitionActive = true;
    presetTransitionFrom = selectedPresetIndex;
    presetTransitionTo = newVisualIndex;
    presetTransitionStartSeconds = juce::Time::getMillisecondCounterHiRes() * 0.001;

    previousChannelId = selectedChannelId;
    selectedChannelId = newChannelId;

    previousPresetIndex = selectedPresetIndex;
    selectedPresetIndex = newVisualIndex;

    processor.setDisplayPresetIndex(newChannelId);

    // 频道参数统一由 display_present.h 驱动：0..11 固定预设，12..9999 衍生预设
    const auto presetCfg = display_present::getPresetParamsForChannel(newChannelId);
    if (presetCfg.cutPreset.applyOnChannelEnter)
    {
        processor.setCutMode(presetCfg.cutPreset.cutMode);
        processor.setCutSlopeDbPerOct(presetCfg.cutPreset.cutSlopeDbPerOct);
        processor.setLowCutHz(presetCfg.cutPreset.lowCutHz);
        processor.setHighCutHz(presetCfg.cutPreset.highCutHz);
    }

    for (auto* light : presetLights)
        if (light != nullptr)
            light->repaint();

    oscilloscope.repaint();
}

void LDSJvstAudioProcessorEditor::resized()
{
    const float scale = (float) getWidth() / (float) baseEditorWidth; // 固定4:3时，宽高缩放一致

    const auto scaledScreen = juce::Rectangle<int>(
        juce::roundToInt(screenX * scale),
        juce::roundToInt(screenY * scale),
        juce::roundToInt(screenW * scale),
        juce::roundToInt(screenH * scale)
    );

    oscilloscope.setBounds(scaledScreen);

    // bypass 触发区（点击此区域切换 bypass）
    bypassHitArea.setBounds(
        juce::roundToInt(bypassX * scale),
        juce::roundToInt(bypassY * scale),
        juce::roundToInt(bypassW * scale),
        juce::roundToInt(bypassH * scale)
    );

    tvOverlay.setBounds(getLocalBounds());
    tvOverlay.toFront(false);

    remoteOverlay.setBounds(getLocalBounds());
    remoteOverlay.toFront(false);

    const int lightSize = juce::jmax(2, juce::roundToInt(presetLightSize * scale));
    const int lightX = juce::roundToInt(presetLightX * scale);
    const int lightY0 = juce::roundToInt(presetLightY * scale);
    const int lightStep = juce::roundToInt((presetLightSize + presetLightGap) * scale);

    for (int i = 0; i < presetLights.size(); ++i)
        if (auto* light = presetLights[i])
            light->setBounds(lightX, lightY0 + i * lightStep, lightSize, lightSize);

    if constexpr (kEnableTempQInput)
    {
        tempNotchQLabel.setBounds(
            juce::roundToInt(tempQLabelX * scale),
            juce::roundToInt(tempQLabelY * scale),
            juce::roundToInt(tempQLabelW * scale),
            juce::roundToInt(tempQLabelH * scale));

        tempNotchQInput.setBounds(
            juce::roundToInt(tempQInputX * scale),
            juce::roundToInt(tempQInputY * scale),
            juce::roundToInt(tempQInputW * scale),
            juce::roundToInt(tempQInputH * scale));
    }

    // 让指示灯在最上层显示（覆盖TV.png），同时保留右下角缩放控件

    for (auto* light : presetLights)
        if (light != nullptr)
            light->toFront(false);

    // bypass 覆盖层也要在 TV.png 之上（否则 BYPASS.png 会被 TV 盖住）
    bypassHitArea.toFront(false);

    if constexpr (kEnableTempQInput)
    {
        tempNotchQLabel.toFront(false);
        tempNotchQInput.toFront(false);
    }

    if (resizableCorner != nullptr)
        resizableCorner->toFront(false);
}

void LDSJvstAudioProcessorEditor::setRemotePulledOut (bool shouldBePulledOut)
{
    const float target = shouldBePulledOut ? 1.0f : 0.0f;

    if (remotePulledOut == shouldBePulledOut && (! remotePullAnimating) && (remotePullAmount == target))
        return;

    remotePulledOut = shouldBePulledOut; // 记录目标状态

    remotePullAnimating = true;
    remotePullAnimFrom = remotePullAmount;
    remotePullAnimTo = target;
    remotePullAnimStartSeconds = juce::Time::getMillisecondCounterHiRes() * 0.001;

    // 遥控器拿出来时，让它永远在最上层（避免被指示灯/热区覆盖），同时也避免底下元素被误操作。
    remoteOverlay.toFront(false);

    remoteOverlay.beginAnimation();
    remoteOverlay.repaint();
}

void LDSJvstAudioProcessorEditor::RemoteControlOverlay::timerCallback()
{
    if (owner.editorShuttingDown || owner.processor.isShuttingDownNow())
    {
        stopTimer();
        return;
    }

    bool stillNeeded = false;
    bool needRepaint = false;

    const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;

    // 1) 遥控器拿出/收回动画
    if (owner.remotePullAnimating)
    {
        const double dt = now - owner.remotePullAnimStartSeconds;
        float t = (float) (dt / remotePullAnimDurationSeconds);

        if (t >= 1.0f)
        {
            owner.remotePullAmount = owner.remotePullAnimTo;
            owner.remotePullAnimating = false;
        }
        else
        {
            t = juce::jlimit(0.0f, 1.0f, t);
            const float e = t * t * (3.0f - 2.0f * t); // smoothstep
            owner.remotePullAmount = juce::jmap(e, owner.remotePullAnimFrom, owner.remotePullAnimTo);
            stillNeeded = true;
        }

        needRepaint = true;
    }

    // 2) VOL+/VOL- 按住自动连发
    // - mouseDown 时先立刻变化 1 次
    // - 按住超过 initialDelay 后，每 interval 继续变化
    if (volumeRepeatActive)
    {
        // 若按下态已经被清掉（例如 mouseUp 先于 timer 到达），确保停止
        if (remotePressedButtonIndex < 0 || volumeRepeatDir == 0)
        {
            volumeRepeatActive = false;
            volumeRepeatDir = 0;
        }
        else
        {
            const double held = now - volumeRepeatPressSeconds;
            if (held >= volumeRepeatInitialDelaySeconds)
            {
                const double dtStep = now - volumeRepeatLastStepSeconds;
                if (dtStep >= volumeRepeatIntervalSeconds)
                {
                    owner.nudgePreGainDbFromUI((float) volumeRepeatDir);
                    volumeRepeatLastStepSeconds = now;
                }
            }

            stillNeeded = true; // 需要保持 timer 继续跑
        }
    }

    // 3) 遥控器按下态：不需要持续刷新（mouseDown/mouseUp 会触发 repaint）
    // 这里不做额外处理，避免按住时因为持续 repaint 造成“闪烁感”。

    if (needRepaint)
        repaint();

    if (! stillNeeded)
        stopTimer();
}

void LDSJvstAudioProcessorEditor::RemoteControlOverlay::paint (juce::Graphics& g)
{
    if (! owner.remoteImage.isValid())
        return;

    const float scale = (float) owner.getWidth() / (float) baseEditorWidth;

    const float x = remoteX * scale;
    const float y = (remoteY - remoteLift * owner.getRemotePullAmount()) * scale;
    const float w = remoteW * scale;
    const float h = remoteH * scale;

    const auto target = juce::Rectangle<float>(x, y, w, h);

    g.drawImage(owner.remoteImage, target, juce::RectanglePlacement::stretchToFit, false);

    if (remotePressedButtonIndex >= 0)
    {
        const auto lightTarget = juce::Rectangle<float>(
            remoteLightX * scale,
            remoteLightY * scale,
            remoteLightW * scale,
            remoteLightH * scale
        );

        if (owner.remoteLightImage.isValid())
        {
            g.drawImage(owner.remoteLightImage, lightTarget, juce::RectanglePlacement::stretchToFit, false);
        }
        else
        {
            g.setColour(juce::Colours::yellow.withAlpha(0.85f));
            g.fillEllipse(lightTarget);
            g.setColour(juce::Colours::white.withAlpha(0.70f));
            g.drawEllipse(lightTarget, juce::jmax(1.0f, 1.5f * scale));
        }
    }

    // 遥控器按下态：对被按下的按钮区域叠加黑色遮罩（模拟按键被压下）

    // 备注：以下名称均为“遥控器上的名字”（用于后续接入逻辑时对照）
    struct RemoteButton
    {
        const char* name;
        int offsetX;
        int offsetY;
        int w;
        int h;
        bool isBypass;
    };

    // 说明：这里的坐标是“相对遥控器图片左上角”的偏移（不依赖 remoteX/remoteY），
    // 避免移动遥控器位置/基准尺寸后，按钮热区错位。
    static constexpr RemoteButton buttons[] = {
        { "BYPASS", 73, 287, remoteBypassW, remoteBypassH, true },

        // 数字按钮
        { "数字1",  28,  71, 23, 10, false },
        { "数字2",  73,  71, 23, 10, false },
        { "数字3", 118,  71, 23, 10, false },
        { "数字4",  28, 101, 23, 10, false },
        { "数字5",  73, 101, 23, 10, false },
        { "数字6", 118, 101, 23, 10, false },
        { "数字7",  28, 132, 23, 10, false },
        { "数字8",  73, 132, 23, 10, false },
        { "数字9", 118, 131, 23, 10, false },
        { "数字0", 118, 162, 23, 10, false },

        // 功能键
        { "TV",      29, 168, 23, 10, false },
        { "SLEEP",   29, 198, 23, 10, false },
        { "RECALL",  29, 228, 23, 10, false },
        { "ST/SAP",  29, 258, 23, 10, false },
        { "MUTE",    29, 287, 23, 10, false },

        // 音量
        { "VOL+",    73, 198, 23, 23, false },
        { "VOL-",    73, 242, 23, 23, false },

        // 频道
        { "频道增加", 118, 198, 23, 23, false },
        { "频道减少", 118, 242, 23, 23, false },
    };

    if (remotePressedButtonIndex >= 0)
    {
        const int idx = juce::jlimit(0, (int) (std::size(buttons) - 1), remotePressedButtonIndex);
        const auto& b = buttons[idx];
        const auto btn = juce::Rectangle<float>(
            x + b.offsetX * scale,
            y + b.offsetY * scale,
            b.w * scale,
            b.h * scale
        );
        g.setColour(juce::Colours::black.withAlpha(0.45f));
        g.fillRect(btn);
    }
}

void LDSJvstAudioProcessorEditor::RemoteControlOverlay::mouseDown (const juce::MouseEvent& e)
{
    if (! owner.remotePulledOut)
        return;

    const auto rel = e.getEventRelativeTo(&owner);
    const auto p = rel.position;

    const float scale = (float) owner.getWidth() / (float) baseEditorWidth;
    const float x = remoteX * scale;
    const float y = (remoteY - remoteLift * owner.getRemotePullAmount()) * scale;

    struct RemoteButton
    {
        const char* name;
        int offsetX;
        int offsetY;
        int w;
        int h;
        bool isBypass;
    };

    static constexpr RemoteButton buttons[] = {
        { "BYPASS", 73, 287, remoteBypassW, remoteBypassH, true },

        { "数字1",  28,  71, 23, 10, false },
        { "数字2",  73,  71, 23, 10, false },
        { "数字3", 118,  71, 23, 10, false },
        { "数字4",  28, 101, 23, 10, false },
        { "数字5",  73, 101, 23, 10, false },
        { "数字6", 118, 101, 23, 10, false },
        { "数字7",  28, 132, 23, 10, false },
        { "数字8",  73, 132, 23, 10, false },
        { "数字9", 118, 131, 23, 10, false },
        { "数字0", 118, 162, 23, 10, false },

        { "TV",      29, 168, 23, 10, false },
        { "SLEEP",   29, 198, 23, 10, false },
        { "RECALL",  29, 228, 23, 10, false },
        { "ST/SAP",  29, 258, 23, 10, false },
        { "MUTE",    29, 287, 23, 10, false },

        { "VOL+",    73, 198, 23, 23, false },
        { "VOL-",    73, 242, 23, 23, false },

        { "频道增加", 118, 198, 23, 23, false },
        { "频道减少", 118, 242, 23, 23, false },
    };

    for (int i = 0; i < (int) std::size(buttons); ++i)
    {
        const auto& b = buttons[i];
        const auto r = juce::Rectangle<float>(
            x + b.offsetX * scale,
            y + b.offsetY * scale,
            b.w * scale,
            b.h * scale
        );

        if (r.contains(p))
        {
            remotePressedButtonIndex = i;

            // VOL+/VOL-：按下立即变化一次，并进入“按住自动连发”
            if (std::strcmp (b.name, "VOL+") == 0)
            {
                volumeRepeatActive = true;
                volumeRepeatDir = +1;
                volumeRepeatPressSeconds = juce::Time::getMillisecondCounterHiRes() * 0.001;
                volumeRepeatLastStepSeconds = volumeRepeatPressSeconds;
                owner.nudgePreGainDbFromUI(+1.0f);
            }
            else if (std::strcmp (b.name, "VOL-") == 0)
            {
                volumeRepeatActive = true;
                volumeRepeatDir = -1;
                volumeRepeatPressSeconds = juce::Time::getMillisecondCounterHiRes() * 0.001;
                volumeRepeatLastStepSeconds = volumeRepeatPressSeconds;
                owner.nudgePreGainDbFromUI(-1.0f);
            }
            else
            {
                volumeRepeatActive = false;
                volumeRepeatDir = 0;
            }

            beginAnimation();
            repaint();
            return;
        }

    }
}

void LDSJvstAudioProcessorEditor::RemoteControlOverlay::mouseUp (const juce::MouseEvent& e)
{
    const auto rel = e.getEventRelativeTo(&owner);
    const auto p = rel.position;

    const float scale = (float) owner.getWidth() / (float) baseEditorWidth;
    const float x = remoteX * scale;
    const float y = (remoteY - remoteLift * owner.getRemotePullAmount()) * scale;
    const float w = remoteW * scale;
    const float h = remoteH * scale;
    const auto r = juce::Rectangle<float>(x, y, w, h);

    struct RemoteButton
    {
        const char* name;
        int offsetX;
        int offsetY;
        int w;
        int h;
        bool isBypass;
    };

    static constexpr RemoteButton buttons[] = {
        { "BYPASS", 73, 287, remoteBypassW, remoteBypassH, true },

        { "数字1",  28,  71, 23, 10, false },
        { "数字2",  73,  71, 23, 10, false },
        { "数字3", 118,  71, 23, 10, false },
        { "数字4",  28, 101, 23, 10, false },
        { "数字5",  73, 101, 23, 10, false },
        { "数字6", 118, 101, 23, 10, false },
        { "数字7",  28, 132, 23, 10, false },
        { "数字8",  73, 132, 23, 10, false },
        { "数字9", 118, 131, 23, 10, false },
        { "数字0", 118, 162, 23, 10, false },

        { "TV",      29, 168, 23, 10, false },
        { "SLEEP",   29, 198, 23, 10, false },
        { "RECALL",  29, 228, 23, 10, false },
        { "ST/SAP",  29, 258, 23, 10, false },
        { "MUTE",    29, 287, 23, 10, false },

        { "VOL+",    73, 198, 23, 23, false },
        { "VOL-",    73, 242, 23, 23, false },

        { "频道增加", 118, 198, 23, 23, false },
        { "频道减少", 118, 242, 23, 23, false },
    };

    // 遥控器：松手触发（release-to-trigger）
    // - mouseDown 只进入“按下态”
    // - mouseUp 时，如果仍然在同一个按钮区域内才触发（与主界面一致）
    if (owner.remotePulledOut && remotePressedButtonIndex >= 0)
    {
        const int idx = juce::jlimit(0, (int) (std::size(buttons) - 1), remotePressedButtonIndex);
        const auto& b = buttons[idx];

        const auto btn = juce::Rectangle<float>(
            x + b.offsetX * scale,
            y + b.offsetY * scale,
            b.w * scale,
            b.h * scale
        );

        const bool releasedOnSameButton = btn.contains(p);

        const bool wasVolButton = (std::strcmp (b.name, "VOL+") == 0) || (std::strcmp (b.name, "VOL-") == 0);

        // 松手后先清掉“按下态”
        remotePressedButtonIndex = -1;

        // 结束 VOL 自动连发（VOL 的逻辑在 mouseDown 已经触发过一次，这里不再额外触发）
        if (wasVolButton)
        {
            volumeRepeatActive = false;
            volumeRepeatDir = 0;
        }

        repaint();

        if (releasedOnSameButton && (! wasVolButton))
        {
            // BYPASS：逻辑与主界面一致
            if (b.isBypass)
            {
                owner.toggleBypassFromUI();
            }
            // 频道增加：频道号 +1（边界循环 0..9999）
            else if (std::strcmp (b.name, "频道增加") == 0)
            {
                const int cur = owner.getSelectedPresetIndex();
                const int next = (cur >= channelIdMax) ? channelIdMin : (cur + 1);
                owner.setSelectedPresetIndex(next);
            }
            // 频道减少：频道号 -1（边界循环 0..9999）
            else if (std::strcmp (b.name, "频道减少") == 0)
            {
                const int cur = owner.getSelectedPresetIndex();
                const int prev = (cur <= channelIdMin) ? channelIdMax : (cur - 1);
                owner.setSelectedPresetIndex(prev);
            }

            // TV：切换高低切算法与斜率（HardMask <-> HPF/LPF 12/24/48）
            else if (std::strcmp (b.name, "TV") == 0)
            {
                owner.cycleCutModeOrSlopeFromTV();
            }
            // ST/SAP：切换频带丢失算法（Legacy <-> Uniform Bandwidth）
            else if (std::strcmp (b.name, "ST/SAP") == 0)
            {
                owner.toggleLossAlgorithmFromUI();
            }
            // SLEEP：冻结/恢复当前频带丢失状态（下方像素格子停/继续变化）
            else if (std::strcmp (b.name, "SLEEP") == 0)
            {
                owner.toggleSleepFreezeFromUI();
            }
            // MUTE：反转频带丢失语义（保留/丢失互换）
            else if (std::strcmp (b.name, "MUTE") == 0)
            {
                owner.toggleLossMaskInvertFromUI();
            }
            // RECALL：回到上一个进入的频道
            else if (std::strcmp (b.name, "RECALL") == 0)
            {
                owner.recallPreviousChannelFromUI();
            }
            // 数字键：输入频道（最多4位；3秒不输入自动跳转）
            else

            {
                const juce::String buttonName = juce::String::fromUTF8(b.name);
                if (buttonName.startsWith("数字"))
                {
                    const juce::juce_wchar c = buttonName.getLastCharacter();
                    if (c >= '0' && c <= '9')
                        owner.pushChannelDigitFromRemote((int) (c - '0'));
                }
            }

        }

        return;
    }

    const bool hitRemote = r.contains(p);

    if (! owner.remotePulledOut)
    {
        if (hitRemote)
            owner.setRemotePulledOut(true);
        return;
    }

    // 已经“拿出来”时：点击遥控器外区域，收回
    if (! hitRemote)
        owner.setRemotePulledOut(false);
}