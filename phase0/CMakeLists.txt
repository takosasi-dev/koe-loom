# KoeLoom Phase 0 experiments (separate project; not part of the product build).
#
#   cmake -S <repo>\phase0 -B "$env:LOCALAPPDATA\KoeLoom\build\phase0" -G "Visual Studio 17 2022" -A x64
#   cmake --build "$env:LOCALAPPDATA\KoeLoom\build\phase0" --config Release --parallel
#
# Pass an ASCII source path (JUCE's build steps break on some characters).

cmake_minimum_required(VERSION 3.22)
project(KoeLoomPhase0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/utf-8 /MP)
endif()

# JUCE 8.0.6 (AGPLv3 chosen, see the repository LICENSE), shared local checkout
add_subdirectory("$ENV{LOCALAPPDATA}/KoeLoom/deps/JUCE" JUCE)
# koeloom_rnnoise (RNNoise v0.2, BSD-3-Clause) and koeloom_signalsmith (Stretch + Linear, MIT)
add_subdirectory(../third_party third_party)

# JUCE modules compiled once into a static library and shared by every experiment.
add_library(p0_juce STATIC)
target_link_libraries(p0_juce
    PRIVATE
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags)
target_compile_definitions(p0_juce PUBLIC
    JUCE_STANDALONE_APPLICATION=1
    JUCE_WASAPI=1
    JUCE_ASIO=0
    JUCE_DIRECTSOUND=0
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JUCE_USE_FLAC=0
    JUCE_USE_OGGVORBIS=0
    JUCE_USE_MP3AUDIOFORMAT=0
    JUCE_USE_WINDOWS_MEDIA_FORMAT=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0)
# forward the module include paths / definitions to the executables without recompiling the modules
target_include_directories(p0_juce INTERFACE $<TARGET_PROPERTY:p0_juce,INCLUDE_DIRECTORIES>)
target_compile_definitions(p0_juce INTERFACE $<TARGET_PROPERTY:p0_juce,COMPILE_DEFINITIONS>)

add_library(p0_common STATIC src/p0_common.cpp src/p0_common.h src/p0_shifter.h src/p0_pitch.h)
target_link_libraries(p0_common PUBLIC p0_juce koeloom_signalsmith avrt winmm synchronization)
target_compile_definitions(p0_common PUBLIC P0_DIR="${CMAKE_CURRENT_SOURCE_DIR}" NOMINMAX)

function(p0_tool name)
    add_executable(${name} src/${name}.cpp)
    target_link_libraries(${name} PRIVATE p0_common ${ARGN})
endfunction()

p0_tool(p0_devices)       # T0/T1: device list (does not open devices unless --open)
p0_tool(p0_passthrough)   # T1: mic / sine -> CABLE Input (needs VB-CABLE)
p0_tool(p0_capture)       # T1: CABLE Output -> WAV (needs VB-CABLE)
p0_tool(p0_latency)       # T1 (e): click loop CABLE Input -> CABLE Output (needs VB-CABLE)
p0_tool(p0_shifters)      # T2: Signalsmith Stretch settings (offline)
p0_tool(p0_rnnoise koeloom_rnnoise)  # T3 (offline)
p0_tool(p0_monitor)       # T4: second-device drift compensation (needs VB-CABLE + headphones)
p0_tool(p0_parallel)      # T5: single vs parallel shifters, real-time paced (no audio device)
p0_tool(p0_pitch)         # T6: YIN vs MPM (offline)
