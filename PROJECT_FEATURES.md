# CRTloss (LDSJvst) — 项目功能与交互逻辑索引

> 本文档面向后续 AI 辅助的 vibe-coding 工作流，梳理插件的**每一个按钮 / 每一个可交互元素**的**位置、语义、映射到的处理器方法、以及副作用（如 OSD、动画等）**。请将本文档视为“唯一事实源”，任何交互调整前先对照本文件。

- **产品名**：CRTloss
- **内部名 / 目标**：`LDSJvst`
- **公司**：iisaacbeats.cn
- **JUCE 版本**：8.0.12
- **插件格式**：VST3 + Standalone
- **CMake 版本号**（`CMakeLists.txt` 中 `juce_add_plugin` 的 `VERSION`）：`1.1.9`
- **UI 版本号**（`PluginEditor.cpp` 中 `kPluginUiVersionText`）：`v1.4.0`
- **文档版本**：`0.0.4`

---

## 0. 项目定位

一个模拟老式 CRT 电视机外观的丢频（band-loss / band-notch）音频效果器：把 20Hz–20kHz 划分为 **100 段**，按当前"频道预设"的概率分布 / 序列规则周期性地对若干频段施加陷波（Notch），配合前置增益、限制器、高低切等模块，实现"频段随机丢失"的音频退化效果。UI 以一台"电视 + 遥控器"为隐喻——所有参数调节都通过点击电视面板或者拉出遥控器来完成。

---

## 1. 项目文件结构（核心）

| 文件 | 作用 |
| --- | --- |
| [`CMakeLists.txt`](CMakeLists.txt) | JUCE 插件构建脚本，定义产品名、版本、格式、资源 |
| [`PluginProcessor.h`](PluginProcessor.h) / [`PluginProcessor.cpp`](PluginProcessor.cpp) | 音频处理器：所有 DSP 参数、状态、`processBlock` 主链路 |
| [`PluginEditor.h`](PluginEditor.h) / [`PluginEditor.cpp`](PluginEditor.cpp) | UI 编辑器：TV 面板、遥控器、示波器、指示灯、OSD 等 |
| [`display_present.h`](display_present.h) | 12 个固定频道预设 + 12..9999 衍生预设算法、丢频概率模型 |
| `assets/TV.png` | 电视机外壳贴图 |
| `assets/BYPASS.png` | Bypass 状态叠加图 |
| `assets/remote_control.png` | 遥控器贴图（收起 / 拉出状态） |
| `assets/remote_control_light.png` | 遥控器指示灯发光贴图 |
| `grid_plugin/` | 子模块（外部依赖，`grid::grid_plugin`） |
| `build_installer.bat` / `CRTloss_installer.iss` | Windows 安装包构建脚本 |

---

## 2. DSP 处理链（`processBlock` 顺序）

```
输入 → [Hard Bypass 直通] → 1) 前置增益 (PreGain, dB)
        → 2) 限制器 (Limiter, 硬钳)
        → 3) 高低切 (Cut) —— HardMask 或 HPF/LPF (12/24/48 dB/oct，带 crossfade)
        → 4) 频段丢失 (Loss)：100 段 Notch，wet 平滑 10ms
        → 5) 最终 0dB 硬削波
        → 示波器采样
```

所有参数在 [PluginProcessor.h](PluginProcessor.h) 内以 `std::atomic<>` 存储，getter/setter 都做了 `juce::jlimit` 钳位。

### 2.1 关键参数与常量

| 参数 | 常量 / 范围 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `bypassed` | bool | false | 硬直通开关（`processBlock` 首行判断） |
| `preGainDb` | -5..+24 dB | +10 dB | 前置增益，遥控/面板 VOL± 每次 ±1dB |
| `randomRetriggerPerBeat` | 0.25..16 | 4 | 随机预设的每拍重触发次数 |
| `limiterThreshold` | 0..1 | 1.0 | 硬钳阈值（示波器上下线拖拽） |
| `lossNotchQ` | 0.1..30 | 6.0 | Loss 的 Notch Q（Legacy 模式全段共用） |
| `lossAlgorithmMode` | 0=Legacy / 1=UniformBandwidth / 2=FFT-Mask | 0 | Loss 陷波算法（遥控 `ST/SAP` 三态循环切换）；FFT-Mask 使用 STFT+OLA 直接把命中频段在频域置零 |
| `lossMaskFrozen` | bool | false | 冻结当前 lossMask，不再重触发（遥控 `SLEEP`） |
| `lossMaskInverted` | bool | false | 保留/丢失掩码反转（遥控 `MUTE`） |
| `lowCutHz` / `highCutHz` | 20..20000 Hz | 20 / 20000 | 高低切频率（示波器底部三角拖拽） |
| `cutMode` | 0=HardMask / 1=HPF-LPF | 0 | 高低切模式（遥控 `TV` 循环切换） |
| `cutSlopeDbPerOct` | 12 / 24 / 48 | 12 | HPF/LPF 斜率（遥控 `TV` 循环切换） |
| `displayPresetIndex` | 0..9999 (kDerivedChannelMax) | 0 | 当前"频道号" |
| `kLossBandCount` | 100 | — | 频段数（编辑器侧 `kLossBandCountForUI`=100） |
| `kLossMaskSmoothingTimeSeconds` | 0.025 | — | wet 系数指数平滑时间（10ms → 25ms） |
| `kCutCrossfadeSamples` | 1024 | — | 高低切切换时的交叉淡化样本数 |

### 2.2 Loss 三种算法

遥控器 `ST/SAP` 按钮按下 → 循环切换：`Legacy → UniformBandwidth → FFT-Mask → Legacy`。三种算法均以 100 段 wet 曲线（10ms 时间常数平滑）为共同输入。

| 模式 | 常量 | 实现方式 | 特点 |
| --- | --- | --- | --- |
| Legacy | `kLossAlgorithmLegacy = 0` | 100 个 `IIRFilter` Notch 串联，Q 全段共用 `lossNotchQ` | 计算最轻；低段带宽偏窄、高段偏宽；重触发时对"新变为丢弃"的 band `reset()` 以避免干路长期激励累积的内部状态尾巴 |
| Uniform Bandwidth | `kLossAlgorithmUniformBandwidth = 1` | 每段 Notch 按频段边界推导 Q，令对数频谱上带宽等宽 | 全频带感受一致的"切除宽度"；与 Legacy 共用同一套 Notch 数组，重触发防病策略相同 |
| FFT-Mask | `kLossAlgorithmFftMask = 2` | STFT（N=2048, hop=512, 75% overlap, Hann 窗）+ 逐 bin 乘目标增益 `g(k)=1−wet(band(k))` + IFFT + OLA（∑w² 归一化） | 频域直接"抠掉"命中频段，相位保留；额外引入 `N−hop=1536` 采样延迟（DAW 会自动补偿） |

**FFT-Mask 关键实现要点**：
- bin→band 采用与 `getLossBandCenterHz` 相反的对数映射：`t = log(hz/20) / log(1000)`, `band = round(t·99)`。
- 逐 bin 一阶 IIR 平滑（时间常数 10ms，与 `lossBandWet` 一致），避免帧间跳变造成的宽带调制感。
- 干路用长度 `N` 的环形缓冲延迟 `N−hop` 采样与 wet 对齐；预热期（前 4 帧）从延迟 dry 兜底输出。
- 切换算法瞬间调用 `resetStftState()` 清空所有 STFT 环形缓冲和平滑增益；同时通过 `setLatencySamples()` 上报/清零延迟。

左上模式 OSD 会显示当前算法：`ST/SAP: LEGACY Q` / `ST/SAP: UNIFORM BW` / `ST/SAP: FFT MASK`。

### 2.3 频段丢失重触发策略

由 `retriggerLossMask` 驱动，取宿主 BPM（无则 120）：

- **BPM 序列型预设**（`LossSequenceParams.enabled == true`）：按 anchor + halfWindow 生成一个"沿频段游走的窗口"掩码，`retriggerSeconds = beatSec * bpmDivision`。
- **随机型预设**：按 `LossDistributionParams` 计算每个频段的"保留概率"，随机采样。`retriggerSeconds = beatSec / (perBeat * perPresetScale)`。

高低切区域外的频段会强制置 0；若掩码全空则兜底保留一个最接近 lowCut 的频段。

### 2.4 频道预设（`display_present::kPresets`）

固定预设 12 个（频道 `0..11`），衍生预设由 `derivePresetParamsFromChannelId(cid)` 根据 `cid` 做确定性随机，覆盖：背景样式、扫描线扭曲、丢频分布模型、序列参数、每预设的默认高低切。频道 `12..9999` 均为衍生。

---

## 3. 状态持久化

`getStateInformation` / `setStateInformation` 通过 `juce::ValueTree("LDSJvstState")` 保存以下字段（XML 二进制）：

`version, preset, bypassed, preGainDb, randomRetriggerPerBeat, limiterThreshold, lossNotchQ, lossAlgorithmMode, lossMaskInverted, lowCutHz, highCutHz, cutMode, cutSlopeDbPerOct`

> ⚠️ 目前 **`lossMaskFrozen` 不参与持久化**（默认加载后为 false）。若后续要保存 Sleep 状态需要在此处同步添加。

---

## 4. 编辑器窗口

- **基准分辨率**：1000 × 750，等比缩放，使用 `ComponentBoundsConstrainer` 限制比例。
- **屏幕区域**：左上 (130,135)，大小 606×466（`screenX/Y/W/H`）。
- **组件层级**：底部 TV 图 → OscilloscopeComponent（屏幕内容/波形/OSD/频段灯带/高低切三角）→ IndicatorLight × 12 → BypassHitArea → TvOverlayComponent（TV 面板按钮层）→ RemoteControlOverlay（遥控器层，最上面）。
- 右下角有版本水印，可点击（详见 5.7）。

---

## 5. 交互清单（按可点击元素）

> 命名约定：**主界面** = 电视机上直接可见的按钮；**遥控器** = 拉出遥控器后可见的按钮；**屏幕内** = OscilloscopeComponent 屏幕区域内的拖拽热区。

### 5.1 主界面 · TV 面板按钮（`TvOverlayComponent`）

面板按钮是**四边形热区**（不是矩形），坐标在 `kTvPanelButtons`：

| 编号 | 名称 | 四点坐标（基准） | 功能 | 处理器调用 |
| --- | --- | --- | --- | --- |
| 0 | `VolUp`（VOL+） | (910,273)-(947,273)-(947,298)-(904,298) | 前置增益 +1 dB，按住触发**自动连发** | `nudgePreGainDbFromUI(+1)` |
| 1 | `VolDown`（VOL-） | (865,273)-(909,273)-(903,298)-(865,298) | 前置增益 -1 dB，按住连发 | `nudgePreGainDbFromUI(-1)` |
| 2 | `ChannelUp`（CH+） | (912,149)-(948,149)-(948,175)-(905,175) | 频道号 +1，边界循环 0..9999 | `setSelectedPresetIndex(cur+1)` |
| 3 | `ChannelDown`（CH-） | (866,149)-(911,149)-(904,175)-(866,175) | 频道号 -1，边界循环 0..9999 | `setSelectedPresetIndex(cur-1)` |

VOL 连发参数：`volumeRepeatInitialDelaySeconds=0.32`，`volumeRepeatIntervalSeconds=0.075`，60Hz 定时器。

> 注意：只有当**遥控器完全收回**（`remotePulledOut==false` 且 `remotePullAmount<=0.001`）时，TV 面板按钮才会命中；否则被遥控器 overlay 屏蔽。

### 5.2 主界面 · 12 个预设指示灯（`IndicatorLight`）

12 个圆点，位于 (829,157) 起，尺寸 17×17，间隔 10px。

| 元素 | 功能 | 处理器调用 |
| --- | --- | --- |
| 灯 0..11 | 点击 → 直接切换到对应固定频道 | `setSelectedPresetIndex(index)` |
| Bypass 开启时 | 所有灯变黑色半透明（不再高亮），仍可点亮切换 | — |
| 当前 `selectedChannelId % 12 == index` | 高亮为亮绿色 | — |

### 5.3 主界面 · Bypass 触发区（`BypassHitArea`）

- 位置：(857,619) 大小 93×53。
- 平时透明；`bypassed==true` 时叠加 `BYPASS.png`。
- **单击** 触发 `toggleBypassFromUI()`，随后启动电视开关机风格过渡动画（时长 0.22s）。

### 5.4 屏幕内 · 示波器 & 拖拽（`OscilloscopeComponent`）

屏幕区域承担多种交互：

1. **限制器上下线拖拽**：
   - 点击屏幕内任意"非频段灯带 / 非高低切三角"位置，将立即根据 `|y - midY| / (h*0.4)` 计算阈值并 `setLimiterThreshold(th)`。
   - 拖动过程中持续更新。松开释放 (`mouseUp`) 清 `limiterDragActive`。
   - 上下两条水平线是对称显示的限制器阈值。

2. **高低切三角手柄**（屏幕下方频段灯带的两侧三角）：
   - 底部有一排 100 格的频段灯带（LOG 频率映射，20Hz..20kHz）。
   - `lowCut` / `highCut` 位置各有一个可拖拽三角（`triW ≈ cellW*2.3, triH ≈ cellH*2.2`）。
   - 点中低切三角 → `cutHandleDragMode=1`，拖动更新 `setLowCutHz(xToHz)`；高切三角 → `cutHandleDragMode=2`，更新 `setHighCutHz`。
   - 若点在灯带整条带上而不在任何三角上，则按"离哪个三角更近"就抓哪个。
   - 拖拽期间 `setCutDragActive(true)`，避免频繁触发 HPF/LPF crossfade 抗锯齿。

3. **波形显示**：`processBlock` 末尾采样 → 屏幕内绘制波形（预设 3/8/10 有 trail 拖影效果）。

4. **OSD 叠加**（不同层）：
   - 频道号 OSD（右上）：切换频道 3 秒 / 遥控数字输入 1.2 秒。
   - 模式 OSD（左上）：TV / ST-SAP / MUTE 触发 3 秒。
   - 音量 OSD：VOL± 触发 3 秒。
   - 由 `getVolumeOsdT / getModeOsdT / getChannelOsdText` 驱动淡入淡出。

5. **背景 & 扫描行扭曲**：按当前频道的 `BackgroundParams` / `ScanlineWarpParams` 生成，不涉及交互。

### 5.5 遥控器（`RemoteControlOverlay`）

- 收回状态：遥控器上半部分露在屏幕下方（`remoteY=684`）；点击遥控器区域 → 拉出。
- 拉出状态：遥控器向上平移 `remoteLift=415` 像素，遮盖主界面，作为 modal overlay 拦截所有点击；点击**遥控器外部**任意区域 → 收回。
- 拉出/收回动画：0.18s。
- 拉出后遥控器灯图（`remote_control_light.png`）叠加在 (789,290,29,29)。

按钮列表（坐标是相对遥控器图片左上角的偏移，触发热区在视觉热区基础上放大 1.5x）：

| 名称 | offset (x,y,w,h) | 功能 | 处理器/编辑器调用 |
| --- | --- | --- | --- |
| `BYPASS` | (73,287, 68,10) | 切换 Bypass | `toggleBypassFromUI()` |
| `数字0..9` | 3×3+2 网格 (~28/73/118, 71~162) | 频道数字输入，累计最多 4 位，3s 无输入自动提交 | `pushChannelDigitFromRemote(digit)` |
| `TV` | (29,168, 23,10) | 循环切换 CutMode / Slope（Hard→HPF12→HPF24→HPF48→Hard...） | `cycleCutModeOrSlopeFromTV()` → 触发模式 OSD |
| `SLEEP` | (29,198, 23,10) | 冻结/恢复 lossMask 变化 | `toggleSleepFreezeFromUI()` |
| `RECALL` | (29,228, 23,10) | 回到上一个频道 | `recallPreviousChannelFromUI()` |
| `ST/SAP` | (29,258, 23,10) | 循环切换 Loss 算法（Legacy → UniformBW → FFT-Mask → Legacy） | `toggleLossAlgorithmFromUI()` → 触发模式 OSD（LEGACY Q / UNIFORM BW / FFT MASK） |
| `MUTE` | (29,287, 23,10) | 反转 lossMask（保留/丢失互换） | `toggleLossMaskInvertFromUI()` → 触发模式 OSD |
| `VOL+` | (73,198, 23,23) | 前置增益 +1 dB，按住连发 | `nudgePreGainDbFromUI(+1)` |
| `VOL-` | (73,242, 23,23) | 前置增益 -1 dB，按住连发 | `nudgePreGainDbFromUI(-1)` |
| `频道增加` | (118,198, 23,23) | 频道号 +1（0..9999 循环） | `setSelectedPresetIndex(cur+1)` |
| `频道减少` | (118,242, 23,23) | 频道号 -1（0..9999 循环） | `setSelectedPresetIndex(cur-1)` |

**交互特性**：
- `mouseDown` 只进入"按下态"（叠加半透明黑）。
- `mouseUp` 且指针仍在同一按钮内 → 触发（release-to-trigger）。
- **VOL± 特例**：`mouseDown` 立即触发一次并进入连发；`mouseUp` 只结束连发，不额外触发。
- **数字输入**：不足 4 位时会累积并显示（右对齐 `-` 填充），4 位或 3s 空闲后 `triggerChannelJumpNow()`；若输入值 > 9999 则仅显示不切换。

### 5.6 频道切换动画

`setSelectedPresetIndex(newChannelId)` 会：
1. 记录 `previousChannelId ← selectedChannelId`（供 RECALL 使用）。
2. 计算灯号维度 `newVisualIndex = newChannelId % 12`。
3. 启动预设切换动画 `presetTransitionActive`（0.26s）。
4. 更新 `processor.setDisplayPresetIndex(newChannelId)`。
5. 触发频道号 OSD（3s）并重绘。

### 5.7 版本水印（右下角）

- 显示文本 `kPluginUiVersionText`（当前 `v1.4.0`）。
- 悬停时下划线 + 手型指针。
- 单击 → `juce::URL("https://iisaacbeats.cn").launchInDefaultBrowser()`。

### 5.8 临时 Notch Q 输入框（默认关闭）

`kEnableTempQInput = false`，代码保留但**不显示**。可用于开发期临时调 `lossNotchQ`。

---

## 6. 快速索引：功能 → 触发方式

| 想做什么 | 从哪个 UI 元素触发 |
| --- | --- |
| 切频道 | 遥控器数字键 / 遥控器 CH± / TV 面板 CH± / 12 个预设灯 |
| 调整前置增益 | 遥控器 VOL± / TV 面板 VOL± |
| 调整限制器 | 屏幕内拖拽（Y 方向） |
| 调整高低切频率 | 屏幕底部灯带的三角手柄 |
| 切换高低切算法/斜率 | 遥控器 `TV` |
| 切换 Loss 算法 | 遥控器 `ST/SAP`（三态：Legacy Q / Uniform BW / FFT Mask） |
| 冻结 Loss | 遥控器 `SLEEP` |
| 反转 Loss 掩码 | 遥控器 `MUTE` |
| Bypass | 主界面 Bypass 触发区 / 遥控器 `BYPASS` |
| 回到上一频道 | 遥控器 `RECALL` |
| 拉出/收回遥控器 | 点击遥控器 / 点击遥控器外部 |
| 打开官网 | 点击右下角版本号 |

---

## 7. 版本历史 & 踩坑记录

> **协作约定**：
> 1. 每次功能开发后，用户会测试。当用户回复"这个功能没问题了"时，将本次开发的功能追加到下方"变更日志"，把文档版本号 +0.0.1，并把代码 push 到 git 主干（`main` / `master`）。
> 2. 每次总结变更日志时，**必须同步记录本次开发中遇到的问题、踩过的坑、临时约束或权衡**（可以是 DSP 层的、可以是 UI 层的、可以是 JUCE 使用陷阱），归入下方"踩坑记录"表。

### 7.1 变更日志

| 日期 | 文档版本 | CMake VERSION | UI 版本 | 变更摘要 |
| --- | --- | --- | --- | --- |
| 2026-07-09 | 0.0.1 | 1.1.6 | v1.3.0 | 建立首版功能与交互索引文档 |
| 2026-07-09 | 0.0.2 | 1.1.7 | v1.3.1 | 新增第 3 种 Loss 算法 **FFT-Mask**（`kLossAlgorithmFftMask=2`）：STFT+Hann+75% overlap+OLA，逐 bin 乘 `g = 1 − wet(band(k))` 保留相位；`ST/SAP` 遥控按钮改为三态循环（Legacy → UniformBW → FFT-Mask → Legacy），左上模式 OSD 支持 `ST/SAP: LEGACY Q / UNIFORM BW / FFT MASK` 三种文案；FFT 模式下 `setLatencySamples(N−hop)=1536`，切回 IIR 模式清 0 |
| 2026-07-09 | 0.0.3 | 1.1.8 | v1.3.1 | 修复 **Legacy / UniformBandwidth 两种 IIR 丢频算法在重触发时产生的"电流声 / 咕嗒声"**：引入 `previousLossMask` 快照，在 `retriggerLossMask` 末尾对"上一帧为保留 (1) 但当帧变为丢弃 (0)"的每一个 band 执行 `lossBandNotchL/R[b].reset()` + `lossBandWet[b] = 0.0f`，强制从零初始状态拉起；同时把 `kLossMaskSmoothingTimeSeconds` 从 10ms 拉长到 25ms，为低频段 Notch 留出建立稳态的时间；FFT-Mask 分支无 IIR 内部状态，不受影响 |
| 2026-07-10 | 0.0.4 | 1.1.9 | v1.4.0 | **调整固定预设 0..11 的顺序**：`display_present::kPresets` 循环左移一位（旧 1..11 → 新 0..10，旧 0 → 新 11），且开机默认停留在新预设 0（原预设 1）。同时在 `PluginEditor.cpp::OscilloscopeComponent::paint()` 中把 `stylePreset` 的"固定频道路径"从 `= preset` 改为 `= mapToLegacyStyleIndex(preset)`（新 0..10→旧 1..11，新 11→旧 0），并把 `getWaveBaseColour()` 内的 `switch (preset)` 改为 `switch (stylePreset)`。使得**波形颜色 / 波形绘制样式 / glitch 撕裂效果**这三块硬编码视觉分支跟随数据一起从旧位置搬到新位置——数据（背景、扫描线扭曲、丢频算法、高低切）与外观（波形颜色、绘制样式、glitch）完全同步 |

### 7.2 踩坑记录

| 类别 | 现象 / 场景 | 结论 / 解决方式 | 关联版本 |
| --- | --- | --- | --- |
| — | 首次建档 | — | 0.0.1 |
| DSP | FFT-Mask 帧间增益跳变会造成明显的宽带调制感（"呼吸感"） | 逐 FFT bin 做一阶 IIR 平滑，`alpha = 1 − exp(−hop_sec/0.010)`，与时域 `lossBandWet` 的 10ms 平滑保持同一时间常数 | 0.0.2 |
| DSP | wet 曲线只在"帧边界"更新会与 IIR 分支听感不一致 | 无论哪种算法，`processBlock` 都逐 sample 推进 `lossBandWet`；FFT 分支只是在 hop 边界读取一次快照 | 0.0.2 |
| DSP | FFT-Mask 模式引入 `N−hop` 的处理延迟（1536 采样 @ N=2048/hop=512） | 只在切换到 FFT 模式时 `setLatencySamples(1536)`，切回 IIR 模式清 0；用 `stftLatencyReported` 缓存避免重复上报 | 0.0.2 |
| DSP | 算法切换瞬间残留在 STFT 环形缓冲里的旧样本会形成一次"pop" | 切换瞬间调用 `resetStftState()` 清空 inputRing/dryDelayRing/outputRing/olaNormRing/outFifo/smoothedGains，并在预热期（前 N/hop=4 帧）从延迟 dry 兜底输出 | 0.0.2 |
| DSP | 带外 bin 若被误参与掩码，会削掉整段高频尾巴 | 对 `hz < 20Hz` 或 `hz > 20kHz` 的 bin 强制保持 unity；DC bin 也恒 unity | 0.0.2 |
| DSP | JUCE `dsp::FFT::performRealOnlyForwardTransform` 的实数打包容易漏 Nyquist | 特殊处理 bin0（存于 `fftWork[0]`）和 binN/2（存于 `fftWork[1]`），中间 bin 才是交错复数 | 0.0.2 |
| DSP | 直接把 IFFT 输出加合成窗后累加会产生轻微幅度起伏 | OLA 累加同时累加 `∑w²`，输出时按 `y / (∑w² + ε)` 归一化，避免 75% overlap 下的 3× 增益偏移 | 0.0.2 |
| DSP | Legacy / UniformBandwidth 每次重触发时都有一下明显的"电流声"（FFT-Mask 无此现象） | 根因：`juce::IIRFilter` 是 direct-form 二阶结构，内部延迟状态 v1/v2 无论 wet 是否为 0 都在被 x 持续激励；长时间处于 wet=0 的 band 内部已累积与信号能量相关的稳态数值，一旦 wet 0→1 就会接入滤波器瞬态尾巴，多 band 叠加即为"咕嗒"。解决：重触发时对 previous=1∧current=0 的 band 手动 `reset()` 并强制 `lossBandWet=0`，从归零的状态向 wet=1 平滑 | 0.0.3 |
| DSP | wet 平滑 10ms 对低频段 Notch（群延迟本身就接近 10ms）“追不上"，即使 reset 也有残留尾巴 | 把 `kLossMaskSmoothingTimeSeconds` 从 0.010f 拉长到 0.025f（对扫频预设听感还不至于"发糊"） | 0.0.3 |
| DSP | 如果对所有变化的 band 都 reset，会引入反方向（1→0）的二次咕嗒 | 仅对 previous=1∧current=0 （新变为丢弃）的方向做 reset；反方向 wet 从 1→0 接入量递减不会激发瞬态，且反方向 reset 会丢失滤波器自然收尾 | 0.0.3 |
| 状态 | `prepareToPlay` / `releaseResources` 中若不同步 `previousLossMask` 初始化，首次 retrigger 会把全部 band 误判为"新丢弃" | 在两处都添加 `previousLossMask.fill(1)`，与 `lossMask.fill(1)` 对齐 | 0.0.3 |
| UI | 只挪 `kPresets` 数组，视觉外观没跟着搬——切到新预设 0 时听感变了但屏幕上的波形颜色 / 绘制样式 / glitch 干扰仍停留在原位 | 根因：`PluginEditor.cpp` 里有 3 处硬编码 `switch` 按预设序号分派视觉效果（波形颜色 `switch (preset)`、波形样式 + 预设 0 雪花底 `switch (stylePreset)`、glitch 干扰 `switch (stylePreset)`）。解决：不动 3 处庞大的 switch 主体，只在 `stylePreset` 计算点做**新序号→旧序号映射**（`mapToLegacyStyleIndex`），一处映射全局同步；`getWaveBaseColour()` 内的 `switch (preset)` 顺手改为 `switch (stylePreset)` 保持一致 | 0.0.4 |
| UI | 直接把 3 处 switch 全部按新顺序重排 case 分支的替代方案被否 | 改动面过大、极易破坏 case 3 的彩虹波形/OSD 手柄特判以及 `stylePreset`-related 的数值算式（如 `burstPeriod = 150 + stylePreset*17`），风险远高于收益。用重映射封装是最小改动且行为等价的方案 | 0.0.4 |
| UI | 衍生频道 (channel ≥ 12) 也可能被误映射 | `mapToLegacyStyleIndex` 显式判断 `p < 0 || p >= kPresetCount` 时返回原值；且衍生频道走的是 `1 + (preset*7+5) % (kPresetCount-1)` 分支，本身就已经落在旧序号语义下，无需再映射 | 0.0.4 |
| 兼容 | 旧工程加载后，保存的 `preset` 索引会指向新排序中的对应位置，听感与保存时不一致 | 已知取舍，属于"预设顺序调整"这类破坏性变更的正常后果；派生频道 (12..9999) 因基准索引 `cid % kPresetCount` 变化，声音也会与之前不同 | 0.0.4 |

<!-- 后续每次开发结束后，在这里追加行。示例格式：
| DSP | HPF/LPF 参数每帧微调导致电流感 | 引入 kCutCrossfadeRetuneHzEpsilon=18Hz 抖动阈值，避免频繁重启 crossfade | 0.0.x |
| UI  | 遥控器拉出时底层 TV 按钮被误触 | RemoteControlOverlay::hitTest 在拉出/动画中返回 true，作为 modal 拦截 | 0.0.x |
| 状态 | lossMaskFrozen 未持久化 | 若需要保存 Sleep 状态，需在 get/setStateInformation 中加字段 | 待办 |
-->
