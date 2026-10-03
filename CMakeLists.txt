cmake_minimum_required(VERSION 3.22)
project(KoeLoom VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# MSVC reads/writes narrow literals as CP932 on Japanese machines unless told otherwise.
if(MSVC)
    add_compile_options(/utf-8 /MP)
endif()

# --- JUCE 8.0.6 (AGPLv3 chosen; see LICENSE / README) -----------------------
# A shared local checkout avoids re-downloading per build tree / worktree.
set(KOELOOM_JUCE_PATH "$ENV{LOCALAPPDATA}/KoeLoom/deps/JUCE" CACHE PATH "Local JUCE 8.0.6 checkout")
if(EXISTS "${KOELOOM_JUCE_PATH}/CMakeLists.txt")
    add_subdirectory("${KOELOOM_JUCE_PATH}" JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        8.0.6
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

add_subdirectory(third_party)

# --- App ---------------------------------------------------------------------
juce_add_gui_app(KoeLoom
    PRODUCT_NAME       "KoeLoom"
    COMPANY_NAME       "KoeLoom"
    BUNDLE_ID          "dev.koeloom.app"
    VERSION            ${PROJECT_VERSION}
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "KoeLoom processes your microphone in real time."
    ICON_BIG           "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon/icon.png"
    ICON_SMALL         "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon/icon-small.png")

file(GLOB_RECURSE KOELOOM_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/Source/*.cpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/Source/*.h")
target_sources(KoeLoom PRIVATE ${KOELOOM_SOURCES})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/Source" FILES ${KOELOOM_SOURCES})
target_include_directories(KoeLoom PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source")

# Built-in presets (1 JSON per preset, koeloom_presets.md §3) are embedded.
file(GLOB KOELOOM_PRESET_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/resources/presets/*.json")
juce_add_binary_data(KoeLoomPresets NAMESPACE KoeLoomPresetData SOURCES ${KOELOOM_PRESET_FILES})

target_compile_definitions(KoeLoom PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_USE_FLAC=1
    JUCE_USE_OGGVORBIS=1
    JUCE_USE_MP3AUDIOFORMAT=0
    JUCE_USE_WINDOWS_MEDIA_FORMAT=1
    JUCE_ASIO=0
    JUCE_WASAPI=1
    JUCE_DIRECTSOUND=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:KoeLoom,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:KoeLoom,JUCE_VERSION>"
    KOELOOM_VERSION_STRING="${PROJECT_VERSION}")

target_link_libraries(KoeLoom PRIVATE
    KoeLoomPresets
    koeloom_rnnoise
    koeloom_signalsmith
    juce::juce_audio_utils
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_dsp
    juce::juce_core
    juce::juce_events
    juce::juce_data_structures
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

if(WIN32)
    target_link_libraries(KoeLoom PRIVATE avrt winmm synchronization)
endif()
