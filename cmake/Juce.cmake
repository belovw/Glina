# JUCE (and the ARA SDK for the plugin). Pass local checkouts to avoid downloads:
#   -DGLINA_JUCE_DIR=/path/to/JUCE -DGLINA_ARA_SDK_DIR=/path/to/ARA_SDK
include(FetchContent)

set(GLINA_JUCE_DIR "" CACHE PATH "Path to a JUCE checkout (9.x)")
set(GLINA_ARA_SDK_DIR "" CACHE PATH "Path to an ARA SDK checkout (2.3)")

if(GLINA_JUCE_DIR)
  add_subdirectory(${GLINA_JUCE_DIR} ${CMAKE_BINARY_DIR}/juce EXCLUDE_FROM_ALL)
else()
  FetchContent_Declare(juce GIT_REPOSITORY https://github.com/juce-framework/JUCE.git GIT_TAG 9.0.5 GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(juce)
endif()

if(GLINA_BUILD_PLUGIN)
  if(NOT GLINA_ARA_SDK_DIR)
    FetchContent_Declare(ara_sdk GIT_REPOSITORY https://github.com/Celemony/ARA_SDK.git GIT_TAG main
                         GIT_SHALLOW TRUE GIT_SUBMODULES_RECURSE TRUE)
    FetchContent_MakeAvailable(ara_sdk) # no top-level CMakeLists: just downloads
    set(GLINA_ARA_SDK_DIR ${ara_sdk_SOURCE_DIR})
  endif()
  juce_set_ara_sdk_path(${GLINA_ARA_SDK_DIR})
endif()

# Build machines without the optional Linux -dev packages (X11 extensions, ALSA).
option(GLINA_MINIMAL_LINUX_DEPS "Disable optional X11 extensions and ALSA/JACK (headless build machines)" OFF)
function(glina_juce_defaults target)
  target_compile_definitions(${target} PUBLIC
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
  if(GLINA_MINIMAL_LINUX_DEPS)
    target_compile_definitions(${target} PUBLIC JUCE_USE_XRANDR=0 JUCE_USE_XINERAMA=0 JUCE_USE_XCURSOR=0
                                                JUCE_USE_XINPUT=0 JUCE_USE_XSHM=0 JUCE_USE_XRENDER=0
                                                JUCE_ALSA=0 JUCE_JACK=0)
  endif()
  if(MSVC)
    target_compile_options(${target} PRIVATE /utf-8)
  endif()
  target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/ui)
endfunction()
