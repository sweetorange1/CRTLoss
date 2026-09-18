#!/usr/bin/env bash
# =============================================================================
# CRTloss (LDSJvst) — macOS 安装包打包脚本
# -----------------------------------------------------------------------------
# 用途：把 CLion 以 Release 构建产出的 VST3 + AU 组件，打包成一个 macOS
#      标准安装器 (.pkg)，可选再封装成 .dmg 便于分发。
#
# 设计原则（对齐 build_installer.bat 的 KISS 原则）：
#   1) 本脚本**不触碰编译**。编译由 CLion / cmake 负责。
#   2) 本脚本只做三件事：
#        a. 校验 Release 版 VST3 + AU 产物是否存在
#        b. 校验产物为 Universal 二进制（同时含 arm64 与 x86_64）
#        c. 用 pkgbuild 分别打两个组件 pkg
#        d. 用 productbuild 合成一个用户可见的安装器 .pkg
#        e. （可选）用 hdiutil 生成一个 .dmg 磁盘映像
#   3) 版本号从 PluginEditor.cpp 的 kPluginUiVersionText 自动读取（去掉前缀 "v"），
#      与 Windows iss 的 MyAppVersion 语义保持一致。
#
# 前置条件：
#   * 已在 CLion 中以 Release 配置构建 LDSJvst_VST3 与 LDSJvst_AU 目标
#   * CMake 已配置 CMAKE_OSX_ARCHITECTURES="arm64;x86_64"（仓库 CMakeLists.txt 已默认设置），
#     若已有旧的 cmake-build-release 缓存，需删掉该目录重新 configure 才会生效
#   * 产物目录：cmake-build-release/LDSJvst_artefacts/Release/{VST3,AU}
#   * macOS 自带 pkgbuild / productbuild / hdiutil / lipo，无需额外安装
#
# 输出目录：dist/
#   * CRTloss_Setup_<ver>_macOS.pkg     ← 双击即可安装
#   * CRTloss_Setup_<ver>_macOS.dmg     ← 便于分发（默认生成）
#
# 用法：
#   chmod +x build_installer_mac.sh
#   ./build_installer_mac.sh              # 打 pkg + dmg
#   ./build_installer_mac.sh --no-dmg     # 只打 pkg
# =============================================================================

set -euo pipefail

# ---------- 路径与常量 ----------
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
BUILD_DIR="${SCRIPT_DIR}/cmake-build-release/LDSJvst_artefacts/Release"
VST3_SRC="${BUILD_DIR}/VST3/CRTloss.vst3"
AU_SRC="${BUILD_DIR}/AU/CRTloss.component"

DIST_DIR="${SCRIPT_DIR}/dist"
STAGING_DIR="${SCRIPT_DIR}/.pkg_staging"

APP_NAME="CRTloss"
APP_PUBLISHER="iisaacbeats.cn"
# 唯一标识符（与 Windows 端 AppId 语义对应，macOS 用反向域名）
VST3_PKG_ID="cn.iisaacbeats.crtloss.vst3"
AU_PKG_ID="cn.iisaacbeats.crtloss.au"
PRODUCT_PKG_ID="cn.iisaacbeats.crtloss.installer"

MAKE_DMG=1
for arg in "$@"; do
  case "$arg" in
    --no-dmg) MAKE_DMG=0 ;;
    -h|--help)
      echo "用法: $0 [--no-dmg]"
      exit 0
      ;;
    *)
      echo "[WARN] 未知参数: $arg（已忽略）"
      ;;
  esac
done

# ---------- 1) 从 PluginEditor.cpp 抽取版本号 ----------
EDITOR_CPP="${SCRIPT_DIR}/PluginEditor.cpp"
if [[ ! -f "$EDITOR_CPP" ]]; then
  echo "[ERROR] 未找到 PluginEditor.cpp: $EDITOR_CPP"
  exit 1
fi
# 匹配形如：static constexpr auto kPluginUiVersionText = "v1.5.0";
RAW_VER="$(grep -Eo 'kPluginUiVersionText[[:space:]]*=[[:space:]]*"v[0-9]+\.[0-9]+\.[0-9]+"' "$EDITOR_CPP" | grep -Eo '[0-9]+\.[0-9]+\.[0-9]+' | head -n1 || true)"
if [[ -z "$RAW_VER" ]]; then
  echo "[ERROR] 无法从 PluginEditor.cpp 提取版本号（kPluginUiVersionText 未匹配到 vX.Y.Z 格式）"
  exit 1
fi
APP_VERSION="$RAW_VER"
echo "[INFO] 检测到 UI 版本号: v${APP_VERSION}"

# ---------- 2) 校验 Release 产物 ----------
if [[ ! -d "$VST3_SRC" ]]; then
  echo "[ERROR] 未找到 Release 版 VST3 产物: $VST3_SRC"
  echo "        请先在 CLion 中以 Release 配置构建 LDSJvst_VST3 目标后再运行本脚本。"
  exit 1
fi
if [[ ! -d "$AU_SRC" ]]; then
  echo "[ERROR] 未找到 Release 版 AU 产物: $AU_SRC"
  echo "        请先在 CLion 中以 Release 配置构建 LDSJvst_AU 目标后再运行本脚本。"
  exit 1
fi
echo "[INFO] VST3 源: $VST3_SRC"
echo "[INFO] AU   源: $AU_SRC"

# ---------- 2.5) 校验产物是 arm64 + x86_64 通用二进制 ----------
# 只打通用包：单架构产物（例如只编了 arm64）必须在这里拦下，避免误分发。
check_universal() {
  local label="$1"
  local bundle="$2"
  local bin="$bundle/Contents/MacOS/${APP_NAME}"

  if [[ ! -f "$bin" ]]; then
    echo "[ERROR] 在 ${bundle} 中未找到可执行文件 Contents/MacOS/${APP_NAME}"
    exit 1
  fi

  local archs
  archs="$(lipo -archs "$bin" 2>/dev/null || true)"
  if [[ -z "$archs" ]]; then
    echo "[ERROR] 无法用 lipo 读取 ${label} 的架构信息: $bin"
    exit 1
  fi

  echo "[INFO] ${label} 架构: ${archs}"
  if [[ "$archs" != *arm64* || "$archs" != *x86_64* ]]; then
    echo "[ERROR] ${label} 不是 Universal 二进制（需同时包含 arm64 与 x86_64，当前: ${archs}）"
    echo "        请删除构建缓存后重新 configure + 完整重编："
    echo "        rm -rf cmake-build-release"
    echo "        cmake -B cmake-build-release -DCMAKE_BUILD_TYPE=Release"
    echo "        cmake --build cmake-build-release --config Release --target LDSJvst_VST3 LDSJvst_AU"
    exit 1
  fi
}

check_universal "VST3" "$VST3_SRC"
check_universal "AU"   "$AU_SRC"

# ---------- 3) 准备目录 ----------
rm -rf "$STAGING_DIR"
mkdir -p "$STAGING_DIR/vst3_root/Library/Audio/Plug-Ins/VST3"
mkdir -p "$STAGING_DIR/au_root/Library/Audio/Plug-Ins/Components"
mkdir -p "$STAGING_DIR/components"
mkdir -p "$STAGING_DIR/resources"
mkdir -p "$DIST_DIR"

# 拷贝产物到 staging 树中的目标绝对路径
# 注意：pkgbuild 用 --root 打包时，root 下的目录结构会 1:1 对应到安装目标机器的根目录
echo "[INFO] 拷贝插件产物到 staging..."
cp -R "$VST3_SRC" "$STAGING_DIR/vst3_root/Library/Audio/Plug-Ins/VST3/"
cp -R "$AU_SRC"   "$STAGING_DIR/au_root/Library/Audio/Plug-Ins/Components/"

# ---------- 4) 分别打两个组件 pkg ----------
VST3_COMPONENT_PKG="$STAGING_DIR/components/CRTloss-VST3.pkg"
AU_COMPONENT_PKG="$STAGING_DIR/components/CRTloss-AU.pkg"

echo "[INFO] pkgbuild → VST3 组件..."
pkgbuild \
  --root "$STAGING_DIR/vst3_root" \
  --identifier "$VST3_PKG_ID" \
  --version "$APP_VERSION" \
  --install-location "/" \
  "$VST3_COMPONENT_PKG"

echo "[INFO] pkgbuild → AU 组件..."
pkgbuild \
  --root "$STAGING_DIR/au_root" \
  --identifier "$AU_PKG_ID" \
  --version "$APP_VERSION" \
  --install-location "/" \
  "$AU_COMPONENT_PKG"

# ---------- 5) 生成 Distribution.xml（内联） ----------
DIST_XML="$STAGING_DIR/Distribution.xml"
cat > "$DIST_XML" <<XML
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>${APP_NAME} ${APP_VERSION}</title>
    <organization>${APP_PUBLISHER}</organization>
    <domains enable_localSystem="true"/>
    <options customize="allow" require-scripts="false" rootVolumeOnly="true" hostArchitectures="x86_64,arm64"/>
    <welcome file="welcome.txt" mime-type="text/plain"/>
    <choices-outline>
        <line choice="default">
            <line choice="vst3"/>
            <line choice="au"/>
        </line>
    </choices-outline>
    <choice id="default" title="${APP_NAME}" description="${APP_NAME} ${APP_VERSION} — CRT 电视机风格丢频音频效果器"/>
    <choice id="vst3" visible="true" title="VST3 插件" description="安装到 /Library/Audio/Plug-Ins/VST3/CRTloss.vst3（所有用户共用）">
        <pkg-ref id="${VST3_PKG_ID}"/>
    </choice>
    <choice id="au" visible="true" title="Audio Unit (AU) 插件" description="安装到 /Library/Audio/Plug-Ins/Components/CRTloss.component（所有用户共用）">
        <pkg-ref id="${AU_PKG_ID}"/>
    </choice>
    <pkg-ref id="${VST3_PKG_ID}" version="${APP_VERSION}" onConclusion="none">CRTloss-VST3.pkg</pkg-ref>
    <pkg-ref id="${AU_PKG_ID}" version="${APP_VERSION}" onConclusion="none">CRTloss-AU.pkg</pkg-ref>
</installer-gui-script>
XML

# 欢迎页文案
cat > "$STAGING_DIR/resources/welcome.txt" <<TXT
CRTloss ${APP_VERSION}

一个模拟老式 CRT 电视机外观的丢频音频效果器。

通用二进制（Universal）：同时支持 Apple Silicon (arm64) 与 Intel (x86_64) Mac。

本安装器将为您安装两种格式的插件（可在下一步自定义勾选）：

  • VST3  → /Library/Audio/Plug-Ins/VST3/CRTloss.vst3
  • AU    → /Library/Audio/Plug-Ins/Components/CRTloss.component

安装完成后，请在您的 DAW 中重新扫描插件目录。

发布方：${APP_PUBLISHER}
TXT

# ---------- 6) productbuild 合成最终安装器 ----------
FINAL_PKG="$DIST_DIR/${APP_NAME}_Setup_${APP_VERSION}_macOS.pkg"
echo "[INFO] productbuild → 最终安装器..."
productbuild \
  --distribution "$DIST_XML" \
  --package-path "$STAGING_DIR/components" \
  --resources "$STAGING_DIR/resources" \
  --identifier "$PRODUCT_PKG_ID" \
  --version "$APP_VERSION" \
  "$FINAL_PKG"

echo "[OK] 已生成安装包: $FINAL_PKG"

# ---------- 7) 可选：封装成 dmg ----------
if [[ "$MAKE_DMG" == "1" ]]; then
  FINAL_DMG="$DIST_DIR/${APP_NAME}_Setup_${APP_VERSION}_macOS.dmg"
  DMG_STAGING="$STAGING_DIR/dmg"
  rm -rf "$DMG_STAGING"
  mkdir -p "$DMG_STAGING"
  cp "$FINAL_PKG" "$DMG_STAGING/"

  # 附带一个简单的 README，便于用户在挂载后看到安装指引
  cat > "$DMG_STAGING/README.txt" <<TXT
CRTloss ${APP_VERSION} — macOS 安装说明

1. 双击 ${APP_NAME}_Setup_${APP_VERSION}_macOS.pkg 启动安装器。
2. 首次运行如遇 Gatekeeper 拦截，请在 系统设置 → 隐私与安全性 中允许运行。
3. 安装完成后在 DAW 中重新扫描 VST3 / AU 插件目录即可看到 CRTloss。

发布方：${APP_PUBLISHER}
TXT

  # 覆盖旧 dmg
  rm -f "$FINAL_DMG"

  echo "[INFO] hdiutil → dmg..."
  hdiutil create \
    -volname "${APP_NAME} ${APP_VERSION}" \
    -srcfolder "$DMG_STAGING" \
    -ov \
    -format UDZO \
    "$FINAL_DMG" >/dev/null

  echo "[OK] 已生成磁盘映像: $FINAL_DMG"
fi

# ---------- 8) 清理 staging ----------
rm -rf "$STAGING_DIR"

echo ""
echo "===================================================================="
echo " 打包完成！输出目录: $DIST_DIR"
ls -lh "$DIST_DIR" | awk 'NR>1 {print "   " $9 "   " $5}'
echo "===================================================================="
