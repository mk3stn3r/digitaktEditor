cmake_minimum_required(VERSION 3.22)
project(DigitaktPatternEditor VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Link-time optimisation is off by default. It needs well over 2 GB to link
# this app, and on a small machine GCC dies with an internal compiler error in
# lto-wrapper rather than a clean out-of-memory message - which is a miserable
# thing to debug. Turn it on with -DDTK_ENABLE_LTO=ON if you have the headroom.
option(DTK_ENABLE_LTO "Build with link-time optimisation (needs plenty of RAM)" OFF)

# Point at an existing JUCE checkout with -DJUCE_PATH=/path/to/JUCE,
# otherwise it is fetched automatically (~200 MB, one time).
if(DEFINED JUCE_PATH)
    add_subdirectory(${JUCE_PATH} JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        8.0.4
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

juce_add_gui_app(DigitaktPatternEditor
    PRODUCT_NAME  "Digitakt Pattern Editor"
    COMPANY_NAME  "digitakt-py"
    VERSION       ${PROJECT_VERSION}
    NEEDS_CURL    FALSE
    NEEDS_WEB_BROWSER FALSE)

target_sources(DigitaktPatternEditor PRIVATE
    Source/Main.cpp
    Source/MainComponent.cpp
    Source/PatternGrid.cpp
    Source/DigitaktCore.cpp
    Source/DigitaktMidi.cpp
    Source/ElektronApi.cpp
    Source/ApiClient.cpp
    Source/DumpRequester.cpp
    Source/SampleImport.cpp
    Source/SamplesPanel.cpp
    Source/SamplePool.cpp
    Source/SequencerEngine.cpp
    Source/PoolPanel.cpp
    Source/RemotePanel.cpp
    Source/RawMidiOut.cpp
    Source/PluginRack.cpp
    Source/InstrumentsPanel.cpp
    Source/Log.cpp
    Source/BlankData.cpp)

target_compile_definitions(DigitaktPatternEditor PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_LV2=1
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:DigitaktPatternEditor,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:DigitaktPatternEditor,JUCE_VERSION>")

# Export symbols and keep frame pointers so the crash handler's backtrace
# names functions instead of printing bare addresses. Costs nothing measurable
# and is the difference between a usable crash report and a useless one.
if(UNIX AND NOT APPLE)
    target_link_options(DigitaktPatternEditor PRIVATE -rdynamic)
    target_compile_options(DigitaktPatternEditor PRIVATE -fno-omit-frame-pointer)
endif()

target_link_libraries(DigitaktPatternEditor PRIVATE
    juce::juce_gui_basics
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_processors
    juce::juce_audio_utils
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

if(DTK_ENABLE_LTO)
    target_link_libraries(DigitaktPatternEditor PUBLIC juce::juce_recommended_lto_flags)
endif()

# The core has no JUCE dependency, so it can be tested on its own. This is
# what proves byte-exact agreement with the Python implementation.
add_executable(TestCore Tests/TestCore.cpp Source/DigitaktCore.cpp Source/BlankData.cpp)
target_include_directories(TestCore PRIVATE Source)

# The API layer is JUCE-free too, so the upload protocol is tested against a
# mock device rather than only on hardware.
add_executable(TestApi Tests/TestApi.cpp Source/ElektronApi.cpp
                       Source/DigitaktCore.cpp Source/BlankData.cpp)
target_include_directories(TestApi PRIVATE Source)

# Audio import needs JUCE's format readers, so this one links JUCE.
juce_add_console_app(TestSampleImport PRODUCT_NAME "TestSampleImport")
target_sources(TestSampleImport PRIVATE
    Tests/TestSampleImport.cpp
    Source/SampleImport.cpp
    Source/ElektronApi.cpp
    Source/DigitaktCore.cpp
    Source/BlankData.cpp)
target_include_directories(TestSampleImport PRIVATE Source)
target_compile_definitions(TestSampleImport PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_STANDALONE_APPLICATION=1)
target_link_libraries(TestSampleImport PRIVATE
    juce::juce_audio_formats
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_recommended_config_flags)

# The sequencer is testable without a sound card: the audio callback can be
# driven directly with scratch buffers.
juce_add_console_app(TestEngine PRODUCT_NAME "TestEngine")
target_sources(TestEngine PRIVATE
    Tests/TestEngine.cpp
    Source/SequencerEngine.cpp
    Source/PluginRack.cpp
    Source/Log.cpp
    Source/SamplePool.cpp
    Source/SampleImport.cpp
    Source/ElektronApi.cpp
    Source/DigitaktCore.cpp
    Source/BlankData.cpp)
target_include_directories(TestEngine PRIVATE Source)
target_compile_definitions(TestEngine PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_STANDALONE_APPLICATION=1
    JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_LV2=1)
target_link_libraries(TestEngine PRIVATE
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_processors
    juce::juce_recommended_config_flags)

# A GUI-free sender, for isolating transfer problems on real hardware.
# Built through JUCE's own CMake API so it works the same whether JUCE came
# from -DJUCE_PATH or was fetched automatically.
juce_add_console_app(dtk-send PRODUCT_NAME "dtk-send")
target_sources(dtk-send PRIVATE
    Tools/dtk-send.cpp
    Source/RawMidiOut.cpp
    Source/DigitaktCore.cpp
    Source/BlankData.cpp)
target_include_directories(dtk-send PRIVATE Source)
target_compile_definitions(dtk-send PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_STANDALONE_APPLICATION=1)
target_link_libraries(dtk-send PRIVATE
    juce::juce_core
    juce::juce_recommended_config_flags)

# RawMidiOut talks to ALSA directly; juce_core does not pull it in.
if(UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(ALSA REQUIRED alsa)
    target_link_libraries(dtk-send PRIVATE ${ALSA_LIBRARIES})
    target_include_directories(dtk-send PRIVATE ${ALSA_INCLUDE_DIRS})
    target_link_directories(dtk-send PRIVATE ${ALSA_LIBRARY_DIRS})
endif()

juce_add_console_app(TestRack PRODUCT_NAME "TestRack")
target_sources(TestRack PRIVATE Tests/TestRack.cpp Source/PluginRack.cpp Source/Log.cpp)
target_include_directories(TestRack PRIVATE Source)
target_compile_definitions(TestRack PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_STANDALONE_APPLICATION=1
    JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_LV2=1)
target_link_libraries(TestRack PRIVATE
    juce::juce_audio_processors
    juce::juce_recommended_config_flags)

enable_testing()
add_test(NAME core COMMAND TestCore)
add_test(NAME api COMMAND TestApi)
add_test(NAME sample_import COMMAND TestSampleImport)
add_test(NAME engine COMMAND TestEngine)
add_test(NAME rack COMMAND TestRack)
