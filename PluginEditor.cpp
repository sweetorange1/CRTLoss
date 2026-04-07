#include "PluginEditor.h"
#include <JuceHeader.h>
#include "BinaryData.h"

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
    startTimerHz(30);
}

void LDSJvstAudioProcessorEditor::OscilloscopeComponent::timerCallback()
{
    processor.getOscilloscopeSnapshot(samples);
    repaint();
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

    // 预设背景（每个预设差异要大一些）
    auto drawBackground = [&]()
    {
        switch (preset)
        {
            case 0: // 老电视：噪声底在后面画
            {
                g.fillAll(juce::Colours::black.withAlpha(0.06f));
                break;
            }
            case 1: // 冷色渐变 + 网格
            {
                juce::ColourGradient grad(juce::Colour::fromRGB(0x05, 0x0E, 0x1A), b.getX(), b.getY(),
                                          juce::Colour::fromRGB(0x00, 0x1A, 0x2C), b.getRight(), b.getBottom(), false);
                g.setGradientFill(grad);
                g.fillRect(b);
                break;
            }
            case 2: // CRT 琥珀
            {
                g.fillAll(juce::Colour::fromRGB(0x10, 0x06, 0x00));
                break;
            }
            case 3: // 彩虹：纯黑背景
            {
                g.fillAll(juce::Colours::black.withAlpha(0.30f));
                break;
            }
            case 4: // 点阵：深灰
            {
                g.fillAll(juce::Colours::black.withAlpha(0.40f));
                break;
            }
            case 5: // 填充：深蓝黑
            {
                g.fillAll(juce::Colour::fromRGB(0x03, 0x05, 0x0A));
                break;
            }
            case 6: // 镜像：深绿黑
            {
                g.fillAll(juce::Colour::fromRGB(0x00, 0x08, 0x04));
                break;
            }
            case 7: // 条形码：偏紫黑
            {
                g.fillAll(juce::Colour::fromRGB(0x08, 0x02, 0x10));
                break;
            }
            case 8: // 故障：灰噪轻底
            {
                g.fillAll(juce::Colours::black.withAlpha(0.18f));
                break;
            }
            case 9: // 霓虹紫：暗底
            {
                g.fillAll(juce::Colour::fromRGB(0x05, 0x00, 0x08));
                break;
            }
            case 10: // 极简：几乎全黑
            {
                g.fillAll(juce::Colours::black.withAlpha(0.15f));
                break;
            }
            case 11: // 绿屏：扫描线
            {
                g.fillAll(juce::Colour::fromRGB(0x00, 0x10, 0x08));
                break;
            }
            default:
                g.fillAll(juce::Colours::black.withAlpha(0.35f));
        }
    };

    auto drawGrid = [&](juce::Colour c, int stepPx)
    {
        g.setColour(c);
        for (float x = b.getX(); x <= b.getRight(); x += (float) stepPx)
            g.drawLine(x, b.getY(), x, b.getBottom(), 1.0f);
        for (float y = b.getY(); y <= b.getBottom(); y += (float) stepPx)
            g.drawLine(b.getX(), y, b.getRight(), y, 1.0f);
    };

    auto drawScanlines = [&](juce::Colour c, int stepPx)
    {
        g.setColour(c);
        for (float y = b.getY(); y <= b.getBottom(); y += (float) stepPx)
            g.drawLine(b.getX(), y, b.getRight(), y, 1.0f);
    };

    drawBackground();

    // bypass：不显示中间波形（但仍允许动画覆盖层在下面继续绘制）
    const bool bypassActive = owner.isBypassedOrTransitioningToBypass();

    // 即使 bypass 时也允许画“关机动画”，所以这里不直接 return。
    if (samples.isEmpty() && ! bypassActive)
        return;

    const float midY = b.getCentreY();
    const float scaleY = b.getHeight() * 0.40f;

    juce::Path waveform;

    const int n = samples.size();
    const float dx = (n > 1 ? (b.getWidth() / (float) (n - 1)) : 0.0f);

    auto sampleToPoint = [&](int i)
    {
        const float x = b.getX() + dx * (float) i;
        const float s = juce::jlimit(-1.0f, 1.0f, samples.getUnchecked(i));
        const float y = midY - s * scaleY;
        return juce::Point<float>(x, y);
    };

    if (! bypassActive)
    {
        for (int i = 0; i < n; ++i)
        {
            const auto p = sampleToPoint(i);

            if (i == 0)
                waveform.startNewSubPath(p.x, p.y);
            else
                waveform.lineTo(p.x, p.y);
        }
    }

    // 预设 0：像素电视雪花
    if (preset == 0 && ! bypassActive)
    {
        const int downsample = 5;
        const int lw = juce::jmax(2, (int) (b.getWidth()  / (float) downsample));
        const int lh = juce::jmax(2, (int) (b.getHeight() / (float) downsample));

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

        g.setImageResamplingQuality(juce::Graphics::lowResamplingQuality);
        g.drawImage(low,
                    b.getX(), b.getY(), b.getWidth(), b.getHeight(),
                    0, 0, lw, lh,
                    false);

        const auto neonGreen = juce::Colour::fromRGB(0x39, 0xFF, 0x14);
        const float pixelStep = 3.0f;
        auto qx = [pixelStep, x0 = b.getX()](float x) { return x0 + std::round((x - x0) / pixelStep) * pixelStep; };
        auto qy = [pixelStep, y0 = b.getY()](float y) { return y0 + std::round((y - y0) / pixelStep) * pixelStep; };

        juce::Path pixelWave;
        for (int i = 0; i < n; ++i)
        {
            const auto p = sampleToPoint(i);
            const float x = qx(p.x);
            const float y = qy(p.y);
            if (i == 0) pixelWave.startNewSubPath(x, y);
            else        pixelWave.lineTo(x, y);
        }

        g.setColour(juce::Colours::black.withAlpha(0.70f));
        g.strokePath(pixelWave, juce::PathStrokeType(4.4f, juce::PathStrokeType::mitered, juce::PathStrokeType::butt));

        g.setColour(neonGreen.withAlpha(1.0f));
        g.strokePath(pixelWave, juce::PathStrokeType(3.0f, juce::PathStrokeType::mitered, juce::PathStrokeType::butt));
    }
    else if (! bypassActive)
    {
        switch (preset)
        {
            case 1: // 冷色辉光 + 网格
            {
                drawGrid(juce::Colours::white.withAlpha(0.04f), 30);
                const auto c = juce::Colour::fromRGB(0x3A, 0xE6, 0xFF);

                g.setColour(c.withAlpha(0.18f));
                g.strokePath(waveform, juce::PathStrokeType(6.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

                g.setColour(c.withAlpha(0.95f));
                g.strokePath(waveform, juce::PathStrokeType(2.3f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                break;
            }
            case 2: // 琥珀 CRT：扫描线 + 残影
            {
                drawScanlines(juce::Colours::black.withAlpha(0.14f), 2);
                const auto amber = juce::Colour::fromRGB(0xFF, 0xB0, 0x30);

                juce::Path trail = waveform;
                trail.applyTransform(juce::AffineTransform::translation(0.0f, 1.0f));
                g.setColour(amber.withAlpha(0.14f));
                g.strokePath(trail, juce::PathStrokeType(5.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

                trail = waveform;
                trail.applyTransform(juce::AffineTransform::translation(0.0f, -1.0f));
                g.setColour(amber.withAlpha(0.10f));
                g.strokePath(trail, juce::PathStrokeType(5.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

                g.setColour(amber.withAlpha(1.0f));
                g.strokePath(waveform, juce::PathStrokeType(2.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                break;
            }
            case 3: // 彩虹分段线
            {
                for (int i = 1; i < n; ++i)
                {
                    const float t = (float) i / (float) (n - 1);
                    auto col = juce::Colour::fromHSV(t, 0.85f, 1.0f, 0.95f);
                    g.setColour(col);

                    const auto p0 = sampleToPoint(i - 1);
                    const auto p1 = sampleToPoint(i);
                    g.drawLine(p0.x, p0.y, p1.x, p1.y, 2.2f);
                }
                break;
            }
            case 4: // 点阵采样（只画点）
            {
                const auto dot = juce::Colour::fromRGB(0xFF, 0x3A, 0xB7);
                const int step = juce::jmax(1, n / 140);
                for (int i = 0; i < n; i += step)
                {
                    const auto p = sampleToPoint(i);
                    g.setColour(dot.withAlpha(0.95f));
                    g.fillEllipse(p.x - 2.2f, p.y - 2.2f, 4.4f, 4.4f);
                    g.setColour(juce::Colours::white.withAlpha(0.10f));
                    g.drawEllipse(p.x - 2.2f, p.y - 2.2f, 4.4f, 4.4f, 1.0f);
                }
                break;
            }
            case 5: // 填充面积（青绿渐变）
            {
                juce::Path area = waveform;
                area.lineTo(b.getRight(), midY);
                area.lineTo(b.getX(), midY);
                area.closeSubPath();

                juce::ColourGradient fill(juce::Colour::fromRGB(0x00, 0xFF, 0xC6).withAlpha(0.35f), b.getX(), b.getY(),
                                          juce::Colour::fromRGB(0x00, 0x40, 0xFF).withAlpha(0.05f), b.getX(), b.getBottom(), false);
                g.setGradientFill(fill);
                g.fillPath(area);

                g.setColour(juce::Colour::fromRGB(0x00, 0xFF, 0xC6).withAlpha(0.95f));
                g.strokePath(waveform, juce::PathStrokeType(2.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                break;
            }
            case 6: // 镜像双波形
            {
                drawGrid(juce::Colours::white.withAlpha(0.03f), 40);
                auto c = juce::Colour::fromRGB(0x7C, 0xFF, 0x6B);

                juce::Path mirror = waveform;
                mirror.applyTransform(juce::AffineTransform::scale(1.0f, -1.0f, 0.0f, midY));

                g.setColour(juce::Colours::black.withAlpha(0.55f));
                g.strokePath(mirror, juce::PathStrokeType(3.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

                g.setColour(c.withAlpha(0.95f));
                g.strokePath(waveform, juce::PathStrokeType(2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                g.strokePath(mirror,  juce::PathStrokeType(2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                break;
            }
            case 7: // 条形码：竖线表示幅度
            {
                const auto mag = juce::Colour::fromRGB(0xFF, 0x4D, 0xFF);
                const int step = juce::jmax(1, n / 180);
                for (int i = 0; i < n; i += step)
                {
                    const auto p = sampleToPoint(i);
                    g.setColour(mag.withAlpha(0.75f));
                    g.drawLine(p.x, midY, p.x, p.y, 2.0f);
                }

                g.setColour(mag.withAlpha(0.95f));
                g.strokePath(waveform, juce::PathStrokeType(1.6f, juce::PathStrokeType::mitered, juce::PathStrokeType::butt));
                break;
            }
            case 8: // 故障：轻微抖动的分段线 + 细噪
            {
                juce::Random rng ((int) juce::Time::getMillisecondCounter());
                const auto c = juce::Colour::fromRGB(0xFF, 0x66, 0x33);

                for (int i = 1; i < n; ++i)
                {
                    const auto p0 = sampleToPoint(i - 1);
                    const auto p1 = sampleToPoint(i);
                    const float jx = (rng.nextFloat() - 0.5f) * 1.6f;
                    const float jy = (rng.nextFloat() - 0.5f) * 1.2f;
                    g.setColour(c.withAlpha(0.85f));
                    g.drawLine(p0.x + jx, p0.y + jy, p1.x + jx, p1.y + jy, 2.0f);
                }

                // 叠一层细颗粒
                const int dots = 120;
                g.setColour(juce::Colours::white.withAlpha(0.06f));
                for (int i = 0; i < dots; ++i)
                {
                    const float x = b.getX() + rng.nextFloat() * b.getWidth();
                    const float y = b.getY() + rng.nextFloat() * b.getHeight();
                    g.fillRect(x, y, 1.0f, 1.0f);
                }
                break;
            }
            case 9: // 霓虹紫：阴影 + 主线
            {
                const auto purp = juce::Colour::fromRGB(0xB7, 0x4D, 0xFF);
                juce::Path shadow = waveform;
                shadow.applyTransform(juce::AffineTransform::translation(2.0f, 2.0f));

                g.setColour(juce::Colours::black.withAlpha(0.60f));
                g.strokePath(shadow, juce::PathStrokeType(5.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

                g.setColour(purp.withAlpha(0.20f));
                g.strokePath(waveform, juce::PathStrokeType(7.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

                g.setColour(purp.withAlpha(1.0f));
                g.strokePath(waveform, juce::PathStrokeType(2.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                break;
            }
            case 10: // 极简白线（细、干净）
            {
                g.setColour(juce::Colours::white.withAlpha(0.10f));
                g.drawLine(b.getX(), midY, b.getRight(), midY, 1.0f);

                g.setColour(juce::Colours::white.withAlpha(0.92f));
                g.strokePath(waveform, juce::PathStrokeType(1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                break;
            }
            case 11: // 绿屏：扫描线 + 绿色辉光
            {
                drawScanlines(juce::Colours::black.withAlpha(0.18f), 2);
                const auto green = juce::Colour::fromRGB(0x00, 0xFF, 0x66);

                g.setColour(green.withAlpha(0.15f));
                g.strokePath(waveform, juce::PathStrokeType(8.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

                g.setColour(green.withAlpha(1.0f));
                g.strokePath(waveform, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                break;
            }
            default:
            {
                g.setColour(juce::Colours::white.withAlpha(0.08f));
                g.drawLine(b.getX(), midY, b.getRight(), midY, 1.0f);

                g.setColour(juce::Colours::lime.withAlpha(0.9f));
                g.strokePath(waveform, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                break;
            }
        }
    }

    // 中心线（大多数预设都更像示波器）
    if (! bypassActive && preset != 10)
    {
        g.setColour(juce::Colours::white.withAlpha(0.06f));
        g.drawLine(b.getX(), midY, b.getRight(), midY, 1.0f);
    }

    // TV Glitch：作为所有预设的统一“疯狂”质感叠加层（不改变基础仍是波形）
    auto applyGlitch = [&](const juce::Path& wave, bool hasWave)
    {
        const int ms = (int) juce::Time::getMillisecondCounter();
        const float seconds = (float) (juce::Time::getMillisecondCounterHiRes() * 0.001);

        // 每个 preset 都有自己节奏（周期/爆发点不同）
        const int burstPeriod = 150 + preset * 17;
        const int burstMod = 7 + (preset % 5);
        const bool burst = (((ms / burstPeriod) % burstMod) == (preset % burstMod));

        // 强度：每个 preset 各自偏置 + burst 加成
        float amount = juce::jlimit(0.18f, 1.0f, 0.32f + 0.055f * (float) preset + (burst ? 0.45f : 0.0f));

        juce::Random rng ((int) (ms ^ (preset * 0x9E3779B9)));

        auto rollBar = [&](float speed, juce::Colour c, float alpha)
        {
            const float phase = std::fmod(seconds * speed, 1.0f);
            const float y = b.getY() + phase * b.getHeight();
            const float h = 6.0f + 40.0f * amount;

            juce::ColourGradient grad(c.withAlpha(0.0f), b.getX(), y - h,
                                      c.withAlpha(alpha), b.getX(), y,
                                      false);
            grad.addColour(0.70, c.withAlpha(0.0f));
            g.setGradientFill(grad);
            g.fillRect(b.getX(), y - h, b.getWidth(), h * 2.0f);
        };

        auto tearStrips = [&](int count, float maxDx, bool colorful)
        {
            for (int i = 0; i < count; ++i)
            {
                const float y = b.getY() + rng.nextFloat() * b.getHeight();
                const float h = 1.5f + rng.nextFloat() * (8.0f + 22.0f * amount);
                const float dx = (rng.nextFloat() - 0.5f) * maxDx;

                g.setColour(juce::Colours::black.withAlpha(0.05f * amount));
                g.fillRect(b.getX(), y, b.getWidth(), h);

                if (colorful)
                    g.setColour(juce::Colour::fromHSV(rng.nextFloat(), 0.95f, 1.0f, 0.12f * amount));
                else
                    g.setColour(juce::Colours::white.withAlpha(0.06f * amount));
                g.fillRect(b.getX() + dx, y, b.getWidth(), h);

                g.setColour(juce::Colours::white.withAlpha(0.04f * amount));
                g.drawLine(b.getX(), y, b.getRight(), y, 1.0f);
            }
        };

        auto macroBlocks = [&](int count, float bwMax, float bhMax, bool tinted)
        {
            for (int i = 0; i < count; ++i)
            {
                const float bw = 3.0f + rng.nextFloat() * bwMax;
                const float bh = 2.0f + rng.nextFloat() * bhMax;
                const float x = b.getX() + rng.nextFloat() * (b.getWidth() - bw);
                const float y = b.getY() + rng.nextFloat() * (b.getHeight() - bh);

                const bool dark = (rng.nextFloat() < 0.45f);
                if (dark)
                    g.setColour(juce::Colours::black.withAlpha(0.10f * amount));
                else if (tinted)
                    g.setColour(juce::Colour::fromHSV(rng.nextFloat(), 0.85f, 1.0f, 0.09f * amount));
                else
                    g.setColour(juce::Colours::white.withAlpha(0.07f * amount));

                g.fillRect(x, y, bw, bh);

                if (burst && rng.nextFloat() < 0.25f)
                {
                    g.setColour(juce::Colours::white.withAlpha(0.05f));
                    g.drawRect(juce::Rectangle<float>(x, y, bw, bh), 1.0f);
                }
            }
        };

        auto rgbSplitWave = [&](float off, float alpha)
        {
            if (! hasWave)
                return;

            juce::Path pr = wave; pr.applyTransform(juce::AffineTransform::translation(+off, 0.0f));
            juce::Path pb = wave; pb.applyTransform(juce::AffineTransform::translation(-off, 0.0f));

            g.setColour(juce::Colours::red.withAlpha(alpha));
            g.strokePath(pr, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

            g.setColour(juce::Colours::deepskyblue.withAlpha(alpha));
            g.strokePath(pb, juce::PathStrokeType(3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        };

        auto grain = [&](int dots, juce::Colour c, float a)
        {
            g.setColour(c.withAlpha(a));
            for (int i = 0; i < dots; ++i)
            {
                const float x = b.getX() + rng.nextFloat() * b.getWidth();
                const float y = b.getY() + rng.nextFloat() * b.getHeight();
                g.fillRect(x, y, 1.0f, 1.0f);
            }
        };

        // 每个预设不同的 glitch 配方（尽量差异化）
        switch (preset)
        {
            case 1: // Digital RGB split + fine tears
            {
                rollBar(0.30f, juce::Colour::fromRGB(0x9A, 0xE6, 0xFF), 0.12f * amount);
                tearStrips(2 + (burst ? 6 : 2), 40.0f + 120.0f * amount, true);
                rgbSplitWave(0.8f + 3.8f * amount, 0.09f * amount);
                grain(80 + (int) (220 * amount), juce::Colours::white, 0.03f * amount);
                break;
            }
            case 2: // VHS tracking（底部跟踪线 + 轻微横向波动）
            {
                // tracking 线
                const float y = b.getBottom() - (8.0f + 18.0f * (0.5f + 0.5f * std::sin(seconds * 1.7f)));
                g.setColour(juce::Colours::white.withAlpha(0.12f * amount));
                g.fillRect(b.getX(), y, b.getWidth(), 2.0f + 4.0f * amount);

                // 上下两条“磁带错位”
                tearStrips(1 + (burst ? 7 : 3), 60.0f + 160.0f * amount, false);
                rgbSplitWave(0.5f + 2.0f * amount, 0.06f * amount);
                grain(120 + (int) (260 * amount), juce::Colours::white, 0.02f * amount);
                break;
            }
            case 3: // Rainbow tearing + 彩色条纹
            {
                rollBar(0.18f, juce::Colour::fromHSV(std::fmod(seconds * 0.3f, 1.0f), 1.0f, 1.0f, 1.0f), 0.10f * amount);
                tearStrips(3 + (burst ? 10 : 4), 90.0f + 220.0f * amount, true);
                macroBlocks(8 + (int) (26 * amount), 60.0f + 90.0f * amount, 18.0f + 20.0f * amount, true);
                if (burst) rgbSplitWave(1.5f + 6.0f * amount, 0.10f);
                break;
            }
            case 4: // Pixel dropout（像素/块损坏）
            {
                macroBlocks(18 + (int) (55 * amount), 22.0f + 50.0f * amount, 10.0f + 22.0f * amount, false);
                tearStrips(1 + (burst ? 6 : 1), 30.0f + 90.0f * amount, false);
                if (burst) grain(400 + (int) (600 * amount), juce::Colours::white, 0.03f);
                break;
            }
            case 5: // Bloom smear（大面积发光拖影 + 轻块）
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
                        g.setColour(juce::Colours::white.withAlpha(0.04f * amount));
                        g.strokePath(ghost, juce::PathStrokeType(6.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                    }
                }

                tearStrips(2 + (burst ? 8 : 2), 70.0f + 160.0f * amount, true);
                break;
            }
            case 6: // Interlace jitter（隔行扫描 + 行偏移）
            {
                // 强扫描线
                g.setColour(juce::Colours::black.withAlpha(0.16f * amount));
                for (float y = b.getY(); y <= b.getBottom(); y += 2.0f)
                    g.drawLine(b.getX(), y, b.getRight(), y, 1.0f);

                // 少量粗 tear
                tearStrips(2 + (burst ? 5 : 2), 80.0f + 180.0f * amount, false);
                rgbSplitWave(0.9f + 3.0f * amount, 0.07f * amount);
                break;
            }
            case 7: // Compression macroblocks（大块马赛克）
            {
                macroBlocks(14 + (int) (30 * amount), 140.0f + 180.0f * amount, 70.0f + 60.0f * amount, true);
                if (burst)
                    tearStrips(1 + (int) (5 * amount), 220.0f, true);
                break;
            }
            case 8: // Static storm（雪花风暴 + 随机亮线）
            {
                grain(900 + (int) (1800 * amount), juce::Colours::white, 0.025f + 0.025f * amount);
                for (int i = 0; i < 6 + (int) (14 * amount); ++i)
                {
                    const float y = b.getY() + rng.nextFloat() * b.getHeight();
                    const float x1 = b.getX();
                    const float x2 = b.getRight();
                    g.setColour(juce::Colours::white.withAlpha(0.02f + 0.05f * amount));
                    g.drawLine(x1, y, x2, y, 1.0f);
                }
                if (burst)
                    macroBlocks(10, 80.0f, 20.0f, false);
                break;
            }
            case 9: // CRT mask（彩色点阵/三色条 + 轻 tearing）
            {
                // 模拟三色荧光点：使用“缓存的小 tile + tiled fill”替代逐像素 fillRect，避免在 preset 9 上卡顿
                const int step = (int) juce::jlimit(2.0f, 6.0f, 6.0f - 3.0f * amount);
                auto getCrtTile = [&](int s) -> const juce::Image&
                {
                    static juce::Image tiles[7];
                    s = juce::jlimit(2, 6, s);
                    auto& img = tiles[s];
                    if (! img.isValid())
                    {
                        img = juce::Image(juce::Image::ARGB, s * 3, s, true);
                        juce::Image::BitmapData bd(img, juce::Image::BitmapData::writeOnly);
                        bd.setPixelColour(0, 0, juce::Colours::red);
                        bd.setPixelColour(1, 0, juce::Colours::green);
                        bd.setPixelColour(2, 0, juce::Colours::blue);
                    }
                    return img;
                };

                const auto& tile = getCrtTile(step);
                const int ox = (int) (b.getX() + std::fmod(seconds * (18.0f + 35.0f * amount), (float) (step * 3)));
                const int oy = (int) (b.getY() + std::fmod(seconds * (6.0f + 10.0f * amount), (float) step));
                g.setTiledImageFill(tile, ox, oy, 0.02f * amount);
                g.fillRect(b);

                rollBar(0.16f, juce::Colour::fromRGB(0xB7, 0x4D, 0xFF), 0.08f * amount);
                tearStrips(2 + (burst ? 7 : 2), 90.0f + 180.0f * amount, true);
                rgbSplitWave(0.7f + 3.0f * amount, 0.06f * amount);
                break;
            }
            case 10: // Minimal glitch（偶发，一条 tracking + 轻块）
            {
                if (burst)
                {
                    rollBar(0.20f, juce::Colours::white, 0.10f);
                    tearStrips(2, 120.0f, false);
                    macroBlocks(6, 120.0f, 26.0f, false);
                }
                else
                {
                    tearStrips(1, 50.0f, false);
                }
                break;
            }
            case 11: // Green corruption（绿色字符化/腐蚀块）
            {
                rollBar(0.12f, juce::Colour::fromRGB(0x00, 0xFF, 0x66), 0.10f * amount);
                // 绿色“数据块”
                for (int i = 0; i < 14 + (int) (40 * amount); ++i)
                {
                    const float w = 2.0f + rng.nextFloat() * (18.0f + 30.0f * amount);
                    const float h = 2.0f + rng.nextFloat() * (10.0f + 24.0f * amount);
                    const float x = b.getX() + rng.nextFloat() * (b.getWidth() - w);
                    const float y = b.getY() + rng.nextFloat() * (b.getHeight() - h);
                    const bool bright = (rng.nextFloat() < 0.35f);
                    g.setColour(juce::Colour::fromRGB(0x00, (juce::uint8) (bright ? 255 : 140), 0x66).withAlpha(0.04f + 0.08f * amount));
                    g.fillRect(x, y, w, h);
                }
                tearStrips(2 + (burst ? 8 : 2), 80.0f + 200.0f * amount, false);
                if (burst && hasWave)
                    rgbSplitWave(2.0f + 5.0f * amount, 0.08f);
                break;
            }
            default: // fallback：通用混合
            {
                rollBar(0.22f, juce::Colours::white, 0.10f * amount);
                tearStrips(2 + (int) (5 * amount), 80.0f + 150.0f * amount, true);
                macroBlocks(10 + (int) (30 * amount), 60.0f + 100.0f * amount, 20.0f + 20.0f * amount, true);
                rgbSplitWave(0.8f + 3.0f * amount, 0.06f * amount);
                break;
            }
        }
    };

    // 所有预设统一叠加 glitch（在开关机动画之前绘制，动画会盖住它）
    if (! owner.processor.bypassed)
        applyGlitch(waveform, ! bypassActive);

    // --- 预设切换动画：换台/调谐风格的过渡（不需要缓存前一帧） ---
    const float pt = owner.getPresetTransitionT();
    if (pt > 0.0f)
    {
        // smoothstep easing
        const float e = pt * pt * (3.0f - 2.0f * pt);
        const float inv = 1.0f - e;

        juce::Random rng ((int) juce::Time::getMillisecondCounter() ^ (preset * 0xBADC0DE));

        // 1) 静电噪声爆发（开始/结束更强）
        const float edgeBoost = juce::jlimit(0.0f, 1.0f, (std::abs(pt - 0.5f) * 2.0f));
        const float snowAmount = (0.12f + 0.28f * inv) * (0.35f + 0.65f * edgeBoost);
        g.setColour(juce::Colours::white.withAlpha(snowAmount));
        const int snowDots = 450 + (int) (1200 * inv);
        for (int i = 0; i < snowDots; ++i)
        {
            const float x = b.getX() + rng.nextFloat() * b.getWidth();
            const float y = b.getY() + rng.nextFloat() * b.getHeight();
            g.fillRect(x, y, 1.0f, 1.0f);
        }

        // 2) 亮的“同步线/扫条”从上往下滚
        const float rollY = b.getY() + std::fmod((float) (juce::Time::getMillisecondCounterHiRes() * 0.001 * 0.95), 1.0f) * b.getHeight();
        juce::ColourGradient roll(juce::Colours::white.withAlpha(0.0f), b.getX(), rollY - 40.0f,
                                  juce::Colours::white.withAlpha(0.12f * inv), b.getX(), rollY,
                                  false);
        roll.addColour(0.60, juce::Colours::white.withAlpha(0.0f));
        g.setGradientFill(roll);
        g.fillRect(b.getX(), rollY - 40.0f, b.getWidth(), 80.0f);

        // 3) 换台遮罩：黑条快速擦除
        const float wipe = juce::jlimit(0.0f, 1.0f, (pt < 0.5f ? pt * 2.0f : (1.0f - pt) * 2.0f));
        const float barH = b.getHeight() * (0.10f + 0.35f * wipe);
        g.setColour(juce::Colours::black.withAlpha(0.35f * inv));
        g.fillRect(b.getX(), b.getCentreY() - barH * 0.5f, b.getWidth(), barH);

        // 4) 少量 macroblock（像数字信号切换时的压缩块）
        const int blocks = 6 + (int) (22 * inv);
        for (int i = 0; i < blocks; ++i)
        {
            const float bw = 8.0f + rng.nextFloat() * (70.0f * inv);
            const float bh = 6.0f + rng.nextFloat() * (35.0f * inv);
            const float x = b.getX() + rng.nextFloat() * (b.getWidth() - bw);
            const float y = b.getY() + rng.nextFloat() * (b.getHeight() - bh);
            const bool dark = (rng.nextFloat() < 0.55f);
            g.setColour((dark ? juce::Colours::black : juce::Colours::white).withAlpha(0.10f * inv));
            g.fillRect(x, y, bw, bh);
        }
    }

    // --- 电视开关机动画（覆盖在波形显示区中间） ---
    // 逻辑：
    // - 关机：先压缩成一条水平亮线 -> 再收缩成一个亮点 -> 熄灭
    // - 开机：反向
    const float t = owner.getBypassTransitionT();
    if (t > 0.0f)
    {
        const bool toBypass = owner.bypassTransitionToOn;
        // smoothstep：更像“啪”的感觉，同时避免僵硬线性
        const float e = t * t * (3.0f - 2.0f * t);
        const float k = toBypass ? e : (1.0f - e);

        // 亮度抖动/闪烁
        juce::Random rng ((int) juce::Time::getMillisecondCounter());
        const float flicker = 0.75f + 0.25f * rng.nextFloat();

        // 注意：不再绘制“矩形放大缩小”的画面压缩动画，只保留故障感效果。

        // 扫描闪烁
        g.setColour(juce::Colours::white.withAlpha(0.18f * (1.0f - k) * flicker));
        g.drawLine(b.getX(), b.getCentreY(), b.getRight(), b.getCentreY(), 2.0f + 2.0f * (1.0f - k));

        // 故障撕裂条（动画期间随机出现几条）
        const int strips = 1 + (int) std::round(6.0f * (1.0f - k));
        for (int i = 0; i < strips; ++i)
        {
            const float y = b.getY() + rng.nextFloat() * b.getHeight();
            const float hh = 1.5f + rng.nextFloat() * (10.0f + 22.0f * (1.0f - k));
            const float dx = (rng.nextFloat() - 0.5f) * (40.0f + 220.0f * (1.0f - k));
            g.setColour(juce::Colours::black.withAlpha(0.08f * (1.0f - k)));
            g.fillRect(b.getX(), y, b.getWidth(), hh);
            g.setColour(juce::Colour::fromHSV(rng.nextFloat(), 0.95f, 1.0f, 0.08f * (1.0f - k)));
            g.fillRect(b.getX() + dx, y, b.getWidth(), hh);
        }

        // 短暂白闪（更像电视啪一下）
        if (t < 0.12f)
        {
            const float flash = (1.0f - (t / 0.12f));
            g.setColour(juce::Colours::white.withAlpha(0.10f * flash));
            g.fillRect(b);
        }

        // 动画期间压暗背景（让开关机更明显）
        g.setColour(juce::Colours::black.withAlpha(0.55f * (1.0f - k)));
        g.fillRect(b);
    }

    // 画边框（在 clipState 作用域内是裁剪的，仍然安全）
    g.setColour(juce::Colours::white.withAlpha(0.10f));
    g.drawRoundedRectangle(b.reduced(0.5f), corner, 1.0f);
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

    if (resizableCorner != nullptr)
        resizableCorner->toFront(false);

    // 某些宿主在初次打开时不会立刻触发 resized()，这里强制布局一次，确保默认就能看到指示灯
    resized();
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

    // 2) 遥控器 bypass 按下态（短暂黑色遮罩）
    if (remotePressedButtonIndex >= 0)
    {
        const double dt = now - remotePressedStartSeconds;
        if (dt >= remotePressedDurationSeconds)
        {
            remotePressedButtonIndex = -1;
            needRepaint = true;
        }
        else
        {
            stillNeeded = true;
        }
    }

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

    // 坐标来源：用户给的是“遥控器被拿出来”时的界面坐标。
    // 我们转换成相对遥控器图片左上角的偏移：offset = abs - (remoteX, remoteY-remoteLift)
    static constexpr RemoteButton buttons[] = {
        { "BYPASS", remoteBypassOffsetX, remoteBypassOffsetY, remoteBypassW, remoteBypassH, true },

        // 数字按钮
        { "数字1",  260 - remoteX, 200 - (remoteY - remoteLift), 23, 10, false },
        { "数字2",  305 - remoteX, 200 - (remoteY - remoteLift), 23, 10, false },
        { "数字3",  350 - remoteX, 200 - (remoteY - remoteLift), 23, 10, false },
        { "数字4",  260 - remoteX, 230 - (remoteY - remoteLift), 23, 10, false },
        { "数字5",  305 - remoteX, 230 - (remoteY - remoteLift), 23, 10, false },
        { "数字6",  350 - remoteX, 230 - (remoteY - remoteLift), 23, 10, false },
        { "数字7",  260 - remoteX, 261 - (remoteY - remoteLift), 23, 10, false },
        { "数字8",  305 - remoteX, 261 - (remoteY - remoteLift), 23, 10, false },
        { "数字9",  350 - remoteX, 260 - (remoteY - remoteLift), 23, 10, false },
        { "数字0",  350 - remoteX, 291 - (remoteY - remoteLift), 23, 10, false },

        // 功能键
        { "TV",      261 - remoteX, 297 - (remoteY - remoteLift), 23, 10, false },
        { "SLEEP",   261 - remoteX, 327 - (remoteY - remoteLift), 23, 10, false },
        { "RECALL",  261 - remoteX, 357 - (remoteY - remoteLift), 23, 10, false },
        { "ST/SAP",  261 - remoteX, 387 - (remoteY - remoteLift), 23, 10, false },
        { "MUTE",    261 - remoteX, 416 - (remoteY - remoteLift), 23, 10, false },

        // 音量
        { "VOL+",    305 - remoteX, 327 - (remoteY - remoteLift), 23, 23, false },
        { "VOL-",    305 - remoteX, 371 - (remoteY - remoteLift), 23, 23, false },

        // 频道
        { "频道增加", 350 - remoteX, 327 - (remoteY - remoteLift), 23, 23, false },
        { "频道减少", 350 - remoteX, 371 - (remoteY - remoteLift), 23, 23, false },
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
        g.setColour(juce::Colours::black.withAlpha(0.70f));
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
        { "BYPASS", remoteBypassOffsetX, remoteBypassOffsetY, remoteBypassW, remoteBypassH, true },

        { "数字1",  260 - remoteX, 200 - (remoteY - remoteLift), 23, 10, false },
        { "数字2",  305 - remoteX, 200 - (remoteY - remoteLift), 23, 10, false },
        { "数字3",  350 - remoteX, 200 - (remoteY - remoteLift), 23, 10, false },
        { "数字4",  260 - remoteX, 230 - (remoteY - remoteLift), 23, 10, false },
        { "数字5",  305 - remoteX, 230 - (remoteY - remoteLift), 23, 10, false },
        { "数字6",  350 - remoteX, 230 - (remoteY - remoteLift), 23, 10, false },
        { "数字7",  260 - remoteX, 261 - (remoteY - remoteLift), 23, 10, false },
        { "数字8",  305 - remoteX, 261 - (remoteY - remoteLift), 23, 10, false },
        { "数字9",  350 - remoteX, 260 - (remoteY - remoteLift), 23, 10, false },
        { "数字0",  350 - remoteX, 291 - (remoteY - remoteLift), 23, 10, false },

        { "TV",      261 - remoteX, 297 - (remoteY - remoteLift), 23, 10, false },
        { "SLEEP",   261 - remoteX, 327 - (remoteY - remoteLift), 23, 10, false },
        { "RECALL",  261 - remoteX, 357 - (remoteY - remoteLift), 23, 10, false },
        { "ST/SAP",  261 - remoteX, 387 - (remoteY - remoteLift), 23, 10, false },
        { "MUTE",    261 - remoteX, 416 - (remoteY - remoteLift), 23, 10, false },

        { "VOL+",    305 - remoteX, 327 - (remoteY - remoteLift), 23, 23, false },
        { "VOL-",    305 - remoteX, 371 - (remoteY - remoteLift), 23, 23, false },

        { "未命名按钮1", 350 - remoteX, 327 - (remoteY - remoteLift), 23, 23, false },
        { "未命名按钮2", 350 - remoteX, 371 - (remoteY - remoteLift), 23, 23, false },
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
            remotePressedStartSeconds = juce::Time::getMillisecondCounterHiRes() * 0.001;
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
        { "BYPASS", remoteBypassOffsetX, remoteBypassOffsetY, remoteBypassW, remoteBypassH, true },

        { "数字1",  260 - remoteX, 200 - (remoteY - remoteLift), 23, 10, false },
        { "数字2",  305 - remoteX, 200 - (remoteY - remoteLift), 23, 10, false },
        { "数字3",  350 - remoteX, 200 - (remoteY - remoteLift), 23, 10, false },
        { "数字4",  260 - remoteX, 230 - (remoteY - remoteLift), 23, 10, false },
        { "数字5",  305 - remoteX, 230 - (remoteY - remoteLift), 23, 10, false },
        { "数字6",  350 - remoteX, 230 - (remoteY - remoteLift), 23, 10, false },
        { "数字7",  260 - remoteX, 261 - (remoteY - remoteLift), 23, 10, false },
        { "数字8",  305 - remoteX, 261 - (remoteY - remoteLift), 23, 10, false },
        { "数字9",  350 - remoteX, 260 - (remoteY - remoteLift), 23, 10, false },
        { "数字0",  350 - remoteX, 291 - (remoteY - remoteLift), 23, 10, false },

        { "TV",      261 - remoteX, 297 - (remoteY - remoteLift), 23, 10, false },
        { "SLEEP",   261 - remoteX, 327 - (remoteY - remoteLift), 23, 10, false },
        { "RECALL",  261 - remoteX, 357 - (remoteY - remoteLift), 23, 10, false },
        { "ST/SAP",  261 - remoteX, 387 - (remoteY - remoteLift), 23, 10, false },
        { "MUTE",    261 - remoteX, 416 - (remoteY - remoteLift), 23, 10, false },

        { "VOL+",    305 - remoteX, 327 - (remoteY - remoteLift), 23, 23, false },
        { "VOL-",    305 - remoteX, 371 - (remoteY - remoteLift), 23, 23, false },

        { "未命名按钮1", 350 - remoteX, 327 - (remoteY - remoteLift), 23, 23, false },
        { "未命名按钮2", 350 - remoteX, 371 - (remoteY - remoteLift), 23, 23, false },
    };

    // 遥控器 BYPASS：保持既有逻辑（mouseDown 进入按下态，mouseUp 在按钮内则触发）
    if (owner.remotePulledOut && remotePressedButtonIndex >= 0)
    {
        const int idx = juce::jlimit(0, (int) (std::size(buttons) - 1), remotePressedButtonIndex);
        const auto& b = buttons[idx];
        if (b.isBypass)
        {
            const auto btn = juce::Rectangle<float>(
                x + b.offsetX * scale,
                y + b.offsetY * scale,
                b.w * scale,
                b.h * scale
            );

            if (btn.contains(p))
                owner.toggleBypassFromUI();
            return;
        }
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