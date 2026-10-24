cmake_minimum_required(VERSION 3.22)

# macOS: one universal binary (Apple Silicon + Intel), macOS 11 and later. Set before
# project() so the toolchain picks them up (the "mac" preset sets the same).
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Architectures of the macOS build")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Oldest macOS KERF runs on")
endif()

project(KERF VERSION 1.1.0 LANGUAGES C CXX)

# Shown to people (About, Check for updates); the plain number above is what versions compare.
set(KERF_VERSION_LABEL "${PROJECT_VERSION}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# Static MSVC runtime: the plugin DLL does not depend on the user having the
# matching Visual C++ redistributable installed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

#------------------------------------------------------------------------------
# Dependencies (pinned versions; their licences: docs/DEPENDENCIES.md)
#------------------------------------------------------------------------------
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# Set KERF_JUCE_DIR to a local JUCE checkout to avoid downloading.
set(KERF_JUCE_DIR "" CACHE PATH "Optional local JUCE checkout")

if(KERF_JUCE_DIR)
    add_subdirectory(${KERF_JUCE_DIR} ${CMAKE_BINARY_DIR}/juce EXCLUDE_FROM_ALL)
else()
    FetchContent_Declare(juce
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        8.0.15
        GIT_SHALLOW    TRUE
        GIT_PROGRESS   TRUE)
    FetchContent_MakeAvailable(juce)
endif()

# Signalsmith Stretch (MIT) - pitch-independent time-stretching. Pulls in Signalsmith
# Linear (MIT) itself. Set KERF_SIGNALSMITH_DIR to use a local checkout.
set(KERF_SIGNALSMITH_DIR "" CACHE PATH "Optional local signalsmith-stretch checkout")
if(KERF_SIGNALSMITH_DIR)
    add_subdirectory(${KERF_SIGNALSMITH_DIR} ${CMAKE_BINARY_DIR}/signalsmith-stretch EXCLUDE_FROM_ALL)
else()
    FetchContent_Declare(signalsmith_stretch
        GIT_REPOSITORY https://github.com/Signalsmith-Audio/signalsmith-stretch.git
        GIT_TAG        1.4.0
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(signalsmith_stretch)
endif()

#------------------------------------------------------------------------------
# Plugin
#------------------------------------------------------------------------------
set(KERF_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND KERF_FORMATS AU)
endif()

juce_add_plugin(Kerf
    PRODUCT_NAME                "KERF"
    COMPANY_NAME                "Kerf Instruments"
    BUNDLE_ID                   "com.kerfinstruments.kerf"
    PLUGIN_MANUFACTURER_CODE    Kerf
    PLUGIN_CODE                 Krf1
    FORMATS                     ${KERF_FORMATS}
    IS_SYNTH                    TRUE
    NEEDS_MIDI_INPUT            TRUE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD     FALSE
    VST3_CATEGORIES             Instrument Sampler
    AU_MAIN_TYPE                kAudioUnitType_MusicDevice
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT  "KERF can record audio input into a new sample."
    # macOS notarisation needs the hardened runtime; recording needs the input entitlement.
    HARDENED_RUNTIME_ENABLED    TRUE
    HARDENED_RUNTIME_OPTIONS    com.apple.security.device.audio-input)

# Core (UI-independent) sources are shared with the unit-test runner.
set(KERF_CORE_SOURCES
    Source/AudioEngine/ChopGrid.cpp
    Source/AudioEngine/ChopModel.cpp
    Source/AudioEngine/SampleVoice.cpp
    Source/AudioEngine/SamplerEngine.cpp
    Source/AudioEngine/GrainCloud.cpp
    Source/AudioEngine/TimeStretch.cpp
    Source/Arrangement/PatternModel.cpp
    Source/Arrangement/PatternPlayer.cpp
    Source/Arrangement/PatternDice.cpp
    Source/Arrangement/DiceStyles.cpp
    Source/Arrangement/DiceGenerator.cpp
    Source/Arrangement/DiceEvaluator.cpp
    Source/Arrangement/AudioRecorder.cpp
    Source/AudioEngine/ChopTools.cpp
    Source/AudioEngine/SampleLoader.cpp
    Source/Analysis/WaveformCache.cpp
    Source/Analysis/TransientDetector.cpp
    Source/Analysis/MusicalAnalysis.cpp
    Source/Analysis/SpectrumAnalyzer.cpp
    Source/Effects/PingPongDelay.cpp
    Source/Effects/ToneProcessor.cpp
    Source/Effects/DirtProcessor.cpp
    Source/Effects/WobbleProcessor.cpp
    Source/Effects/EqProcessor.cpp
    Source/Effects/PumpProcessor.cpp
    Source/Effects/TimeShaper.cpp
    Source/Effects/GrainHaze.cpp
    Source/Parameters.cpp
    Source/FactoryPresets.cpp
    Source/UpdateCheck.cpp)

set(KERF_PLUGIN_SOURCES
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/UI/Theme.cpp
    Source/UI/Controls.cpp
    Source/UI/WaveformView.cpp
    Source/UI/ChopStrip.cpp
    Source/UI/ChopToolbar.cpp
    Source/UI/HeaderBar.cpp
    Source/UI/SawBlade.cpp
    Source/UI/StartBurst.cpp
    Source/UI/DropVortex.cpp
    Source/UI/TimeView.cpp
    Source/UI/ControlPanel.cpp
    Source/UI/PatternView.cpp
    Source/UI/RecordOverlay.cpp
    Source/UI/AboutOverlay.cpp
    Source/UI/UpdateOverlay.cpp
    Source/UI/SavePresetOverlay.cpp
    Source/UI/GuideOverlay.cpp
    Source/UI/GrainView.cpp
    Source/UI/FxView.cpp
    Source/UI/EqView.cpp)

juce_add_binary_data(KerfBinaryData SOURCES
    Resources/Fonts/Inter-Regular.ttf
    Resources/Fonts/Inter-Medium.ttf
    Resources/Fonts/Inter-SemiBold.ttf
    Resources/Fonts/JetBrainsMono-Regular.ttf
    Resources/Fonts/JetBrainsMono-Medium.ttf
    Resources/Fonts/Inter-OFL.txt
    Resources/Fonts/JetBrainsMono-OFL.txt
    Resources/Licenses/SignalsmithStretch.txt
    Resources/Licenses/SignalsmithLinear.txt
    Resources/Licenses/VST3_SDK.txt)

target_sources(Kerf PRIVATE ${KERF_CORE_SOURCES} ${KERF_PLUGIN_SOURCES})
target_include_directories(Kerf PRIVATE Source)

set(KERF_COMMON_DEFINITIONS
    KERF_VERSION="${PROJECT_VERSION}"
    KERF_VERSION_LABEL="${KERF_VERSION_LABEL}"
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
    JUCE_USE_MP3AUDIOFORMAT=0)

target_compile_definitions(Kerf PUBLIC ${KERF_COMMON_DEFINITIONS})

set(KERF_JUCE_MODULES
    juce::juce_audio_utils
    juce::juce_audio_formats
    juce::juce_dsp
    signalsmith-stretch)

target_link_libraries(Kerf
    PRIVATE
        KerfBinaryData
        ${KERF_JUCE_MODULES}
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

#------------------------------------------------------------------------------
# Tests (only in the developer's working copy)
#------------------------------------------------------------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Tests/Tests.cmake")
    include("${CMAKE_CURRENT_SOURCE_DIR}/Tests/Tests.cmake")
endif()
