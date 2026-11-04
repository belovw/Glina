# VST3 (+AU on macOS) ARA2 plugin. JUCE and the ARA SDK come from cmake/Juce.cmake.
set(GLINA_FORMATS VST3)
if(APPLE)
  list(APPEND GLINA_FORMATS AU)
endif()

juce_add_plugin(GlinaPlugin
  PRODUCT_NAME "Glina"
  COMPANY_NAME "Belovw"
  COMPANY_WEBSITE "https://github.com/belovw"
  BUNDLE_ID "com.belovw.glina"
  VERSION ${PROJECT_VERSION}
  PLUGIN_MANUFACTURER_CODE Belw
  PLUGIN_CODE Glna
  FORMATS ${GLINA_FORMATS}
  IS_SYNTH FALSE
  NEEDS_MIDI_INPUT FALSE
  IS_ARA_EFFECT TRUE
  ARA_FACTORY_ID "\"com.belovw.glina.arafactory\""
  ARA_DOCUMENT_ARCHIVE_ID "\"com.belovw.glina.aradocumentarchive.1\""
  EDITOR_WANTS_KEYBOARD_FOCUS TRUE
  VST3_CATEGORIES Fx Pitch Shift
  COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(GlinaPlugin PRIVATE
  Source/GlinaModel.cpp
  Source/PlaybackRenderer.cpp
  Source/PluginProcessor.cpp
  Source/PluginEditor.cpp
  ${PROJECT_SOURCE_DIR}/ui/PitchEditor.cpp)

glina_juce_defaults(GlinaPlugin)

target_link_libraries(GlinaPlugin
  PRIVATE
    glina_core
    juce::juce_audio_processors
    juce::juce_audio_formats
    juce::juce_gui_basics
  PUBLIC
    juce::juce_recommended_config_flags)
