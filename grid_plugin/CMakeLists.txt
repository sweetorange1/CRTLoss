cmake_minimum_required(VERSION 3.22)

project(grid_plugin LANGUAGES CXX)

# 这是一个“占位”实现：提供与主工程一致的 target 名称，确保 CMake 能顺利配置与链接。
# 如需恢复真实的 grid_plugin 功能，请用原始库代码替换本目录内容。

add_library(grid_plugin INTERFACE)
add_library(grid::grid_plugin ALIAS grid_plugin)

target_compile_features(grid_plugin INTERFACE cxx_std_17)
