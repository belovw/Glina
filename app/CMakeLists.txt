# Standalone editor (Windows / Linux / macOS).
juce_add_gui_app(GlinaApp
  PRODUCT_NAME "Glina"
  COMPANY_NAME "Belovw"
  COMPANY_WEBSITE "https://github.com/belovw"
  BUNDLE_ID "com.belovw.glina.app"
  VERSION ${PROJECT_VERSION})

target_sources(GlinaApp PRIVATE
  Main.cpp
  Session.cpp
  ${PROJECT_SOURCE_DIR}/ui/PitchEditor.cpp)

glina_juce_defaults(GlinaApp)
target_compile_definitions(GlinaApp PRIVATE
  JUCE_USE_MP3AUDIOFORMAT=1
  JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:GlinaApp,JUCE_PRODUCT_NAME>"
  JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:GlinaApp,JUCE_VERSION>")

target_link_libraries(GlinaApp
  PRIVATE
    glina_core
    juce::juce_audio_utils
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_gui_extra
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags)
