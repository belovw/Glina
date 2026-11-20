# VST3 (+AU on macOS) ARA2 plugin. JUCE and the ARA SDK come from cmake/Juce.cmake.
set(GLINA_FORMATS VST3)
if(APPLE)
  list(APPEND GLINA_FORMATS AU)
endif()

# Test build: logs what the host reports (Desktop/glina_host_test.txt). Gets its own name and IDs,
# so it installs next to the real Glina instead of replacing it.
option(GLINA_HOST_PROBE "Test build: log IHostApplication / REAPER project path to the Desktop" OFF)
if(GLINA_HOST_PROBE)
  set(GLINA_PLUGIN_NAME "GlinaProbe")
  set(GLINA_PLUGIN_ID_SUFFIX ".probe")
  set(GLINA_PLUGIN_CODE GlPr)
else()
  set(GLINA_PLUGIN_NAME "Glina")
  set(GLINA_PLUGIN_ID_SUFFIX "")
  set(GLINA_PLUGIN_CODE Glna)
endif()

juce_add_plugin(GlinaPlugin
  PRODUCT_NAME "${GLINA_PLUGIN_NAME}"
  COMPANY_NAME "Belovw"
  COMPANY_WEBSITE "https://github.com/belovw"
  BUNDLE_ID "com.belovw.glina${GLINA_PLUGIN_ID_SUFFIX}"
  VERSION ${PROJECT_VERSION}
  PLUGIN_MANUFACTURER_CODE Belw
  PLUGIN_CODE ${GLINA_PLUGIN_CODE}
  FORMATS ${GLINA_FORMATS}
  IS_SYNTH FALSE
  NEEDS_MIDI_INPUT FALSE
  IS_ARA_EFFECT TRUE
  ARA_FACTORY_ID "\"com.belovw.glina${GLINA_PLUGIN_ID_SUFFIX}.arafactory\""
  ARA_DOCUMENT_ARCHIVE_ID "\"com.belovw.glina${GLINA_PLUGIN_ID_SUFFIX}.aradocumentarchive.1\""
  EDITOR_WANTS_KEYBOARD_FOCUS TRUE
  VST3_CATEGORIES Fx Pitch Shift
  COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(GlinaPlugin PRIVATE
  Source/GlinaModel.cpp
  Source/PlaybackRenderer.cpp
  Source/PluginProcessor.cpp
  Source/PluginEditor.cpp
  ${PROJECT_SOURCE_DIR}/ui/PitchEditor.cpp)

if(GLINA_HOST_PROBE)
  target_sources(GlinaPlugin PRIVATE Source/ReaperHostProbe.cpp)
  target_compile_definitions(GlinaPlugin PRIVATE GLINA_HOST_PROBE=1)
endif()

glina_juce_defaults(GlinaPlugin)

target_link_libraries(GlinaPlugin
  PRIVATE
    glina_core
    juce::juce_audio_processors
    juce::juce_audio_formats
    juce::juce_gui_basics
  PUBLIC
    juce::juce_recommended_config_flags)
