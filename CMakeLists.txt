cmake_minimum_required(VERSION 3.22)
project(Glina VERSION 0.3.0 LANGUAGES C CXX)
# One version and build id for every target (shown in the Help overlay): the version from
# project(), and the short commit hash. Re-configure when HEAD moves, so the hash stays current.
execute_process(COMMAND git rev-parse --short HEAD WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                OUTPUT_VARIABLE GLINA_GIT_HASH OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(GLINA_GIT_HASH STREQUAL "")
  set(GLINA_GIT_HASH "unknown")
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/.git/HEAD")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/.git/HEAD")
endif()
add_compile_definitions(GLINA_VERSION_STRING="${PROJECT_VERSION}" GLINA_BUILD_STRING="${GLINA_GIT_HASH}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(GLINA_BUILD_CLI "Build the command-line tool" ON)
option(GLINA_BUILD_TESTS "Build engine tests" ON)
option(GLINA_BUILD_PLUGIN "Build the VST3/ARA2 plugin (needs JUCE + ARA SDK)" OFF)
option(GLINA_BUILD_APP "Build the standalone editor (needs JUCE)" OFF)

# Static C++ runtime on Windows: Glina.exe and Glina.vst3 run without the
# Visual C++ Redistributable being installed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC)
  # Sources are UTF-8 (Russian UI strings); M_PI etc. from <cmath>.
  add_compile_options(/utf-8)
  add_compile_definitions(_USE_MATH_DEFINES NOMINMAX)
endif()

# ---- Engine (no dependencies: shared by CLI, tests and plugin) ----------------
add_library(glina_core STATIC
  core/src/FFT.cpp
  core/src/PitchDetector.cpp
  core/src/Analysis.cpp
  core/src/Renderer.cpp
  core/src/Tuning.cpp
  core/src/Notes.cpp
  core/src/Harmony.cpp
  core/src/Document.cpp
  core/src/Editing.cpp
  core/src/Transfer.cpp
  core/src/Thread.cpp
  core/src/TrackView.cpp)
target_include_directories(glina_core PUBLIC core/include)
find_package(Threads REQUIRED)
target_link_libraries(glina_core PUBLIC Threads::Threads)
if(MSVC)
  target_compile_options(glina_core PRIVATE /W3)
else()
  target_compile_options(glina_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

if(GLINA_BUILD_CLI)
  add_executable(glina cli/main.cpp cli/WavIO.cpp)
  target_link_libraries(glina PRIVATE glina_core)
endif()

if(GLINA_BUILD_TESTS)
  enable_testing()
  add_executable(glina_tests tests/test_core.cpp)
  target_link_libraries(glina_tests PRIVATE glina_core)
  add_test(NAME core COMMAND glina_tests)
  add_executable(glina_synth tests/synth_main.cpp cli/WavIO.cpp)
endif()

if(GLINA_BUILD_APP OR GLINA_BUILD_PLUGIN)
  include(cmake/Juce.cmake)
endif()
if(GLINA_BUILD_APP)
  add_subdirectory(app)
endif()
if(GLINA_BUILD_PLUGIN)
  add_subdirectory(plugin)
endif()
