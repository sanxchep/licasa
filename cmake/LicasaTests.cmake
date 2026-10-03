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

add_executable(licasa_background_mode_tests
        tests/tst_background_mode.cpp
        src/app/application_settings.cpp)
target_include_directories(licasa_background_mode_tests PRIVATE src)
target_link_libraries(licasa_background_mode_tests PRIVATE Qt6::Core Qt6::Test)
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

add_executable(licasa_qml_tests
        tests/quicktest_main.cpp
        src/imaging/codec_plugins.cpp
        src/imaging/image_animation.cpp
        )
target_link_libraries(licasa_qml_tests PRIVATE licasa_image_services)
target_include_directories(licasa_qml_tests PRIVATE src)

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
if(LICASA_SANITIZER MATCHES "address" AND Qt6_VERSION VERSION_LESS "6.8.0"
        AND NOT LICASA_QT_QUICK_BACKPORT_LIBRARY)
    set_property(TEST licasa_qml_tests APPEND PROPERTY ENVIRONMENT
            "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tests/lsan-qt-software.supp:print_suppressions=1")
endif()

add_executable(licasa_async_image_provider_tests tests/tst_async_image_provider.cpp)
target_link_libraries(licasa_async_image_provider_tests PRIVATE licasa_image_services Qt6::Test)
licasa_enable_warnings(licasa_async_image_provider_tests)
list(APPEND LICASA_TEST_TARGETS licasa_async_image_provider_tests)
list(APPEND LICASA_CORE_TEST_TARGETS licasa_async_image_provider_tests)
add_test(NAME licasa_async_image_provider_tests COMMAND licasa_async_image_provider_tests)
set_tests_properties(licasa_async_image_provider_tests PROPERTIES
        TIMEOUT 15 LABELS "core;async;lifecycle;fast")

add_executable(licasa_image_edit_pipeline_tests tests/tst_image_edit_pipeline.cpp)
add_executable(licasa_gpu_image_edit_tests tests/tst_gpu_image_edits.cpp)
target_link_libraries(licasa_gpu_image_edit_tests PRIVATE licasa_image_services Qt6::Test)
licasa_enable_warnings(licasa_gpu_image_edit_tests)
list(APPEND LICASA_TEST_TARGETS licasa_gpu_image_edit_tests)
list(APPEND LICASA_CORE_TEST_TARGETS licasa_gpu_image_edit_tests)
foreach(backend IN ITEMS opencl cuda)
    add_test(NAME licasa_gpu_image_edits_${backend} COMMAND licasa_gpu_image_edit_tests)
    set_tests_properties(licasa_gpu_image_edits_${backend} PROPERTIES
        TIMEOUT 120 RUN_SERIAL TRUE LABELS "editing;gpu"
        ENVIRONMENT "LICASA_TEST_EDIT_BACKEND=${backend}")
endforeach()
target_link_libraries(licasa_image_edit_pipeline_tests PRIVATE licasa_image_services Qt6::Test)
licasa_enable_warnings(licasa_image_edit_pipeline_tests)
list(APPEND LICASA_TEST_TARGETS licasa_image_edit_pipeline_tests)
list(APPEND LICASA_CORE_TEST_TARGETS licasa_image_edit_pipeline_tests)
add_test(NAME licasa_image_edit_pipeline_tests COMMAND licasa_image_edit_pipeline_tests)
set_tests_properties(licasa_image_edit_pipeline_tests PROPERTIES
        TIMEOUT 30 LABELS "core;editing;fast")

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
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
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

include(${CMAKE_CURRENT_LIST_DIR}/LicasaTemporalTests.cmake)

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
