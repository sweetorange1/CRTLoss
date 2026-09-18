cmake_minimum_required(VERSION 3.22)

# macOS：默认构建 Universal 2 二进制（arm64 + x86_64），同一个 bundle 同时支持
# Apple Silicon 与 Intel。必须在 project() 之前设置才会被 CMake 用于编译器配置。
# 需要单架构时用 -DCMAKE_OSX_ARCHITECTURES=arm64 覆盖即可（Windows / Linux 下此设置被忽略）。
if (NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
endif()
if (NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13")
endif()

project(LDSJvst VERSION 1.5.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
FetchContent_Declare(
        juce
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.12
)
FetchContent_MakeAvailable(juce)

add_subdirectory(grid_plugin)

juce_add_plugin(
        LDSJvst
        PRODUCT_NAME "CRTloss"
        VERSION 1.5.0
        COMPANY_NAME "iisaacbeats.cn"
        PLUGIN_MANUFACTURER_CODE Ldsj
        PLUGIN_CODE Ldsj
        FORMATS VST3 AU Standalone
        IS_SYNTH FALSE
        NEEDS_MIDI_INPUT FALSE
        NEEDS_MIDI_OUTPUT FALSE
        VST3_AUTO_MANIFEST FALSE
        COPY_PLUGIN_AFTER_BUILD TRUE
        NEEDS_CURL TRUE
)

juce_generate_juce_header(LDSJvst)

juce_add_binary_data(
        LDSJvst_BinaryData
        SOURCES
        assets/TV.png
        assets/BYPASS.png
        assets/remote_control.png
        assets/remote_control_light.png
)

target_compile_definitions(LDSJvst
  PUBLIC
    JUCE_VST3_CAN_REPLACE_VST2=0
    # https 请求（遥测 / 更新检查）依赖 CURL 后端，关闭会静默失败
    JUCE_USE_CURL=1
)

target_sources(LDSJvst PRIVATE
  PluginProcessor.cpp
  PluginProcessor.h
  PluginEditor.cpp
  PluginEditor.h
  shared/IisaacTelemetry.h
  source/network/Version.h
  source/network/Version.cpp
  source/network/UpdateChecker.h
  source/network/UpdateChecker.cpp
  source/ui/UpdateDialog.h
  source/ui/UpdateDialog.cpp
)

target_link_libraries(LDSJvst PRIVATE grid::grid_plugin juce::juce_audio_utils juce::juce_dsp LDSJvst_BinaryData)
