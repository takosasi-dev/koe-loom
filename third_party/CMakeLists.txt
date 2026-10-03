# Vendored third-party code. Licenses: see each folder and THIRD_PARTY_NOTICES.md.

# --- RNNoise (BSD-3-Clause, xiph/rnnoise v0.2 release tarball incl. model) ---
add_library(koeloom_rnnoise STATIC
    rnnoise/src/denoise.c
    rnnoise/src/rnn.c
    rnnoise/src/pitch.c
    rnnoise/src/kiss_fft.c
    rnnoise/src/celt_lpc.c
    rnnoise/src/nnet.c
    rnnoise/src/nnet_default.c
    rnnoise/src/parse_lpcnet_weights.c
    rnnoise/src/rnnoise_data.c
    rnnoise/src/rnnoise_tables.c)
target_include_directories(koeloom_rnnoise
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/rnnoise/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/rnnoise/src)
target_compile_definitions(koeloom_rnnoise PRIVATE _USE_MATH_DEFINES RNNOISE_BUILD)
if(MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    # MSVC never defines __SSE2__; without it vec.h takes a generic path that includes a header
    # missing from the v0.2 release (os_support.h). Every x64 CPU has SSE2.
    target_compile_definitions(koeloom_rnnoise PRIVATE __SSE2__)
endif()
if(MSVC)
    # third-party C: keep warnings quiet, always optimise (it runs in the audio thread)
    target_compile_options(koeloom_rnnoise PRIVATE /W0 $<$<CONFIG:Debug>:/O2> /fp:fast)
    string(REPLACE "/RTC1" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
endif()
set_target_properties(koeloom_rnnoise PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- Signalsmith Stretch 1.4.0 + Signalsmith Linear 0.6.4 (MIT, header-only) ---
add_library(koeloom_signalsmith INTERFACE)
target_include_directories(koeloom_signalsmith SYSTEM INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/signalsmith-stretch/include
    ${CMAKE_CURRENT_SOURCE_DIR}/signalsmith-linear/include)
