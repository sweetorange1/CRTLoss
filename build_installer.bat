@echo off
setlocal EnableExtensions

set "SCRIPT_DIR=%~dp0"
set "ISS_FILE=%SCRIPT_DIR%CRTloss_installer.iss"
REM 与 iss 中 [Files] Source 保持一致：Release 版 VST3 产物目录
set "VST3_SRC_DIR=%SCRIPT_DIR%cmake-build-release-visual-studio\LDSJvst_artefacts\Release\VST3"
set "VST3_BIN=%VST3_SRC_DIR%\CRTloss.vst3\Contents\x86_64-win\CRTloss.vst3"

if not exist "%ISS_FILE%" (
  echo [ERROR] 未找到安装脚本: "%ISS_FILE%"
  exit /b 1
)

REM ---------- 1) 校验已存在 Release 版 VST3 产物 ----------
if not exist "%VST3_BIN%" (
  echo [ERROR] 未找到 Release 版 VST3 产物:
  echo         "%VST3_BIN%"
  echo         请先在 CLion 中以 Release 配置构建 LDSJvst_VST3 目标后再运行本脚本。
  exit /b 1
)

REM ---------- 2) 定位 ISCC ----------
set "ISCC_PATH=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
if not exist "%ISCC_PATH%" set "ISCC_PATH=%ProgramFiles%\Inno Setup 6\ISCC.exe"
if not exist "%ISCC_PATH%" set "ISCC_PATH=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"

if not exist "%ISCC_PATH%" (
  echo [ERROR] 未找到 ISCC.exe，请先安装 Inno Setup 6。
  echo         你可以运行: winget install --id JRSoftware.InnoSetup -e
  exit /b 1
)

REM ---------- 3) 调用 ISCC 打包 ----------
echo [INFO] 使用编译器: "%ISCC_PATH%"
echo [INFO] VST3 源目录: "%VST3_SRC_DIR%"
echo [INFO] 开始打包:   "%ISS_FILE%"
"%ISCC_PATH%" "%ISS_FILE%"

if errorlevel 1 (
  echo [ERROR] 打包失败，请查看上方日志。
  exit /b 1
)

echo [OK] 打包完成，输出目录通常为: "%SCRIPT_DIR%dist"
endlocal
exit /b 0
