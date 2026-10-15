cmake_minimum_required(VERSION 3.28...3.30)

# 版本号约定（独立于 OBS）：
#   1.0.x  初版：H.264 SEI 注入
#   1.1.x  补 ENABLE_HEVC，H.265 生效
#   1.2.x  内置 SNTP 校时 + 发布包装对齐 OBS 32.2
#   1.2.1  保存设置不再拆掉 SNTP；客户端名改为定长缓冲；修正推流输出引用泄漏
set(XINXANG_SEI_VERSION_MAJOR 1)
set(XINXANG_SEI_VERSION_MINOR 2)
set(XINXANG_SEI_VERSION_PATCH 1)
set(XINXANG_SEI_VERSION
    "${XINXANG_SEI_VERSION_MAJOR}.${XINXANG_SEI_VERSION_MINOR}.${XINXANG_SEI_VERSION_PATCH}")

option(ENABLE_XINXANG_SEI "Enable XinXiang SEI frame sync plugin" ON)
if(NOT ENABLE_XINXANG_SEI)
  add_custom_target(xinxang-sei-plugin)
  target_disable(xinxang-sei-plugin)
  return()
endif()

add_library(xinxang-sei-plugin MODULE)
add_library(OBS::xinxang-sei-plugin ALIAS xinxang-sei-plugin)

find_package(Qt6 REQUIRED Widgets)

target_sources(
  xinxang-sei-plugin
  PRIVATE xinxang-sei-plugin.cpp
          xinxang-sei-inject.c
          xinxang-sei-inject.h
          xinxang-sei-internal.h
          xinxang-sntp.c
          xinxang-sntp.h)

target_link_libraries(xinxang-sei-plugin PRIVATE OBS::libobs OBS::frontend-api Qt::Widgets)

# SNTP 校时需要 UDP socket：Windows 上要链 Winsock
if(WIN32)
  target_link_libraries(xinxang-sei-plugin PRIVATE ws2_32)
endif()

# Enables the H.265/HEVC prefix-SEI injection branch in xinxang-sei-inject.c
# (#ifdef ENABLE_HEVC). Without this definition the branch is preprocessed away
# and HEVC outputs silently get no SEI at all.
target_compile_definitions(
  xinxang-sei-plugin
  PRIVATE ENABLE_HEVC
          XINXANG_SEI_VERSION="${XINXANG_SEI_VERSION}")

# 源文件是 UTF-8（含中文注释与日志字符串）。MSVC 默认按当前代码页(936)读取，
# 会把中文字符串截断并报 C4819 / C2001「常量中有换行符」。显式指定 /utf-8 兜底。
if(MSVC)
  target_compile_options(xinxang-sei-plugin PRIVATE /utf-8)
endif()

# Windows VERSIONINFO：右键 DLL → 详细信息 可见公司/版本
if(OS_WINDOWS)
  configure_file(cmake/windows/obs-module.rc.in xinxang-sei-plugin.rc)
  target_sources(xinxang-sei-plugin PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/xinxang-sei-plugin.rc")
endif()

set_target_properties_obs(xinxang-sei-plugin PROPERTIES FOLDER plugins/xinxang-sei PREFIX "")

