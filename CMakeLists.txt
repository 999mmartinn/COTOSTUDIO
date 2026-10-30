cmake_minimum_required(VERSION 3.22)

file(READ VERSION.txt SYNCTEST_VERSION)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS VERSION.txt)   # a new version reconfigures the build
string(STRIP "${SYNCTEST_VERSION}" SYNCTEST_VERSION)
project(COTOSTUDIO VERSION ${SYNCTEST_VERSION} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static MSVC runtime so the .exe runs on a PC without extra redistributables.
if (MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# VST2 (older plugins such as Omnisphere's VST2): hosted through FST, a free, reverse-
# engineered VST2 header (GPLv3 or later, external/fst; no Steinberg code), which
# stands in for the discontinued VST2 SDK at the path JUCE includes it from.
set(COTOSTUDIO_VST2_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/external/fst")

set(TRACKTION_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/tracktion_engine" CACHE PATH "Tracktion Engine checkout")
add_subdirectory("${TRACKTION_DIR}/modules/juce" juce)
juce_set_vst2_sdk_path("${COTOSTUDIO_VST2_HEADERS}")
add_subdirectory("${TRACKTION_DIR}/modules" tracktion)

option(SYNCTEST_BUILD_TEST_PLUGIN "Build the tiny VST3 gain plugin used by the self-test" ON)

#==============================================================================
# The app: a multiplayer, FL-style DAW (formerly "SyncTest").
juce_add_gui_app(COTOSTUDIO
    PRODUCT_NAME "COTOSTUDIO"
    VERSION ${SYNCTEST_VERSION}
    COMPANY_NAME "COTOSTUDIO contributors"
    BUNDLE_ID "org.cotostudio.app"
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/Resources/icon.png"
    MICROPHONE_PERMISSION_ENABLED TRUE
    NEEDS_CURL FALSE
    NEEDS_WEB_BROWSER FALSE)

juce_generate_juce_header(COTOSTUDIO)

target_sources(COTOSTUDIO PRIVATE
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
    Source/Bouncer.cpp
    Source/Backups.cpp
    Source/Mp3Encoder.cpp
    Source/AutomationEditor.cpp
    Source/SamplerPlugin.cpp
    Source/ParametricEQPlugin.cpp
    Source/ParametricEQEditor.cpp
    Source/LimiterPlugin.cpp
    Source/LimiterEditor.cpp
    Source/DelayPlugin.cpp
    Source/DelayEditor.cpp
    Source/SidechainPlugin.cpp
    Source/SidechainEditor.cpp
    Source/MoreFx.cpp
    Source/ReverbPlugin.cpp
    Source/ReverbEditor.cpp
    Source/SamplerEditor.cpp
    Source/SamplePreviewer.cpp
    Source/FileImport.cpp
    Source/FlpImport.cpp
    Source/FlpImportDialog.cpp
    Source/SoundBrowser.cpp
    Source/AppData.cpp
    Source/Theme.cpp
    Source/Chrome.cpp)

# Built-in files: the fonts (Inter, JetBrains Mono - SIL Open Font License), the logo
# and the Spanish translation.
juce_add_binary_data(COTOSTUDIOData
    HEADER_NAME BinaryData.h
    NAMESPACE BinaryData
    SOURCES
        Resources/Fonts/Inter-Regular.ttf
        Resources/Fonts/Inter-Medium.ttf
        Resources/Fonts/Inter-SemiBold.ttf
        Resources/Fonts/Inter-Bold.ttf
        Resources/Fonts/JetBrainsMono-Regular.ttf
        Resources/Fonts/JetBrainsMono-Bold.ttf
        Resources/logo-badge.jpg
        Resources/logo-full.jpg
        Resources/Translations/es.txt)

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set_target_properties(COTOSTUDIOData PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

target_link_libraries(COTOSTUDIO PRIVATE COTOSTUDIOData)
target_include_directories(COTOSTUDIO PRIVATE "${COTOSTUDIO_VST2_HEADERS}")

# MP3 export uses the MP3 encoder that comes with Windows (Media Foundation).
if (WIN32)
    target_sources(COTOSTUDIO PRIVATE Source/Mp3WriterWin.cpp Source/HardLinkWin.cpp Source/PluginKeysWin.cpp)
    target_link_libraries(COTOSTUDIO PRIVATE mfplat mfreadwrite mfuuid)
endif()

target_compile_definitions(COTOSTUDIO PRIVATE
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_VST=1
    JUCE_PLUGINHOST_AU=0
    JUCE_PLUGINHOST_LADSPA=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_DISPLAY_SPLASH_SCREEN=0
    # Time-stretching and pitch-shifting of audio clips: Signalsmith Stretch (MIT licence,
    # header-only, shipped inside Tracktion Engine).
    TRACKTION_ENABLE_TIMESTRETCH_SIGNALSMITH=1)

# ASIO: the low-latency Windows driver of most audio interfaces. Steinberg's ASIO
# SDK is GPLv3 (dual-licensed) since October 2025; the Windows build downloads it
# and passes its folder here. Without it the app uses Windows Audio (WASAPI).
set(SYNCTEST_ASIO_SDK "" CACHE PATH "ASIO SDK folder (the one containing common/iasiodrv.h)")

if (WIN32 AND SYNCTEST_ASIO_SDK AND EXISTS "${SYNCTEST_ASIO_SDK}/common/iasiodrv.h")
    target_compile_definitions(COTOSTUDIO PRIVATE JUCE_ASIO=1)
    target_include_directories(COTOSTUDIO PRIVATE "${SYNCTEST_ASIO_SDK}/common")
    message(STATUS "ASIO support: ON (${SYNCTEST_ASIO_SDK})")
else()
    message(STATUS "ASIO support: OFF (no ASIO SDK given)")
endif()

# The Linux build only exists so the sync logic can be tested on a headless
# Linux box; it turns off desktop pieces that need extra system libraries.
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(COTOSTUDIO PRIVATE
        JUCE_USE_XRANDR=0 JUCE_USE_XINERAMA=0 JUCE_USE_XCURSOR=0
        JUCE_USE_XRENDER=0 JUCE_USE_XSHM=0 JUCE_USE_XINPUT=0 JUCE_ALSA=0 JUCE_JACK=0)
    target_link_libraries(COTOSTUDIO PRIVATE "-latomic")
endif()

target_link_libraries(COTOSTUDIO PRIVATE
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
# A tiny effect with one "Gain" knob, as VST3 and VST2. The self-test loads it as a
# stand-in for a third-party plugin (Waves, FabFilter, Omnisphere's VST2...) to prove
# knob sync and VST2 hosting end to end.
if (SYNCTEST_BUILD_TEST_PLUGIN)
    juce_add_plugin(SyncTestGain
        PRODUCT_NAME "SyncTestGain"
        COMPANY_NAME "SyncTest contributors"
        BUNDLE_ID "org.synctest.gain"
        PLUGIN_MANUFACTURER_CODE Synt
        PLUGIN_CODE StGn
        FORMATS VST3 VST
        VST2_CATEGORY kPlugCategEffect
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
