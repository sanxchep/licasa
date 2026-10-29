# Test executables, CTest registration, and local test-bench targets.
# Included from the root so source/build paths retain their original scope.

find_package(Qt6 REQUIRED COMPONENTS QuickTest Test)
include(ProcessorCount)
ProcessorCount(LICASA_TEST_JOBS)
if(NOT LICASA_TEST_JOBS OR LICASA_TEST_JOBS LESS 1)
    set(LICASA_TEST_JOBS 1)
endif()
set(LICASA_TEST_TARGETS)
set(LICASA_CORE_TEST_TARGETS)
set(LICASA_RAW_TEST_TARGETS)
set(LICASA_AVIF_TEST_TARGETS)
set(LICASA_APNG_TEST_TARGETS)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME licasa_offline_source_tests
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_offline_sources.py")
set_tests_properties(licasa_offline_source_tests PROPERTIES
        TIMEOUT 30 LABELS "packaging;offline;fast")

add_executable(licasa_runtime_error_log_tests
        tests/tst_runtime_error_log.cpp src/app/runtime_error_log.cpp)
target_include_directories(licasa_runtime_error_log_tests PRIVATE src)
target_link_libraries(licasa_runtime_error_log_tests PRIVATE Qt6::Core Qt6::Test)
licasa_enable_warnings(licasa_runtime_error_log_tests)
list(APPEND LICASA_TEST_TARGETS licasa_runtime_error_log_tests)
list(APPEND LICASA_CORE_TEST_TARGETS licasa_runtime_error_log_tests)
add_test(NAME licasa_runtime_error_log_tests COMMAND licasa_runtime_error_log_tests)
set_tests_properties(licasa_runtime_error_log_tests PROPERTIES
        TIMEOUT 15 LABELS "core;logging;fast")

add_executable(licasa_graphics_backend_tests
        tests/tst_graphics_backend.cpp src/platform/graphics_backend.cpp)
target_include_directories(licasa_graphics_backend_tests PRIVATE src)
target_link_libraries(licasa_graphics_backend_tests PRIVATE Qt6::Quick Qt6::Test)
licasa_enable_warnings(licasa_graphics_backend_tests)
list(APPEND LICASA_TEST_TARGETS licasa_graphics_backend_tests)
list(APPEND LICASA_CORE_TEST_TARGETS licasa_graphics_backend_tests)
add_test(NAME licasa_graphics_backend_tests COMMAND licasa_graphics_backend_tests)
set_tests_properties(licasa_graphics_backend_tests PROPERTIES
        TIMEOUT 15 LABELS "core;startup;graphics;fast")

add_test(NAME licasa_native_lifecycle_tests
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_lifecycle.py")
set_tests_properties(licasa_native_lifecycle_tests PROPERTIES TIMEOUT 30 LABELS "lifecycle;packaging;security;fast")

add_executable(licasa_lifecycle_platform_tests tests/tst_lifecycle_platform.cpp)
target_include_directories(licasa_lifecycle_platform_tests PRIVATE src)
target_link_libraries(licasa_lifecycle_platform_tests PRIVATE licasa_lifecycle_platform Qt6::Test)
licasa_enable_warnings(licasa_lifecycle_platform_tests)
list(APPEND LICASA_TEST_TARGETS licasa_lifecycle_platform_tests)
list(APPEND LICASA_CORE_TEST_TARGETS licasa_lifecycle_platform_tests)
# Permission-denial fixtures need desktop credentials even in root package builders.
add_test(NAME licasa_lifecycle_platform_tests
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_unprivileged.py"
                --exec $<TARGET_FILE:licasa_lifecycle_platform_tests>)
set_tests_properties(licasa_lifecycle_platform_tests PROPERTIES TIMEOUT 15 LABELS "core;lifecycle;security;fast")

add_executable(licasa_application_lifecycle_tests tests/tst_application_lifecycle.cpp)
target_include_directories(licasa_application_lifecycle_tests PRIVATE src)
target_link_libraries(licasa_application_lifecycle_tests PRIVATE Qt6::Core Qt6::Test)
licasa_enable_warnings(licasa_application_lifecycle_tests)
list(APPEND LICASA_TEST_TARGETS licasa_application_lifecycle_tests)
list(APPEND LICASA_CORE_TEST_TARGETS licasa_application_lifecycle_tests)
add_test(NAME licasa_application_lifecycle_tests COMMAND licasa_application_lifecycle_tests)
set_tests_properties(licasa_application_lifecycle_tests PROPERTIES TIMEOUT 15 LABELS "core;lifecycle;fast")
add_test(NAME licasa_lifecycle_protocol_tests
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_lifecycle_protocol.py")
set_tests_properties(licasa_lifecycle_protocol_tests PROPERTIES TIMEOUT 30 LABELS "lifecycle;fast")
add_test(NAME licasa_lifecycle_update_tests
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_unprivileged.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_lifecycle_update.py")
set_tests_properties(licasa_lifecycle_update_tests PROPERTIES TIMEOUT 60 LABELS "lifecycle;packaging")

add_executable(licasa_background_mode_tests
        tests/tst_background_mode.cpp
        src/app/application_settings.cpp)
target_include_directories(licasa_background_mode_tests PRIVATE src)
target_link_libraries(licasa_background_mode_tests PRIVATE Qt6::Core Qt6::Test licasa_lifecycle_platform)
licasa_enable_warnings(licasa_background_mode_tests)
list(APPEND LICASA_TEST_TARGETS licasa_background_mode_tests)
list(APPEND LICASA_CORE_TEST_TARGETS licasa_background_mode_tests)
add_test(NAME licasa_background_mode_tests COMMAND licasa_background_mode_tests)
set_tests_properties(licasa_background_mode_tests PROPERTIES
        TIMEOUT 15 LABELS "core;startup;fast")

add_executable(licasa_format_navigation_tests
        tests/tst_format_navigation.cpp
        src/imaging/format_support.cpp)
target_include_directories(licasa_format_navigation_tests PRIVATE src)
target_link_libraries(licasa_format_navigation_tests PRIVATE Qt6::Gui Qt6::Test)
licasa_enable_warnings(licasa_format_navigation_tests)
list(APPEND LICASA_TEST_TARGETS licasa_format_navigation_tests)
list(APPEND LICASA_CORE_TEST_TARGETS licasa_format_navigation_tests)
add_test(NAME licasa_format_navigation_tests COMMAND licasa_format_navigation_tests)
set_tests_properties(licasa_format_navigation_tests PROPERTIES
        TIMEOUT 20 LABELS "core;navigation;fast")

if(LICASA_ENABLE_EARLY_PROGRESSIVE_JPEG)
    add_executable(licasa_progressive_jpeg_preview_tests
            tests/tst_progressive_jpeg_preview.cpp
            src/imaging/progressive_jpeg_preview.cpp)
    target_include_directories(licasa_progressive_jpeg_preview_tests PRIVATE src)
    target_compile_definitions(licasa_progressive_jpeg_preview_tests PRIVATE
            LICASA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(licasa_progressive_jpeg_preview_tests PRIVATE
            Qt6::Gui Qt6::Test JPEG::JPEG)
    licasa_enable_warnings(licasa_progressive_jpeg_preview_tests)
    list(APPEND LICASA_TEST_TARGETS licasa_progressive_jpeg_preview_tests)
    add_test(NAME licasa_progressive_jpeg_preview_tests
            COMMAND licasa_progressive_jpeg_preview_tests -platform offscreen)
    set_tests_properties(licasa_progressive_jpeg_preview_tests PROPERTIES
            TIMEOUT 20 LABELS "codec;jpeg;preview;fast")
endif()

if(LICASA_ENABLE_QOI OR LICASA_ENABLE_JP2)
    add_executable(licasa_extra_image_reader_tests tests/tst_extra_image_readers.cpp)
    target_link_libraries(licasa_extra_image_reader_tests PRIVATE Qt6::Gui Qt6::Test)
    target_compile_definitions(licasa_extra_image_reader_tests PRIVATE
            LICASA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    if(LICASA_ENABLE_QOI)
        target_compile_definitions(licasa_extra_image_reader_tests PRIVATE LICASA_TEST_QOI=1)
        add_dependencies(licasa_extra_image_reader_tests licasa_qoi_plugin)
    endif()
    if(LICASA_ENABLE_JP2)
        target_compile_definitions(licasa_extra_image_reader_tests PRIVATE LICASA_TEST_JP2=1)
        add_dependencies(licasa_extra_image_reader_tests licasa_jp2_plugin)
    endif()
    licasa_enable_warnings(licasa_extra_image_reader_tests)
    list(APPEND LICASA_TEST_TARGETS licasa_extra_image_reader_tests)
    add_test(NAME licasa_extra_image_reader_tests COMMAND licasa_extra_image_reader_tests -platform offscreen)
    set_tests_properties(licasa_extra_image_reader_tests PROPERTIES
            ENVIRONMENT "QT_PLUGIN_PATH=${CMAKE_CURRENT_BINARY_DIR}/plugins"
            TIMEOUT 30 LABELS "codec;format;security;fast")
endif()

add_executable(licasa_single_instance_endpoint_tests
        tests/tst_single_instance_endpoint.cpp
        src/platform/single_instance_endpoint.cpp)
target_include_directories(licasa_single_instance_endpoint_tests PRIVATE src)
target_link_libraries(licasa_single_instance_endpoint_tests PRIVATE
        Qt6::Core Qt6::Network Qt6::Test)
licasa_enable_warnings(licasa_single_instance_endpoint_tests)
list(APPEND LICASA_TEST_TARGETS licasa_single_instance_endpoint_tests)
list(APPEND LICASA_CORE_TEST_TARGETS licasa_single_instance_endpoint_tests)
add_test(NAME licasa_single_instance_endpoint_tests
        COMMAND licasa_single_instance_endpoint_tests)
set_tests_properties(licasa_single_instance_endpoint_tests PROPERTIES
        TIMEOUT 15
        LABELS "core;ipc;security;fast")

if(LICASA_ENABLE_MOTION_PLAYBACK)
    add_executable(licasa_motion_playback_tests
            tests/tst_motion_photo_playback.cpp
            src/media/photo_asset_probe.cpp
            src/media/live_photo_pairing.cpp
            src/media/android_motion_photo.cpp
            src/io/byte_range_device.cpp
            src/imaging/codec_plugins.cpp
            )
    target_link_libraries(licasa_motion_playback_tests PRIVATE licasa_image_services)
    target_include_directories(licasa_motion_playback_tests PRIVATE src)
    target_compile_definitions(licasa_motion_playback_tests PRIVATE
            LICASA_TEST_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/test-assets"
            LICASA_EXPECTED_FFMPEG_PLUGIN="${CMAKE_CURRENT_BINARY_DIR}/plugins/multimedia/libffmpegmediaplugin.so")
    # The playback target stays independent of the Qt Quick image services.
    target_link_libraries(licasa_motion_playback_tests PRIVATE
            licasa_motion_playback
            Qt6::Quick
            Qt6::Test)
    licasa_enable_warnings(licasa_motion_playback_tests)
    list(APPEND LICASA_TEST_TARGETS licasa_motion_playback_tests)
    add_test(NAME licasa_motion_playback_tests COMMAND licasa_motion_playback_tests -platform offscreen)
    set_tests_properties(licasa_motion_playback_tests PROPERTIES
            TIMEOUT 90
            ENVIRONMENT "QT_MEDIA_BACKEND=ffmpeg;QT_PLUGIN_PATH=${CMAKE_CURRENT_BINARY_DIR}/plugins;QT_FFMPEG_DECODING_HW_DEVICE_TYPES=,"
            LABELS "motion-photo;playback;integration;security;lifecycle")
    add_test(NAME licasa_motion_playback_metadata_complexity_preflight
            COMMAND licasa_motion_playback_tests
                    metadataComplexityLimitIsFailClosed -platform offscreen)
    set_tests_properties(licasa_motion_playback_metadata_complexity_preflight PROPERTIES
            TIMEOUT 20
            ENVIRONMENT "QT_MEDIA_BACKEND=ffmpeg;QT_PLUGIN_PATH=${CMAKE_CURRENT_BINARY_DIR}/plugins;LICASA_EXPECT_PREPLAYBACK_LAZY=1"
            LABELS "motion-photo;playback;security;preflight;fast")
    if(LICASA_SANITIZER)
        set_property(TEST licasa_motion_playback_metadata_complexity_preflight
                APPEND PROPERTY ENVIRONMENT "QT_FFMPEG_DECODING_HW_DEVICE_TYPES=,")
    endif()
endif()

add_executable(licasa_apple_live_photo_tests
        tests/tst_live_photo_pairing.cpp
        src/media/live_photo_pairing.cpp)
target_include_directories(licasa_apple_live_photo_tests PRIVATE src)
target_link_libraries(licasa_apple_live_photo_tests PRIVATE Qt6::Core Qt6::Test)
licasa_enable_warnings(licasa_apple_live_photo_tests)
list(APPEND LICASA_TEST_TARGETS licasa_apple_live_photo_tests)
list(APPEND LICASA_CORE_TEST_TARGETS licasa_apple_live_photo_tests)
add_test(NAME licasa_apple_live_photo_tests COMMAND licasa_apple_live_photo_tests)
set_tests_properties(licasa_apple_live_photo_tests PROPERTIES
        TIMEOUT 30
        LABELS "core;compound;motion-photo;apple-live-photo;security;fast")

add_executable(licasa_byte_range_tests tests/tst_byte_range_device.cpp
        src/io/byte_range_device.cpp)
target_include_directories(licasa_byte_range_tests PRIVATE src)
target_link_libraries(licasa_byte_range_tests PRIVATE Qt6::Core Qt6::Test)
licasa_enable_warnings(licasa_byte_range_tests)
list(APPEND LICASA_TEST_TARGETS licasa_byte_range_tests)
list(APPEND LICASA_CORE_TEST_TARGETS licasa_byte_range_tests)
add_test(NAME licasa_byte_range_tests COMMAND licasa_byte_range_tests)
set_tests_properties(licasa_byte_range_tests PROPERTIES
        TIMEOUT 30 LABELS "core;compound;byte-range;security;fast")

add_executable(licasa_android_motion_photo_tests
        tests/tst_android_motion_photo.cpp
        tests/tst_android_motion_photo.h
        src/media/android_motion_photo.cpp src/io/byte_range_device.cpp)
add_executable(licasa_photo_asset_probe_tests
        tests/tst_photo_asset_probe.cpp
        tests/tst_photo_asset_probe.h
        src/media/photo_asset_probe.cpp src/media/live_photo_pairing.cpp
        src/media/android_motion_photo.cpp src/io/byte_range_device.cpp src/imaging/codec_plugins.cpp)
target_include_directories(licasa_photo_asset_probe_tests PRIVATE src)
target_link_libraries(licasa_photo_asset_probe_tests PRIVATE Qt6::Gui Qt6::Test)
if(LICASA_ENABLE_HEIF)
    add_dependencies(licasa_photo_asset_probe_tests licasa_heif licasa_heif_plugin)
endif()
if(LICASA_ENABLE_AVIF)
    add_dependencies(licasa_photo_asset_probe_tests licasa_avif licasa_avif_plugin)
endif()
licasa_enable_warnings(licasa_photo_asset_probe_tests)
list(APPEND LICASA_TEST_TARGETS licasa_photo_asset_probe_tests)
list(APPEND LICASA_CORE_TEST_TARGETS licasa_photo_asset_probe_tests)
add_test(NAME licasa_photo_asset_probe_tests COMMAND licasa_photo_asset_probe_tests)
set_tests_properties(licasa_photo_asset_probe_tests PROPERTIES
        TIMEOUT 30 LABELS "core;compound;motion-photo;integration;security;fast")

add_executable(licasa_atomic_file_copy_tests
        tests/tst_atomic_file_copy.cpp
        src/io/atomic_file_copy.cpp
        src/io/byte_range_device.cpp)
target_include_directories(licasa_atomic_file_copy_tests PRIVATE src)
target_link_libraries(licasa_atomic_file_copy_tests PRIVATE Qt6::Core Qt6::Test)
licasa_enable_warnings(licasa_atomic_file_copy_tests)
list(APPEND LICASA_TEST_TARGETS licasa_atomic_file_copy_tests)
list(APPEND LICASA_CORE_TEST_TARGETS licasa_atomic_file_copy_tests)
add_test(NAME licasa_atomic_file_copy_tests COMMAND licasa_atomic_file_copy_tests)
set_tests_properties(licasa_atomic_file_copy_tests PROPERTIES
        TIMEOUT 15 LABELS "core;export;security;fast")

add_executable(licasa_motion_photo_export_tests
        tests/tst_motion_photo_export.cpp
        src/export/motion_photo_export.cpp
        src/io/atomic_file_copy.cpp
        src/media/photo_asset_probe.cpp
        src/media/live_photo_pairing.cpp
        src/media/android_motion_photo.cpp
        src/io/byte_range_device.cpp
        src/imaging/codec_plugins.cpp)
target_include_directories(licasa_motion_photo_export_tests PRIVATE src)
target_link_libraries(licasa_motion_photo_export_tests PRIVATE Qt6::Gui Qt6::Test)
if(LICASA_ENABLE_HEIF)
    add_dependencies(licasa_motion_photo_export_tests licasa_heif licasa_heif_plugin)
endif()
if(LICASA_ENABLE_AVIF)
    add_dependencies(licasa_motion_photo_export_tests licasa_avif licasa_avif_plugin)
endif()
licasa_enable_warnings(licasa_motion_photo_export_tests)
list(APPEND LICASA_TEST_TARGETS licasa_motion_photo_export_tests)
list(APPEND LICASA_CORE_TEST_TARGETS licasa_motion_photo_export_tests)
add_test(NAME licasa_motion_photo_export_tests COMMAND licasa_motion_photo_export_tests)
set_tests_properties(licasa_motion_photo_export_tests PROPERTIES
        TIMEOUT 30 LABELS "core;compound;motion-photo;export;security;fast")

add_executable(licasa_animation_export_tests
        tests/tst_animation_export.cpp
        src/export/animation_export.cpp
        src/io/atomic_file_copy.cpp
        src/io/byte_range_device.cpp
        src/imaging/codec_plugins.cpp)
target_include_directories(licasa_animation_export_tests PRIVATE src)
target_compile_definitions(licasa_animation_export_tests PRIVATE
        LICASA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(licasa_animation_export_tests PRIVATE
        licasa_image_services Qt6::Gui Qt6::Test)
if(LICASA_ENABLE_HEIF)
    add_dependencies(licasa_animation_export_tests licasa_heif licasa_heif_plugin)
endif()
if(LICASA_ENABLE_AVIF)
    add_dependencies(licasa_animation_export_tests licasa_avif licasa_avif_plugin)
endif()
licasa_enable_warnings(licasa_animation_export_tests)
list(APPEND LICASA_TEST_TARGETS licasa_animation_export_tests)
list(APPEND LICASA_CORE_TEST_TARGETS licasa_animation_export_tests)
add_test(NAME licasa_animation_export_tests COMMAND licasa_animation_export_tests -platform offscreen)
set_tests_properties(licasa_animation_export_tests PROPERTIES
        TIMEOUT 30 LABELS "core;animation;export;security;fast")

add_executable(licasa_preferred_cover_frame_store_tests
        tests/tst_preferred_cover_frame_store.cpp
        src/media/preferred_cover_frame_store.cpp)
target_include_directories(licasa_preferred_cover_frame_store_tests PRIVATE src)
target_link_libraries(licasa_preferred_cover_frame_store_tests PRIVATE Qt6::Core Qt6::Test)
licasa_enable_warnings(licasa_preferred_cover_frame_store_tests)
list(APPEND LICASA_TEST_TARGETS licasa_preferred_cover_frame_store_tests)
list(APPEND LICASA_CORE_TEST_TARGETS licasa_preferred_cover_frame_store_tests)
add_test(NAME licasa_preferred_cover_frame_store_tests
        COMMAND licasa_preferred_cover_frame_store_tests)
set_tests_properties(licasa_preferred_cover_frame_store_tests PROPERTIES
        TIMEOUT 30 LABELS "core;temporal;cover-frame;persistence;security;fast")

add_executable(licasa_preferred_cover_frame_coordinator_tests
        tests/tst_preferred_cover_frame_coordinator.cpp
        src/media/preferred_cover_frame_coordinator.cpp
        src/media/preferred_cover_frame_store.cpp)
target_include_directories(licasa_preferred_cover_frame_coordinator_tests PRIVATE src)
target_link_libraries(licasa_preferred_cover_frame_coordinator_tests PRIVATE Qt6::Core Qt6::Test)
licasa_enable_warnings(licasa_preferred_cover_frame_coordinator_tests)
list(APPEND LICASA_TEST_TARGETS licasa_preferred_cover_frame_coordinator_tests)
list(APPEND LICASA_CORE_TEST_TARGETS licasa_preferred_cover_frame_coordinator_tests)
add_test(NAME licasa_preferred_cover_frame_coordinator_tests
        COMMAND licasa_preferred_cover_frame_coordinator_tests)
set_tests_properties(licasa_preferred_cover_frame_coordinator_tests PROPERTIES
        TIMEOUT 30 LABELS "core;temporal;cover-frame;persistence;security;fast")

target_include_directories(licasa_android_motion_photo_tests PRIVATE src)
target_link_libraries(licasa_android_motion_photo_tests PRIVATE Qt6::Core Qt6::Test)
licasa_enable_warnings(licasa_android_motion_photo_tests)
list(APPEND LICASA_TEST_TARGETS licasa_android_motion_photo_tests)
list(APPEND LICASA_CORE_TEST_TARGETS licasa_android_motion_photo_tests)
add_test(NAME licasa_android_motion_photo_tests COMMAND licasa_android_motion_photo_tests)
set_tests_properties(licasa_android_motion_photo_tests PROPERTIES
        TIMEOUT 30 LABELS "core;compound;motion-photo;security;fast")

if(LICASA_ENABLE_RAW)
    add_executable(licasa_raw_tests
            tests/tst_raw_plugin.cpp
            src/imaging/codec_plugins.cpp
            )
    target_link_libraries(licasa_raw_tests PRIVATE licasa_image_services)
    target_include_directories(licasa_raw_tests PRIVATE src)
    target_compile_definitions(licasa_raw_tests PRIVATE LICASA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(licasa_raw_tests PRIVATE Qt6::Quick Qt6::Test PkgConfig::LICASA_RAW)
    licasa_enable_warnings(licasa_raw_tests)
    add_dependencies(licasa_raw_tests licasa_raw licasa_raw_plugin)
    list(APPEND LICASA_TEST_TARGETS licasa_raw_tests)
    list(APPEND LICASA_RAW_TEST_TARGETS licasa_raw_tests)
    add_test(NAME licasa_raw_tests COMMAND licasa_raw_tests -platform offscreen)
    add_test(NAME licasa_raw_hostile_environment COMMAND licasa_raw_tests -platform offscreen
            oversizedSensorKeepsPreview malformedPreviewsAndMetadata boundedStreamArithmetic)
    set_tests_properties(licasa_raw_hostile_environment PROPERTIES ENVIRONMENT "QT_IMAGEIO_MAXALLOC=0")
    set_tests_properties(licasa_raw_tests PROPERTIES TIMEOUT 120 LABELS "codec;raw;fast")
    set_tests_properties(licasa_raw_hostile_environment PROPERTIES
            TIMEOUT 120 LABELS "codec;raw;security")
endif()

if(LICASA_ENABLE_AVIF)
    add_executable(licasa_avif_tests
            tests/tst_avif_plugin.cpp
            src/imaging/codec_plugins.cpp
            )
    target_link_libraries(licasa_avif_tests PRIVATE licasa_image_services)
    target_include_directories(licasa_avif_tests PRIVATE src)
    target_compile_definitions(licasa_avif_tests PRIVATE LICASA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(licasa_avif_tests PRIVATE Qt6::Quick Qt6::Test)
    licasa_enable_warnings(licasa_avif_tests)
    add_dependencies(licasa_avif_tests licasa_avif licasa_avif_plugin)
    list(APPEND LICASA_TEST_TARGETS licasa_avif_tests)
    list(APPEND LICASA_AVIF_TEST_TARGETS licasa_avif_tests)
    add_test(NAME licasa_avif_tests COMMAND licasa_avif_tests -platform offscreen)
    add_test(NAME licasa_avif_hostile_environment COMMAND licasa_avif_tests -platform offscreen
            selectedPixelLimitRejectsBeforeAv1Decode cancellationBeforeDecode
            containerStructureBounds malformedAndTruncatedInputs)
    set_tests_properties(licasa_avif_hostile_environment PROPERTIES ENVIRONMENT "QT_IMAGEIO_MAXALLOC=0")
    set_tests_properties(licasa_avif_tests PROPERTIES TIMEOUT 90 LABELS "codec;avif;fast")
    set_tests_properties(licasa_avif_hostile_environment PROPERTIES
            TIMEOUT 90 LABELS "codec;avif;security")

endif()

if(LICASA_ENABLE_APNG)
    add_executable(licasa_apng_tests
            tests/tst_apng_plugin.cpp
            src/imaging/codec_plugins.cpp
            src/imaging/image_animation.cpp
            )
    target_link_libraries(licasa_apng_tests PRIVATE licasa_image_services)
    target_include_directories(licasa_apng_tests PRIVATE src)
    target_compile_definitions(licasa_apng_tests PRIVATE
            LICASA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(licasa_apng_tests PRIVATE Qt6::Quick Qt6::Test)
    licasa_enable_warnings(licasa_apng_tests)
    add_dependencies(licasa_apng_tests licasa_apng licasa_apng_plugin)
    list(APPEND LICASA_TEST_TARGETS licasa_apng_tests)
    list(APPEND LICASA_APNG_TEST_TARGETS licasa_apng_tests)

    add_test(NAME licasa_apng_tests COMMAND licasa_apng_tests -platform offscreen
            animationContractAndComposition pngExtensionIsDetectedButStaticPngFallsThrough
            selectedPixelLimitRejectsBeforeRasterDecode cancellationBeforeRasterDecode)
    set_tests_properties(licasa_apng_tests PROPERTIES
            TIMEOUT 60 LABELS "codec;apng;animation;fast")

    add_test(NAME licasa_apng_hostile_environment COMMAND licasa_apng_tests -platform offscreen
            selectedPixelLimitRejectsBeforeRasterDecode cancellationBeforeRasterDecode
            malformedInputsFailBeforeRasterDecode)
    set_tests_properties(licasa_apng_hostile_environment PROPERTIES
            ENVIRONMENT "QT_IMAGEIO_MAXALLOC=0"
            TIMEOUT 60 LABELS "codec;apng;animation;security")

    add_test(NAME licasa_apng_animation_integration COMMAND licasa_apng_tests -platform offscreen
            genericAnimationServiceSeeksApng)
    set_tests_properties(licasa_apng_animation_integration PROPERTIES
            TIMEOUT 60 LABELS "codec;apng;animation;integration")
endif()

if(LICASA_ENABLE_JXL)
    add_executable(licasa_jxl_tests
            tests/tst_jxl_plugin.cpp
            src/imaging/codec_plugins.cpp
            )
    target_link_libraries(licasa_jxl_tests PRIVATE licasa_image_services)
    target_include_directories(licasa_jxl_tests PRIVATE src)
    target_compile_definitions(licasa_jxl_tests PRIVATE LICASA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(licasa_jxl_tests PRIVATE Qt6::Quick Qt6::Test)
    licasa_enable_warnings(licasa_jxl_tests)
    add_dependencies(licasa_jxl_tests licasa_jxl licasa_jxl_plugin)
    list(APPEND LICASA_TEST_TARGETS licasa_jxl_tests)
    add_test(NAME licasa_jxl_tests COMMAND licasa_jxl_tests -platform offscreen)
    add_test(NAME licasa_jxl_hostile_environment COMMAND licasa_jxl_tests -platform offscreen
            metadataAndOversizedAdmission embeddedPreviewSkipsPrimary malformedAndAllocationFailures)
    set_tests_properties(licasa_jxl_hostile_environment PROPERTIES ENVIRONMENT "QT_IMAGEIO_MAXALLOC=0")
    set_tests_properties(licasa_jxl_tests PROPERTIES TIMEOUT 150 LABELS "codec;jxl;fast")
    set_tests_properties(licasa_jxl_hostile_environment PROPERTIES
            TIMEOUT 150 LABELS "codec;jxl;security")
    add_executable(licasa_animation_tests
            tests/tst_image_animation.cpp
            src/imaging/codec_plugins.cpp
            src/imaging/image_animation.cpp
            )
    target_link_libraries(licasa_animation_tests PRIVATE licasa_image_services)
    target_include_directories(licasa_animation_tests PRIVATE src)
    target_compile_definitions(licasa_animation_tests PRIVATE LICASA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(licasa_animation_tests PRIVATE Qt6::Quick Qt6::Test)
    licasa_enable_warnings(licasa_animation_tests)
    add_dependencies(licasa_animation_tests licasa_jxl licasa_jxl_plugin)
    list(APPEND LICASA_TEST_TARGETS licasa_animation_tests)
    add_test(NAME licasa_animation_tests COMMAND licasa_animation_tests -platform offscreen)
    set_tests_properties(licasa_animation_tests PROPERTIES
            TIMEOUT 90 LABELS "codec;animation;fast")
endif()

if(LICASA_ENABLE_HEIF)
    add_executable(licasa_heif_tests
            tests/tst_heif_plugin.cpp
            src/imaging/codec_plugins.cpp
            src/imaging/image_animation.cpp
            )
    target_link_libraries(licasa_heif_tests PRIVATE licasa_image_services)
    target_include_directories(licasa_heif_tests PRIVATE src)
    # Qt Quick's Snap SDK headers include an older libheif. The reference
    # decoder must compile against the same pinned headers as the HEIF plugin.
    target_include_directories(licasa_heif_tests BEFORE PRIVATE "${LICASA_HEIF_INCLUDES}")
    target_compile_definitions(licasa_heif_tests PRIVATE LICASA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(licasa_heif_tests PRIVATE Qt6::Quick Qt6::Test heif de265)
    licasa_enable_warnings(licasa_heif_tests)
    add_dependencies(licasa_heif_tests licasa_heif licasa_heif_plugin)
    list(APPEND LICASA_TEST_TARGETS licasa_heif_tests)
    add_test(NAME licasa_heif_tests COMMAND licasa_heif_tests -platform offscreen)
    add_test(NAME licasa_heif_hostile_environment COMMAND licasa_heif_tests -platform offscreen
            oversizedGridUsesSmallTiles embeddedPreviewAvoidsPrimaryRaster malformedInputs)
    set_tests_properties(licasa_heif_hostile_environment PROPERTIES
            ENVIRONMENT "LIBHEIF_SECURITY_LIMITS=off;QT_IMAGEIO_MAXALLOC=0")
    set_tests_properties(licasa_heif_tests PROPERTIES TIMEOUT 90 LABELS "codec;heif;fast")
    set_tests_properties(licasa_heif_hostile_environment PROPERTIES
            TIMEOUT 90 LABELS "codec;heif;security")
endif()

add_executable(licasa_accessibility_tests tests/tst_accessibility_controls.cpp)
target_link_libraries(licasa_accessibility_tests PRIVATE Qt6::Quick Qt6::Qml Qt6::Test)
target_compile_definitions(licasa_accessibility_tests PRIVATE
        LICASA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
licasa_enable_warnings(licasa_accessibility_tests)
list(APPEND LICASA_TEST_TARGETS licasa_accessibility_tests)
list(APPEND LICASA_CORE_TEST_TARGETS licasa_accessibility_tests)
add_test(NAME licasa_accessibility_controls COMMAND licasa_accessibility_tests -platform offscreen)
set_tests_properties(licasa_accessibility_controls PROPERTIES
        TIMEOUT 15 LABELS "core;qml;accessibility;fast" ENVIRONMENT "QT_QUICK_BACKEND=software")

# FileDialog failures live in native Qt code, beyond Loader.Error. Exercise
# the shipped QML wrappers in a clean engine and a deliberately incomplete
# import tree in separate processes before qualification can pass.
add_executable(licasa_file_dialog_tests tests/tst_file_dialog_runtime.cpp)
target_link_libraries(licasa_file_dialog_tests PRIVATE Qt6::Quick Qt6::Qml Qt6::Test)
get_target_property(LICASA_TEST_QMAKE Qt6::qmake IMPORTED_LOCATION)
execute_process(COMMAND "${LICASA_TEST_QMAKE}" -query QT_INSTALL_QML
        OUTPUT_VARIABLE LICASA_TEST_QML_ROOT OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY)
target_compile_definitions(licasa_file_dialog_tests PRIVATE
        LICASA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        LICASA_TEST_QML_ROOT="${LICASA_TEST_QML_ROOT}")
licasa_enable_warnings(licasa_file_dialog_tests)
list(APPEND LICASA_TEST_TARGETS licasa_file_dialog_tests)
list(APPEND LICASA_CORE_TEST_TARGETS licasa_file_dialog_tests)
add_test(NAME licasa_file_dialog_runtime COMMAND licasa_file_dialog_tests -platform offscreen)
add_test(NAME licasa_file_dialog_missing_module COMMAND licasa_file_dialog_tests -platform offscreen)
set_tests_properties(licasa_file_dialog_runtime licasa_file_dialog_missing_module PROPERTIES
        TIMEOUT 30 LABELS "core;qml;dialogs;packaging"
        ENVIRONMENT "QT_QUICK_BACKEND=software")
set_property(TEST licasa_file_dialog_missing_module APPEND PROPERTY ENVIRONMENT
        "LICASA_TEST_MISSING_DIALOG_MODULE=1")

add_executable(licasa_qml_tests
        tests/quicktest_main.cpp
        tests/editor_test_support.h
        src/imaging/codec_plugins.cpp
        src/imaging/image_animation.cpp
        )
target_link_libraries(licasa_qml_tests PRIVATE licasa_image_services)
target_include_directories(licasa_qml_tests PRIVATE src)
qt_add_resources(licasa_qml_tests "editor_dialog_fixtures"
        PREFIX "/Licasa/qml"
        BASE "${CMAKE_CURRENT_SOURCE_DIR}/qml"
        FILES qml/SaveDialog.qml qml/FileUrls.js qml/DialogRuntime.js)

# The harness uses Qt Gui/Qml APIs directly; the production service target
# additionally propagates its Qt Quick requirements.
target_link_libraries(licasa_qml_tests PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        Qt6::QuickTest
        Qt6::Test
)
if(LICASA_QT_QUICK_BACKPORT_LIBRARY)
    # Keep Qt Quick as a direct NEEDED entry when qualifying the verified
    # same-version backport, even with --as-needed linkers.
    target_link_libraries(licasa_qml_tests PRIVATE
            "-Wl,--push-state,--no-as-needed" Qt6::Quick "-Wl,--pop-state")
else()
    target_link_libraries(licasa_qml_tests PRIVATE Qt6::Quick)
endif()
licasa_enable_warnings(licasa_qml_tests)
list(APPEND LICASA_TEST_TARGETS licasa_qml_tests)
list(APPEND LICASA_CORE_TEST_TARGETS licasa_qml_tests)

target_compile_definitions(licasa_qml_tests PRIVATE
        QUICK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests"
)

add_test(NAME licasa_qml_tests
        COMMAND licasa_qml_tests -platform offscreen)
set_tests_properties(licasa_qml_tests PROPERTIES
        ENVIRONMENT "QT_QUICK_BACKEND=software"
        LABELS "core;qml;fast"
)
add_executable(licasa_async_image_provider_tests tests/tst_async_image_provider.cpp)
target_link_libraries(licasa_async_image_provider_tests PRIVATE licasa_image_services Qt6::Test)
if(LICASA_HAVE_FAST_TIFF_PREVIEW)
    target_compile_definitions(licasa_async_image_provider_tests PRIVATE
            LICASA_HAVE_FAST_TIFF_PREVIEW=1)
endif()
if(LICASA_HAVE_FAST_PNG_PREVIEW)
    target_compile_definitions(licasa_async_image_provider_tests PRIVATE
            LICASA_HAVE_FAST_PNG_PREVIEW=1)
endif()
if(LICASA_WEBP_FOUND)
    target_compile_definitions(licasa_async_image_provider_tests PRIVATE
            LICASA_HAVE_FAST_WEBP_PREVIEW=1)
endif()
licasa_enable_warnings(licasa_async_image_provider_tests)
list(APPEND LICASA_TEST_TARGETS licasa_async_image_provider_tests)
list(APPEND LICASA_CORE_TEST_TARGETS licasa_async_image_provider_tests)
add_test(NAME licasa_async_image_provider_tests
        COMMAND licasa_async_image_provider_tests -platform offscreen)
set_tests_properties(licasa_async_image_provider_tests PROPERTIES
        TIMEOUT 15 LABELS "core;async;lifecycle;fast")

add_executable(licasa_fast_pnm_preview_tests tests/tst_fast_pnm_preview.cpp
        src/imaging/fast_pnm_preview.cpp src/imaging/fast_pnm_preview.h)
target_include_directories(licasa_fast_pnm_preview_tests PRIVATE src)
target_link_libraries(licasa_fast_pnm_preview_tests PRIVATE Qt6::Gui Qt6::Test)
licasa_enable_warnings(licasa_fast_pnm_preview_tests)
list(APPEND LICASA_TEST_TARGETS licasa_fast_pnm_preview_tests)
list(APPEND LICASA_CORE_TEST_TARGETS licasa_fast_pnm_preview_tests)
add_test(NAME licasa_fast_pnm_preview_tests COMMAND licasa_fast_pnm_preview_tests)
set_tests_properties(licasa_fast_pnm_preview_tests PROPERTIES
        TIMEOUT 30 LABELS "core;codec;preview;security")

add_executable(licasa_image_edit_pipeline_tests tests/tst_image_edit_pipeline.cpp)
if(LICASA_HAVE_FAST_TIFF_PREVIEW)
    add_executable(licasa_preview_runtime_tests tests/tst_preview_runtime.cpp)
    target_link_libraries(licasa_preview_runtime_tests PRIVATE
            licasa_image_services Qt6::Test TIFF::TIFF)
    licasa_enable_warnings(licasa_preview_runtime_tests)
    list(APPEND LICASA_TEST_TARGETS licasa_preview_runtime_tests)
    add_test(NAME licasa_preview_runtime_tests COMMAND licasa_preview_runtime_tests)
    set_tests_properties(licasa_preview_runtime_tests PROPERTIES
            TIMEOUT 60 LABELS "codec;preview;security")
endif()

add_executable(licasa_runtime_edit_round2_tests tests/tst_runtime_edit_round2.cpp)
target_link_libraries(licasa_runtime_edit_round2_tests PRIVATE licasa_image_services Qt6::Test)
licasa_enable_warnings(licasa_runtime_edit_round2_tests)
list(APPEND LICASA_TEST_TARGETS licasa_runtime_edit_round2_tests)
list(APPEND LICASA_CORE_TEST_TARGETS licasa_runtime_edit_round2_tests)
add_test(NAME licasa_runtime_edit_round2_tests COMMAND licasa_runtime_edit_round2_tests)
set_tests_properties(licasa_runtime_edit_round2_tests PROPERTIES
        TIMEOUT 120 LABELS "core;editing")

add_executable(licasa_runtime_edit_round3_tests tests/tst_runtime_edit_round3.cpp)
target_link_libraries(licasa_runtime_edit_round3_tests PRIVATE licasa_image_services Qt6::Test)
licasa_enable_warnings(licasa_runtime_edit_round3_tests)
list(APPEND LICASA_TEST_TARGETS licasa_runtime_edit_round3_tests)
list(APPEND LICASA_CORE_TEST_TARGETS licasa_runtime_edit_round3_tests)
add_test(NAME licasa_runtime_edit_round3_tests COMMAND licasa_runtime_edit_round3_tests)
set_tests_properties(licasa_runtime_edit_round3_tests PROPERTIES
        TIMEOUT 120 LABELS "core;editing")

add_executable(licasa_metadata_round3_tests tests/tst_metadata_round3.cpp
        src/media/android_motion_photo.cpp src/media/live_photo_pairing.cpp
        src/media/photo_asset_probe.cpp src/io/byte_range_device.cpp
        src/imaging/format_support.cpp)
target_include_directories(licasa_metadata_round3_tests PRIVATE src)
target_link_libraries(licasa_metadata_round3_tests PRIVATE Qt6::Core Qt6::Gui Qt6::Test)
licasa_enable_warnings(licasa_metadata_round3_tests)
list(APPEND LICASA_TEST_TARGETS licasa_metadata_round3_tests)
list(APPEND LICASA_CORE_TEST_TARGETS licasa_metadata_round3_tests)
add_test(NAME licasa_metadata_round3_tests COMMAND licasa_metadata_round3_tests)
set_tests_properties(licasa_metadata_round3_tests PROPERTIES
        TIMEOUT 60 LABELS "core;metadata;navigation;security")

if(LICASA_HAVE_FAST_PNG_PREVIEW AND LICASA_ENABLE_APNG AND LICASA_ENABLE_QOI)
    add_executable(licasa_decode_round3_tests tests/tst_decode_round3.cpp
            src/plugins/apng_handler.cpp)
    target_include_directories(licasa_decode_round3_tests PRIVATE src)
    target_link_libraries(licasa_decode_round3_tests PRIVATE
            licasa_image_services PNG::PNG Qt6::Test)
    add_dependencies(licasa_decode_round3_tests licasa_qoi_plugin)
    licasa_enable_warnings(licasa_decode_round3_tests)
    list(APPEND LICASA_TEST_TARGETS licasa_decode_round3_tests)
    add_test(NAME licasa_decode_round3_tests COMMAND licasa_decode_round3_tests)
    set_tests_properties(licasa_decode_round3_tests PROPERTIES
            TIMEOUT 90 LABELS "codec;preview;animation;security"
            ENVIRONMENT "QT_PLUGIN_PATH=${CMAKE_CURRENT_BINARY_DIR}/plugins")
endif()

add_executable(licasa_navigation_runtime_tests
        tests/tst_navigation_runtime.cpp src/imaging/format_support.cpp)
target_include_directories(licasa_navigation_runtime_tests PRIVATE src)
target_link_libraries(licasa_navigation_runtime_tests PRIVATE Qt6::Gui Qt6::Test)
licasa_enable_warnings(licasa_navigation_runtime_tests)
list(APPEND LICASA_TEST_TARGETS licasa_navigation_runtime_tests)
list(APPEND LICASA_CORE_TEST_TARGETS licasa_navigation_runtime_tests)
add_test(NAME licasa_navigation_runtime_tests
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_unprivileged.py"
                --exec $<TARGET_FILE:licasa_navigation_runtime_tests>)
set_tests_properties(licasa_navigation_runtime_tests PROPERTIES
        TIMEOUT 30 LABELS "core;navigation;fast")

add_executable(licasa_gpu_image_edit_tests tests/tst_gpu_image_edits.cpp)
target_link_libraries(licasa_gpu_image_edit_tests PRIVATE licasa_image_services Qt6::Test)
licasa_enable_warnings(licasa_gpu_image_edit_tests)
list(APPEND LICASA_TEST_TARGETS licasa_gpu_image_edit_tests)
list(APPEND LICASA_CORE_TEST_TARGETS licasa_gpu_image_edit_tests)
foreach(backend IN ITEMS opencl cuda)
    add_test(NAME licasa_gpu_image_edits_${backend} COMMAND licasa_gpu_image_edit_tests)
    set_tests_properties(licasa_gpu_image_edits_${backend} PROPERTIES
        TIMEOUT 120 RUN_SERIAL TRUE LABELS "editing;gpu"
        SKIP_RETURN_CODE 77
        ENVIRONMENT "LICASA_TEST_EDIT_BACKEND=${backend}")
endforeach()
target_link_libraries(licasa_image_edit_pipeline_tests PRIVATE licasa_image_services Qt6::Test)
licasa_enable_warnings(licasa_image_edit_pipeline_tests)
list(APPEND LICASA_TEST_TARGETS licasa_image_edit_pipeline_tests)
list(APPEND LICASA_CORE_TEST_TARGETS licasa_image_edit_pipeline_tests)
add_test(NAME licasa_image_edit_pipeline_tests COMMAND licasa_image_edit_pipeline_tests)
set_tests_properties(licasa_image_edit_pipeline_tests PROPERTIES
        TIMEOUT 30 LABELS "core;editing;fast")

add_executable(licasa_sharpen_runtime_tests tests/tst_sharpen_runtime.cpp)
target_link_libraries(licasa_sharpen_runtime_tests PRIVATE licasa_image_services Qt6::Test)
licasa_enable_warnings(licasa_sharpen_runtime_tests)
list(APPEND LICASA_TEST_TARGETS licasa_sharpen_runtime_tests)
list(APPEND LICASA_CORE_TEST_TARGETS licasa_sharpen_runtime_tests)
add_test(NAME licasa_sharpen_runtime_tests COMMAND licasa_sharpen_runtime_tests)
set_tests_properties(licasa_sharpen_runtime_tests PROPERTIES
        TIMEOUT 120 LABELS "core;editing")

if(LICASA_ENABLE_QOI)
    add_executable(licasa_qoi_runtime_tests tests/tst_qoi_runtime.cpp)
    target_include_directories(licasa_qoi_runtime_tests PRIVATE src)
    target_link_libraries(licasa_qoi_runtime_tests PRIVATE Qt6::Gui Qt6::Test)
    add_dependencies(licasa_qoi_runtime_tests licasa_qoi_plugin)
    licasa_enable_warnings(licasa_qoi_runtime_tests)
    list(APPEND LICASA_TEST_TARGETS licasa_qoi_runtime_tests)
    add_test(NAME licasa_qoi_runtime_tests COMMAND licasa_qoi_runtime_tests)
    set_tests_properties(licasa_qoi_runtime_tests PROPERTIES
            TIMEOUT 30 LABELS "imaging;security"
            ENVIRONMENT "QT_PLUGIN_PATH=${CMAKE_CURRENT_BINARY_DIR}/plugins")
endif()

add_executable(licasa_image_save_tests
        tests/tst_image_save_service.cpp
        src/imaging/codec_plugins.cpp
        src/app/app_constants.h
        )
target_link_libraries(licasa_image_save_tests PRIVATE licasa_image_services)
target_include_directories(licasa_image_save_tests PRIVATE src)
target_compile_definitions(licasa_image_save_tests PRIVATE
        LICASA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(licasa_image_save_tests PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Quick
        Qt6::Test
)
licasa_enable_warnings(licasa_image_save_tests)
list(APPEND LICASA_TEST_TARGETS licasa_image_save_tests)
list(APPEND LICASA_CORE_TEST_TARGETS licasa_image_save_tests)
add_test(NAME licasa_image_save_tests
        COMMAND licasa_image_save_tests -platform offscreen)
foreach(value IN ITEMS 0 1 2147483647 -1 invalid 9999999999999999999999)
    add_test(NAME licasa_policy_env_${value}
            COMMAND licasa_image_save_tests -platform offscreen
            configurableImageLimitPersistsAndUpdatesDecoderBudget
            rejectsOversizedImageBeforeRasterAllocation
            previewsImageAboveFullResolutionLimitAtBoundedSize)
    set_tests_properties(licasa_policy_env_${value} PROPERTIES
            ENVIRONMENT "QT_IMAGEIO_MAXALLOC=${value};QT_QUICK_BACKEND=software"
            TIMEOUT 30
            LABELS "core;policy;security")
endforeach()
set_tests_properties(licasa_image_save_tests PROPERTIES
        TIMEOUT 60 LABELS "core;policy;fast")

add_executable(licasa_editor_export_tests tests/tst_editor_export_contracts.cpp src/imaging/codec_plugins.cpp)
target_link_libraries(licasa_editor_export_tests PRIVATE licasa_image_services Qt6::Test Qt6::Quick)
licasa_enable_warnings(licasa_editor_export_tests)
list(APPEND LICASA_TEST_TARGETS licasa_editor_export_tests)
list(APPEND LICASA_CORE_TEST_TARGETS licasa_editor_export_tests)
add_test(NAME licasa_editor_export_tests
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_unprivileged.py"
                --exec $<TARGET_FILE:licasa_editor_export_tests> -platform offscreen)
set_tests_properties(licasa_editor_export_tests PROPERTIES TIMEOUT 60 LABELS "core;editing;export;fast")

# Compile the production observers into the test even when the standalone
# diagnostics executable is disabled, including sanitizer-only test builds.
add_executable(licasa_diagnostic_driver_tests
        tests/tst_diagnostic_drivers.cpp
        tools/diagnostics_common.cpp
        tools/diagnostics_raw_behavior.cpp
        tools/diagnostics_viewer_measurement.cpp)
target_include_directories(licasa_diagnostic_driver_tests PRIVATE src tools)
target_link_libraries(licasa_diagnostic_driver_tests PRIVATE
        licasa_image_services Qt6::Core Qt6::Gui Qt6::Quick Qt6::Test)
licasa_enable_warnings(licasa_diagnostic_driver_tests)
list(APPEND LICASA_TEST_TARGETS licasa_diagnostic_driver_tests)
list(APPEND LICASA_CORE_TEST_TARGETS licasa_diagnostic_driver_tests)
add_test(NAME licasa_diagnostic_driver_tests
        COMMAND licasa_diagnostic_driver_tests -platform offscreen)
set_tests_properties(licasa_diagnostic_driver_tests PROPERTIES
        TIMEOUT 45 LABELS "core;diagnostics;fast"
        ENVIRONMENT "QT_QUICK_BACKEND=software")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    foreach(runtime_contract_test IN ITEMS runtime_profiles qt_runtime)
        add_test(NAME licasa_${runtime_contract_test}_contract
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_${runtime_contract_test}.py)
        set_tests_properties(licasa_${runtime_contract_test}_contract PROPERTIES
                TIMEOUT 30 LABELS "packaging;runtime-profile;fast")
    endforeach()
    foreach(package_check IN ITEMS reader motion)
        add_test(NAME licasa_package_${package_check}_checks
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_package_${package_check}_checks.py)
        set_tests_properties(licasa_package_${package_check}_checks PROPERTIES
                TIMEOUT 30 LABELS "packaging;${package_check};fast")
    endforeach()
    set_property(TEST licasa_package_motion_checks APPEND PROPERTY LABELS "motion-photo")
    add_test(NAME licasa_mate_accessibility_wrapper
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mate_accessibility_wrapper.py)
    set_tests_properties(licasa_mate_accessibility_wrapper PROPERTIES
            TIMEOUT 30 LABELS "packaging;accessibility;fast")
    add_test(NAME licasa_release_hardening
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_hardening.py
            $<TARGET_FILE:licasa>)
    set_tests_properties(licasa_release_hardening PROPERTIES LABELS "security;packaging")
    if(LICASA_BUILD_DIAGNOSTICS)
        list(APPEND LICASA_TEST_TARGETS licasa_diagnostics)
        add_test(NAME licasa_navigation_transition
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_navigation_transition.py
                $<TARGET_FILE:licasa_diagnostics>
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/test-assets/dimensions/odd-photo-997x613.png
                ${CMAKE_CURRENT_SOURCE_DIR}/assets/licasa.png)
        set_tests_properties(licasa_navigation_transition PROPERTIES
                TIMEOUT 25 LABELS "integration;navigation;qml")
        add_test(NAME licasa_navigation_burst
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_navigation_burst.py
                $<TARGET_FILE:licasa_diagnostics>)
        set_tests_properties(licasa_navigation_burst PROPERTIES
                TIMEOUT 90 LABELS "integration;navigation;qml")
        if(LICASA_ENABLE_RAW)
            list(APPEND LICASA_RAW_TEST_TARGETS licasa_diagnostics)
            add_test(NAME licasa_raw_viewer_behavior
                    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_raw_behavior.py
                    $<TARGET_FILE:licasa_diagnostics>
                    --output ${CMAKE_CURRENT_BINARY_DIR}/raw-viewer-behavior.json)
            set_tests_properties(licasa_raw_viewer_behavior PROPERTIES
                    TIMEOUT 150 LABELS "codec;raw;integration")
        endif()
        set(LICASA_LIFECYCLE_OPTIONS)
        set(LICASA_LIFECYCLE_SAMPLE_COUNT 1) # core 8K JPEG
        if(LICASA_ENABLE_HEIF)
            list(APPEND LICASA_LIFECYCLE_OPTIONS --heif)
            math(EXPR LICASA_LIFECYCLE_SAMPLE_COUNT "${LICASA_LIFECYCLE_SAMPLE_COUNT} + 2")
        endif()
        if(LICASA_ENABLE_JXL)
            list(APPEND LICASA_LIFECYCLE_OPTIONS --jxl)
            math(EXPR LICASA_LIFECYCLE_SAMPLE_COUNT "${LICASA_LIFECYCLE_SAMPLE_COUNT} + 2")
        endif()
        if(LICASA_ENABLE_RAW)
            list(APPEND LICASA_LIFECYCLE_OPTIONS --raw)
            math(EXPR LICASA_LIFECYCLE_SAMPLE_COUNT "${LICASA_LIFECYCLE_SAMPLE_COUNT} + 1")
        endif()
        if(LICASA_ENABLE_APNG)
            list(APPEND LICASA_LIFECYCLE_OPTIONS --apng)
            math(EXPR LICASA_LIFECYCLE_SAMPLE_COUNT "${LICASA_LIFECYCLE_SAMPLE_COUNT} + 1")
        endif()
        if(LICASA_SANITIZER)
            list(APPEND LICASA_LIFECYCLE_OPTIONS --sanitized)
        endif()

        if(LICASA_TEST_JOBS LESS LICASA_LIFECYCLE_SAMPLE_COUNT)
            set(LICASA_LIFECYCLE_JOBS ${LICASA_TEST_JOBS})
        else()
            set(LICASA_LIFECYCLE_JOBS ${LICASA_LIFECYCLE_SAMPLE_COUNT})
        endif()
        list(APPEND LICASA_LIFECYCLE_OPTIONS --jobs ${LICASA_LIFECYCLE_JOBS})

        add_test(NAME licasa_viewer_lifecycle
                COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_viewer_lifecycle.py
                $<TARGET_FILE:licasa_diagnostics> ${LICASA_LIFECYCLE_OPTIONS}
                --output ${CMAKE_CURRENT_BINARY_DIR}/viewer-lifecycle.json)
        set_tests_properties(licasa_viewer_lifecycle PROPERTIES
                TIMEOUT 150 LABELS "integration;lifecycle"
                RUN_SERIAL TRUE PROCESSORS ${LICASA_LIFECYCLE_JOBS})
    endif()
endif()

# Qt 6.4's software scene graph retains textures at shutdown. Apply the
# existing, narrowly scoped suppression to every test that launches it.
if(LICASA_SANITIZER MATCHES "address" AND Qt6_VERSION VERSION_LESS "6.8.0"
        AND NOT LICASA_QT_QUICK_BACKPORT_LIBRARY)
    foreach(test_name licasa_qml_tests licasa_navigation_transition
            licasa_navigation_burst licasa_viewer_lifecycle)
        if(TEST ${test_name})
            set_property(TEST ${test_name} APPEND PROPERTY ENVIRONMENT
                    "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tests/lsan-qt-software.supp:print_suppressions=1")
        endif()
    endforeach()
endif()

if(LICASA_ENABLE_MOTION_PLAYBACK)
    add_executable(licasa_qt_custom_input_tests tests/tst_qt_custom_input.cpp)
    target_link_libraries(licasa_qt_custom_input_tests PRIVATE Qt6::Gui Qt6::Multimedia Qt6::Test)
    target_compile_definitions(licasa_qt_custom_input_tests PRIVATE
            LICASA_TEST_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/test-assets")
    licasa_enable_warnings(licasa_qt_custom_input_tests)
    list(APPEND LICASA_TEST_TARGETS licasa_qt_custom_input_tests)
    add_test(NAME licasa_qt_custom_input_tests COMMAND licasa_qt_custom_input_tests -platform offscreen)
    set_tests_properties(licasa_qt_custom_input_tests PROPERTIES
            TIMEOUT 150 LABELS "motion-photo;runtime-profile;lifecycle;security"
            ENVIRONMENT "QT_MEDIA_BACKEND=ffmpeg;QT_FFMPEG_DECODING_HW_DEVICE_TYPES=,;QT_QPA_PLATFORM=offscreen")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/LicasaTemporalTests.cmake)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    include(${CMAKE_CURRENT_LIST_DIR}/LicasaComputeFailureTests.cmake)
endif()

# CLion exposes custom CMake targets directly in the target selector. These
# targets turn the existing CTest suite into a repeatable local bench: one
# target builds the required test binaries and runs the chosen test group.
add_custom_target(licasa_test_bench
        COMMAND ${CMAKE_CTEST_COMMAND} --test-dir "${CMAKE_BINARY_DIR}" --output-on-failure
                --parallel ${LICASA_TEST_JOBS}
        DEPENDS licasa ${LICASA_TEST_TARGETS}
        USES_TERMINAL
        COMMENT "Build and run the complete Licasa CTest bench")

add_custom_target(licasa_test_bench_fast
        COMMAND ${CMAKE_CTEST_COMMAND} --test-dir "${CMAKE_BINARY_DIR}" --output-on-failure
                --parallel ${LICASA_TEST_JOBS} -LE "integration|vendor"
        DEPENDS licasa ${LICASA_TEST_TARGETS}
        USES_TERMINAL
        COMMENT "Run the fast Licasa bench (skip integration/vendor tests)")

add_custom_target(licasa_test_bench_core
        COMMAND ${CMAKE_CTEST_COMMAND} --test-dir "${CMAKE_BINARY_DIR}" --output-on-failure
                --parallel ${LICASA_TEST_JOBS} -L "core"
        DEPENDS licasa ${LICASA_CORE_TEST_TARGETS}
        USES_TERMINAL
        COMMENT "Run core QML, image-service and policy tests")

if(LICASA_ENABLE_RAW)
    add_custom_target(licasa_test_bench_raw
            COMMAND ${CMAKE_CTEST_COMMAND} --test-dir "${CMAKE_BINARY_DIR}" --output-on-failure
                    --parallel ${LICASA_TEST_JOBS} -L "raw"
            DEPENDS licasa ${LICASA_RAW_TEST_TARGETS}
            USES_TERMINAL
            COMMENT "Run RAW/DNG unit, policy and viewer-behavior tests")
endif()

if(LICASA_ENABLE_AVIF)
    add_custom_target(licasa_test_bench_avif
            COMMAND ${CMAKE_CTEST_COMMAND} --test-dir "${CMAKE_BINARY_DIR}" --output-on-failure
                    --parallel ${LICASA_TEST_JOBS} -L "avif"
            DEPENDS licasa ${LICASA_AVIF_TEST_TARGETS}
            USES_TERMINAL
            COMMENT "Run AVIF still-reader unit and hostile-input tests")
endif()

if(LICASA_ENABLE_APNG)
    add_custom_target(licasa_test_bench_apng
            COMMAND ${CMAKE_CTEST_COMMAND} --test-dir "${CMAKE_BINARY_DIR}" --output-on-failure
                    --parallel ${LICASA_TEST_JOBS} -L "apng"
            DEPENDS licasa ${LICASA_APNG_TEST_TARGETS}
            USES_TERMINAL
            COMMENT "Run APNG animation, policy, hostile-input and integration tests")
endif()
