#include "PluginEditor.h"
#include <JuceHeader.h>
#include "BinaryData.h"

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

    const bool pixelPreset = (owner.getSelectedPresetIndex() == 0);

    g.setColour(juce::Colours::black.withAlpha(pixelPreset ? 0.06f : 0.35f));
    g.fillRoundedRectangle(b, 6.0f);

    g.setColour(juce::Colours::white.withAlpha(0.06f));
    g.drawRoundedRectangle(b.reduced(0.5f), 6.0f, 1.0f);

    if (samples.isEmpty())
        return;

    const float midY = b.getCentreY();
    const float scaleY = b.getHeight() * 0.40f;

    juce::Path waveform;

    const int n = samples.size();
    const float dx = (n > 1 ? (b.getWidth() / (float) (n - 1)) : 0.0f);

    for (int i = 0; i < n; ++i)
    {
        const float x = b.getX() + dx * (float) i;
        const float s = juce::jlimit(-1.0f, 1.0f, samples.getUnchecked(i));
        const float y = midY - s * scaleY;

        if (i == 0)
            waveform.startNewSubPath(x, y);
        else
            waveform.lineTo(x, y);
    }

    if (pixelPreset)
    {
        const int downsample = 5;
        const int lw = juce::jmax(2, (int) (b.getWidth()  / (float) downsample));
        const int lh = juce::jmax(2, (int) (b.getHeight() / (float) downsample));

        juce::Image low (juce::Image::ARGB, lw, lh, true);

        {
            // 生成老电视“雪花”白噪声底图（低分辨率，再放大产生像素感）
            // 注意：同一张 Image 上不能在 Graphics 活跃时同时创建 BitmapData（否则 Debug 下会触发 Direct2D 断言）
            juce::Image::BitmapData bd (low, juce::Image::BitmapData::readWrite);
            juce::Random rng ((int) juce::Time::getMillisecondCounter());

            for (int y = 0; y < lh; ++y)
            {
                auto* line = reinterpret_cast<juce::PixelARGB*> (bd.getLinePointer(y));
                const bool scanline = ((y & 1) == 0);

                for (int x = 0; x < lw; ++x)
                {
                    // 偏亮的“雪花”：大部分是浅灰/白，少量是深色点
                    juce::uint8 v;
                    if (rng.nextFloat() < 0.12f)
                        v = (juce::uint8) rng.nextInt(55);          // 少量黑点
                    else
                        v = (juce::uint8) (180 + rng.nextInt(76));  // 大部分亮点

                    v = (juce::uint8) ((v / 16) * 16); // 量化：更像颗粒感“雪花”

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

        // 波形不要画进 low 图里（那样会先被噪声混合再整体被 TV 叠图压暗），
        // 直接画到目标 Graphics 上，亮度/对比度更可控。
        const auto neonGreen = juce::Colour::fromRGB(0x39, 0xFF, 0x14);

        const float pixelStep = 3.0f; // 像素感步进（可按喜好调大/调小）
        auto qx = [pixelStep, x0 = b.getX()](float x) { return x0 + std::round((x - x0) / pixelStep) * pixelStep; };
        auto qy = [pixelStep, y0 = b.getY()](float y) { return y0 + std::round((y - y0) / pixelStep) * pixelStep; };

        juce::Path pixelWave;
        for (int i = 0; i < n; ++i)
        {
            const float x = qx(b.getX() + dx * (float) i);
            const float s = juce::jlimit(-1.0f, 1.0f, samples.getUnchecked(i));
            const float y = qy(midY - s * scaleY);

            if (i == 0)
                pixelWave.startNewSubPath(x, y);
            else
                pixelWave.lineTo(x, y);
        }

        g.setColour(juce::Colours::black.withAlpha(0.70f));
        g.strokePath(pixelWave, juce::PathStrokeType(4.4f, juce::PathStrokeType::mitered, juce::PathStrokeType::butt));

        g.setColour(neonGreen.withAlpha(1.0f));
        g.strokePath(pixelWave, juce::PathStrokeType(3.0f, juce::PathStrokeType::mitered, juce::PathStrokeType::butt));

        // 重新画一遍中心线/边框，让它压在噪声层之上
        g.setColour(juce::Colours::black.withAlpha(0.18f));
        g.drawLine(b.getX(), midY, b.getRight(), midY, 1.0f);

        g.setColour(juce::Colours::white.withAlpha(0.14f));
        g.drawRoundedRectangle(b.reduced(0.5f), 6.0f, 1.0f);
    }
    else
    {
        g.setColour(juce::Colours::white.withAlpha(0.08f));
        g.drawLine(b.getX(), midY, b.getRight(), midY, 1.0f);

        g.setColour(juce::Colours::lime.withAlpha(0.9f));
        g.strokePath(waveform, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
}

LDSJvstAudioProcessorEditor::LDSJvstAudioProcessorEditor(LDSJvstAudioProcessor& p)
    : juce::AudioProcessorEditor(&p), processor(p), oscilloscope(*this, p)
{
    tvImage = juce::ImageCache::getFromMemory(BinaryData::TV_png, BinaryData::TV_pngSize);

    setResizable(true, true);

    resizeConstrainer.setFixedAspectRatio(editorAspectRatio);
    resizeConstrainer.setSizeLimits(320, 240, 1600, 1200);
    setConstrainer(&resizeConstrainer);

    setSize(baseEditorWidth, baseEditorHeight);

    bypassButton.onClick = [this]() {
        processor.bypassed = !processor.bypassed;
        bypassButton.setButtonText(processor.bypassed ? juce::String("Bypassed") : juce::String("Bypass"));
    };

    addAndMakeVisible(oscilloscope);
    addAndMakeVisible(bypassButton);
    addAndMakeVisible(tvOverlay);

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

void LDSJvstAudioProcessorEditor::setSelectedPresetIndex (int newIndex)
{
    newIndex = juce::jlimit(0, presetCount - 1, newIndex);
    if (selectedPresetIndex == newIndex)
        return;

    selectedPresetIndex = newIndex;

    for (auto* light : presetLights)
        if (light != nullptr)
            light->repaint();

    oscilloscope.repaint();

    // 预设对应的“波形样式逻辑”后续再接入
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

    // 把按钮放到“屏幕”区域内（TV.png屏幕区域透明时，按钮可见且可点）
    auto screenInner = scaledScreen.reduced(10);
    bypassButton.setBounds(screenInner.removeFromTop(30).removeFromRight(120));

    tvOverlay.setBounds(getLocalBounds());
    tvOverlay.toFront(false);

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

    if (resizableCorner != nullptr)
        resizableCorner->toFront(false);
}