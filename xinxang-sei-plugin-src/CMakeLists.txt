cmake_minimum_required(VERSION 3.28...3.30)

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
          xinxang-sei-internal.h)

target_link_libraries(xinxang-sei-plugin PRIVATE OBS::libobs OBS::frontend-api Qt::Widgets)

set_target_properties_obs(xinxang-sei-plugin PROPERTIES FOLDER plugins/xinxang-sei PREFIX "")

