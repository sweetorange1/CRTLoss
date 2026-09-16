// ==========================================================
// source/ui/UpdateDialog.cpp
//   CRTloss 风格（CRT 显像管 / 荧光绿）更新提示弹窗实现。
//
//   通过 addToDesktop 创建独立原生小窗口（480×340），
//   setAlwaysOnTop(true) 确保始终置顶。
//   标题栏支持鼠标拖拽移动窗口位置。
//   两个操作按钮：Download / Remind Me Later。
//   按钮操作后通过 CloseDialog() → removeFromDesktop + 异步自删除。
// ==========================================================

#include "UpdateDialog.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace crtloss {
namespace ui {

namespace {

// 荧光绿强调色：与 CRTloss 电视机指示灯 / 屏幕风格保持一致
constexpr auto kAccentColour = 0xFF66FF66;

juce::Colour accent() { return juce::Colour(kAccentColour); }

// CRT 扫描线：每 3px 一条极淡的暗线，模拟显像管行扫描
void drawScanlines(juce::Graphics& g, juce::Rectangle<int> area) {
  g.setColour(juce::Colours::black.withAlpha(0.18f));
  for (int y = area.getY(); y < area.getBottom(); y += 3) {
    g.fillRect(area.getX(), y, area.getWidth(), 1);
  }
}

}  // namespace

// ==========================================================
// 构造
// ==========================================================
UpdateDialog::UpdateDialog(const crtloss::network::UpdateInfo& info,
                           std::function<void()> onClose)
    : info_(info), onClose_(std::move(onClose)) {
  setOpaque(true);
  setInterceptsMouseClicks(true, true);
}

// ==========================================================
// paint：CRT 深色背景 + 扫描线 + 标题栏 + 内容区 + 按钮行
// ==========================================================
void UpdateDialog::paint(juce::Graphics& g) {
  const auto dlg = getLocalBounds();

  // 深色渐变背景（电视机外壳）
  juce::ColourGradient bg(juce::Colour(0xFF1A1D1A), 0.0f, 0.0f,
                          juce::Colour(0xFF0C0E0C), 0.0f,
                          static_cast<float>(dlg.getBottom()), false);
  g.setGradientFill(bg);
  g.fillAll();

  // 外边框（荧光绿微光）
  g.setColour(accent().withAlpha(0.18f));
  g.drawRect(dlg.toFloat(), 1.0f);

  // 标题栏
  auto titleRow = dlg.reduced(1).removeFromTop(kTitleBarH);
  g.setColour(juce::Colours::black.withAlpha(0.45f));
  g.fillRect(titleRow);

  g.setColour(accent().withAlpha(0.90f));
  g.setFont(juce::Font(14.0f, juce::Font::bold));
  g.drawText("Update Available", titleRow.reduced(10, 0),
             juce::Justification::centredLeft, false);

  g.setColour(accent().withAlpha(0.20f));
  g.fillRect(titleRow.getX(), titleRow.getBottom(), titleRow.getWidth(), 1);

  // 内容区
  auto body = dlg.reduced(16);
  body.removeFromTop(kTitleBarH + 10);

  // 左侧状态图标
  auto iconCol = body.removeFromLeft(52);
  auto iconBox = juce::Rectangle<int>(iconCol.getX(), iconCol.getY(), 40, 40);
  if (info_.force_update) {
    g.setColour(accent().withAlpha(0.16f));
    g.fillRoundedRectangle(iconBox.toFloat(), 6.0f);
    g.setColour(accent());
    g.setFont(juce::Font(22.0f, juce::Font::bold));
    g.drawText("!!", iconBox, juce::Justification::centred, false);
  } else {
    g.setColour(accent().withAlpha(0.10f));
    g.fillRoundedRectangle(iconBox.toFloat(), 6.0f);
    g.setColour(accent().withAlpha(0.85f));
    g.setFont(juce::Font(22.0f, juce::Font::bold));
    g.drawText("i", iconBox, juce::Justification::centred, false);
  }

  // 第一行：有新版本提示
  g.setColour(juce::Colours::white.withAlpha(0.92f));
  g.setFont(juce::Font(14.0f, juce::Font::bold));
  g.drawText("A new version of CRTloss is available!",
             body.getX(), body.getY(), body.getWidth(), 20,
             juce::Justification::centredLeft, false);

  // 第二行：最新版本
  auto verRow = body.withY(body.getY() + 24).withHeight(18);
  g.setColour(accent().withAlpha(0.90f));
  g.setFont(juce::Font(12.0f));
  g.drawText("Latest: " + info_.latest_version, verRow,
             juce::Justification::centredLeft, false);

  // 更新日志区域
  if (info_.changelog.isNotEmpty()) {
    auto changeArea = juce::Rectangle<int>(
        body.getX(), verRow.getBottom() + 8, body.getWidth(), 120);

    g.setColour(juce::Colours::white.withAlpha(0.55f));
    g.setFont(juce::Font(11.0f, juce::Font::bold));
    g.drawText("What's new:", changeArea.getX(), changeArea.getY(),
               changeArea.getWidth(), 16,
               juce::Justification::centredLeft, false);

    auto logArea = changeArea.withY(changeArea.getY() + 16)
                             .withHeight(changeArea.getHeight() - 16);
    g.setColour(juce::Colours::white.withAlpha(0.06f));
    g.fillRoundedRectangle(logArea.toFloat(), 4.0f);

    g.setColour(juce::Colours::white.withAlpha(0.70f));
    g.setFont(juce::Font(11.0f));
    g.drawFittedText(info_.changelog, logArea.reduced(8, 6),
                     juce::Justification::topLeft, 6);
  }

  // 按钮行
  const int numBtns = HasRemindButton() ? 2 : 1;
  for (int i = 0; i < numBtns; ++i) {
    const auto btnRect = GetButtonBounds(i);
    const bool isDownload = (i == static_cast<int>(ButtonId::kDownload));

    juce::Colour fill = juce::Colours::black.withAlpha(0.55f);
    if (pressedBtn_ == i) {
      fill = accent().withAlpha(0.45f);
    } else if (hoveredBtn_ == i) {
      fill = accent().withAlpha(0.16f);
    }

    g.setColour(fill);
    g.fillRoundedRectangle(btnRect.toFloat(), 4.0f);

    g.setColour(isDownload ? accent() : juce::Colours::white.withAlpha(0.80f));
    g.drawRoundedRectangle(btnRect.toFloat(), 4.0f, 1.0f);

    g.setColour(isDownload ? accent()
                           : juce::Colours::white.withAlpha(0.85f));
    g.setFont(juce::Font(13.0f, juce::Font::bold));
    g.drawText(isDownload ? "Download" : "Remind Me Later",
               btnRect, juce::Justification::centred, false);
  }

  // 扫描线覆盖在最上层（不干扰按钮文字可读性：透明度很低）
  drawScanlines(g, dlg.reduced(1));
}

// ==========================================================
// 鼠标事件
// ==========================================================
void UpdateDialog::mouseDown(const juce::MouseEvent& e) {
  const int hit = HitTestButton(e.getPosition());
  if (hit >= 0) {
    pressedBtn_ = hit;
    repaint();
    return;
  }

  // 标题栏区域：开始拖拽
  if (GetTitleBarBounds().contains(e.getPosition())) {
    isDragging_ = true;
    dragOffset_ = e.getPosition();
  }
}

void UpdateDialog::mouseDrag(const juce::MouseEvent& e) {
  if (!isDragging_) return;

  const auto delta = e.getPosition() - dragOffset_;
  auto newBounds = getScreenBounds();
  newBounds.translate(delta.x, delta.y);
  setBounds(newBounds);
}

void UpdateDialog::mouseMove(const juce::MouseEvent& e) {
  if (isDragging_) return;

  const int hit = HitTestButton(e.getPosition());
  if (hit != hoveredBtn_) {
    hoveredBtn_ = hit;
    repaint();
  }
}

void UpdateDialog::mouseUp(const juce::MouseEvent& e) {
  if (isDragging_) {
    isDragging_ = false;
    return;
  }

  if (pressedBtn_ >= 0) {
    const int hit = HitTestButton(e.getPosition());
    const int triggered = (hit == pressedBtn_) ? pressedBtn_ : -1;
    pressedBtn_ = -1;
    repaint();

    if (triggered >= 0) {
      ExecuteButton(triggered);
    }
  }
}

// ==========================================================
// 布局计算
// ==========================================================
juce::Rectangle<int> UpdateDialog::GetTitleBarBounds() const {
  return getLocalBounds().reduced(1).removeFromTop(kTitleBarH);
}

juce::Rectangle<int> UpdateDialog::GetButtonBounds(int idx) const {
  const auto dlg = getLocalBounds();
  const int numBtns = HasRemindButton() ? 2 : 1;
  const int totalBtnW = kBtnW * numBtns + kBtnGap * (numBtns - 1);
  const int startX = dlg.getX() + (dlg.getWidth() - totalBtnW) / 2;
  const int btnY = dlg.getBottom() - 42;

  return juce::Rectangle<int>(startX + idx * (kBtnW + kBtnGap), btnY,
                              kBtnW, kBtnH);
}

int UpdateDialog::HitTestButton(juce::Point<int> pos) const {
  const int numBtns = HasRemindButton() ? 2 : 1;
  for (int i = 0; i < numBtns; ++i) {
    if (GetButtonBounds(i).contains(pos)) {
      return i;
    }
  }
  return -1;
}

bool UpdateDialog::HasRemindButton() const {
  return !info_.force_update;
}

void UpdateDialog::ExecuteButton(int idx) {
  switch (static_cast<ButtonId>(idx)) {
    case ButtonId::kDownload: {
      if (info_.download_url.isNotEmpty()) {
        juce::URL(info_.download_url).launchInDefaultBrowser();
      } else {
        juce::URL("https://iisaacbeats.cn").launchInDefaultBrowser();
      }
      break;
    }
    case ButtonId::kRemind: {
      break;
    }
  }

  CloseDialog();
}

// ==========================================================
// CloseDialog：隐藏、回调、从桌面移除并异步自删除
// ==========================================================
void UpdateDialog::CloseDialog() {
  setVisible(false);
  if (onClose_) {
    onClose_();
  }
  juce::MessageManager::callAsync([self = this] {
    self->removeFromDesktop();
    delete self;
  });
}

// ==========================================================
// ShowInComponent（静态工厂）
// ==========================================================
UpdateDialog* UpdateDialog::ShowInComponent(
    const crtloss::network::UpdateInfo& info,
    std::function<void()> onClose) {
  auto* dlg = new UpdateDialog(info, std::move(onClose));

  dlg->addToDesktop(juce::ComponentPeer::windowIsTemporary);
  dlg->setAlwaysOnTop(true);
  dlg->toFront(true);

  // 居中于主显示器屏幕（userArea 排除任务栏等系统区域）
  const auto& displays = juce::Desktop::getInstance().getDisplays();
  const auto screenArea = displays.getPrimaryDisplay() != nullptr
      ? displays.getPrimaryDisplay()->userArea
      : juce::Rectangle<int>(0, 0, 1280, 720);
  const int x = screenArea.getCentreX() - kDlgW / 2;
  const int y = screenArea.getCentreY() - kDlgH / 2 - 20;
  dlg->setBounds(x, y, kDlgW, kDlgH);
  dlg->setVisible(true);

  return dlg;
}

}  // namespace ui
}  // namespace crtloss
