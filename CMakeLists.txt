cmake_minimum_required(VERSION 3.16)
project(voice_runtime LANGUAGES C CXX)

if(WIN32)
    add_compile_definitions(_WIN32_WINNT=0x0A00 NOMINMAX _USE_MATH_DEFINES)
endif()

# Default build type to Release — our Find modules set per-config
# IMPORTED_LOCATION_RELEASE so a build type must be set for resolution.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Warnings
if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# Module path — use the project's Find*.cmake modules
# ---------------------------------------------------------------------------
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/libraries/cmake")

# ---------------------------------------------------------------------------
# Options — informational flags (Find modules handle presence detection)
# ---------------------------------------------------------------------------
option(VOICE_RUNTIME_ENABLE_REAL_AUDIO "Build real miniaudio mic/output nodes" OFF)
option(VOICE_RUNTIME_ENABLE_WEBRTC_APM "Build WebRTC APM DSP node"            OFF)
option(VOICE_RUNTIME_BUILD_TESTS       "Build individual node test programs"   ON)
option(VOICE_RUNTIME_ENABLE_WASAPI     "Build Windows-native WASAPI mic/output nodes (Windows only)" OFF)

# ---------------------------------------------------------------------------
# Windows WASAPI — native audio nodes (WasapiMicNode, WasapiOutputNode)
# ---------------------------------------------------------------------------
if(WIN32 AND VOICE_RUNTIME_ENABLE_WASAPI)
    # ole32 / oleaut32 are already linked via #pragma comment in the headers,
    # but we add them here explicitly for non-MSVC toolchains (MinGW, Clang-CL).
    set(WASAPI_SYSTEM_LIBS ole32 oleaut32 ksuser)
    add_compile_definitions(VOICE_RUNTIME_ENABLE_WASAPI=1)
    message(STATUS "WASAPI Windows audio nodes: ENABLED")
elseif(VOICE_RUNTIME_ENABLE_WASAPI AND NOT WIN32)
    message(WARNING "VOICE_RUNTIME_ENABLE_WASAPI is ON but the target is not Windows — ignored")
    set(WASAPI_SYSTEM_LIBS "")
else()
    set(WASAPI_SYSTEM_LIBS "")
endif()

# ---------------------------------------------------------------------------
# Find all libraries from our local libraries/ directory
# ---------------------------------------------------------------------------
find_package(Ggml)
if(TARGET Ggml::ggml-cuda)
    find_package(CUDAToolkit REQUIRED)
    target_link_libraries(Ggml::Ggml INTERFACE CUDA::cudart CUDA::cublas CUDA::cublasLt CUDA::cuda_driver)
endif()
find_package(Qwen3Asr)
find_package(Kokoro)
find_package(EspeakNg)

if(VOICE_RUNTIME_ENABLE_WEBRTC_APM)
    find_package(WebRtcAudioProcessing)
    if(TARGET WebRtcAudioProcessing::WebRtcAudioProcessing)
        add_compile_definitions(VOICE_RUNTIME_ENABLE_WEBRTC_APM=1)
        message(STATUS "WebRTC Audio Processing found and enabled")
    else()
        message(FATAL_ERROR
            "VOICE_RUNTIME_ENABLE_WEBRTC_APM=ON but WebRTC Audio Processing library not found.\n"
            "Expected at: libraries/lib/webrtc_audio_processing/<platform>/<config>/")
    endif()
endif()

# ---------------------------------------------------------------------------
# Header-only dependencies (cpp-httplib, nlohmann/json)
# ---------------------------------------------------------------------------
set(HTTPLIB_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libraries/include")
set(JSON_INCLUDE_DIR    "${CMAKE_CURRENT_SOURCE_DIR}/libraries/include")

# ---------------------------------------------------------------------------
# Apple frameworks (Metal + Accelerate for GGML, audio frameworks for miniaudio)
# ---------------------------------------------------------------------------
if(APPLE)
    find_library(METAL_FW         Metal)
    find_library(ACCELERATE_FW    Accelerate)
    find_library(FOUNDATION_FW    Foundation)
    find_library(AUDIOTOOLBOX_FW  AudioToolbox)
    find_library(COREAUDIO_FW     CoreAudio)
    find_library(COREFOUNDATION_FW CoreFoundation)

    set(APPLE_GGML_FRAMEWORKS "")
    if(METAL_FW)
        list(APPEND APPLE_GGML_FRAMEWORKS ${METAL_FW})
    endif()
    if(ACCELERATE_FW)
        list(APPEND APPLE_GGML_FRAMEWORKS ${ACCELERATE_FW})
    endif()
    if(FOUNDATION_FW)
        list(APPEND APPLE_GGML_FRAMEWORKS ${FOUNDATION_FW})
    endif()
endif()

# ---------------------------------------------------------------------------
# Common include directories
# ---------------------------------------------------------------------------
set(VOICE_RUNTIME_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
    ${CMAKE_CURRENT_SOURCE_DIR}/nodes
    ${CMAKE_CURRENT_SOURCE_DIR}/libraries/include
)

# ---------------------------------------------------------------------------
# miniaudio — single compilation unit, built as an object library
# ---------------------------------------------------------------------------
add_library(miniaudio_impl OBJECT
    nodes/miniaudio_impl.cpp
)
target_include_directories(miniaudio_impl PRIVATE include)

# Platform libraries required by miniaudio
if(APPLE)
    set(MINIAUDIO_PLATFORM_LIBS
        ${AUDIOTOOLBOX_FW} ${COREAUDIO_FW} ${COREFOUNDATION_FW})
elseif(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    set(MINIAUDIO_PLATFORM_LIBS Threads::Threads dl m)
    find_library(ALSA_LIB asound)
    if(ALSA_LIB)
        list(APPEND MINIAUDIO_PLATFORM_LIBS ${ALSA_LIB})
    endif()
elseif(WIN32)
    set(MINIAUDIO_PLATFORM_LIBS "")
endif()

# ---------------------------------------------------------------------------
# espeak-ng speechPlayer stub — provides missing Klatt symbols
# ---------------------------------------------------------------------------
add_library(espeak_stub OBJECT nodes/espeak_speechplayer_stub.c)

# ---------------------------------------------------------------------------
# Helper: platform threading library
# ---------------------------------------------------------------------------
if(UNIX)
    find_package(Threads QUIET)
    set(PLATFORM_THREAD_LIBS pthread)
else()
    set(PLATFORM_THREAD_LIBS "")
endif()

# ===========================================================================
# Simulation targets (always built — use simulated/stub nodes)
# ===========================================================================

# Primary simulation
add_executable(voice_runtime_simulation
    tests/simulation_main.cpp
)
target_include_directories(voice_runtime_simulation PRIVATE ${VOICE_RUNTIME_INCLUDES})
target_link_libraries(voice_runtime_simulation PRIVATE ${PLATFORM_THREAD_LIBS})
if(VOICE_RUNTIME_ENABLE_WEBRTC_APM AND TARGET WebRtcAudioProcessing::WebRtcAudioProcessing)
    target_link_libraries(voice_runtime_simulation PRIVATE WebRtcAudioProcessing::WebRtcAudioProcessing)
endif()

# WebRTC DSP simulation
add_executable(voice_runtime_webrtc_simulation
    tests/simulation_main_webrtc_dsp.cpp
)
target_include_directories(voice_runtime_webrtc_simulation PRIVATE ${VOICE_RUNTIME_INCLUDES})
target_link_libraries(voice_runtime_webrtc_simulation PRIVATE ${PLATFORM_THREAD_LIBS})
if(VOICE_RUNTIME_ENABLE_WEBRTC_APM AND TARGET WebRtcAudioProcessing::WebRtcAudioProcessing)
    target_link_libraries(voice_runtime_webrtc_simulation PRIVATE WebRtcAudioProcessing::WebRtcAudioProcessing)
endif()

# Miniaudio loopback simulation
add_executable(voice_runtime_miniaudio_simulation
    tests/simulation_main_miniaudio.cpp
    $<TARGET_OBJECTS:miniaudio_impl>
)
target_include_directories(voice_runtime_miniaudio_simulation PRIVATE ${VOICE_RUNTIME_INCLUDES})
target_link_libraries(voice_runtime_miniaudio_simulation PRIVATE
    ${MINIAUDIO_PLATFORM_LIBS}
    ${PLATFORM_THREAD_LIBS}
)

# ===========================================================================
# Individual node tests (when VOICE_RUNTIME_BUILD_TESTS is ON)
# ===========================================================================

if(VOICE_RUNTIME_BUILD_TESTS)

    # --- Test: Microphone node ---
    add_executable(test_mic_node tests/test_mic_node.cpp
        $<TARGET_OBJECTS:miniaudio_impl>)
    target_include_directories(test_mic_node PRIVATE ${VOICE_RUNTIME_INCLUDES})
    target_link_libraries(test_mic_node PRIVATE
        ${MINIAUDIO_PLATFORM_LIBS}
        ${PLATFORM_THREAD_LIBS}
    )

    # --- Test: VAD node ---
    add_executable(test_vad_node tests/test_vad_node.cpp)
    target_include_directories(test_vad_node PRIVATE ${VOICE_RUNTIME_INCLUDES})
    target_link_libraries(test_vad_node PRIVATE ${PLATFORM_THREAD_LIBS})
    if(TARGET Ggml::Ggml)
        target_link_libraries(test_vad_node PRIVATE Ggml::Ggml)
        if(APPLE)
            target_link_libraries(test_vad_node PRIVATE ${APPLE_GGML_FRAMEWORKS})
        endif()
    endif()

    # --- Test: STT node ---
    add_executable(test_stt_node tests/test_stt_node.cpp)
    target_include_directories(test_stt_node PRIVATE ${VOICE_RUNTIME_INCLUDES})
    target_link_libraries(test_stt_node PRIVATE ${PLATFORM_THREAD_LIBS})
    if(TARGET Qwen3Asr::Qwen3Asr)
        target_link_libraries(test_stt_node PRIVATE Qwen3Asr::Qwen3Asr)
        if(APPLE)
            target_link_libraries(test_stt_node PRIVATE ${APPLE_GGML_FRAMEWORKS})
        endif()
    endif()

    # --- Test: TTS node ---
    add_executable(test_tts_node tests/test_tts_node.cpp
        $<TARGET_OBJECTS:espeak_stub>
        $<TARGET_OBJECTS:miniaudio_impl>)
    target_include_directories(test_tts_node PRIVATE ${VOICE_RUNTIME_INCLUDES})
    target_link_libraries(test_tts_node PRIVATE
        ${MINIAUDIO_PLATFORM_LIBS}
        ${PLATFORM_THREAD_LIBS}
    )
    if(TARGET Kokoro::Kokoro)
        target_link_libraries(test_tts_node PRIVATE Kokoro::Kokoro)
    endif()
    if(TARGET EspeakNg::EspeakNg)
        target_link_libraries(test_tts_node PRIVATE EspeakNg::EspeakNg)
    endif()

    # --- Test: LLM node (header-only deps: httplib + nlohmann/json) ---
    add_executable(test_llm_node tests/test_llm_node.cpp)
    target_include_directories(test_llm_node PRIVATE ${VOICE_RUNTIME_INCLUDES})
    target_link_libraries(test_llm_node PRIVATE ${PLATFORM_THREAD_LIBS})

    # --- Test: Interpreter node ---
    add_executable(test_interpreter_node tests/test_interpreter_node.cpp)
    target_include_directories(test_interpreter_node PRIVATE ${VOICE_RUNTIME_INCLUDES})
    target_link_libraries(test_interpreter_node PRIVATE ${PLATFORM_THREAD_LIBS})

    # --- Test: VAD with real model ---
    add_executable(test_vad_real tests/test_vad_real.cpp)
    target_include_directories(test_vad_real PRIVATE ${VOICE_RUNTIME_INCLUDES})
    target_link_libraries(test_vad_real PRIVATE ${PLATFORM_THREAD_LIBS})
    if(TARGET Ggml::Ggml)
        target_link_libraries(test_vad_real PRIVATE Ggml::Ggml)
        if(APPLE)
            target_link_libraries(test_vad_real PRIVATE ${APPLE_GGML_FRAMEWORKS})
        endif()
    endif()

    # --- Test: WebRTC AEC Unit ---
    add_executable(test_webrtc_aec_unit tests/test_webrtc_aec_unit.cpp)
    target_include_directories(test_webrtc_aec_unit PRIVATE ${VOICE_RUNTIME_INCLUDES})
    target_link_libraries(test_webrtc_aec_unit PRIVATE ${PLATFORM_THREAD_LIBS})
    if(VOICE_RUNTIME_ENABLE_WEBRTC_APM AND TARGET WebRtcAudioProcessing::WebRtcAudioProcessing)
        target_link_libraries(test_webrtc_aec_unit PRIVATE WebRtcAudioProcessing::WebRtcAudioProcessing)
    endif()

    # --- Test: Transcribe WAV ---
    add_executable(test_transcribe_wav tests/test_transcribe_wav.cpp)
    target_include_directories(test_transcribe_wav PRIVATE ${VOICE_RUNTIME_INCLUDES})
    target_link_libraries(test_transcribe_wav PRIVATE ${PLATFORM_THREAD_LIBS})
    if(TARGET Qwen3Asr::Qwen3Asr)
        target_link_libraries(test_transcribe_wav PRIVATE Qwen3Asr::Qwen3Asr)
        if(APPLE)
            target_link_libraries(test_transcribe_wav PRIVATE ${APPLE_GGML_FRAMEWORKS})
        endif()
    endif()

    # --- Test: WASAPI Mic (Windows only) ---
    if(WIN32 AND VOICE_RUNTIME_ENABLE_WASAPI)
        add_executable(test_wasapi_mic tests/test_wasapi_mic.cpp)
        target_include_directories(test_wasapi_mic PRIVATE ${VOICE_RUNTIME_INCLUDES})
        target_link_libraries(test_wasapi_mic PRIVATE
            ${WASAPI_SYSTEM_LIBS}
            ${PLATFORM_THREAD_LIBS}
        )

        # --- Test: WASAPI Output (Windows only) ---
        add_executable(test_wasapi_output tests/test_wasapi_output.cpp)
        target_include_directories(test_wasapi_output PRIVATE ${VOICE_RUNTIME_INCLUDES})
        target_link_libraries(test_wasapi_output PRIVATE
            ${WASAPI_SYSTEM_LIBS}
            ${PLATFORM_THREAD_LIBS}
        )
    endif()

endif()

# ===========================================================================
# Real pipeline target — builds with all real nodes
# ===========================================================================

add_executable(voice_runtime_real_pipeline
    tests/simulation_main.cpp
    $<TARGET_OBJECTS:miniaudio_impl>
    $<TARGET_OBJECTS:espeak_stub>
)
target_include_directories(voice_runtime_real_pipeline PRIVATE ${VOICE_RUNTIME_INCLUDES})
target_link_libraries(voice_runtime_real_pipeline PRIVATE
    ${MINIAUDIO_PLATFORM_LIBS}
    ${PLATFORM_THREAD_LIBS}
)

# Link all available real-node libraries
if(TARGET Ggml::Ggml)
    target_link_libraries(voice_runtime_real_pipeline PRIVATE Ggml::Ggml)
    if(APPLE)
        target_link_libraries(voice_runtime_real_pipeline PRIVATE ${APPLE_GGML_FRAMEWORKS})
    endif()
endif()

if(TARGET Qwen3Asr::Qwen3Asr)
    target_link_libraries(voice_runtime_real_pipeline PRIVATE Qwen3Asr::Qwen3Asr)
endif()

if(TARGET Kokoro::Kokoro)
    target_link_libraries(voice_runtime_real_pipeline PRIVATE Kokoro::Kokoro)
endif()

if(TARGET EspeakNg::EspeakNg)
    target_link_libraries(voice_runtime_real_pipeline PRIVATE EspeakNg::EspeakNg)
endif()

if(VOICE_RUNTIME_ENABLE_WEBRTC_APM AND TARGET WebRtcAudioProcessing::WebRtcAudioProcessing)
    target_link_libraries(voice_runtime_real_pipeline PRIVATE
        WebRtcAudioProcessing::WebRtcAudioProcessing)
endif()

# ===========================================================================
# Live transcription test — Mic → VAD → STT → console
# ===========================================================================

add_executable(test_live_transcription
    tests/test_live_transcription.cpp
    $<TARGET_OBJECTS:miniaudio_impl>
)
target_include_directories(test_live_transcription PRIVATE ${VOICE_RUNTIME_INCLUDES})
target_link_libraries(test_live_transcription PRIVATE
    ${MINIAUDIO_PLATFORM_LIBS}
    ${PLATFORM_THREAD_LIBS}
)

# Ggml (for Silero VAD)
if(TARGET Ggml::Ggml)
    target_link_libraries(test_live_transcription PRIVATE Ggml::Ggml)
    if(APPLE)
        target_link_libraries(test_live_transcription PRIVATE ${APPLE_GGML_FRAMEWORKS})
    endif()
endif()

# Qwen3 ASR (for STT)
if(TARGET Qwen3Asr::Qwen3Asr)
    target_link_libraries(test_live_transcription PRIVATE Qwen3Asr::Qwen3Asr)
endif()

# ===========================================================================
# AEC transcription test — Mic → WebRTC AEC → STT with TTS echo
# ===========================================================================

add_executable(test_aec_transcription
    tests/test_aec_transcription.cpp
    $<TARGET_OBJECTS:miniaudio_impl>
    $<TARGET_OBJECTS:espeak_stub>
)
target_include_directories(test_aec_transcription PRIVATE ${VOICE_RUNTIME_INCLUDES})
target_link_libraries(test_aec_transcription PRIVATE
    ${MINIAUDIO_PLATFORM_LIBS}
    ${PLATFORM_THREAD_LIBS}
)

if(TARGET Ggml::Ggml)
    target_link_libraries(test_aec_transcription PRIVATE Ggml::Ggml)
    if(APPLE)
        target_link_libraries(test_aec_transcription PRIVATE ${APPLE_GGML_FRAMEWORKS})
    endif()
endif()

if(TARGET Qwen3Asr::Qwen3Asr)
    target_link_libraries(test_aec_transcription PRIVATE Qwen3Asr::Qwen3Asr)
endif()

if(TARGET Kokoro::Kokoro)
    target_link_libraries(test_aec_transcription PRIVATE Kokoro::Kokoro)
endif()

if(TARGET EspeakNg::EspeakNg)
    target_link_libraries(test_aec_transcription PRIVATE EspeakNg::EspeakNg)
endif()

if(VOICE_RUNTIME_ENABLE_WEBRTC_APM AND TARGET WebRtcAudioProcessing::WebRtcAudioProcessing)
    target_link_libraries(test_aec_transcription PRIVATE
        WebRtcAudioProcessing::WebRtcAudioProcessing)
endif()

# WASAPI system libs for test_aec_transcription on Windows
if(WIN32 AND VOICE_RUNTIME_ENABLE_WASAPI)
    target_link_libraries(test_aec_transcription PRIVATE ${WASAPI_SYSTEM_LIBS})
endif()

# ===========================================================================
# Minimal STT crash reproduction test
# ===========================================================================
add_executable(test_stt_crash tests/test_stt_crash.cpp)
target_include_directories(test_stt_crash PRIVATE ${VOICE_RUNTIME_INCLUDES})
target_link_libraries(test_stt_crash PRIVATE ${PLATFORM_THREAD_LIBS})
if(TARGET Qwen3Asr::Qwen3Asr)
    target_link_libraries(test_stt_crash PRIVATE Qwen3Asr::Qwen3Asr)
    if(APPLE)
        target_link_libraries(test_stt_crash PRIVATE ${APPLE_GGML_FRAMEWORKS})
    endif()
endif()

add_executable(test_stt_thread_crash tests/test_stt_thread_crash.cpp)
target_include_directories(test_stt_thread_crash PRIVATE ${VOICE_RUNTIME_INCLUDES})
target_link_libraries(test_stt_thread_crash PRIVATE ${PLATFORM_THREAD_LIBS})
if(TARGET Qwen3Asr::Qwen3Asr)
    target_link_libraries(test_stt_thread_crash PRIVATE Qwen3Asr::Qwen3Asr)
    if(APPLE)
        target_link_libraries(test_stt_thread_crash PRIVATE ${APPLE_GGML_FRAMEWORKS})
    endif()
endif()
if(TARGET Ggml::Ggml)
    target_link_libraries(test_stt_thread_crash PRIVATE Ggml::Ggml)
endif()

# ===========================================================================
# Status summary
# ===========================================================================
message(STATUS "")
message(STATUS "=== voice_runtime build summary ===")
if(TARGET Ggml::Ggml)
    message(STATUS "  Ggml:              FOUND")
else()
    message(STATUS "  Ggml:              not found (VAD/STT will use fallback)")
endif()
if(TARGET Qwen3Asr::Qwen3Asr)
    message(STATUS "  Qwen3Asr:          FOUND")
else()
    message(STATUS "  Qwen3Asr:          not found (STT will use stub)")
endif()
if(TARGET Kokoro::Kokoro)
    message(STATUS "  Kokoro:            FOUND")
else()
    message(STATUS "  Kokoro:            not found (TTS will use stub)")
endif()
if(TARGET EspeakNg::EspeakNg)
    message(STATUS "  EspeakNg:          FOUND")
else()
    message(STATUS "  EspeakNg:          not found (TTS G2P unavailable)")
endif()
if(VOICE_RUNTIME_ENABLE_WEBRTC_APM AND TARGET WebRtcAudioProcessing::WebRtcAudioProcessing)
    message(STATUS "  WebRTC APM:        FOUND (enabled)")
else()
    message(STATUS "  WebRTC APM:        disabled")
endif()
message(STATUS "  httplib:           ${HTTPLIB_INCLUDE_DIR}/httplib.h")
message(STATUS "  nlohmann/json:     ${JSON_INCLUDE_DIR}/nlohmann/json.hpp")
message(STATUS "  Build tests:       ${VOICE_RUNTIME_BUILD_TESTS}")
message(STATUS "===================================")
message(STATUS "")
