cmake_minimum_required(VERSION 3.22)

file(READ VERSION.txt SYNCTEST_VERSION)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS VERSION.txt)   # a new version reconfigures the build
string(STRIP "${SYNCTEST_VERSION}" SYNCTEST_VERSION)
project(SyncTest VERSION ${SYNCTEST_VERSION} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static MSVC runtime so the .exe runs on a PC without extra redistributables.
if (MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(TRACKTION_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/tracktion_engine" CACHE PATH "Tracktion Engine checkout")
add_subdirectory("${TRACKTION_DIR}/modules/juce" juce)
add_subdirectory("${TRACKTION_DIR}/modules" tracktion)

option(SYNCTEST_BUILD_TEST_PLUGIN "Build the tiny VST3 gain plugin used by the self-test" ON)

#==============================================================================
# The app: a minimal plugin host with two-peer (or more) project sync.
juce_add_gui_app(SyncTest
    PRODUCT_NAME "SyncTest"
    VERSION ${SYNCTEST_VERSION}
    COMPANY_NAME "SyncTest contributors"
    BUNDLE_ID "org.synctest.app"
    MICROPHONE_PERMISSION_ENABLED TRUE
    NEEDS_CURL FALSE
    NEEDS_WEB_BROWSER FALSE)

juce_generate_juce_header(SyncTest)

target_sources(SyncTest PRIVATE
    Source/Main.cpp
    Source/EditSync.cpp
    Source/Session.cpp
    Source/MainComponent.cpp
    Source/SelfTest.cpp
    Source/SongModel.cpp
    Source/PatternCompiler.cpp
    Source/DrumSynthPlugin.cpp
    Source/ChannelRack.cpp
    Source/PianoRoll.cpp
    Source/Playlist.cpp
    Source/MixerRouter.cpp
    Source/Mixer.cpp
    Source/AssetStore.cpp
    Source/Recorder.cpp
    Source/Browser.cpp
    Source/ProjectFile.cpp
    Source/Exporter.cpp
    Source/Mp3Encoder.cpp
    Source/AutomationEditor.cpp
    Source/SamplerPlugin.cpp
    Source/SamplePreviewer.cpp
    Source/FileImport.cpp
    Source/SoundBrowser.cpp)

# MP3 export uses the MP3 encoder that comes with Windows (Media Foundation).
if (WIN32)
    target_sources(SyncTest PRIVATE Source/Mp3WriterWin.cpp)
    target_link_libraries(SyncTest PRIVATE mfplat mfreadwrite mfuuid)
endif()

target_compile_definitions(SyncTest PRIVATE
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_AU=0
    JUCE_PLUGINHOST_LADSPA=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_DISPLAY_SPLASH_SCREEN=0)

# ASIO: the low-latency Windows driver of most audio interfaces. Steinberg's ASIO
# SDK is GPLv3 (dual-licensed) since October 2025; the Windows build downloads it
# and passes its folder here. Without it the app uses Windows Audio (WASAPI).
set(SYNCTEST_ASIO_SDK "" CACHE PATH "ASIO SDK folder (the one containing common/iasiodrv.h)")

if (WIN32 AND SYNCTEST_ASIO_SDK AND EXISTS "${SYNCTEST_ASIO_SDK}/common/iasiodrv.h")
    target_compile_definitions(SyncTest PRIVATE JUCE_ASIO=1)
    target_include_directories(SyncTest PRIVATE "${SYNCTEST_ASIO_SDK}/common")
    message(STATUS "ASIO support: ON (${SYNCTEST_ASIO_SDK})")
else()
    message(STATUS "ASIO support: OFF (no ASIO SDK given)")
endif()

# The Linux build only exists so the sync logic can be tested on a headless
# Linux box; it turns off desktop pieces that need extra system libraries.
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(SyncTest PRIVATE
        JUCE_USE_XRANDR=0 JUCE_USE_XINERAMA=0 JUCE_USE_XCURSOR=0
        JUCE_USE_XRENDER=0 JUCE_USE_XSHM=0 JUCE_USE_XINPUT=0 JUCE_ALSA=0 JUCE_JACK=0)
    target_link_libraries(SyncTest PRIVATE "-latomic")
endif()

target_link_libraries(SyncTest PRIVATE
    tracktion::tracktion_core
    tracktion::tracktion_engine
    tracktion::tracktion_graph
    juce::juce_audio_devices
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_cryptography
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags)

#==============================================================================
# A tiny VST3 effect with one "Gain" knob. The self-test loads it as a stand-in
# for a third-party plugin (Waves, FabFilter...) to prove knob sync end to end.
if (SYNCTEST_BUILD_TEST_PLUGIN)
    juce_add_plugin(SyncTestGain
        PRODUCT_NAME "SyncTestGain"
        COMPANY_NAME "SyncTest contributors"
        BUNDLE_ID "org.synctest.gain"
        PLUGIN_MANUFACTURER_CODE Synt
        PLUGIN_CODE StGn
        FORMATS VST3
        IS_SYNTH FALSE
        NEEDS_MIDI_INPUT FALSE
        COPY_PLUGIN_AFTER_BUILD FALSE
        VST3_AUTO_MANIFEST FALSE)

    target_sources(SyncTestGain PRIVATE TestPlugin/GainPlugin.cpp)
    target_compile_definitions(SyncTestGain PUBLIC
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0)

    if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_compile_definitions(SyncTestGain PUBLIC
            JUCE_USE_XRANDR=0 JUCE_USE_XINERAMA=0 JUCE_USE_XCURSOR=0
            JUCE_USE_XRENDER=0 JUCE_USE_XSHM=0 JUCE_USE_XINPUT=0 JUCE_ALSA=0 JUCE_JACK=0)
    endif()

    target_link_libraries(SyncTestGain PRIVATE
        juce::juce_audio_utils
        juce::juce_recommended_config_flags)
endif()
