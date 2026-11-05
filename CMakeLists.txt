cmake_minimum_required(VERSION 3.22)

project(ScribaPitch VERSION 0.5.5 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCE: use a local copy in ./JUCE if present, otherwise download it.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/JUCE/CMakeLists.txt")
    add_subdirectory(JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.4
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

option(SCRIBA_PITCH_BUILD_TESTS "Build the engine test console app" ON)

# ARA2 (instant reading of the track in Logic, Cubase, Studio One, Reaper...).
# Needs the ARA SDK: git clone --recursive --branch releases/2.2.0 https://github.com/Celemony/ARA_SDK
set(SCRIBA_ARA_SDK "${CMAKE_CURRENT_SOURCE_DIR}/ARA_SDK" CACHE PATH "ARA SDK folder")
if(EXISTS "${SCRIBA_ARA_SDK}/ARA_API/ARAInterface.h")
    juce_set_ara_sdk_path("${SCRIBA_ARA_SDK}")
    set(SCRIBA_ARA TRUE)
    message(STATUS "Scriba Pitch: ARA2 enabled")
else()
    set(SCRIBA_ARA FALSE)
    message(WARNING "Scriba Pitch: ARA SDK not found in ${SCRIBA_ARA_SDK} - building without ARA2")
endif()

# ---------------------------------------------------------------------------
# Plugin
# ---------------------------------------------------------------------------
juce_add_plugin(ScribaPitch
    COMPANY_NAME "Meteora Plugins"
    COMPANY_WEBSITE "https://meteoraplugins.gumroad.com"
    COMPANY_EMAIL "meteora_plugins@aol.com"
    PLUGIN_MANUFACTURER_CODE Mtor
    PLUGIN_CODE ScPt
    FORMATS VST3 AU Standalone
    PRODUCT_NAME "Scriba Pitch"
    BUNDLE_ID com.meteoraplugins.scribapitch
    DESCRIPTION "Vocal pitch editor with automatic key correction"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    VST3_CATEGORIES Fx Pitch\ Shift
    AU_MAIN_TYPE kAudioUnitType_Effect
    IS_ARA_EFFECT ${SCRIBA_ARA}
    ARA_FACTORY_ID "\"com.meteoraplugins.scribapitch.arafactory\""
    ARA_DOCUMENT_ARCHIVE_ID "\"com.meteoraplugins.scribapitch.aradocument.1\""
    COPY_PLUGIN_AFTER_BUILD FALSE)

juce_add_binary_data(ScribaPitchData
    HEADER_NAME BinaryData.h
    NAMESPACE BinaryData
    SOURCES
        Resources/logo_light.png
        Resources/Nunito-SemiBold.ttf
        Resources/Nunito-Bold.ttf
        Resources/Nunito-Black.ttf)

set(SCRIBA_ENGINE_SOURCES
    Source/Engine/PitchTypes.cpp
    Source/Engine/Analyzer.cpp
    Source/Engine/Renderer.cpp)

target_sources(ScribaPitch PRIVATE
    ${SCRIBA_ENGINE_SOURCES}
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/ARA/ScribaARA.cpp
    Source/UI/Theme.cpp
    Source/UI/PianoRoll.cpp
    Source/UI/Widgets.cpp
    Source/Host/ReaperHost.cpp)

target_include_directories(ScribaPitch PRIVATE Source
    "${JUCE_MODULES_DIR}/juce_audio_processors/format_types/VST3_SDK")

target_compile_definitions(ScribaPitch PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0)

# Linux is only used here to check that everything compiles.
if(UNIX AND NOT APPLE)
    target_compile_definitions(ScribaPitch PUBLIC
        JUCE_ALSA=0 JUCE_JACK=0
        JUCE_USE_XRANDR=0 JUCE_USE_XINERAMA=0 JUCE_USE_XCURSOR=0
        JUCE_USE_XSHM=0 JUCE_USE_XRENDER=0
        JUCE_MODAL_LOOPS_PERMITTED=1)   # only for the headless test on Linux
endif()

target_link_libraries(ScribaPitch
    PRIVATE
        ScribaPitchData
        juce::juce_audio_utils
        juce::juce_audio_formats
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------------------
# Engine test (no GUI): synthesises an out-of-tune voice, corrects it and
# checks the result.
# ---------------------------------------------------------------------------
if(SCRIBA_PITCH_BUILD_TESTS)
    juce_add_console_app(ScribaPitchEngineTest PRODUCT_NAME "ScribaPitchEngineTest")
    target_sources(ScribaPitchEngineTest PRIVATE ${SCRIBA_ENGINE_SOURCES} Tests/EngineTest.cpp)
    target_include_directories(ScribaPitchEngineTest PRIVATE Source)
    target_compile_definitions(ScribaPitchEngineTest PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(ScribaPitchEngineTest
        PRIVATE juce::juce_core juce::juce_audio_basics juce::juce_audio_formats juce::juce_dsp
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)


    # Sound quality on a realistic voice (roughness / clicks after shifting)
    juce_add_console_app(ScribaPitchQualityTest PRODUCT_NAME "ScribaPitchQualityTest")
    target_sources(ScribaPitchQualityTest PRIVATE ${SCRIBA_ENGINE_SOURCES} Tests/QualityTest.cpp)
    target_include_directories(ScribaPitchQualityTest PRIVATE Source)
    target_compile_definitions(ScribaPitchQualityTest PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(ScribaPitchQualityTest
        PRIVATE juce::juce_core juce::juce_audio_basics juce::juce_audio_formats juce::juce_dsp
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

    # Engine on a real vocal file: ScribaPitchFileTest <vocal.wav> <outDir>
    juce_add_console_app(ScribaPitchFileTest PRODUCT_NAME "ScribaPitchFileTest")
    target_sources(ScribaPitchFileTest PRIVATE ${SCRIBA_ENGINE_SOURCES} Tests/FileTest.cpp)
    target_include_directories(ScribaPitchFileTest PRIVATE Source)
    target_compile_definitions(ScribaPitchFileTest PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(ScribaPitchFileTest
        PRIVATE juce::juce_core juce::juce_audio_basics juce::juce_audio_formats juce::juce_dsp
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

    # Full plugin test (no DAW, Linux only): capture, analysis, correction,
    # mouse editing, save/reload, GUI snapshots.
    if(UNIX AND NOT APPLE)
        add_executable(ScribaPitchPluginTest Tests/PluginTest.cpp)
        if(SCRIBA_ARA)   # the ARA library normally lives in the format wrappers
            target_sources(ScribaPitchPluginTest PRIVATE JUCE/modules/juce_audio_plugin_client/juce_audio_plugin_client_ARA.cpp)
        endif()
        target_link_libraries(ScribaPitchPluginTest PRIVATE ScribaPitch)
        target_include_directories(ScribaPitchPluginTest PRIVATE $<TARGET_PROPERTY:ScribaPitch,INCLUDE_DIRECTORIES>)
        target_compile_definitions(ScribaPitchPluginTest PRIVATE $<TARGET_PROPERTY:ScribaPitch,COMPILE_DEFINITIONS>)
        target_compile_features(ScribaPitchPluginTest PRIVATE cxx_std_17)
    endif()

    # A minimal ARA "DAW" that loads the built VST3, hands it the vocal as an
    # audio file and checks that the notes appear without playing the song.
    if(SCRIBA_ARA AND UNIX AND NOT APPLE)
        juce_add_console_app(ScribaPitchARATest PRODUCT_NAME "ScribaPitchARATest")
        target_sources(ScribaPitchARATest PRIVATE Tests/ARAHostTest.cpp ${SCRIBA_ENGINE_SOURCES})
        target_include_directories(ScribaPitchARATest PRIVATE Source "${SCRIBA_ARA_SDK}" "${SCRIBA_ARA_SDK}/ARA_Library/..")
        target_compile_definitions(ScribaPitchARATest PRIVATE
            JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_ARA=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
            JUCE_ALSA=0 JUCE_JACK=0 JUCE_USE_XRANDR=0 JUCE_USE_XINERAMA=0 JUCE_USE_XCURSOR=0
            JUCE_USE_XSHM=0 JUCE_USE_XRENDER=0 JUCE_MODAL_LOOPS_PERMITTED=1)
        target_link_libraries(ScribaPitchARATest
            PRIVATE juce::juce_audio_processors juce::juce_audio_formats juce::juce_dsp juce::juce_gui_basics
            PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
        add_dependencies(ScribaPitchARATest ScribaPitch_VST3)
    endif()

    # Scriba -> Scriba Pitch through Meteora Link with the real Scriba.vst3
    juce_add_console_app(ScribaPitchLinkTest PRODUCT_NAME "ScribaPitchLinkTest")
    target_sources(ScribaPitchLinkTest PRIVATE Tests/LinkHostTest.cpp)
    target_include_directories(ScribaPitchLinkTest PRIVATE Source)
    target_compile_definitions(ScribaPitchLinkTest PRIVATE
        JUCE_PLUGINHOST_VST3=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
        JUCE_ALSA=0 JUCE_JACK=0 JUCE_USE_XRANDR=0 JUCE_USE_XINERAMA=0 JUCE_USE_XCURSOR=0
        JUCE_USE_XSHM=0 JUCE_USE_XRENDER=0 JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(ScribaPitchLinkTest
        PRIVATE juce::juce_audio_processors juce::juce_gui_basics
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
endif()
