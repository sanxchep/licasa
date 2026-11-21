cmake_minimum_required(VERSION 3.21)
project(LicasaHdrProbe LANGUAGES CXX)

# This lab tool uses version-specific QRhi interfaces. The app and its
# packaging do not build it.
find_package(Qt6 6.11 REQUIRED COMPONENTS Quick GuiPrivate)
include(../../cmake/LicasaWaylandHdr.cmake)
if(Qt6_VERSION VERSION_GREATER_EQUAL "6.12")
    message(FATAL_ERROR "Requalify the private Vulkan swapchain reader before using Qt 6.12 or newer.")
endif()

# Qt does not export QVkSwapChain RTTI. Disable only the vptr check for the
# reader's backend-guarded cast, leaving the rest of UBSan and ASan enabled.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(surface_format.cpp PROPERTIES COMPILE_OPTIONS "-fno-sanitize=vptr")
endif()

add_executable(licasa_hdr_probe main.cpp surface_format.cpp ../../src/imaging/hdr/adapter_budget.cpp)
target_compile_features(licasa_hdr_probe PRIVATE cxx_std_17)
target_link_libraries(licasa_hdr_probe PRIVATE Qt6::Quick Qt6::GuiPrivate licasa_hdr_wayland)

option(LICASA_HDR_PROBE_GAINMAP "Reconstruct JPEG gain maps for offscreen readback" OFF)
if(LICASA_HDR_PROBE_GAINMAP)
    get_filename_component(LICASA_HDR_PROBE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
    target_sources(licasa_hdr_probe PRIVATE photo_readback.cpp)
    target_include_directories(licasa_hdr_probe PRIVATE "${LICASA_HDR_PROBE_ROOT}/src")
endif()


if(LICASA_HDR_PROBE_GAINMAP)
    find_package(Qt6 REQUIRED COMPONENTS Concurrent)
    set_target_properties(licasa_hdr_probe PROPERTIES AUTOMOC ON)
    find_path(UHDR_INCLUDE_DIR NAMES ultrahdr_api.h REQUIRED)
    find_library(UHDR_LIBRARY NAMES uhdr REQUIRED)
    target_sources(licasa_hdr_probe PRIVATE gainmap_photo.cpp native_retirement.cpp ../gainmap-lab/jpeg_gainmap.cpp
        ../gainmap-lab/async_decoder.cpp ../gainmap-lab/request_coordinator.cpp)
    target_include_directories(licasa_hdr_probe PRIVATE "${UHDR_INCLUDE_DIR}")
    target_compile_definitions(licasa_hdr_probe PRIVATE LICASA_HDR_PROBE_GAINMAP)
    target_link_libraries(licasa_hdr_probe PRIVATE Qt6::Concurrent "${UHDR_LIBRARY}")
endif()

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    find_library(LICASA_WAYLAND_SERVER_LIBRARY NAMES wayland-server REQUIRED)
    find_package(Threads REQUIRED)
    add_executable(licasa_wayland_output_retirement wayland_output_retirement.cpp)
    target_compile_features(licasa_wayland_output_retirement PRIVATE cxx_std_17)
    target_include_directories(licasa_wayland_output_retirement PRIVATE "${LICASA_WAYLAND_INCLUDE_DIR}")
    target_link_libraries(licasa_wayland_output_retirement PRIVATE Qt6::Core Threads::Threads
        "${LICASA_WAYLAND_CLIENT_LIBRARY}" "${LICASA_WAYLAND_SERVER_LIBRARY}" "${CMAKE_DL_LIBS}")
    option(LICASA_HDR_TEST_WAYLAND_OUTPUT_RETIREMENT
        "Qualify queued output-reference retirement in the selected client library (requires leak-enabled instrumentation)" OFF)
    set(LICASA_HDR_RETIREMENT_PRELOAD "" CACHE STRING
        "Optional LD_PRELOAD for private dependency qualification, applied only to retirement tests")
    if(LICASA_HDR_TEST_WAYLAND_OUTPUT_RETIREMENT)
        foreach(retirement_case live retired receiver discard duplicate)
            add_test(NAME hdr_wayland_output_retirement_${retirement_case}
                COMMAND licasa_wayland_output_retirement "${retirement_case}" 32)
            set_tests_properties(hdr_wayland_output_retirement_${retirement_case} PROPERTIES
                TIMEOUT 30 SKIP_RETURN_CODE 77
                ENVIRONMENT "ASAN_OPTIONS=detect_leaks=1:halt_on_error=1;UBSAN_OPTIONS=halt_on_error=1")
            if(LICASA_HDR_RETIREMENT_PRELOAD)
                set_property(TEST hdr_wayland_output_retirement_${retirement_case} APPEND PROPERTY
                    ENVIRONMENT "LD_PRELOAD=${LICASA_HDR_RETIREMENT_PRELOAD}")
            endif()
        endforeach()
    endif()
    add_executable(licasa_wayland_hdr_tests tst_wayland_hdr.cpp)
    set_target_properties(licasa_wayland_hdr_tests PROPERTIES AUTOMOC ON)
    target_link_libraries(licasa_wayland_hdr_tests PRIVATE licasa_hdr_wayland Qt6::Test "${LICASA_WAYLAND_SERVER_LIBRARY}")
    add_test(NAME hdr_wayland_protocol COMMAND licasa_wayland_hdr_tests)
    set_tests_properties(hdr_wayland_protocol PROPERTIES TIMEOUT 60)
    add_test(NAME hdr_probe_software COMMAND licasa_hdr_probe --software-check -platform offscreen)
    set_tests_properties(hdr_probe_software PROPERTIES TIMEOUT 15)

    add_executable(licasa_hdr_surface_format_test surface_format_test.cpp surface_format.cpp)
    target_compile_features(licasa_hdr_surface_format_test PRIVATE cxx_std_17)
    target_link_libraries(licasa_hdr_surface_format_test PRIVATE Qt6::GuiPrivate)
    add_test(NAME hdr_probe_surface_format COMMAND licasa_hdr_surface_format_test)
    add_test(NAME hdr_probe_surface_fallback
        COMMAND "${CMAKE_COMMAND}" "-DPROBE=$<TARGET_FILE:licasa_hdr_probe>"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/check_surface_fallback.cmake")
    set_tests_properties(hdr_probe_surface_format hdr_probe_surface_fallback PROPERTIES TIMEOUT 15)

    option(LICASA_HDR_TEST_VULKAN_READBACK "Run readback with the current Vulkan-capable QPA" OFF)
    if(LICASA_HDR_TEST_VULKAN_READBACK)
        add_test(NAME hdr_probe_fp16_readback COMMAND licasa_hdr_probe --readback)
        set_tests_properties(hdr_probe_fp16_readback PROPERTIES TIMEOUT 15 SKIP_RETURN_CODE 77)
    endif()

    if(LICASA_HDR_TEST_VULKAN_READBACK AND LICASA_HDR_PROBE_GAINMAP)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        add_test(NAME hdr_wayland_e2e_verifier
            COMMAND "${Python3_EXECUTABLE}" -B -E "${CMAKE_CURRENT_SOURCE_DIR}/test_wayland_e2e.py")
        set_tests_properties(hdr_wayland_e2e_verifier PROPERTIES TIMEOUT 15)
        add_test(NAME hdr_wayland_multiwindow_verifier
            COMMAND "${Python3_EXECUTABLE}" -B -E "${CMAKE_CURRENT_SOURCE_DIR}/test_wayland_multiwindow.py")
        set_tests_properties(hdr_wayland_multiwindow_verifier PROPERTIES TIMEOUT 15)
        add_test(NAME hdr_wayland_output_migration_verifier
            COMMAND "${Python3_EXECUTABLE}" -B -E "${CMAKE_CURRENT_SOURCE_DIR}/test_wayland_output_migration.py")
        set_tests_properties(hdr_wayland_output_migration_verifier PROPERTIES TIMEOUT 15)
        add_test(NAME hdr_wayland_replacement_verifier
            COMMAND "${Python3_EXECUTABLE}" -B -E "${CMAKE_CURRENT_SOURCE_DIR}/test_wayland_replacement.py")
        set_tests_properties(hdr_wayland_replacement_verifier PROPERTIES TIMEOUT 15)
        add_test(NAME hdr_wayland_motion_verifier
            COMMAND "${Python3_EXECUTABLE}" -B -E "${CMAKE_CURRENT_SOURCE_DIR}/test_wayland_motion.py")
        set_tests_properties(hdr_wayland_motion_verifier PROPERTIES TIMEOUT 15)
        add_test(NAME hdr_wayland_camera_verifier
            COMMAND "${Python3_EXECUTABLE}" -B -E "${CMAKE_CURRENT_SOURCE_DIR}/test_wayland_camera.py")
        set_tests_properties(hdr_wayland_camera_verifier PROPERTIES TIMEOUT 15)
        add_test(NAME hdr_wayland_soak_verifier
            COMMAND "${Python3_EXECUTABLE}" -B -E "${CMAKE_CURRENT_SOURCE_DIR}/test_wayland_soak.py")
        set_tests_properties(hdr_wayland_soak_verifier PROPERTIES TIMEOUT 15)
    endif()
    if(LICASA_HDR_PROBE_GAINMAP AND LICASA_HDR_TEST_VULKAN_READBACK)
        find_package(Qt6 REQUIRED COMPONENTS Test)
        set(licasa_hdr_viewer_item_sources
            ../../src/imaging/hdr/hdr_image_item.cpp ../../src/imaging/hdr/hdr_image_item.h
            ../../src/imaging/hdr/vulkan_frame_fence.cpp
            ../../src/imaging/hdr/output_negotiation.cpp
            ../../src/imaging/hdr/render_target_budget.cpp ../../src/imaging/hdr/adapter_budget.cpp
            ../../src/app/application_settings.cpp ../../src/app/application_settings.h
            ../../src/platform/application_identity.cpp ../../src/platform/single_instance_endpoint.cpp
            gainmap_photo.cpp surface_format.cpp ../gainmap-lab/jpeg_gainmap.cpp
            ../gainmap-lab/async_decoder.cpp ../gainmap-lab/request_coordinator.cpp)
        add_executable(licasa_hdr_viewer_tests tst_viewer_hdr.cpp ${licasa_hdr_viewer_item_sources})
        qt_add_resources(licasa_hdr_viewer_tests sdr_composition
            PREFIX "/Licasa" BASE "${LICASA_HDR_PROBE_ROOT}"
            FILES "${LICASA_HDR_PROBE_ROOT}/assets/shaders/sdr-composition.frag.qsb"
                  "${LICASA_HDR_PROBE_ROOT}/assets/shaders/shortcuts-blur.frag.qsb")
        # The memory-pressure rows decode camera-sized photos. They have their
        # own executable so that the suite above keeps its short time limit.
        add_executable(licasa_hdr_memory_tests tst_viewer_memory.cpp ${licasa_hdr_viewer_item_sources})
        foreach(viewer_test licasa_hdr_viewer_tests licasa_hdr_memory_tests)
            set_target_properties(${viewer_test} PROPERTIES AUTOMOC ON)
            target_compile_features(${viewer_test} PRIVATE cxx_std_17)
            target_compile_definitions(${viewer_test} PRIVATE
                LICASA_ENABLE_HDR_VIEWER_LAB LICASA_HDR_RENDER_TEST
                SDR_COMPONENT_DIR="${LICASA_HDR_PROBE_ROOT}/qml/components"
                GAINMAP_FIXTURE="${LICASA_HDR_PROBE_ROOT}/tests/test-assets/formats/hdr/jpeg-gainmap-dual.jpg")
            target_include_directories(${viewer_test} PRIVATE "${LICASA_HDR_PROBE_ROOT}/src" "${UHDR_INCLUDE_DIR}")
            target_link_libraries(${viewer_test} PRIVATE Qt6::Quick Qt6::GuiPrivate Qt6::Concurrent Qt6::Test "${UHDR_LIBRARY}" licasa_hdr_wayland "${LICASA_WAYLAND_SERVER_LIBRARY}")
        endforeach()
        foreach(render_loop basic threaded)
            add_test(NAME hdr_viewer_memory_${render_loop} COMMAND licasa_hdr_memory_tests)
            set_tests_properties(hdr_viewer_memory_${render_loop} PROPERTIES
                TIMEOUT 900 RUN_SERIAL TRUE ENVIRONMENT "QSG_RENDER_LOOP=${render_loop}")
        endforeach()
        add_test(NAME hdr_viewer_render_loop COMMAND licasa_hdr_viewer_tests)
        set_tests_properties(hdr_viewer_render_loop PROPERTIES TIMEOUT 30 ENVIRONMENT "QSG_RENDER_LOOP=basic")
        add_test(NAME hdr_viewer_threaded_loop COMMAND licasa_hdr_viewer_tests windowCloseAndMove nativeRecovery
            twoWindowsKeepIndependentPhotoHeadroom replaceWhilePeerWaitsForUpload presentedRevisionRejectsReplacedSource
            monitorNotificationsAndRecalibration
            waylandReplacementFeedback outputRequestAndFallback)
        set_tests_properties(hdr_viewer_threaded_loop PROPERTIES TIMEOUT 30 ENVIRONMENT "QSG_RENDER_LOOP=threaded")
        add_test(NAME hdr_probe_gainmap_gpu_lease COMMAND licasa_hdr_probe
            --gainmap-lease-check "${LICASA_HDR_PROBE_ROOT}/tests/test-assets/formats/hdr/jpeg-gainmap-dual.jpg")
        set_tests_properties(hdr_probe_gainmap_gpu_lease PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)
        add_test(NAME hdr_probe_gainmap_fence_retirement COMMAND licasa_hdr_probe
            --gainmap-fence-check "${LICASA_HDR_PROBE_ROOT}/tests/test-assets/formats/hdr/jpeg-gainmap-dual.jpg")
        set_tests_properties(hdr_probe_gainmap_fence_retirement PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)
        foreach(gainmap_case 1 2 4 8 p3 rgb-rotated invalid)
            add_test(NAME hdr_probe_gainmap_${gainmap_case}
                COMMAND "${Python3_EXECUTABLE}" -E "${CMAKE_CURRENT_SOURCE_DIR}/check_gainmap_readback.py"
                        "$<TARGET_FILE:licasa_hdr_probe>" "${LICASA_HDR_PROBE_ROOT}/tests/test-assets/formats/hdr"
                        "${gainmap_case}")
            set_tests_properties(hdr_probe_gainmap_${gainmap_case} PROPERTIES
                TIMEOUT 30 SKIP_RETURN_CODE 77)
        endforeach()
    endif()
endif()
