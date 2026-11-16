cmake_minimum_required(VERSION 3.22)

if(CMAKE_HOST_APPLE)
  set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures")
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
endif()

project(SculptClean VERSION 0.2.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(SCULPT_JUCE_SOURCE_DIR "" CACHE PATH "Optional local JUCE 8.0.4 directory")
if(SCULPT_JUCE_SOURCE_DIR)
  add_subdirectory("${SCULPT_JUCE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/JUCE")
else()
  include(FetchContent)
  FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.4
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(JUCE)
endif()

option(SCULPT_BUILD_STUDIO "Build the offline Sculpt Clean Studio editor" ON)
if(SCULPT_BUILD_STUDIO)
  include(cmake/RNNoise.cmake)
  include(cmake/Cathar.cmake)
  juce_add_gui_app(SculptCleanStudio
    COMPANY_NAME "Sculpt"
    BUNDLE_ID "com.sculpt.clean.studio"
    PRODUCT_NAME "Sculpt Clean Studio")
  target_sources(SculptCleanStudio PRIVATE
    Studio/Main.cpp Source/OfflineTools.h Source/OfflineRepair.h Source/VoiceDenoiser.h)
  target_compile_definitions(SculptCleanStudio PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_USE_MP3AUDIOFORMAT=1)
  target_link_libraries(SculptCleanStudio PRIVATE
    SculptRNNoise juce::juce_audio_utils juce::juce_dsp
    juce::juce_recommended_config_flags juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)
  if(APPLE)
    target_link_libraries(SculptCleanStudio PRIVATE "-framework Accelerate")
    add_dependencies(SculptCleanStudio SculptCathar)
    add_custom_command(TARGET SculptCleanStudio POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:SculptCleanStudio>/Resources"
      COMMAND ${CMAKE_COMMAND} -E copy "${SCULPT_CATHAR_BINARY}" "$<TARGET_BUNDLE_CONTENT_DIR:SculptCleanStudio>/Resources/cathar"
      COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty" "$<TARGET_BUNDLE_CONTENT_DIR:SculptCleanStudio>/Resources/ThirdParty"
      COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_CONTENT_DIR:SculptCleanStudio>/Resources/stems"
      COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/sculpt-stems.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/setup-stems.sh"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/requirements-stems.txt"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/stems-models.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/stems-linux-wheels.json"
        "$<TARGET_BUNDLE_CONTENT_DIR:SculptCleanStudio>/Resources/stems"
      VERBATIM)
  endif()
endif()

juce_add_plugin(SculptClean
  COMPANY_NAME "Sculpt"
  BUNDLE_ID "com.sculpt.clean"
  PRODUCT_NAME "Sculpt Clean"
  PLUGIN_MANUFACTURER_CODE Scpt
  PLUGIN_CODE Scln
  FORMATS VST3
  VST3_CATEGORIES Fx Restoration
  IS_SYNTH FALSE
  IS_MIDI_EFFECT FALSE
  NEEDS_MIDI_INPUT FALSE
  NEEDS_MIDI_OUTPUT FALSE
  EDITOR_WANTS_KEYBOARD_FOCUS FALSE
  COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(SculptClean PRIVATE
  Source/CleanEngine.h
  Source/SpectralCleaner.h
  Source/SpectralDereverb.h
  Source/ClickRepair.h
  Source/HumRemover.h
  Source/SourceExtractor.h
  Source/PluginProcessor.h
  Source/PluginProcessor.cpp
  Source/PluginEditor.h
  Source/PluginEditor.cpp)

target_compile_definitions(SculptClean PUBLIC
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(SculptClean
  PRIVATE juce::juce_audio_processors
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

if(APPLE)
  target_link_libraries(SculptClean PUBLIC "-framework Accelerate")
endif()

option(SCULPT_BUILD_STATE_TESTS "Build the JUCE state and host-bypass checks" OFF)
if(SCULPT_BUILD_STATE_TESTS)
  enable_testing()
  juce_add_console_app(SculptCleanStateTests PRODUCT_NAME "SculptCleanStateTests")
  # Compile the same wrapper sources directly: linking the plug-in shared-code
  # target would export JUCE_STANDALONE_APPLICATION=0 into this console app,
  # which JUCE correctly builds with JUCE_STANDALONE_APPLICATION=1.
  target_sources(SculptCleanStateTests PRIVATE
    Tests/plugin_state_test.cpp
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)
  target_include_directories(SculptCleanStateTests PRIVATE Source)
  target_compile_definitions(SculptCleanStateTests PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)
  target_link_libraries(SculptCleanStateTests PRIVATE
    juce::juce_audio_processors
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)
  if(APPLE)
    target_link_libraries(SculptCleanStateTests PRIVATE "-framework Accelerate")
  endif()
  add_test(NAME plugin-state COMMAND $<TARGET_FILE:SculptCleanStateTests>)
  set_tests_properties(plugin-state PROPERTIES TIMEOUT 60)
endif()
