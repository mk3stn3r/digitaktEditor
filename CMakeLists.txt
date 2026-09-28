cmake_minimum_required(VERSION 3.22)
project(DigitaktPatternEditor VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
    Source/BlankData.cpp)

target_compile_definitions(DigitaktPatternEditor PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:DigitaktPatternEditor,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:DigitaktPatternEditor,JUCE_VERSION>")

target_link_libraries(DigitaktPatternEditor PRIVATE
    juce::juce_gui_basics
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_utils
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

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
    juce::juce_audio_utils
    juce::juce_recommended_config_flags)

# The sequencer is testable without a sound card: the audio callback can be
# driven directly with scratch buffers.
juce_add_console_app(TestEngine PRODUCT_NAME "TestEngine")
target_sources(TestEngine PRIVATE
    Tests/TestEngine.cpp
    Source/SequencerEngine.cpp
    Source/SamplePool.cpp
    Source/SampleImport.cpp
    Source/ElektronApi.cpp
    Source/DigitaktCore.cpp
    Source/BlankData.cpp)
target_include_directories(TestEngine PRIVATE Source)
target_compile_definitions(TestEngine PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_STANDALONE_APPLICATION=1)
target_link_libraries(TestEngine PRIVATE
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_recommended_config_flags)

enable_testing()
add_test(NAME core COMMAND TestCore)
add_test(NAME api COMMAND TestApi)
add_test(NAME sample_import COMMAND TestSampleImport)
add_test(NAME engine COMMAND TestEngine)
