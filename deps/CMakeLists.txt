cmake_minimum_required(VERSION 3.16)
project(webrtc_audio_processing_deps LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Configure Abseil-cpp
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/abseil-cpp")
    # Prevent Abseil from overriding our compiler standard settings
    set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)
    # Enable test builds off
    set(ABSL_RUN_TESTS OFF CACHE BOOL "" FORCE)
    add_subdirectory(abseil-cpp)
endif()

# <<< WEBRTC_AUTO_GENERATED_SOURCES_START >>>
set(COMMON_AUDIO_SOURCES
    WebRTC/common_audio/audio_converter.cc
    WebRTC/common_audio/audio_util.cc
    WebRTC/common_audio/channel_buffer.cc
    WebRTC/common_audio/fir_filter_c.cc
    WebRTC/common_audio/fir_filter_factory.cc
    WebRTC/common_audio/real_fourier.cc
    WebRTC/common_audio/real_fourier_ooura.cc
    WebRTC/common_audio/resampler/push_resampler.cc
    WebRTC/common_audio/resampler/push_sinc_resampler.cc
    WebRTC/common_audio/resampler/resampler.cc
    WebRTC/common_audio/resampler/sinc_resampler.cc
    WebRTC/common_audio/resampler/sinusoidal_linear_chirp_source.cc
    WebRTC/common_audio/ring_buffer.c
    WebRTC/common_audio/signal_processing/auto_corr_to_refl_coef.c
    WebRTC/common_audio/signal_processing/auto_correlation.c
    WebRTC/common_audio/signal_processing/complex_bit_reverse.c
    WebRTC/common_audio/signal_processing/complex_fft.c
    WebRTC/common_audio/signal_processing/copy_set_operations.c
    WebRTC/common_audio/signal_processing/cross_correlation.c
    WebRTC/common_audio/signal_processing/division_operations.c
    WebRTC/common_audio/signal_processing/dot_product_with_scale.cc
    WebRTC/common_audio/signal_processing/downsample_fast.c
    WebRTC/common_audio/signal_processing/energy.c
    WebRTC/common_audio/signal_processing/filter_ar.c
    WebRTC/common_audio/signal_processing/filter_ar_fast_q12.c
    WebRTC/common_audio/signal_processing/filter_ma_fast_q12.c
    WebRTC/common_audio/signal_processing/get_hanning_window.c
    WebRTC/common_audio/signal_processing/get_scaling_square.c
    WebRTC/common_audio/signal_processing/levinson_durbin.c
    WebRTC/common_audio/signal_processing/lpc_to_refl_coef.c
    WebRTC/common_audio/signal_processing/min_max_operations.c
    WebRTC/common_audio/signal_processing/randomization_functions.c
    WebRTC/common_audio/signal_processing/refl_coef_to_lpc.c
    WebRTC/common_audio/signal_processing/resample.c
    WebRTC/common_audio/signal_processing/resample_48khz.c
    WebRTC/common_audio/signal_processing/resample_by_2.c
    WebRTC/common_audio/signal_processing/resample_by_2_internal.c
    WebRTC/common_audio/signal_processing/resample_fractional.c
    WebRTC/common_audio/signal_processing/spl_init.c
    WebRTC/common_audio/signal_processing/spl_inl.c
    WebRTC/common_audio/signal_processing/spl_sqrt.c
    WebRTC/common_audio/signal_processing/splitting_filter.c
    WebRTC/common_audio/signal_processing/sqrt_of_one_minus_x_squared.c
    WebRTC/common_audio/signal_processing/vector_operations.c
    WebRTC/common_audio/signal_processing/vector_scaling_operations.c
    WebRTC/common_audio/smoothing_filter.cc
    WebRTC/common_audio/third_party/ooura/fft_size_128/ooura_fft.cc
    WebRTC/common_audio/third_party/ooura/fft_size_256/fft4g.cc
    WebRTC/common_audio/third_party/spl_sqrt_floor/spl_sqrt_floor.c
    WebRTC/common_audio/vad/vad.cc
    WebRTC/common_audio/vad/vad_core.c
    WebRTC/common_audio/vad/vad_filterbank.c
    WebRTC/common_audio/vad/vad_gmm.c
    WebRTC/common_audio/vad/vad_sp.c
    WebRTC/common_audio/vad/webrtc_vad.c
    WebRTC/common_audio/wav_file.cc
    WebRTC/common_audio/wav_header.cc
    WebRTC/common_audio/window_generator.cc
)

set(SYSTEM_WRAPPERS_SOURCES
    WebRTC/system_wrappers/source/clock.cc
    WebRTC/system_wrappers/source/metrics.cc
)

set(API_AUDIO_SOURCES
    WebRTC/api/audio/audio_frame.cc
    WebRTC/api/audio/audio_processing.cc
    WebRTC/api/audio/audio_processing_statistics.cc
    WebRTC/api/audio/builtin_audio_processing_builder.cc
    WebRTC/api/audio/channel_layout.cc
    WebRTC/api/audio/echo_canceller3_config.cc
    WebRTC/api/audio/echo_canceller3_factory.cc
    WebRTC/api/audio/echo_detector_creator.cc
    WebRTC/api/rtc_event_log/rtc_event_log.cc
    WebRTC/api/rtc_event_log/rtc_event_log_factory.cc
)

set(API_UNITS_SOURCES
    WebRTC/api/units/data_rate.cc
    WebRTC/api/units/data_size.cc
    WebRTC/api/units/frequency.cc
    WebRTC/api/units/time_delta.cc
    WebRTC/api/units/timestamp.cc
)

set(API_ENV_SOURCES
    WebRTC/api/environment/deprecated_global_field_trials.cc
    WebRTC/api/environment/environment_factory.cc
    WebRTC/api/field_trials_registry.cc
)

set(API_TQ_SOURCES
    WebRTC/api/task_queue/default_task_queue_factory_stdlib.cc
    WebRTC/api/task_queue/pending_task_safety_flag.cc
    WebRTC/api/task_queue/task_queue_base.cc
)

set(RTC_BASE_SOURCES
    WebRTC/api/field_trials.cc
    WebRTC/api/task_queue/default_task_queue_factory_stdlib.cc
    WebRTC/rtc_base/checks.cc
    WebRTC/rtc_base/cpu_info.cc
    WebRTC/rtc_base/denormal_disabler.cc
    WebRTC/rtc_base/event.cc
    WebRTC/rtc_base/event_tracer.cc
    WebRTC/rtc_base/experiments/field_trial_parser.cc
    WebRTC/rtc_base/logging.cc
    WebRTC/rtc_base/memory/aligned_malloc.cc
    WebRTC/rtc_base/platform_thread.cc
    WebRTC/rtc_base/platform_thread_types.cc
    WebRTC/rtc_base/race_checker.cc
    WebRTC/rtc_base/random.cc
    WebRTC/rtc_base/ref_counter.cc
    WebRTC/rtc_base/string_encode.cc
    WebRTC/rtc_base/string_to_number.cc
    WebRTC/rtc_base/string_utils.cc
    WebRTC/rtc_base/strings/string_builder.cc
    WebRTC/rtc_base/strings/string_format.cc
    WebRTC/rtc_base/synchronization/sequence_checker_internal.cc
    WebRTC/rtc_base/synchronization/yield.cc
    WebRTC/rtc_base/synchronization/yield_policy.cc
    WebRTC/rtc_base/system/file_wrapper.cc
    WebRTC/rtc_base/system/system_time.cc
    WebRTC/rtc_base/system_time.cc
    WebRTC/rtc_base/task_queue_stdlib.cc
    WebRTC/rtc_base/time_utils.cc
)

set(AUDIO_PROCESSING_SOURCES
    WebRTC/modules/audio_processing/aec_dump/null_aec_dump_factory.cc
    WebRTC/modules/audio_processing/agc/agc.cc
    WebRTC/modules/audio_processing/agc/agc_manager_direct.cc
    WebRTC/modules/audio_processing/agc/legacy/analog_agc.cc
    WebRTC/modules/audio_processing/agc/legacy/digital_agc.cc
    WebRTC/modules/audio_processing/agc/loudness_histogram.cc
    WebRTC/modules/audio_processing/agc/utility.cc
    WebRTC/modules/audio_processing/audio_buffer.cc
    WebRTC/modules/audio_processing/audio_processing_impl.cc
    WebRTC/modules/audio_processing/capture_levels_adjuster/audio_samples_scaler.cc
    WebRTC/modules/audio_processing/capture_levels_adjuster/capture_levels_adjuster.cc
    WebRTC/modules/audio_processing/capture_mixer/audio_content_analyzer.cc
    WebRTC/modules/audio_processing/capture_mixer/capture_mixer.cc
    WebRTC/modules/audio_processing/capture_mixer/channel_content_remixer.cc
    WebRTC/modules/audio_processing/capture_mixer/dc_levels_estimator.cc
    WebRTC/modules/audio_processing/capture_mixer/energy_estimator.cc
    WebRTC/modules/audio_processing/capture_mixer/remixing_logic.cc
    WebRTC/modules/audio_processing/capture_mixer/saturation_estimator.cc
    WebRTC/modules/audio_processing/gain_control_impl.cc
    WebRTC/modules/audio_processing/gain_controller2.cc
    WebRTC/modules/audio_processing/high_pass_filter.cc
    WebRTC/modules/audio_processing/include/aec_dump.cc
    WebRTC/modules/audio_processing/include/audio_frame_proxies.cc
    WebRTC/modules/audio_processing/logging/apm_data_dumper.cc
    WebRTC/modules/audio_processing/post_filter.cc
    WebRTC/modules/audio_processing/residual_echo_detector.cc
    WebRTC/modules/audio_processing/rms_level.cc
    WebRTC/modules/audio_processing/splitting_filter.cc
    WebRTC/modules/audio_processing/three_band_filter_bank.cc
    WebRTC/modules/audio_processing/utility/cascaded_biquad_filter.cc
    WebRTC/modules/audio_processing/utility/delay_estimator.cc
    WebRTC/modules/audio_processing/utility/delay_estimator_wrapper.cc
    WebRTC/modules/audio_processing/utility/pffft_wrapper.cc
)

set(AEC3_SOURCES
    WebRTC/modules/audio_processing/aec3/adaptive_fir_filter.cc
    WebRTC/modules/audio_processing/aec3/adaptive_fir_filter_erl.cc
    WebRTC/modules/audio_processing/aec3/aec3_common.cc
    WebRTC/modules/audio_processing/aec3/aec3_fft.cc
    WebRTC/modules/audio_processing/aec3/aec_state.cc
    WebRTC/modules/audio_processing/aec3/alignment_mixer.cc
    WebRTC/modules/audio_processing/aec3/api_call_jitter_metrics.cc
    WebRTC/modules/audio_processing/aec3/block_buffer.cc
    WebRTC/modules/audio_processing/aec3/block_delay_buffer.cc
    WebRTC/modules/audio_processing/aec3/block_framer.cc
    WebRTC/modules/audio_processing/aec3/block_processor.cc
    WebRTC/modules/audio_processing/aec3/block_processor_metrics.cc
    WebRTC/modules/audio_processing/aec3/clockdrift_detector.cc
    WebRTC/modules/audio_processing/aec3/coarse_filter_update_gain.cc
    WebRTC/modules/audio_processing/aec3/comfort_noise_generator.cc
    WebRTC/modules/audio_processing/aec3/config_selector.cc
    WebRTC/modules/audio_processing/aec3/decimator.cc
    WebRTC/modules/audio_processing/aec3/dominant_nearend_detector.cc
    WebRTC/modules/audio_processing/aec3/downsampled_render_buffer.cc
    WebRTC/modules/audio_processing/aec3/echo_audibility.cc
    WebRTC/modules/audio_processing/aec3/echo_canceller3.cc
    WebRTC/modules/audio_processing/aec3/echo_path_delay_estimator.cc
    WebRTC/modules/audio_processing/aec3/echo_path_variability.cc
    WebRTC/modules/audio_processing/aec3/echo_remover.cc
    WebRTC/modules/audio_processing/aec3/echo_remover_metrics.cc
    WebRTC/modules/audio_processing/aec3/erl_estimator.cc
    WebRTC/modules/audio_processing/aec3/erle_estimator.cc
    WebRTC/modules/audio_processing/aec3/fft_buffer.cc
    WebRTC/modules/audio_processing/aec3/filter_analyzer.cc
    WebRTC/modules/audio_processing/aec3/frame_blocker.cc
    WebRTC/modules/audio_processing/aec3/fullband_erle_estimator.cc
    WebRTC/modules/audio_processing/aec3/matched_filter.cc
    WebRTC/modules/audio_processing/aec3/matched_filter_lag_aggregator.cc
    WebRTC/modules/audio_processing/aec3/moving_average_spectrum.cc
    WebRTC/modules/audio_processing/aec3/multi_channel_content_detector.cc
    WebRTC/modules/audio_processing/aec3/refined_filter_update_gain.cc
    WebRTC/modules/audio_processing/aec3/render_buffer.cc
    WebRTC/modules/audio_processing/aec3/render_delay_buffer.cc
    WebRTC/modules/audio_processing/aec3/render_delay_controller.cc
    WebRTC/modules/audio_processing/aec3/render_delay_controller_metrics.cc
    WebRTC/modules/audio_processing/aec3/render_signal_analyzer.cc
    WebRTC/modules/audio_processing/aec3/residual_echo_estimator.cc
    WebRTC/modules/audio_processing/aec3/reverb_decay_estimator.cc
    WebRTC/modules/audio_processing/aec3/reverb_frequency_response.cc
    WebRTC/modules/audio_processing/aec3/reverb_model.cc
    WebRTC/modules/audio_processing/aec3/reverb_model_estimator.cc
    WebRTC/modules/audio_processing/aec3/signal_dependent_erle_estimator.cc
    WebRTC/modules/audio_processing/aec3/spectrum_buffer.cc
    WebRTC/modules/audio_processing/aec3/stationarity_estimator.cc
    WebRTC/modules/audio_processing/aec3/subband_erle_estimator.cc
    WebRTC/modules/audio_processing/aec3/subband_nearend_detector.cc
    WebRTC/modules/audio_processing/aec3/subtractor.cc
    WebRTC/modules/audio_processing/aec3/subtractor_output.cc
    WebRTC/modules/audio_processing/aec3/subtractor_output_analyzer.cc
    WebRTC/modules/audio_processing/aec3/suppression_filter.cc
    WebRTC/modules/audio_processing/aec3/suppression_gain.cc
    WebRTC/modules/audio_processing/aec3/transparent_mode.cc
)

set(AGC2_SOURCES
    WebRTC/modules/audio_processing/agc2/adaptive_digital_gain_controller.cc
    WebRTC/modules/audio_processing/agc2/biquad_filter.cc
    WebRTC/modules/audio_processing/agc2/clipping_predictor.cc
    WebRTC/modules/audio_processing/agc2/clipping_predictor_level_buffer.cc
    WebRTC/modules/audio_processing/agc2/compute_interpolated_gain_curve.cc
    WebRTC/modules/audio_processing/agc2/cpu_features.cc
    WebRTC/modules/audio_processing/agc2/fixed_digital_level_estimator.cc
    WebRTC/modules/audio_processing/agc2/gain_applier.cc
    WebRTC/modules/audio_processing/agc2/input_volume_controller.cc
    WebRTC/modules/audio_processing/agc2/input_volume_stats_reporter.cc
    WebRTC/modules/audio_processing/agc2/interpolated_gain_curve.cc
    WebRTC/modules/audio_processing/agc2/limiter.cc
    WebRTC/modules/audio_processing/agc2/limiter_db_gain_curve.cc
    WebRTC/modules/audio_processing/agc2/noise_level_estimator.cc
    WebRTC/modules/audio_processing/agc2/rnn_vad/auto_correlation.cc
    WebRTC/modules/audio_processing/agc2/rnn_vad/features_extraction.cc
    WebRTC/modules/audio_processing/agc2/rnn_vad/lp_residual.cc
    WebRTC/modules/audio_processing/agc2/rnn_vad/pitch_search.cc
    WebRTC/modules/audio_processing/agc2/rnn_vad/pitch_search_internal.cc
    WebRTC/modules/audio_processing/agc2/rnn_vad/rnn.cc
    WebRTC/modules/audio_processing/agc2/rnn_vad/rnn_fc.cc
    WebRTC/modules/audio_processing/agc2/rnn_vad/rnn_gru.cc
    WebRTC/modules/audio_processing/agc2/rnn_vad/spectral_features.cc
    WebRTC/modules/audio_processing/agc2/rnn_vad/spectral_features_internal.cc
    WebRTC/modules/audio_processing/agc2/saturation_protector.cc
    WebRTC/modules/audio_processing/agc2/saturation_protector_buffer.cc
    WebRTC/modules/audio_processing/agc2/speech_level_estimator.cc
    WebRTC/modules/audio_processing/agc2/speech_level_estimator_experimental_impl.cc
    WebRTC/modules/audio_processing/agc2/speech_level_estimator_impl.cc
    WebRTC/modules/audio_processing/agc2/speech_probability_buffer.cc
    WebRTC/modules/audio_processing/agc2/vad_wrapper.cc
    WebRTC/modules/audio_processing/agc2/vector_float_frame.cc
)

set(NS_SOURCES
    WebRTC/modules/audio_processing/ns/fast_math.cc
    WebRTC/modules/audio_processing/ns/histograms.cc
    WebRTC/modules/audio_processing/ns/noise_estimator.cc
    WebRTC/modules/audio_processing/ns/noise_suppressor.cc
    WebRTC/modules/audio_processing/ns/ns_fft.cc
    WebRTC/modules/audio_processing/ns/prior_signal_model.cc
    WebRTC/modules/audio_processing/ns/prior_signal_model_estimator.cc
    WebRTC/modules/audio_processing/ns/quantile_noise_estimator.cc
    WebRTC/modules/audio_processing/ns/signal_model.cc
    WebRTC/modules/audio_processing/ns/signal_model_estimator.cc
    WebRTC/modules/audio_processing/ns/speech_probability_estimator.cc
    WebRTC/modules/audio_processing/ns/suppression_params.cc
    WebRTC/modules/audio_processing/ns/wiener_filter.cc
)

set(VAD_SOURCES
    WebRTC/modules/audio_coding/codecs/isac/main/source/filter_functions.c
    WebRTC/modules/audio_coding/codecs/isac/main/source/isac_vad.c
    WebRTC/modules/audio_coding/codecs/isac/main/source/pitch_estimator.c
    WebRTC/modules/audio_coding/codecs/isac/main/source/pitch_filter.c
    WebRTC/modules/audio_processing/vad/gmm.cc
    WebRTC/modules/audio_processing/vad/pitch_based_vad.cc
    WebRTC/modules/audio_processing/vad/pitch_internal.cc
    WebRTC/modules/audio_processing/vad/pole_zero_filter.cc
    WebRTC/modules/audio_processing/vad/standalone_vad.cc
    WebRTC/modules/audio_processing/vad/vad_audio_proc.cc
    WebRTC/modules/audio_processing/vad/vad_circular_buffer.cc
    WebRTC/modules/audio_processing/vad/voice_activity_detector.cc
)

set(THIRD_PARTY_SOURCES
    WebRTC/third_party/pffft/src/fftpack.c
    WebRTC/third_party/pffft/src/pffft.c
    WebRTC/third_party/rnnoise/src/rnn_vad_weights.cc
)
# <<< WEBRTC_AUTO_GENERATED_SOURCES_END >>>

# SIMD sources (AVX2, Neon, SSE2)
set(ARCH_SOURCES)
set(ARCH_COMPILE_FLAGS)

# Detect architecture from CMake variables
if(CMAKE_SYSTEM_PROCESSOR MATCHES "amd64|x86_64|AMD64|x86")
    # SSE2 and AVX2 sources for x86/x64
    set(ARCH_SOURCES
        WebRTC/common_audio/fir_filter_sse.cc
        WebRTC/common_audio/resampler/sinc_resampler_sse.cc
        WebRTC/common_audio/fir_filter_avx2.cc
        WebRTC/common_audio/resampler/sinc_resampler_avx2.cc
        WebRTC/modules/audio_processing/aec3/adaptive_fir_filter_avx2.cc
        WebRTC/modules/audio_processing/aec3/adaptive_fir_filter_erl_avx2.cc
        WebRTC/modules/audio_processing/aec3/fft_data_avx2.cc
        WebRTC/modules/audio_processing/aec3/matched_filter_avx2.cc
        WebRTC/modules/audio_processing/aec3/vector_math_avx2.cc
        WebRTC/modules/audio_processing/agc2/rnn_vad/vector_math_avx2.cc
        WebRTC/common_audio/third_party/ooura/fft_size_128/ooura_fft_sse2.cc
    )
    # Enable AVX2 on x86/x64
    if(MSVC)
        # AVX2 flags for MSVC
        set(ARCH_COMPILE_FLAGS "/arch:AVX2")
    else()
        # AVX2 flags for GCC/Clang
        set(ARCH_COMPILE_FLAGS "-mavx2 -mfma -msse2")
    endif()
    add_compile_definitions(WEBRTC_ENABLE_AVX2)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "arm|armv7|arm64|aarch64|ARM|ARM64")
    # Neon sources for ARM
    set(ARCH_SOURCES
        WebRTC/common_audio/fir_filter_neon.cc
        WebRTC/common_audio/resampler/sinc_resampler_neon.cc
        WebRTC/common_audio/signal_processing/cross_correlation_neon.c
        WebRTC/common_audio/signal_processing/downsample_fast_neon.c
        WebRTC/common_audio/signal_processing/min_max_operations_neon.c
        WebRTC/common_audio/third_party/ooura/fft_size_128/ooura_fft_neon.cc
    )
    # On 32-bit ARM, we might need neon compile flags. On 64-bit ARM (arm64/aarch64), Neon is mandatory and compile flags are not needed.
    if(CMAKE_SIZEOF_VOID_P EQUAL 4 AND NOT MSVC)
        set(ARCH_COMPILE_FLAGS "-mfpu=neon")
    endif()
    add_compile_definitions(WEBRTC_HAS_NEON)
endif()

# Create static library target
add_library(webrtc_audio_processing STATIC
    ${COMMON_AUDIO_SOURCES}
    ${SYSTEM_WRAPPERS_SOURCES}
    ${API_AUDIO_SOURCES}
    ${API_UNITS_SOURCES}
    ${API_ENV_SOURCES}
    ${API_TQ_SOURCES}
    ${RTC_BASE_SOURCES}
    ${AUDIO_PROCESSING_SOURCES}
    ${AEC3_SOURCES}
    ${AGC2_SOURCES}
    ${NS_SOURCES}
    ${VAD_SOURCES}
    ${THIRD_PARTY_SOURCES}
    ${ARCH_SOURCES}
)

# Apply architecture compile flags to specific SIMD files
if(ARCH_COMPILE_FLAGS)
    set_source_files_properties(${ARCH_SOURCES} PROPERTIES COMPILE_FLAGS "${ARCH_COMPILE_FLAGS}")
endif()

# Set up include directories
target_include_directories(webrtc_audio_processing PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/WebRTC>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/abseil-cpp>
)

# Set compile definitions based on target platform
if(WIN32)
    target_compile_definitions(webrtc_audio_processing PRIVATE WEBRTC_WIN NOMINMAX WIN32_LEAN_AND_MEAN _USE_MATH_DEFINES)
elseif(ANDROID)
    target_compile_definitions(webrtc_audio_processing PRIVATE WEBRTC_ANDROID WEBRTC_POSIX)
elseif(APPLE)
    # Apple can be OSX or iOS
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        target_compile_definitions(webrtc_audio_processing PRIVATE WEBRTC_MAC WEBRTC_IOS WEBRTC_POSIX)
    else()
        target_compile_definitions(webrtc_audio_processing PRIVATE WEBRTC_MAC WEBRTC_POSIX)
    endif()
else()
    # Assume Linux or other POSIX
    target_compile_definitions(webrtc_audio_processing PRIVATE WEBRTC_LINUX WEBRTC_POSIX)
endif()

target_compile_definitions(webrtc_audio_processing PRIVATE
    WEBRTC_APM_DEBUG_DUMP=0
)

# Standard C++ definitions
if(MSVC)
    target_compile_options(webrtc_audio_processing PRIVATE /wd4244 /wd4267 /wd4018) # Disable conversions and mismatch warnings
else()
    target_compile_options(webrtc_audio_processing PRIVATE -Wno-narrowing -Wno-unused-parameter)
endif()

# Link with Abseil
if(TARGET absl::strings)
    target_link_libraries(webrtc_audio_processing PUBLIC
        absl::strings
        absl::base
        absl::throw_delegate
        absl::bad_optional_access
        absl::synchronization
    )
endif()
