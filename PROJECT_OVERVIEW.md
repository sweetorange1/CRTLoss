# CRTloss 项目全景简介（AI 上下文导航文档）

> 本文档是为 AI 助手上下文初始化设计的项目导航说明。阅读完本文档，你应能立刻定位到"改哪个文件、调哪个类、走哪条数据流"。

---

## 1. 项目概述

### 1.1 项目定位
- **产品名**：`CRTloss`（版本：`1.5.0`）
- **内部目标名**：`LDSJvst`（CMake target / 类名前缀均为 `LDSJvst*`，历史遗留命名）
- **产品形态**：一台**老式 CRT 电视机**外观的**丢频（band-loss / band-notch）音频效果器**。把 20Hz–20kHz 切成 **100 段**，按当前"频道"的概率分布 / 序列规则周期性对若干频段施加陷波，配合前置增益、限制器、高低切，做出"信号衰减、频段随机丢失"的退化质感。UI 隐喻是**电视 + 遥控器**：所有操作都通过点击电视面板、拖拽屏幕、或拉出遥控器完成。
- **发行形态**（在 [CMakeLists.txt](/I:/CRTLoss/CMakeLists.txt) 中通过 `juce_add_plugin` 定义）：
  - **Windows**：`VST3` + `Standalone`
  - **macOS**：`VST3` + `AU` + `Standalone`
  - 插件代码 `Ldsj`、厂商代码 `Ldsj`、公司名 `iisaacbeats.cn`
- **代码仓库**：`https://github.com/sweetorange1/LDSJvst.git`
- **代码骨架来源**：JUCE CMake 标准模板 + `grid_plugin/` 占位 INTERFACE 模块（见 §6.7）；全部业务代码（DSP / UI / 频道预设 / 网络）均为本项目原创。

### 1.2 主要功能一览
- **100 段丢频（Loss）**：三种算法可切换 —— `Legacy`（全段固定 Q 的 IIR Notch）、`UniformBandwidth`（按频段边界推导 Q，对数频谱等宽）、`FFT-Mask`（STFT 频域直接置零 + OLA）。
- **10000 个频道**：12 个固定预设（频道 0..11）+ 频道 12..9999 由频道号确定性派生（背景样式、扫描线扭曲、丢频分布、序列参数、默认高低切全部由 `cid` 推导）。
- **高低切（Cut）**：`HardMask`（直接裁掉掩码）或 `HPF/LPF` 滤波器链（12 / 24 / 48 dB·oct），切换带 1024 采样交叉淡化。
- **前置增益 PreGain**（−5..+24 dB）+ **硬限制器 Limiter**（阈值 0..1，屏幕内上下线拖拽）。
- **遥控器交互**：`BYPASS` / 数字 0–9 / `TV` / `SLEEP` / `RECALL` / `ST·SAP` / `MUTE` / `VOL±` / `CH±`。
- **电视面板交互**：`VOL±`（按住连发）/ `CH±` / 12 个预设指示灯 / Bypass 触发区（带开关机过渡动画）。
- **屏幕内交互**：示波器波形、100 格频段灯带、限位器阈值拖拽、高低切三角手柄、多级 OSD（频道号 / 模式 / 音量）。
- **宿主自动化**：9 个参数暴露给 DAW（Bypass / PreGain / Limiter / LowCut / HighCut / CutMode / CutSlope / LossAlgorithm / LossMaskInvert）。
- **自动更新检查** + CRT 风格更新弹窗（§5.6）。
- **每日一次匿名遥测**（§5.7）。

### 1.3 技术栈
| 项目 | 版本 / 说明 |
| --- | --- |
| 语言 | **C++17**（`CMAKE_CXX_STANDARD 17`，`CXX_EXTENSIONS OFF`） |
| 框架 | **JUCE 8.0.12**（通过 `FetchContent` 自动拉取，首次配置需联网） |
| 构建 | CMake ≥ 3.22 |
| 安装器（Win） | Inno Setup 6（[CRTloss_installer.iss](/I:/CRTLoss/CRTloss_installer.iss) + [build_installer.bat](/I:/CRTLoss/build_installer.bat)） |
| 安装器（mac） | 原生 `pkgbuild` + `productbuild` + `hdiutil`（[build_installer_mac.sh](/I:/CRTLoss/build_installer_mac.sh)） |
| 资源打包 | `juce_add_binary_data`（TV 外壳 / Bypass 叠加图 / 遥控器 / 遥控器指示灯） |
| DSP 依赖 | `juce::dsp::FFT`（FFT-Mask 模式）、`juce::IIRFilter`（Notch / HPF / LPF） |
| 网络宏 | `JUCE_USE_CURL=1`（https 请求必需，否则静默失败）、`JUCE_VST3_CAN_REPLACE_VST2=0` |

---

## 2. 核心分层架构

```
┌────────────────────────────────────────────────────────────────┐
│  构建层（CMakeLists.txt）                                        │
│    LDSJvst（juce_add_plugin：VST3 + AU + Standalone）           │
│    LDSJvst_BinaryData（juce_add_binary_data：4 张贴图）          │
├────────────────────────────────────────────────────────────────┤
│  Plugin 层（根目录）                                             │
│    LDSJvstAudioProcessor  (PluginProcessor.cpp)                 │
│      ↑ 音频线程 processBlock：完整 DSP 链路                      │
│    LDSJvstAudioProcessorEditor (PluginEditor.cpp)               │
│      ↑ UI 线程：电视/遥控器绘制、屏幕动画、拖拽、遥测 Session      │
├────────────────────────────────────────────────────────────────┤
│  预设数据层（display_present.h，header-only）                     │
│    12 个固定频道 + 12..9999 确定性派生 + 丢频分布模型             │
├────────────────────────────────────────────────────────────────┤
│  Network 层（source/network）                                   │
│    Version.cpp        —— SemVer 解析与比较                       │
│    UpdateChecker.cpp  —— 异步 GET 更新检查（进程级去重）          │
├────────────────────────────────────────────────────────────────┤
│  UI Dialog 层（source/ui）                                       │
│    UpdateDialog.cpp   —— CRT 风格（荧光绿+扫描线）原生更新弹窗     │
├────────────────────────────────────────────────────────────────┤
│  Shared 层（shared/）                                           │
│    IisaacTelemetry.h  —— header-only 每日遥测（跨项目共享）       │
└────────────────────────────────────────────────────────────────┘
```

### 2.1 关键调用关系
1. **音频链路**：`LDSJvstAudioProcessor::processBlock` 按序执行：Bypass 直通 → 前置增益 → 限制器 → 高低切 → 100 段丢频 → 0dB 硬削波 → 示波器采样（详见 §5.1）。
2. **UI → 音频线程**：Editor 调 Processor 的 setter → setter 优先写**宿主参数**（`writeFloatParamFromUI` 等，带 gesture + `setValueNotifyingHost`）→ 参数回调 `parameterValueChanged` 同步回 `std::atomic`。无宿主参数兜底时直接写原子。
3. **音频 → UI 反馈**：`getOscilloscopeSnapshot()`（2048 点环形缓冲 + `SpinLock`）与 `getLossMaskSnapshot()`（100 字节掩码），Editor 以 `Timer` 轮询读取。
4. **频道切换**：`setSelectedPresetIndex()`（Editor 侧）→ `processor.setDisplayPresetIndex()` → 音频线程按新预设重算丢频掩码与重触发节奏。

---

## 3. 代码结构说明

### 3.1 根目录关键文件
| 文件 | 作用 |
| --- | --- |
| [CMakeLists.txt](/I:/CRTLoss/CMakeLists.txt) | CMake 主构建脚本：FetchContent 拉 JUCE、插件目标、贴图打包、更新/遥测源文件 |
| [PluginProcessor.h/.cpp](/I:/CRTLoss/PluginProcessor.h) | 音频处理器：全部原子参数、宿主参数注册、三种丢频算法、STFT、状态序列化 |
| [PluginEditor.h/.cpp](/I:/CRTLoss/PluginEditor.h) | UI 编辑器：电视面板、遥控器、示波器屏幕、指示灯、OSD、遥测 Session |
| [display_present.h](/I:/CRTLoss/display_present.h) | header-only 频道预设数据层：12 个固定预设 + 派生算法 + 丢频概率/序列模型 |
| [CRTloss_installer.iss](/I:/CRTLoss/CRTloss_installer.iss) | Inno Setup 安装器：装到 `{commoncf}\VST3\iisaacbeats.cn`，需管理员；非默认目录弹 DAW 重扫描提示 |
| [build_installer.bat](/I:/CRTLoss/build_installer.bat) | Windows 一键打包：自动探测 VST3 产物目录 → 找 ISCC.exe → 产物 `dist\` |
| [build_installer_mac.sh](/I:/CRTLoss/build_installer_mac.sh) | macOS 打包：VST3 + AU 两个组件 pkg → productbuild 合成 → 可选 dmg；版本号自动从 `kPluginUiVersionText` 抽取 |
| [grid_plugin/](/I:/CRTLoss/grid_plugin) | **占位** JUCE INTERFACE 模块（`grid::grid_plugin`），无实际业务代码 |
| [assets/](/I:/CRTLoss/assets) | 4 张贴图（见 §3.3） |
| [shared/IisaacTelemetry.h](/I:/CRTLoss/shared/IisaacTelemetry.h) | header-only 匿名遥测（与 iisaacbeats 旗下其他产品共享同一份） |
| [source/](/I:/CRTLoss/source) | 更新检查（network）+ 更新弹窗（ui），见 §3.4 |

### 3.2 `PluginEditor.h` 内部组件层级（绘制顺序由下到上）
| 内部类 | 作用 |
| --- | --- |
| `OscilloscopeComponent` | 屏幕内容：背景/扫描线扭曲、波形、OSD、100 格频段灯带、高低切三角、限位器线；**所有屏幕内拖拽热区**都在它这里 |
| `IndicatorLight` ×12 | 12 个预设指示灯（点击直达固定频道） |
| `BypassHitArea` | Bypass 触发区（857,619,93×53），bypass 时叠加 `BYPASS.png` |
| `TvOverlayComponent` | TV 面板按钮层（VOL± / CH± 四个四边形热区）+ 音量连发定时器（60Hz） |
| `RemoteControlOverlay` | 遥控器层（最上层）：拉出即 modal 拦截所有点击，含全部遥控按钮与数字输入逻辑 |

### 3.3 `assets/`
| 路径 | 内容 |
| --- | --- |
| `TV.png` | 电视机外壳（基准 1000×750 底图） |
| `BYPASS.png` | Bypass 状态叠加图 |
| `remote_control.png` | 遥控器贴图（收起 / 拉出共用一张，靠位移区分） |
| `remote_control_light.png` | 遥控器指示灯发光贴图 |

### 3.4 `source/` 与 `shared/`
| 路径 | 作用 |
| --- | --- |
| `source/network/Version.h/.cpp` | SemVer（`X.Y.Z[-prerelease]`）解析与比较，命名空间 `crtloss::network` |
| `source/network/UpdateChecker.h/.cpp` | 异步更新检查：GET `iisaacbeats.cn/api/update/check?product=crtloss&version=..&platform=..`，5s 超时，进程级去重，回调切回主线程 |
| `source/ui/UpdateDialog.h/.cpp` | 独立原生置顶小窗（480×340），荧光绿 + 扫描线风格，Download / Remind Me Later，`force_update` 时仅有 Download |
| `shared/IisaacTelemetry.h` | header-only 每日遥测；**多产品共享文件，改动需兼容其他产品** |

---

## 4. 关键类 / 接口清单

### 4.1 `LDSJvstAudioProcessor`（[PluginProcessor.h](/I:/CRTLoss/PluginProcessor.h)）
- **参数常量**：`kPreGainDbMin/Max = −5/+24`、`kLimiterThresholdMin/Max = 0/1`、`kLossNotchQMin/Max = 0.1/30`、`kLowCutHzMin/Max = 20/20000`、`kLossBandCount = 100`、`kLossMaskSmoothingTimeSeconds = 0.025`、`kCutCrossfadeSamples = 1024`、`oscilloscopeBufferSize = 2048`。
- **丢频算法常量**：`kLossAlgorithmLegacy=0` / `kLossAlgorithmUniformBandwidth=1` / `kLossAlgorithmFftMask=2`；`toggleLossAlgorithmMode()` 三态循环。
- **切频模式常量**：`kCutModeHardMask=0` / `kCutModeHpfLpf=1`；`kCutSlope12/24/48dB`；`cycleCutModeOrSlopeFromTV()` 四态循环（Hard → HPF12 → HPF24 → HPF48 → Hard）。
- **频道**：`setDisplayPresetIndex(int)`（0..9999，`kDisplayChannelMax = display_present::kDerivedChannelMax`）。
- **冻结 / 反转**：`toggleLossMaskFrozen()`（SLEEP）、`toggleLossMaskInverted()`（MUTE）。
- **UI 快照**：`getOscilloscopeSnapshot(Array<float>&)`、`getLossMaskSnapshot(Array<uint8_t>&)`。
- **宿主参数**（9 个，`addParameter` 注册，指针成员见 `PluginProcessor.h`）：`bypass` / `preGainDb` / `limiterThreshold` / `lowCutHz` / `highCutHz` / `cutMode` / `cutSlope` / `lossAlgorithm` / `lossMaskInvert`；`getBypassParameter()` 把 `bypass` 认领为标准 Bypass。
- **状态持久化**：`getStateInformation / setStateInformation` 走 `juce::ValueTree("LDSJvstState")`（XML 二进制）：`version, preset, bypassed, preGainDb, randomRetriggerPerBeat, limiterThreshold, lossNotchQ, lossAlgorithmMode, lossMaskInverted, lowCutHz, highCutHz, cutMode, cutSlopeDbPerOct`。**`lossMaskFrozen`（Sleep）不参与持久化**。

### 4.2 `LDSJvstAudioProcessorEditor`（[PluginEditor.h](/I:/CRTLoss/PluginEditor.h)）
- **基准分辨率** 1000×750，等比缩放（`ComponentBoundsConstrainer` 固定 4:3）；构造时按主屏可用区域（四周留 60px）限制最大窗口尺寸。
- **关键布局常量**：屏幕区 `(130,135,606×466)`；指示灯起点 `(829,157)`、17px、间隔 10；Bypass 区 `(857,619,93×53)`；遥控器 `(782,684,168×488)`、拉出上移 `remoteLift=415`；遥控器灯 `(789,290,29×29)`。
- **频道切换** `setSelectedPresetIndex()`：记录上一频道（供 RECALL）→ 启动 0.26s 切换动画 → 下发 `processor.setDisplayPresetIndex()` → 触发频道号 OSD（3s）。
- **电视面板按钮**（`kTvPanelButtons`，四边形热区）：`VolUp/VolDown/ChannelUp/ChannelDown`；VOL 按住连发（首延迟 0.32s，间隔 0.075s）。**仅当遥控器完全收回时才命中**。
- **屏幕内交互**（`OscilloscopeComponent`）：限位器阈值拖拽（Y 方向）、高低切三角手柄拖拽、100 格 LOG 频率灯带。
- **遥控器按钮**（偏移相对遥控器图左上，热区放大 1.5×）：见 §5.4。
- **版本水印**：右下角显示 `kPluginUiVersionText`，点击打开 `https://iisaacbeats.cn`。
- **遥测**：成员 `std::unique_ptr<iisaac::telemetry::Session> telemetrySession`（构造末尾创建、析构开头 reset）。

### 4.3 `display_present`（[display_present.h](/I:/CRTLoss/display_present.h)）
- `kPresetCount = 12`、`kDerivedChannelMin = 12`、`kDerivedChannelMax = 9999`。
- 预设结构 `DisplayPresetParams`：`BackgroundParams` / `ScanlineWarpParams` / `OSDParams` / `LossDistributionParams` / `LossRandomParams` / `LossSequenceParams` / `CutPresetParams`。
- `derivePresetParamsFromChannelId(cid)`：按频道号确定性随机派生，覆盖背景、扫描线扭曲、丢频分布模型、序列参数、每预设默认高低切。

### 4.4 `crtloss::network::UpdateChecker`（[UpdateChecker.h](/I:/CRTLoss/source/network/UpdateChecker.h)）
- `CheckForUpdatesAsync(product, current_version, platform, callback)`：后台线程 HTTP，回调经 `MessageManager::callAsync` 切回主线程；失败/超时静默返回 `has_update=false`。
- 进程级去重：`static std::atomic<bool>` 保证**多实例只检查一次**。
- 平台标识格式 `<os>-<arch>`（如 `win-x64`、`mac-arm64`），由 `PluginProcessor.cpp` 的 `GetUpdatePlatformString()` 生成，与遥测口径一致。

### 4.5 `crtloss::ui::UpdateDialog`（[UpdateDialog.h](/I:/CRTLoss/source/ui/UpdateDialog.h)）
- 通过 `addToDesktop` 创建独立原生窗口（非宿主子窗口），`setAlwaysOnTop` 置顶，标题栏可拖拽。
- `force_update=true` 时只显示 Download 且不可关闭；`download_url` 为空则打开官网。

---

## 5. 业务逻辑流程

### 5.1 音频 DSP 链路（processBlock）

```
输入 x
  │
  ├─⓪ Hard Bypass：bypassed==true 直接把输入拷到输出（首行判断）
  │
  ├─① 前置增益 PreGain：−5..+24 dB（默认 +10），遥控器/面板 VOL± 每次 ±1 dB
  │
  ├─② 硬限制器 Limiter：严格把幅度钳在 [−th, +th]，th = 0..1（默认 1.0）
  │
  ├─③ 高低切 Cut（两种模式，切换 1024 采样交叉淡化）：
  │     · HardMask：把高低切范围外的频段掩码直接置 0
  │     · HPF/LPF：最多 4 级 juce::IIRFilter 串接，斜率 12 / 24 / 48 dB·oct
  │
  ├─④ 频段丢失 Loss（100 段，wet 25ms 一阶平滑）：
  │     · Legacy          —— 100 个 IIR Notch 串联，Q 全段共用 lossNotchQ
  │     · UniformBandwidth—— 每段按频段边界推导 Q，令对数频谱上带宽等宽
  │     · FFT-Mask        —— STFT(N=2048, hop=512, 75% overlap, Hann) + 逐 bin
  │                          乘 g(k)=1−wet(band(k)) + IFFT + OLA(∑w² 归一化)
  │
  ├─⑤ 最终 0 dB 硬削波（clamp ±1）
  │
  └─→ 示波器采样 pushSamplesToOscilloscope（2048 点环形缓冲 + SpinLock）
```

**全链路位置约定**：Cut 在 Loss 之前（先裁掉高低切范围，再在剩余频段上丢频）；Limiter 在 PreGain 之后，防止大增益打爆后级。

### 5.2 三种丢频算法要点

| 模式 | 实现 | 特点 / 注意事项 |
| --- | --- | --- |
| Legacy | 100 个 `IIRFilter` Notch 串联，Q 全段共用 | 计算最轻；低段带宽偏窄、高段偏宽；重触发时必须对"新变为丢弃"的 band 做 `reset()`（见 §6.2） |
| Uniform Bandwidth | 每段按频段边界推导 Q | 全频带"切除宽度"一致；与 Legacy 共用同一套 Notch 数组与防病策略 |
| FFT-Mask | STFT + 频域置零 + OLA | 相位保留；引入 `N−hop = 1536` 采样延迟（通过 `setLatencySamples()` 上报，切回 IIR 清 0） |

FFT-Mask 关键实现点：
- bin→band 反向对数映射：`t = log(hz/20)/log(1000)`，`band = round(t·99)`。
- 逐 bin 一阶 IIR 平滑（时间常数 10ms，与时域 `lossBandWet` 一致），避免帧间跳变产生宽带"呼吸感"。
- 干路用长度 N 的环形缓冲延迟 `N−hop` 采样与 wet 对齐；预热期（前 4 帧）从延迟 dry 兜底。
- 切换算法瞬间 `resetStftState()` 清空全部 STFT 环形缓冲与平滑增益。
- 20Hz 以下 / 20kHz 以上 bin 与 DC bin 恒为 unity，避免削掉高频尾巴。

### 5.3 丢频重触发策略（`retriggerLossMask`）
取宿主 BPM（无 playhead 时 fallback 120）：
- **BPM 序列型预设**（`LossSequenceParams.enabled == true`）：按 anchor + halfWindow 生成"沿频段游走的窗口"掩码，`retriggerSeconds = beatSec * bpmDivision`。
- **随机型预设**：按 `LossDistributionParams` 计算每段保留概率并随机采样，`retriggerSeconds = beatSec / (perBeat * perPresetScale)`。
- 高低切范围外的频段强制置 0；若掩码全空则兜底保留一个最接近 lowCut 的频段。
- `lossMaskFrozen == true`（SLEEP）时不再重触发，保持当前掩码。

### 5.4 UI 交互清单

#### 5.4.1 电视面板（`TvOverlayComponent`，四边形热区）
| 按钮 | 基准四点坐标 | 功能 |
| --- | --- | --- |
| `VolUp` | (910,273)-(947,273)-(947,298)-(904,298) | PreGain +1 dB，按住连发 |
| `VolDown` | (865,273)-(909,273)-(903,298)-(865,298) | PreGain −1 dB，按住连发 |
| `ChannelUp` | (912,149)-(948,149)-(948,175)-(905,175) | 频道 +1（0..9999 循环） |
| `ChannelDown` | (866,149)-(911,149)-(904,175)-(866,175) | 频道 −1（0..9999 循环） |

> 仅当遥控器**完全收回**（`remotePulledOut==false` 且 `remotePullAmount<=0.001`）时才命中；否则被遥控器 overlay 屏蔽。

#### 5.4.2 12 个预设指示灯 / Bypass 区
- 灯 0..11 → 直达对应固定频道；当前 `selectedChannelId % 12` 高亮为亮绿色；bypass 时全部变暗但仍可点击。
- Bypass 触发区单击 → `toggleBypassFromUI()`（写宿主 `bypass` 参数）+ 0.22s 电视开关机过渡动画。

#### 5.4.3 屏幕内（示波器）
1. **限位器阈值**：点击/拖动屏幕内非灯带非三角区域，按 `|y − midY| / (h*0.4)` 计算阈值 → `setLimiterThreshold()`。
2. **高低切三角**：底部 100 格 LOG 灯带两侧各一个三角手柄（`triW ≈ cellW*2.3`，`triH ≈ cellH*2.2`）；点在灯带上则抓"更近的那个"；拖拽期间 `setCutDragActive(true)` 抑制频繁 crossfade。
3. **OSD**：频道号（右上，3s）/ 模式（左上，3s）/ 音量（3s）。
4. **波形**：预设 3/8/10 带 trail 拖影；由 `getVolumeOsdT / getModeOsdT / getChannelOsdText` 驱动淡入淡出。

#### 5.4.4 遥控器（`RemoteControlOverlay`）
- 收起时上半部分露在屏幕下方（`remoteY=684`），点击 → 拉出（上移 415px，0.18s 动画），拉出后作为 modal overlay 拦截全部点击；点击遥控器外部 → 收回。
- 按钮触发语义：`mouseDown` 进入按下态，`mouseUp` 且指针仍在同一按钮内才触发（release-to-trigger）；**VOL± 特例**：`mouseDown` 立即触发一次并进入连发，`mouseUp` 只结束连发。

| 按钮 | offset (x,y,w,h) | 功能 |
| --- | --- | --- |
| `BYPASS` | (73,287,68,10) | 切换 Bypass |
| 数字 0..9 | 3×3+2 网格 (~28/73/118, 71~162) | 频道数字输入，最多 4 位，3s 无输入自动提交；>9999 仅显示不切换 |
| `TV` | (29,168,23,10) | 循环切换 CutMode/Slope（Hard → HPF12 → HPF24 → HPF48 → Hard） |
| `SLEEP` | (29,198,23,10) | 冻结 / 恢复丢频掩码 |
| `RECALL` | (29,228,23,10) | 回到上一个频道 |
| `ST/SAP` | (29,258,23,10) | 循环切换丢频算法（Legacy → UniformBW → FFT-Mask） |
| `MUTE` | (29,287,23,10) | 反转丢频掩码（保留/丢失互换） |
| `VOL+ / VOL−` | (73,198,23,23) / (73,242,23,23) | PreGain ±1 dB，按住连发 |
| `CH+ / CH−` | (118,198,23,23) / (118,242,23,23) | 频道 ±1 |

### 5.5 状态持久化流程
宿主保存工程 → `getStateInformation`（ValueTree → XML → MemoryBlock）→ 宿主回放 → `setStateInformation` 校验 version → 用**静默方式**同步宿主参数与内部原子（避免触发宿主回调）→ Editor 打开时按 `displayPresetIndex` 还原频道与指示灯。

### 5.6 更新检查流程
Processor 构造 →（进程级仅一次）`Timer::callAfterDelay(5000)` → `CheckForUpdatesAsync("crtloss", JucePlugin_VersionString, "win-x64")` → 后台 GET（5s 超时）→ 主线程回调 → 本地再用 SemVer 兜底比较一次 → `has_update` 则弹 `UpdateDialog` → Download 开浏览器 / Remind Me Later 关闭。

### 5.7 遥测流程
Editor 构造末尾创建 `iisaac::telemetry::Session`（消息线程）→ 5s 后起工作线程 → 读/建 `%APPDATA%\iisaacbeats\Telemetry\crtloss.xml`（进程锁）→ 当 UTC 日未上报则 POST ping → 成功记 `last_success_day`，失败留 15min 冷却；跨 UTC 日自动再报，每日至多一次。Editor 析构开头 `telemetrySession.reset()` 保证在消息线程 join 工作线程。

---

## 6. 特殊约定与注意事项

### 6.1 音频线程约束
- UI→音频全部走 `std::atomic`（`memory_order_relaxed`，布尔开关用 acquire/release）；可听参数在音频线程内再做一阶平滑（丢频 wet 25ms）。
- `prepareToPlay` 完成滤波器/STFT 缓冲分配，`releaseResources` 与 `prepareToPlay` 都要 `previousLossMask.fill(1)` 与 `lossMask.fill(1)` 对齐，否则首次 retrigger 会把全部 band 误判为"新丢弃"。
- `processingStateLock`（SpinLock）只在状态恢复/释放阶段保护非原子缓存，不在 processBlock 热路径加锁。

### 6.2 丢频重触发的"电流声"防病（**重要**）
- 根因：`juce::IIRFilter` 内部延迟状态在 wet=0 时仍被输入持续激励，长时间后累积稳态值；wet 0→1 的瞬间接入滤波器瞬态尾巴，多 band 叠加就是"咕嗒"。
- 对策：`retriggerLossMask` 末尾对 `previous==1 && current==0`（**仅这个方向**）的 band 执行 `reset()` 并强制 `lossBandWet=0`。
- 反方向（1→0）**不要** reset：接入量递减不会激发瞬态，且 reset 会丢掉滤波器自然收尾。

### 6.3 宿主参数双向同步的递归保护
- UI setter → `writeXxxParamFromUI`（gesture + `setValueNotifyingHost`，注意**归一化 0..1**）→ `parameterValueChanged`（收到的是**实际值**）→ 校准原子。
- 用 `parameterCallbackDepth` 原子计数器防递归；`setStateInformation` 恢复时用 `setValue()` 静默同步，避免回灌宿主。
- 只有 9 个参数走了宿主参数；**频道号 `displayPresetIndex` 不走宿主参数**，靠 ValueTree 的 `preset` 字段持久化。

### 6.4 macOS / 渲染性能铁律（**踩过坑，别回退**）
1. **必须** `setBufferedToImage(true)`：否则每次绘制都直连 `CGContext` 跨进程 IPC，表现为 CPU 8% 但帧率 ~10fps。
2. 波形描边**全部用 `PathStrokeType::mitered`**（共 28 处）：`curved` 会让 CoreGraphics 对每段做贝塞尔拟合 + 子像素累加，是卡顿根因。
3. 波形 Path 下采样到 ~300 段（stride = `jmax(1, n/300)`，末尾补一次 `lineTo`）。
4. 静音短路：整帧 `|s| < 1e-4` 时只画 2 点水平直线。
5. 自适应帧率：用 `getMillisecondCounterHiRes()`（**不要用 `getHighResolutionTicks()`**，macOS 上是纳秒原始计数）测帧间隔，超预算自动降 30→24→18→12→10Hz，负载回落自动回升。
6. 不要用半分辨率离屏缓冲（`internalRenderScale`）：电视中文字会被裁切、波形模糊泛光。
7. remap 用整像素偏移 + 整行 `memcpy`（逐像素浮点 LERP 是 ~850 万次运算/帧的绝对瓶颈）。

### 6.5 版本号一致性（**五处必须同步**）
| 位置 | 字段 |
| --- | --- |
| [CMakeLists.txt](/I:/CRTLoss/CMakeLists.txt) | `project(LDSJvst VERSION x.y.z)` |
| [CMakeLists.txt](/I:/CRTLoss/CMakeLists.txt) | `juce_add_plugin(... VERSION x.y.z)`（决定 `JucePlugin_VersionString`，更新检查自动跟随） |
| [PluginEditor.cpp](/I:/CRTLoss/PluginEditor.cpp) | `kPluginUiVersionText = "vx.y.z"`（右下角水印；**mac 打包脚本自动抽取它作为 pkg 版本号**） |
| [CRTloss_installer.iss](/I:/CRTLoss/CRTloss_installer.iss) | `MyAppVersion`（决定安装包文件名） |
| [build_installer.bat](/I:/CRTLoss/build_installer.bat) | `APP_VERSION`（仅用于脚本回显与产物名校验） |
| [README.md](/I:/CRTLoss/README.md) | 版本 badge + 产物名 |

### 6.6 安装器与构建目录
- Windows：`.iss` 的 `VST3_DIR` 默认 `cmake-build-release-visual-studio\LDSJvst_artefacts\Release\VST3`，`build_installer.bat` 会依次探测 `cmake-build-release-visual-studio` → `cmake-build-release` → `%LOCALAPPDATA%\Programs\Common\VST3`（`COPY_PLUGIN_AFTER_BUILD` 的落点），并用 `-DVST3_DIR` 覆盖传给 ISCC。
- **打包脚本严格 KISS、不触碰编译**：编译由 CLion / cmake 负责，bat 只做「校验产物 → 定位 ISCC → 调 ISCC」三步（历史上在 bat 里自动 configure/build 反复因 Generator 不匹配失败）。
- 安装器装到系统级 `{commoncf}\VST3\iisaacbeats.cn`（需管理员）；构建后自动复制到的 `%LOCALAPPDATA%\Programs\Common\VST3` 是免管理员的调试落点。
- macOS：`build_installer_mac.sh` 用 `.pkg_staging/{vst3_root,au_root}/Library/Audio/Plug-Ins/...` 建树（pkgbuild `--root` 的目录结构必须就是目标机器的绝对路径），再 `productbuild` 合成，最后可选 `hdiutil` 出 dmg。
- 两个平台的 `AppId` / pkg identifier 必须互不相同：`iss` 用 `{{C4E17B2A-6D58-4F31-9A72-5B0E83D6F241}}`，mac 用 `cn.iisaacbeats.crtloss.*`（**不要复用其他产品的 GUID**）。

### 6.7 隐私口径
- 更新检查与遥测均为匿名、每进程/每日至多一次、失败静默；遥测可用环境变量 `IISAAC_TELEMETRY_DISABLED=1` 关闭。
- `shared/IisaacTelemetry.h` 是多产品共享文件，**改动必须兼容其他产品**；遥测状态文件损坏时不会重置 UUID（防串号）。

### 6.8 存在但未使用 / 待办
- `grid_plugin/` 是占位 INTERFACE 模块（仅 `CMakeLists.txt`），无业务代码。
- `kEnableTempQInput = false`：Notch Q 临时输入框，代码保留但默认不显示，仅供开发期调试。
- 待办：`lossMaskFrozen`（Sleep）未纳入持久化，若需要保存 Sleep 状态需在 `get/setStateInformation` 中加字段。

### 6.9 常见修改场景速查
| 场景 | 改哪里 |
| --- | --- |
| 新增丢频算法 | `kLossAlgorithm*` 常量 + `kLossAlgorithmModeCount` + `processBlock` 分支 + 宿主 `paramLossAlgorithm` 选项文本 + 遥控器 `ST/SAP` OSD 文案 |
| 调整固定频道听感 / 视觉 | `display_present::kPresets`（注意 §6.10 的新旧序号映射）+ `mapToLegacyStyleIndex` |
| 新增宿主可自动化参数 | `createAndRegisterHostParameters()` + 指针成员 + setter 走 `writeXxxParamFromUI` + `syncAtomicFromParameter` + ValueTree 字段（升 version） |
| 改遥控器按钮布局 | `PluginEditor.h` 的 `remoteXxx` 常量 + `RemoteControlOverlay` 按钮表（偏移 + 1.5× 热区） |
| 改屏幕内热区 | `OscilloscopeComponent::mouseDown/Drag/Up`（限位器 / 高低切三角 / 灯带） |
| 改窗口基准尺寸 | `baseEditorWidth/Height` + `screenX/Y/W/H` + 各组件常量（全部按 1000×750 基准设计） |
| 换贴图 | `assets/` + `juce_add_binary_data` 列表 |
| 加/改更新弹窗文案与配色 | `source/ui/UpdateDialog.cpp`（`kAccentColour` 为主色，扫描线在 `drawScanlines`） |

### 6.10 固定预设序号的历史映射
- 预设 0..11 曾整体循环左移一位（旧 1..11 → 新 0..10，旧 0 → 新 11）。
- `PluginEditor.cpp` 里有 3 处**按预设序号硬编码**的视觉分支（波形颜色、波形绘制样式、glitch 撕裂），因此引入 `mapToLegacyStyleIndex()` 做"新序号 → 旧序号"的**单点映射**，不要去重排那 3 个庞大 switch。
- 衍生频道（≥12）走 `1 + (preset*7+5) % (kPresetCount-1)` 分支，本身已落在旧序号语义下，**无需**再映射。

---

## 7. 附：目录树（简化版）

```
I:\CRTLoss\
├── CMakeLists.txt              # 主构建脚本（JUCE 8.0.12 FetchContent）
├── PluginProcessor.h/.cpp      # DSP 核心（丢频 / 高低切 / STFT / 宿主参数）
├── PluginEditor.h/.cpp         # UI 核心（电视 / 遥控器 / 屏幕 / OSD / 遥测 Session）
├── display_present.h           # 频道预设数据层（header-only）
├── CRTloss_installer.iss       # Inno Setup 安装器脚本（Win）
├── build_installer.bat         # Windows 一键打包
├── build_installer_mac.sh      # macOS 打包（pkg + dmg）
├── assets\                     # TV / BYPASS / remote_control / remote_control_light
├── grid_plugin\                # 占位 INTERFACE 模块（无业务代码）
├── shared\
│   └── IisaacTelemetry.h       # header-only 每日遥测（多产品共享）
├── source\
│   ├── network\                # Version / UpdateChecker（crtloss::network）
│   └── ui\                     # UpdateDialog（crtloss::ui）
├── readme_1.png / readme_2.png # README 截图
├── dist\                       # 安装包输出
└── cmake-build-release(-visual-studio)\  # 构建目录
```

---

## 8. 开发记录

### 8.1 变更日志

| 日期 | CMake VERSION | UI 版本 | 变更摘要 |
| --- | --- | --- | --- |
| 2026-07-09 | 1.1.6 | v1.3.0 | 建立首版功能与交互索引文档 |
| 2026-07-09 | 1.1.7 | v1.3.1 | 新增第 3 种丢频算法 **FFT-Mask**（STFT + Hann + 75% overlap + OLA，逐 bin 乘 `g = 1 − wet(band(k))` 保留相位）；`ST/SAP` 改三态循环；FFT 模式下报 `setLatencySamples(N−hop)=1536`，切回 IIR 清 0 |
| 2026-07-09 | 1.1.8 | v1.3.1 | 修复 Legacy / UniformBandwidth 重触发的"电流声"：引入 `previousLossMask` 快照，对新变为丢弃的 band `reset()` + `lossBandWet=0`；wet 平滑 10ms → 25ms |
| 2026-07-10 | 1.1.9 | v1.4.0 | 固定预设 0..11 循环左移一位；新增 `mapToLegacyStyleIndex()` 单点映射，同步波形颜色 / 绘制样式 / glitch 三处硬编码视觉分支 |
| 2026-07-10 | 1.1.9 | v1.4.3 | macOS 性能优化：窗口大小保护、背景缓存、remap 整像素 memcpy、自适应帧率、恢复 `setBufferedToImage(true)` |
| 2026-07-10 | 1.1.9 | v1.4.4 | macOS 波形绘制收尾（28 处 `curved`→`mitered`、波形下采样 ~300 段、静音短路）+ 新增 `build_installer_mac.sh` |
| 2026-09-16 | **1.5.0** | **v1.5.0** | **接入遥测 + 产品更新推送**：新增 `source/network/{Version,UpdateChecker}` 与 `source/ui/UpdateDialog`（CRT 荧光绿风格弹窗）；Processor 构造延迟 5s 做进程级去重检查；Editor 持有 `iisaac::telemetry::Session`（product_id = `crtloss`）；CMake 加 `JUCE_USE_CURL=1` + `NEEDS_CURL TRUE`；重写 Windows 打包脚本（自动探测 VST3 目录 + `-DVST3_DIR`、独立 AppId）；版本号五处同步到 1.5.0；新增 README.md 并按 rorrerror 规范重写本文档 |

### 8.2 踩坑记录

| 类别 | 现象 / 场景 | 结论 / 解决方式 |
| --- | --- | --- |
| DSP | FFT-Mask 帧间增益跳变造成宽带"呼吸感" | 逐 bin 一阶 IIR 平滑，`alpha = 1 − exp(−hop_sec/0.010)`，与时域 `lossBandWet` 同时间常数 |
| DSP | FFT-Mask 引入 `N−hop` 延迟 | 切到 FFT 时 `setLatencySamples(1536)`，切回 IIR 清 0；`stftLatencyReported` 缓存避免重复上报 |
| DSP | 算法切换瞬间 STFT 环形缓冲残留旧样本 → pop | 切换瞬间 `resetStftState()` 清空全部缓冲；预热期（前 4 帧）从延迟 dry 兜底 |
| DSP | OLA 叠加产生幅度起伏 | 同时累加 `∑w²`，输出按 `y / (∑w² + ε)` 归一化 |
| DSP | `dsp::FFT` 实数打包漏 Nyquist | bin0 存 `fftWork[0]`、binN/2 存 `fftWork[1]`，中间 bin 才是交错复数 |
| DSP | Legacy / UniformBW 重触发"咕嗒" | 见 §6.2：仅对 `previous==1 && current==0` 方向 reset |
| 状态 | 首次 retrigger 误判全部 band 为新丢弃 | `prepareToPlay` / `releaseResources` 都加 `previousLossMask.fill(1)` |
| UI | 挪了 `kPresets` 但视觉没跟着搬 | 不动 3 处庞大 switch，只在 `stylePreset` 计算点做新旧序号映射（§6.10） |
| 渲染 | 移除 `setBufferedToImage(true)` 后 macOS CPU 8% 但 10fps | 恢复缓冲：避免每次绘制走窗口服务器 IPC |
| 渲染 | 半分辨率渲染导致中文字被裁、波形泛光 | 移除 `internalRenderScale`，恢复全分辨率 |
| 渲染 | 全帧缓存（`sampleHash` 指纹）从不命中 | 音频每帧都在变，指纹无效；彻底移除 |
| 渲染 | 自适应帧率永不触发 | `getHighResolutionTicks()` 在 macOS 返回纳秒原始计数，单位错；改用 `getMillisecondCounterHiRes()` |
| 渲染 | `curved` 描边 + 2048 段 Path 打爆光栅化队列 | 全改 `mitered` + 下采样 ~300 段 + 静音短路 |
| 渲染 | 逐像素浮点 LERP remap（~850 万次/帧） | 改整像素偏移 + 整行 `memcpy/fill_n` |
| 打包 | bat 里自动 CMake configure/build 反复失败（`nmake` 不匹配 / 产物目录只有壳） | 打包脚本不触碰编译，只做校验 + 调 ISCC |
| 打包 | `iss` 的 `MyAppVersion` 与 UI 版本长期错位 | 约定：iss 版本跟随"用户可感知的主版本"（`kPluginUiVersionText` 去 `v`）；CMake VERSION 是构建版本，两者不必强绑但 iss 必须与 UI 对齐 |
| 打包 | 直接复用其他产品的 Inno `AppId` GUID | 安装器会把不同产品视为同一应用（覆盖/卸载混乱）；CRTloss 已改用独立 GUID |
| 打包 | `pkgbuild --root` 的目录结构不对 | 必须先建 `.pkg_staging/*/Library/Audio/Plug-Ins/...` 完整树再打包 |
| 网络 | https 请求静默失败 | 必须 `JUCE_USE_CURL=1`（Linux 还需 `NEEDS_CURL TRUE`）；本项目已在 CMake 中开启 |
