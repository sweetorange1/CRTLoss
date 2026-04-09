#include "PluginEditor.h"
#include <JuceHeader.h>
#include "BinaryData.h"
#include "display_present.h"
#include <cstring>

// --- BypassHitArea ---
void LDSJvstAudioProcessorEditor::BypassHitArea::paint (juce::Graphics& g)
{
    // 触发区本身保持透明；如果 bypass 开启，则用图片覆盖该区域
    if (owner.processor.bypassed && owner.bypassImage.isValid())
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
    processor.getOscilloscopeSnapshot(samples);
    repaint();
}

void LDSJvstAudioProcessorEditor::OscilloscopeComponent::mouseDown (const juce::MouseEvent& e)
{
    const auto b = getLocalBounds().toFloat();

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
}

void LDSJvstAudioProcessorEditor::OscilloscopeComponent::paint(juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();

    const int preset = owner.getSelectedPresetIndex();
    const float corner = 6.0f;

    // 先裁剪到圆角矩形，避免各种背景绘制溢出
    juce::Path clip;
    clip.addRoundedRectangle(b, corner);
    const juce::Graphics::ScopedSaveState clipState(g);
    g.reduceClipRegion(clip);

    // bypass：不显示中间波形（但仍允许动画覆盖层在下面继续绘制）
    const bool bypassActive = owner.isBypassedOrTransitioningToBypass();

    // 即使 bypass 时也允许画“关机动画”，所以这里不直接 return。
    if (samples.isEmpty() && ! bypassActive)
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
        const auto& presetParams = display_present::getPresetParams(preset);
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
                static juce::Image tile;
                if (! tile.isValid())
                {
                    tile = juce::Image(juce::Image::ARGB, step * 2, step * 2, true);
                    juce::Graphics tg(tile);
                    tg.fillAll(juce::Colours::transparentBlack);
                    tg.setColour(juce::Colours::white.withAlpha(0.06f));
                    tg.fillEllipse(0.0f, 0.0f, (float) step, (float) step);
                    tg.fillEllipse((float) step, (float) step, (float) step, (float) step);
                }

                gg.setTiledImageFill(tile, (int) std::fmod(seconds * 12.0f * bg.motionSpeed, (float) (step * 2)),
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
        if (preset == 0 && ! bypassActive)
        {
            const int downsample = 5;
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
                        if (rng.nextFloat() < 0.12f)
                            v = (juce::uint8) rng.nextInt(55);
                        else
                            v = (juce::uint8) (180 + rng.nextInt(76));

                        v = (juce::uint8) ((v / 16) * 16);
                        if (scanline)
                            v = (juce::uint8) juce::jlimit(0, 255, (int) (v * 0.92f));

                        line[x].setARGB((juce::uint8) 255, v, v, v);
                    }
                }
            }

            gg.drawImage(low,
                         sb.getX(), sb.getY(), sb.getWidth(), sb.getHeight(),
                         0, 0, lw, lh,
                         false);

            const auto neonGreen = juce::Colour::fromRGB(0x39, 0xFF, 0x14);
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
            switch (preset)
            {
                case 1:
                {
                    // 网格由背景算法（带玻璃弧形扭曲）负责，这里只画波形，避免“正方形格子 + 扭曲格子”叠在一起
                    const auto c = juce::Colour::fromRGB(0x3A, 0xE6, 0xFF);

                    gg.setColour(c.withAlpha(0.18f));
                    gg.strokePath(waveform, juce::PathStrokeType(6.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

                    gg.setColour(c.withAlpha(0.95f));
                    gg.strokePath(waveform, juce::PathStrokeType(2.3f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                    break;
                }

                case 2:
                {
                    drawScanlines(gg, juce::Colours::black.withAlpha(0.14f), 2);
                    const auto amber = juce::Colour::fromRGB(0xFF, 0xB0, 0x30);

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
                    const auto baseTrail = juce::Colour::fromRGB(0x88, 0xFF, 0xFF);
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
                    const auto neonA = juce::Colour::fromRGB(0xA0, 0x50, 0xFF);
                    const auto neonB = juce::Colour::fromRGB(0x5A, 0xFF, 0xE5);

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
                    const auto c = juce::Colour::fromRGB(0x5A, 0xFF, 0xE5);

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
                    auto c = juce::Colour::fromRGB(0x7C, 0xFF, 0x6B);

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
                    const auto mag = juce::Colour::fromRGB(0xFF, 0x4D, 0xFF);
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
                    const auto c = juce::Colour::fromRGB(0xFF, 0x66, 0x33);

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
                    const auto purp = juce::Colour::fromRGB(0xB7, 0x4D, 0xFF);
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

                    gg.setColour(juce::Colours::lime.withAlpha(0.9f));
                    gg.strokePath(waveform, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                    break;
                }
            }
        }

        // 中心线（大多数预设都更像示波器）
        if (! bypassActive && preset != 10)
        {
            gg.setColour(juce::Colours::white.withAlpha(0.06f));
            gg.drawLine(sb.getX(), midY, sb.getRight(), midY, 1.0f);
        }

        // TV Glitch：统一叠加层
        auto applyGlitch = [&](const juce::Path& wave, bool hasWave)
        {
            const int ms = (int) juce::Time::getMillisecondCounter();
            const float seconds = (float) (juce::Time::getMillisecondCounterHiRes() * 0.001);

            const int burstPeriod = 150 + preset * 17;
            const int burstMod = 7 + (preset % 5);
            const bool burst = (((ms / burstPeriod) % burstMod) == (preset % burstMod));

            float amount = juce::jlimit(0.18f, 1.0f, 0.32f + 0.055f * (float) preset + (burst ? 0.45f : 0.0f));

            juce::Random rng ((int) (ms ^ (preset * 0x9E3779B9)));

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

            switch (preset)
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
                    const int step = (int) juce::jlimit(2.0f, 6.0f, 6.0f - 3.0f * amount);
                    auto getCrtTile = [&](int s) -> const juce::Image&
                    {
                        static juce::Image img;
                        if (! img.isValid() || img.getWidth() != s * 3)
                        {
                            img = juce::Image(juce::Image::ARGB, s * 3, s, true);
                            juce::Image::BitmapData bd(img, juce::Image::BitmapData::readWrite);
                            for (int y = 0; y < s; ++y)
                            {
                                bd.setPixelColour(0, y, juce::Colours::red);
                                bd.setPixelColour(1, y, juce::Colours::green);
                                bd.setPixelColour(2, y, juce::Colours::blue);
                            }
                        }
                        return img;
                    };

                    const auto& tile = getCrtTile(step);
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

        if (! owner.processor.bypassed)
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

        // 音量 OSD：画在屏幕上方（基准坐标：左上(135,129) 大小 340*35）
        // 注意：这里处于“屏幕组件局部坐标系”，所以将基准坐标映射到 sb(0..W/H)
        const float osdT = owner.getVolumeOsdT();
        if (osdT > 0.0f)
        {
            // 前段全亮，尾段淡出
            const float fade = 1.0f - juce::jlimit(0.0f, 1.0f, (osdT - 0.80f) / 0.20f);

            const float minDb = LDSJvstAudioProcessor::kPreGainDbMin;
            const float maxDb = LDSJvstAudioProcessor::kPreGainDbMax;
            const float curDb = owner.processor.getPreGainDb();
            const float u = juce::jlimit(0.0f, 1.0f, (curDb - minDb) / (maxDb - minDb));

            // 将“基准坐标”转换到屏幕局部坐标（screen: 107,120,643,529）
            const float ox = sb.getWidth()  * (86.0f  / 643.0f); // 193 - 107
            const float oy = sb.getHeight() * (64.0f  / 529.0f); // 184 - 120
            const float ow = sb.getWidth()  * (486.0f / 643.0f);
            const float oh = sb.getHeight() * (50.0f  / 529.0f);

            const auto outer = juce::Rectangle<float>(sb.getX() + ox, sb.getY() + oy, ow, oh);
            const auto inner = outer.reduced(6.0f, 6.0f);

            const auto& presetParams = display_present::getPresetParams(preset);

            juce::Colour accent;
            switch (presetParams.bg.kind)
            {
                case display_present::BackgroundKind::digitalGrid:         accent = juce::Colour::fromRGB(0x9A, 0xE6, 0xFF); break;
                case display_present::BackgroundKind::amberVignette:       accent = juce::Colour::fromRGB(0xFF, 0xB0, 0x00); break;
                case display_present::BackgroundKind::radarSweep:          accent = juce::Colour::fromRGB(0xB7, 0x4D, 0xFF); break;
                case display_present::BackgroundKind::phosphorBloom:       accent = juce::Colour::fromRGB(0x00, 0xFF, 0xC6); break;
                case display_present::BackgroundKind::rainbowInterference: accent = juce::Colour::fromHSV(std::fmod((float) (juce::Time::getMillisecondCounterHiRes() * 0.001 * 0.18), 1.0f), 0.95f, 1.0f, 1.0f); break;
                case display_present::BackgroundKind::dotMask:             accent = juce::Colour::fromRGB(0xFF, 0x4D, 0xB7); break;
                case display_present::BackgroundKind::oceanBlobs:          accent = juce::Colour::fromRGB(0x4D, 0xB7, 0xFF); break;
                case display_present::BackgroundKind::mirrorCross:         accent = juce::Colour::fromRGB(0x00, 0xFF, 0x66); break;
                case display_present::BackgroundKind::barcode:             accent = juce::Colour::fromRGB(0xB7, 0x4D, 0xFF); break;
                case display_present::BackgroundKind::glitchStatic:        accent = juce::Colours::white; break;
                case display_present::BackgroundKind::neonStarfield:       accent = juce::Colour::fromRGB(0xB7, 0x4D, 0xFF); break;
                case display_present::BackgroundKind::minimalVignette:     accent = juce::Colours::white; break;
                case display_present::BackgroundKind::greenTerminal:       accent = juce::Colour::fromRGB(0x00, 0xFF, 0x66); break;
                case display_present::BackgroundKind::legacySolid:
                default:                                                  accent = juce::Colours::white; break;
            }

            // 背板（带一点预设色调）
            gg.setColour(juce::Colours::black.withAlpha(0.55f * fade));
            gg.fillRoundedRectangle(outer, 6.0f);
            gg.setColour(accent.withAlpha(0.10f * fade));
            gg.fillRoundedRectangle(outer, 6.0f);

            // 外框
            gg.setColour(accent.withAlpha(0.28f * fade));
            gg.drawRoundedRectangle(outer, 6.0f, 1.0f);

            // 刻度（10 段）
            const int ticks = 10;
            gg.setColour(accent.withAlpha(0.12f * fade));
            for (int i = 1; i < ticks; ++i)
            {
                const float tx = inner.getX() + inner.getWidth() * ((float) i / (float) ticks);
                gg.drawLine(tx, inner.getY(), tx, inner.getBottom(), 1.0f);
            }

            // 填充条
            auto fill = inner;
            fill.setWidth(inner.getWidth() * u);
            gg.setColour(accent.withAlpha(0.85f * fade));
            gg.fillRect(fill);

            // 文本：VOL + 数值（dB）
            const float fontSize = juce::jlimit(10.0f, 16.0f, outer.getHeight() * 0.60f);
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
    juce::Random rng ((int) (ms ^ (preset * 0x6A09E667)));

    const auto& presetParams = display_present::getPresetParams(preset);
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
                                       warpParams.blockBase + (preset % warpParams.blockPresetMod)
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
                          * std::sin(seconds * (warpParams.driftFreqBase + warpParams.driftFreqPerPreset * (float) preset)
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

        const auto& presetParams = display_present::getPresetParams(preset);
        const float accent = presetParams.bg.accentAlpha;

        // 强度：比波形线明显更弱，但在不同预设亮度下保持可见
        const float mainA   = juce::jlimit(0.10f, 0.42f, 0.14f + 1.10f * accent);
        const float glowA   = juce::jlimit(0.04f, 0.20f, mainA * 0.38f);
        const float shadowA = juce::jlimit(0.05f, 0.25f, mainA * 0.55f);

        auto getWaveBaseColour = [&]() -> juce::Colour
        {
            switch (preset)
            {
                case 0:  return juce::Colour::fromRGB(0x39, 0xFF, 0x14); // 像素绿
                case 1:  return juce::Colour::fromRGB(0x3A, 0xE6, 0xFF); // 冷色青
                case 2:  return juce::Colour::fromRGB(0xFF, 0xB0, 0x30); // 琥珀
                case 4:  return juce::Colour::fromRGB(0x5A, 0xFF, 0xE5); // neonB
                case 5:  return juce::Colour::fromRGB(0x5A, 0xFF, 0xE5); // 磷光青
                case 6:  return juce::Colour::fromRGB(0x7C, 0xFF, 0x6B); // 绿
                case 7:  return juce::Colour::fromRGB(0xFF, 0x4D, 0xFF); // 品红
                case 8:  return juce::Colour::fromRGB(0xFF, 0x66, 0x33); // 橙
                case 9:  return juce::Colour::fromRGB(0xB7, 0x4D, 0xFF); // 紫
                case 10: return juce::Colours::white;
                case 11: return juce::Colour::fromRGB(0x00, 0xFF, 0x66);
                case 3:  // 彩虹：下面用渐变来画
                default: return juce::Colours::white;
            }
        };

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

            const auto base = getWaveBaseColour();

            // 外辉光（弱）
            g.setColour(base.withAlpha(glowA));
            g.drawLine(b.getX(), y, b.getRight(), y, 6.0f);

            // 主线（更细、更弱）
            g.setColour(base.withAlpha(mainA));
            g.drawLine(b.getX(), y, b.getRight(), y, 2.0f);
        };

        const bool rainbow = (preset == 3);
        drawLineWithStyle(yTop, rainbow);
        drawLineWithStyle(yBot, rainbow);
    }
}

LDSJvstAudioProcessorEditor::LDSJvstAudioProcessorEditor(LDSJvstAudioProcessor& p)
    : juce::AudioProcessorEditor(&p), processor(p), oscilloscope(*this, p)
{
    tvImage = juce::ImageCache::getFromMemory(BinaryData::TV_png, BinaryData::TV_pngSize);
    bypassImage = juce::ImageCache::getFromMemory(BinaryData::BYPASS_png, BinaryData::BYPASS_pngSize);
    remoteImage = juce::ImageCache::getFromMemory(BinaryData::remote_control_png, BinaryData::remote_control_pngSize);

    setResizable(true, true);

    resizeConstrainer.setFixedAspectRatio(editorAspectRatio);
    resizeConstrainer.setSizeLimits(320, 240, 1600, 1200);
    setConstrainer(&resizeConstrainer);

    setSize(baseEditorWidth, baseEditorHeight);

    addAndMakeVisible(oscilloscope);
    addAndMakeVisible(bypassHitArea);
    addAndMakeVisible(tvOverlay);
    addAndMakeVisible(remoteOverlay);

    presetLights.ensureStorageAllocated(presetCount);
    for (int i = 0; i < presetCount; ++i)
    {
        auto* light = presetLights.add(new IndicatorLight(*this, i));
        addAndMakeVisible(light);
    }

    // 从宿主恢复的 state 里读取预设选择（如果没有则为默认 0）
    selectedPresetIndex = juce::jlimit(0, presetCount - 1, processor.getDisplayPresetIndex());

    if (resizableCorner != nullptr)
        resizableCorner->toFront(false);

    // 某些宿主在初次打开时不会立刻触发 resized()，这里强制布局一次，确保默认就能看到指示灯
    resized();

    // 让指示灯立刻反映当前预设
    for (auto* light : presetLights)
        if (light != nullptr)
            light->repaint();
}

LDSJvstAudioProcessorEditor::~LDSJvstAudioProcessorEditor() {}

void LDSJvstAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::black);
}

void LDSJvstAudioProcessorEditor::toggleBypassFromUI()
{
    const bool next = ! processor.bypassed;

    bypassTransitionActive = true;
    bypassTransitionToOn = next;
    bypassTransitionStartSeconds = juce::Time::getMillisecondCounterHiRes() * 0.001;

    processor.bypassed = next;

    // 刷新触发区图片与屏幕
    bypassHitArea.repaint();
    oscilloscope.repaint();
}

bool LDSJvstAudioProcessorEditor::isBypassedOrTransitioningToBypass() const noexcept
{
    // 目标是 bypass ON 时，波形不显示；动画开始时也先隐藏波形避免“残影”
    return processor.bypassed || (bypassTransitionActive && bypassTransitionToOn);
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
    processor.addPreGainDb(deltaDb);

    // 触发一次 OSD 显示
    volumeOsdActive = true;
    volumeOsdStartSeconds = juce::Time::getMillisecondCounterHiRes() * 0.001;

    oscilloscope.repaint();
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

void LDSJvstAudioProcessorEditor::setSelectedPresetIndex (int newIndex)
{
    newIndex = juce::jlimit(0, presetCount - 1, newIndex);
    if (selectedPresetIndex == newIndex)
        return;

    // 启动预设切换动画
    presetTransitionActive = true;
    presetTransitionFrom = selectedPresetIndex;
    presetTransitionTo = newIndex;
    presetTransitionStartSeconds = juce::Time::getMillisecondCounterHiRes() * 0.001;

    selectedPresetIndex = newIndex;
    processor.setDisplayPresetIndex(newIndex);

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

    // 让指示灯在最上层显示（覆盖TV.png），同时保留右下角缩放控件
    for (auto* light : presetLights)
        if (light != nullptr)
            light->toFront(false);

    // bypass 覆盖层也要在 TV.png 之上（否则 BYPASS.png 会被 TV 盖住）
    bypassHitArea.toFront(false);

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
            // 频道增加：预设切换到下一个（边界循环）
            else if (std::strcmp (b.name, "频道增加") == 0)
            {
                const int cur = owner.getSelectedPresetIndex();
                const int next = (cur + 1) % presetCount;
                owner.setSelectedPresetIndex(next);
            }
            // 频道减少：预设切换到上一个（边界循环）
            else if (std::strcmp (b.name, "频道减少") == 0)
            {
                const int cur = owner.getSelectedPresetIndex();
                const int prev = (cur + presetCount - 1) % presetCount;
                owner.setSelectedPresetIndex(prev);
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