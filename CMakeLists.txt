cmake_minimum_required(VERSION 3.16)
project(obs-cue-chapters VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(TESS REQUIRED IMPORTED_TARGET tesseract lept)

add_library(obs-cue-chapters MODULE src/plugin.cpp)
set_target_properties(obs-cue-chapters PROPERTIES PREFIX "")
target_link_libraries(obs-cue-chapters PRIVATE OBS::libobs OBS::obs-frontend-api PkgConfig::TESS ${CMAKE_DL_LIBS})

# Installation in the per-user OBS plugin directory:
#   ~/.config/obs-studio/plugins/obs-cue-chapters/bin/64bit/obs-cue-chapters.so
set(USER_PLUGIN_DIR "$ENV{HOME}/.config/obs-studio/plugins/obs-cue-chapters/bin/64bit")
install(TARGETS obs-cue-chapters LIBRARY DESTINATION "${USER_PLUGIN_DIR}")
