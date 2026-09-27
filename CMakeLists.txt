# Licasa
# Release-oriented CMake configuration for the Qt Quick application.
# Author attribution: Sanxchep

cmake_minimum_required(VERSION 3.21)

project(Licasa
        VERSION 0.1.0
        LANGUAGES CXX
)

# Qt 6 requires C++17 or newer.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to Release for single-configuration generators when the user does not
# specify a build type.
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Standard install directory variables such as CMAKE_INSTALL_BINDIR.
include(GNUInstallDirs)
include(CTest)

install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/licasa)

# Optional IPO/LTO switch. Enabled by default, but only applied when supported.
option(LICASA_ENABLE_IPO "Enable IPO/LTO for release builds when supported" ON)
option(LICASA_BUILD_DIAGNOSTICS "Build format diagnostics and performance runner" OFF)
option(LICASA_ENABLE_GPU_EDITS "Build optional, lazily loaded GPU edit backends" ON)
option(LICASA_ENABLE_HEIF "Build the application-private HEIC/HEIF Qt reader" OFF)
option(LICASA_ENABLE_JXL "Build the application-private JPEG XL Qt reader" OFF)
option(LICASA_ENABLE_RAW "Build the application-private RAW preview Qt reader" OFF)
option(LICASA_ENABLE_AVIF "Build the application-private AVIF still-image Qt reader" OFF)
option(LICASA_ENABLE_APNG "Build the application-private APNG animation Qt reader" OFF)
option(LICASA_ENABLE_QOI "Build the QOI still-image Qt reader" ON)
option(LICASA_ENABLE_JP2 "Build the JPEG 2000 still-image Qt reader" OFF)
option(LICASA_ENABLE_EARLY_PROGRESSIVE_JPEG "Show an early progressive JPEG scan while full detail loads" ON)
option(LICASA_ENABLE_MOTION_PLAYBACK "Build the lazy Motion Photo playback backend and viewer controls" OFF)
set(LICASA_QT_EXTRA_IMAGEFORMATS_DIR "" CACHE PATH
        "Directory containing matching Qt TIFF and WebP image plugins")
set(LICASA_QT_EXTRA_IMAGEFORMATS_LICENSE "" CACHE FILEPATH
        "License file for bundled Qt TIFF and WebP plugins")
set(LICASA_QT_FFMPEG_PLUGIN "" CACHE FILEPATH
        "Qualified Qt 6.4.2 FFmpeg plugin with custom-input cleanup fixes")
set(LICASA_QT_QUICK_BACKPORT_LIBRARY "" CACHE FILEPATH
        "Same-version Qt Quick library with the software texture cleanup backport")
set(LICASA_SANITIZER "" CACHE STRING "Development sanitizer: address, undefined, or address,undefined")
set_property(CACHE LICASA_SANITIZER PROPERTY STRINGS "" address undefined "address,undefined")
if(LICASA_SANITIZER AND NOT LICASA_SANITIZER MATCHES "^(address|undefined|address,undefined)$")
    message(FATAL_ERROR "Unsupported LICASA_SANITIZER")
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Network Qml Quick QuickDialogs2)
if(LICASA_ENABLE_MOTION_PLAYBACK)
    find_package(Qt6 6.4 REQUIRED COMPONENTS Multimedia)
    # Bounded QIODevice playback is qualified only against the patched private
    # Qt 6.4.2 FFmpeg plugin. Never let a motion-enabled build silently fall
    # back to the distro plugin, because that would invalidate teardown/leak
    # qualification while still appearing functionally correct.
    if(NOT LICASA_QT_FFMPEG_PLUGIN)
        message(FATAL_ERROR
                "LICASA_ENABLE_MOTION_PLAYBACK requires the qualified private FFmpeg plugin; "
                "build it with tools/build_qt_ffmpeg.py and set LICASA_QT_FFMPEG_PLUGIN")
    endif()
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT Qt6_VERSION VERSION_EQUAL "6.4.2"
            OR NOT EXISTS "${LICASA_QT_FFMPEG_PLUGIN}")
        message(FATAL_ERROR "The private FFmpeg plugin requires Linux Qt 6.4.2 and an existing plugin")
    endif()
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/plugins/multimedia")
    configure_file("${LICASA_QT_FFMPEG_PLUGIN}"
            "${CMAKE_CURRENT_BINARY_DIR}/plugins/multimedia/libffmpegmediaplugin.so" COPYONLY)
    install(FILES "${LICASA_QT_FFMPEG_PLUGIN}"
            DESTINATION "${CMAKE_INSTALL_LIBDIR}/licasa/plugins/multimedia")
    get_filename_component(LICASA_QT_FFMPEG_RUNTIME_DIR "${LICASA_QT_FFMPEG_PLUGIN}" DIRECTORY)
    install(DIRECTORY "${LICASA_QT_FFMPEG_RUNTIME_DIR}/licenses/"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/licasa/qt-multimedia")
endif()
if(LICASA_QT_QUICK_BACKPORT_LIBRARY)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT Qt6_VERSION VERSION_EQUAL "6.4.2")
        message(FATAL_ERROR "The recorded Qt Quick backport is for Linux Qt 6.4.2 only")
    endif()
    get_filename_component(LICASA_QT_QUICK_BACKPORT_LIBRARY
            "${LICASA_QT_QUICK_BACKPORT_LIBRARY}" REALPATH)
    get_filename_component(LICASA_QT_QUICK_BACKPORT_NAME
            "${LICASA_QT_QUICK_BACKPORT_LIBRARY}" NAME)
    if(NOT EXISTS "${LICASA_QT_QUICK_BACKPORT_LIBRARY}" OR
            NOT LICASA_QT_QUICK_BACKPORT_NAME STREQUAL "libQt6Quick.so.6.4.2")
        message(FATAL_ERROR "Build the verified Qt Quick backport with tools/build_qt_backport.py first")
    endif()
    get_filename_component(LICASA_QT_QUICK_BACKPORT_DIR
            "${LICASA_QT_QUICK_BACKPORT_LIBRARY}" DIRECTORY)
    # Qt::QuickTest only references Quick indirectly. Relink that dependency
    # explicitly below so the executable's own RUNPATH selects the same runtime.
    set(CMAKE_BUILD_RPATH "${LICASA_QT_QUICK_BACKPORT_DIR}")
    file(RELATIVE_PATH LICASA_PRIVATE_LIB_RELATIVE
            "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}"
            "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/licasa/lib")
    set(CMAKE_INSTALL_RPATH "$ORIGIN/${LICASA_PRIVATE_LIB_RELATIVE}")
    install(FILES "${LICASA_QT_QUICK_BACKPORT_LIBRARY}"
            "${LICASA_QT_QUICK_BACKPORT_DIR}/libQt6Quick.so.6"
            DESTINATION "${CMAKE_INSTALL_LIBDIR}/licasa/lib")
    install(DIRECTORY "${LICASA_QT_QUICK_BACKPORT_DIR}/licenses/"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/licasa/qt-quick" )
endif()

if(LICASA_ENABLE_IPO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT LTO_SUPPORTED OUTPUT LTO_ERROR)
endif()

# Applies standard Qt project defaults (AUTOMOC and related setup).
qt_standard_project_setup()

set(LICASA_PLUGIN_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/licasa/plugins")
if(LICASA_QT_EXTRA_IMAGEFORMATS_DIR)
    foreach(extra_plugin libqtiff.so libqwebp.so)
        if(NOT EXISTS "${LICASA_QT_EXTRA_IMAGEFORMATS_DIR}/${extra_plugin}")
            message(FATAL_ERROR "Missing Qt image plugin: ${LICASA_QT_EXTRA_IMAGEFORMATS_DIR}/${extra_plugin}")
        endif()
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/plugins/imageformats")
        configure_file("${LICASA_QT_EXTRA_IMAGEFORMATS_DIR}/${extra_plugin}"
                "${CMAKE_CURRENT_BINARY_DIR}/plugins/imageformats/${extra_plugin}" COPYONLY)
        install(FILES "${LICASA_QT_EXTRA_IMAGEFORMATS_DIR}/${extra_plugin}"
                DESTINATION "${LICASA_PLUGIN_INSTALL_DIR}/imageformats")
    endforeach()
    if(LICASA_QT_EXTRA_IMAGEFORMATS_LICENSE)
        install(FILES "${LICASA_QT_EXTRA_IMAGEFORMATS_LICENSE}"
                DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/licasa/qt-imageformats")
    endif()
endif()
file(RELATIVE_PATH LICASA_INSTALL_PLUGIN_RELATIVE
        "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}"
        "${CMAKE_INSTALL_PREFIX}/${LICASA_PLUGIN_INSTALL_DIR}")
configure_file(cmake/codec_plugin_paths.h.in generated/codec_plugin_paths.h @ONLY)
include_directories("${CMAKE_CURRENT_BINARY_DIR}/generated")

function(licasa_enable_warnings target_name)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic)
        if(LICASA_SANITIZER)
            target_compile_options(${target_name} PRIVATE
                    -fsanitize=${LICASA_SANITIZER} -fno-omit-frame-pointer
                    -fno-sanitize-recover=all)
            target_link_options(${target_name} PRIVATE
                    -fsanitize=${LICASA_SANITIZER} -fno-sanitize-recover=all)
        endif()
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            set_property(TARGET ${target_name} PROPERTY POSITION_INDEPENDENT_CODE ON)
            target_compile_options(${target_name} PRIVATE -fstack-protector-strong)
            target_link_options(${target_name} PRIVATE -Wl,-z,relro,-z,now,-z,noexecstack)
        endif()
    endif()
endfunction()

if(LICASA_ENABLE_QOI)
    add_library(licasa_qoi_plugin MODULE src/plugins/qoi_plugin.cpp src/plugins/qoi.json)
    target_include_directories(licasa_qoi_plugin PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(licasa_qoi_plugin PRIVATE Qt6::Core Qt6::Gui)
    licasa_enable_warnings(licasa_qoi_plugin)
    set_target_properties(licasa_qoi_plugin PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/plugins/imageformats")
    install(TARGETS licasa_qoi_plugin LIBRARY DESTINATION "${LICASA_PLUGIN_INSTALL_DIR}/imageformats")
endif()

if(LICASA_ENABLE_JP2)
    find_path(LICASA_OPENJPEG_INCLUDE_DIR openjpeg.h
            PATHS "${CMAKE_CURRENT_SOURCE_DIR}/build/deps/openjpeg-dev/usr/include/openjpeg-2.5"
            PATH_SUFFIXES openjpeg-2.5 REQUIRED)
    find_library(LICASA_OPENJPEG_LIBRARY NAMES libopenjp2.so.7 openjp2 REQUIRED)
    add_library(licasa_jp2_plugin MODULE src/plugins/jp2_plugin.cpp src/plugins/jp2.json)
    target_include_directories(licasa_jp2_plugin PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
            "${LICASA_OPENJPEG_INCLUDE_DIR}")
    target_link_libraries(licasa_jp2_plugin PRIVATE Qt6::Core Qt6::Gui
            "${LICASA_OPENJPEG_LIBRARY}")
    licasa_enable_warnings(licasa_jp2_plugin)
    set_target_properties(licasa_jp2_plugin PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/plugins/imageformats")
    install(TARGETS licasa_jp2_plugin LIBRARY DESTINATION "${LICASA_PLUGIN_INSTALL_DIR}/imageformats")
endif()

if(LICASA_ENABLE_HEIF)
    find_package(libheif 1.23.4 CONFIG REQUIRED)
    find_package(libde265 1.1.2 CONFIG REQUIRED)
    add_library(licasa_heif MODULE src/plugins/heif_handler.cpp)
    target_include_directories(licasa_heif PRIVATE src)
    target_link_libraries(licasa_heif PRIVATE Qt6::Gui heif)
    set_target_properties(licasa_heif PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/plugins/licasa-codecs"
            INSTALL_RPATH "$ORIGIN/../../lib")
    licasa_enable_warnings(licasa_heif)
    add_library(licasa_heif_plugin MODULE src/plugins/heif_plugin.cpp)
    target_include_directories(licasa_heif_plugin PRIVATE src)
    target_link_libraries(licasa_heif_plugin PRIVATE Qt6::Gui)
    set_target_properties(licasa_heif_plugin PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/plugins/imageformats")
    licasa_enable_warnings(licasa_heif_plugin)
    install(TARGETS licasa_heif LIBRARY DESTINATION "${LICASA_PLUGIN_INSTALL_DIR}/licasa-codecs")
    install(TARGETS licasa_heif_plugin LIBRARY DESTINATION "${LICASA_PLUGIN_INSTALL_DIR}/imageformats")
    install(IMPORTED_RUNTIME_ARTIFACTS heif de265
            LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/licasa/lib")
    get_target_property(LICASA_HEIF_INCLUDES heif INTERFACE_INCLUDE_DIRECTORIES)
    get_filename_component(LICASA_CODEC_PREFIX "${LICASA_HEIF_INCLUDES}" DIRECTORY)
    install(DIRECTORY "${LICASA_CODEC_PREFIX}/share/licenses/licasa-codecs"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/licasa" OPTIONAL)
endif()

if(LICASA_ENABLE_JXL)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LICASA_JXL REQUIRED IMPORTED_TARGET libjxl_decoder>=0.12.0)
    pkg_check_modules(LICASA_JXL_THREADS REQUIRED IMPORTED_TARGET libjxl_threads>=0.12.0)
    pkg_get_variable(LICASA_JXL_PREFIX libjxl_decoder prefix)
    add_library(licasa_jxl MODULE src/plugins/jxl_handler.cpp)
    target_include_directories(licasa_jxl PRIVATE src)
    target_link_libraries(licasa_jxl PRIVATE Qt6::Gui PkgConfig::LICASA_JXL
            PkgConfig::LICASA_JXL_THREADS)
    set_target_properties(licasa_jxl PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/plugins/licasa-codecs"
            INSTALL_RPATH "$ORIGIN/../../lib")
    licasa_enable_warnings(licasa_jxl)
    add_library(licasa_jxl_plugin MODULE src/plugins/jxl_plugin.cpp)
    target_include_directories(licasa_jxl_plugin PRIVATE src)
    target_link_libraries(licasa_jxl_plugin PRIVATE Qt6::Gui)
    set_target_properties(licasa_jxl_plugin PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/plugins/imageformats")
    licasa_enable_warnings(licasa_jxl_plugin)
    install(TARGETS licasa_jxl LIBRARY DESTINATION "${LICASA_PLUGIN_INSTALL_DIR}/licasa-codecs")
    install(TARGETS licasa_jxl_plugin LIBRARY DESTINATION "${LICASA_PLUGIN_INSTALL_DIR}/imageformats")
    foreach(directory IN LISTS LICASA_JXL_LIBRARY_DIRS)
        file(GLOB LICASA_JXL_RUNTIME "${directory}/libjxl_dec.so.*"
                "${directory}/libjxl_cms.so.*" "${directory}/libjxl_threads.so.*")
        install(FILES ${LICASA_JXL_RUNTIME} DESTINATION "${CMAKE_INSTALL_LIBDIR}/licasa/lib")
    endforeach()
    install(DIRECTORY "${LICASA_JXL_PREFIX}/share/licenses/licasa-codecs"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/licasa" OPTIONAL)
endif()

if(LICASA_ENABLE_RAW)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LICASA_RAW REQUIRED IMPORTED_TARGET libraw_r>=0.22.2)
    pkg_get_variable(LICASA_RAW_PREFIX libraw_r prefix)
    add_library(licasa_raw MODULE src/plugins/raw_handler.cpp)
    target_include_directories(licasa_raw PRIVATE src)
    target_link_libraries(licasa_raw PRIVATE Qt6::Gui PkgConfig::LICASA_RAW)
    set_target_properties(licasa_raw PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/plugins/licasa-codecs"
            INSTALL_RPATH "$ORIGIN/../../lib")
    licasa_enable_warnings(licasa_raw)
    add_library(licasa_raw_plugin MODULE src/plugins/raw_plugin.cpp)
    target_include_directories(licasa_raw_plugin PRIVATE src)
    target_link_libraries(licasa_raw_plugin PRIVATE Qt6::Gui)
    set_target_properties(licasa_raw_plugin PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/plugins/imageformats")
    licasa_enable_warnings(licasa_raw_plugin)
    install(TARGETS licasa_raw LIBRARY DESTINATION "${LICASA_PLUGIN_INSTALL_DIR}/licasa-codecs")
    install(TARGETS licasa_raw_plugin LIBRARY DESTINATION "${LICASA_PLUGIN_INSTALL_DIR}/imageformats")
    file(GLOB LICASA_RAW_RUNTIME "${LICASA_RAW_PREFIX}/lib/libraw_r.so.*")
    install(FILES ${LICASA_RAW_RUNTIME} DESTINATION "${CMAKE_INSTALL_LIBDIR}/licasa/lib")
    file(GLOB LICASA_RAWSPEED3_RUNTIME "${LICASA_RAW_PREFIX}/lib/librawspeed3.so.*")
    if(LICASA_RAWSPEED3_RUNTIME)
        install(FILES ${LICASA_RAWSPEED3_RUNTIME}
                DESTINATION "${CMAKE_INSTALL_LIBDIR}/licasa/lib")
    endif()
    install(DIRECTORY "${LICASA_RAW_PREFIX}/share/licenses/licasa-codecs"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/licasa" OPTIONAL)
endif()

if(LICASA_ENABLE_AVIF)
    find_package(libavif 1.4.2 CONFIG REQUIRED)
    add_library(licasa_avif MODULE src/plugins/avif_handler.cpp)
    target_include_directories(licasa_avif PRIVATE src)
    target_link_libraries(licasa_avif PRIVATE Qt6::Gui avif)
    set_target_properties(licasa_avif PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/plugins/licasa-codecs"
            INSTALL_RPATH "$ORIGIN/../../lib")
    licasa_enable_warnings(licasa_avif)

    # Keep Qt's startup format scan independent of libavif/dav1d/libyuv. The
    # small imageformats plugin performs only a bounded ftyp probe; QLibrary
    # loads this native backend after an AVIF is actually opened.
    add_library(licasa_avif_plugin MODULE src/plugins/avif_plugin.cpp)
    target_include_directories(licasa_avif_plugin PRIVATE src)
    target_link_libraries(licasa_avif_plugin PRIVATE Qt6::Gui)
    set_target_properties(licasa_avif_plugin PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/plugins/imageformats")
    licasa_enable_warnings(licasa_avif_plugin)

    install(TARGETS licasa_avif LIBRARY DESTINATION "${LICASA_PLUGIN_INSTALL_DIR}/licasa-codecs")
    # AVIF_PLUGIN_METADATA_DEPENDENCY_V1
    # Q_PLUGIN_METADATA embeds avif.json through AUTOMOC. Keep the JSON
    # as an explicit autogen dependency so MIME/metadata-only edits
    # cannot leave a stale liblicasa_avif_plugin.so in incremental builds.
    set_property(TARGET licasa_avif_plugin APPEND PROPERTY
            AUTOGEN_TARGET_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/plugins/avif.json")

    install(TARGETS licasa_avif_plugin LIBRARY DESTINATION "${LICASA_PLUGIN_INSTALL_DIR}/imageformats")

    # The Stage-2 dependency gate guarantees these are the private pinned
    # libavif/dav1d/libyuv artifacts. Package them beside the other codec
    # runtimes; desktop image/avif advertisement is now qualified and enabled.
    get_target_property(LICASA_AVIF_INCLUDES avif INTERFACE_INCLUDE_DIRECTORIES)
    list(GET LICASA_AVIF_INCLUDES 0 LICASA_AVIF_INCLUDE_DIR)
    get_filename_component(LICASA_AVIF_PREFIX "${LICASA_AVIF_INCLUDE_DIR}" DIRECTORY)
    file(GLOB LICASA_AVIF_RUNTIME
            "${LICASA_AVIF_PREFIX}/lib/libavif.so.*"
            "${LICASA_AVIF_PREFIX}/lib/libdav1d.so.*"
            "${LICASA_AVIF_PREFIX}/lib/libyuv.so")
    if(NOT LICASA_AVIF_RUNTIME)
        message(FATAL_ERROR "Pinned AVIF runtime libraries were not found beside libavif")
    endif()
    install(FILES ${LICASA_AVIF_RUNTIME} DESTINATION "${CMAKE_INSTALL_LIBDIR}/licasa/lib")
    install(DIRECTORY "${LICASA_AVIF_PREFIX}/share/licenses/licasa-codecs"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/licasa" OPTIONAL)
endif()

if(LICASA_ENABLE_APNG)
    # APNG reuses Qt's existing PNG decoder for one compressed frame at a time.
    # The backend owns only strict APNG chunk parsing/composition; there is no
    # second general image stack and no new runtime codec dependency.
    add_library(licasa_apng MODULE src/plugins/apng_handler.cpp)
    target_include_directories(licasa_apng PRIVATE src)
    target_link_libraries(licasa_apng PRIVATE Qt6::Gui)
    set_target_properties(licasa_apng PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/plugins/licasa-codecs")
    licasa_enable_warnings(licasa_apng)

    # The cheap imageformats front end probes for acTL. It also advertises the
    # png key so .png APNG files are intercepted, while ordinary PNG returns no
    # capability and remains on Qt's pre-existing PNG handler.
    add_library(licasa_apng_plugin MODULE src/plugins/apng_plugin.cpp)
    target_include_directories(licasa_apng_plugin PRIVATE src)
    target_link_libraries(licasa_apng_plugin PRIVATE Qt6::Gui)
    set_target_properties(licasa_apng_plugin PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/plugins/imageformats")
    licasa_enable_warnings(licasa_apng_plugin)
    set_property(TARGET licasa_apng_plugin APPEND PROPERTY
            AUTOGEN_TARGET_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/plugins/apng.json")

    install(TARGETS licasa_apng LIBRARY DESTINATION "${LICASA_PLUGIN_INSTALL_DIR}/licasa-codecs")
    install(TARGETS licasa_apng_plugin LIBRARY DESTINATION "${LICASA_PLUGIN_INSTALL_DIR}/imageformats")
endif()

# Shared production services: compile once per configuration for the viewer,
# diagnostics and integration tests. These sources have no per-consumer defines.
add_library(licasa_image_services STATIC
        src/app/application_settings.cpp
        src/app/application_settings.h
        src/imaging/image_edit_pipeline.cpp
        src/imaging/image_edit_pipeline.h
        src/imaging/compute/edit_compute.cpp
        src/imaging/compute/edit_compute.h
        src/imaging/compute/edit_kernels.h
        src/imaging/image_resource_policy.cpp
        src/imaging/image_resource_policy.h
        src/imaging/image_probe.cpp
        src/imaging/image_probe.h
        src/imaging/format_support.cpp
        src/imaging/format_support.h
        src/export/image_save_operations.cpp
        src/export/image_save_operations.h
        src/export/image_save_service.cpp
        src/export/image_save_service.h
        src/imaging/async_image_provider.cpp
        src/imaging/async_image_provider.h
        src/imaging/image_processing.cpp
        src/imaging/image_processing.h)
target_include_directories(licasa_image_services PUBLIC src)
target_link_libraries(licasa_image_services PUBLIC Qt6::Quick)
if(LICASA_ENABLE_EARLY_PROGRESSIVE_JPEG)
    find_package(JPEG REQUIRED)
    target_sources(licasa_image_services PRIVATE
            src/imaging/progressive_jpeg_preview.cpp
            src/imaging/progressive_jpeg_preview.h)
    target_link_libraries(licasa_image_services PUBLIC JPEG::JPEG)
    target_compile_definitions(licasa_image_services PRIVATE LICASA_EARLY_PROGRESSIVE_JPEG=1)
endif()
if(LICASA_ENABLE_GPU_EDITS)
    find_path(LICASA_OPENCL_INCLUDE_DIR CL/cl.h)
    find_path(LICASA_CUDA_INCLUDE_DIR cuda.h HINTS /usr/local/cuda/include)
    find_path(LICASA_NVRTC_INCLUDE_DIR nvrtc.h HINTS /usr/local/cuda/include)
    find_library(LICASA_NVRTC_LIBRARY NAMES nvrtc libnvrtc.so.13 libnvrtc.so.12
        HINTS /usr/local/cuda/lib64 "${LICASA_NVRTC_INCLUDE_DIR}/../lib")
    if(LICASA_OPENCL_INCLUDE_DIR)
        target_sources(licasa_image_services PRIVATE src/imaging/compute/opencl_edit_device.cpp)
        target_include_directories(licasa_image_services SYSTEM PRIVATE "${LICASA_OPENCL_INCLUDE_DIR}")
        target_compile_definitions(licasa_image_services PRIVATE LICASA_HAVE_OPENCL_EDITS=1)
    endif()
    if(LICASA_CUDA_INCLUDE_DIR AND LICASA_NVRTC_INCLUDE_DIR AND LICASA_NVRTC_LIBRARY)
        add_executable(licasa_cuda_ptx_generator tools/generate_cuda_edit_ptx.cpp)
        target_include_directories(licasa_cuda_ptx_generator PRIVATE src
            "${LICASA_NVRTC_INCLUDE_DIR}")
        target_link_libraries(licasa_cuda_ptx_generator PRIVATE Qt6::Core
            "${LICASA_NVRTC_LIBRARY}")
        set(LICASA_CUDA_EDIT_PTX_HEADER
            "${CMAKE_CURRENT_BINARY_DIR}/generated/cuda_edit_ptx.h")
        add_custom_command(OUTPUT "${LICASA_CUDA_EDIT_PTX_HEADER}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory
                    "${CMAKE_CURRENT_BINARY_DIR}/generated"
            COMMAND licasa_cuda_ptx_generator "${LICASA_CUDA_EDIT_PTX_HEADER}"
            DEPENDS licasa_cuda_ptx_generator src/imaging/compute/edit_kernels.h
            VERBATIM)
        target_sources(licasa_image_services PRIVATE src/imaging/compute/cuda_edit_device.cpp)
        target_sources(licasa_image_services PRIVATE "${LICASA_CUDA_EDIT_PTX_HEADER}")
        target_include_directories(licasa_image_services SYSTEM PRIVATE
            "${LICASA_CUDA_INCLUDE_DIR}")
        target_include_directories(licasa_image_services PRIVATE
            "${CMAKE_CURRENT_BINARY_DIR}/generated")
        target_compile_definitions(licasa_image_services PRIVATE LICASA_HAVE_CUDA_EDITS=1)
    endif()
    message(STATUS "GPU edits: OpenCL headers=${LICASA_OPENCL_INCLUDE_DIR}; CUDA headers=${LICASA_CUDA_INCLUDE_DIR}; build-time NVRTC=${LICASA_NVRTC_LIBRARY}")
endif()
licasa_enable_warnings(licasa_image_services)
if(LICASA_ENABLE_IPO AND LTO_SUPPORTED)
    set_property(TARGET licasa_image_services PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(licasa_image_services PRIVATE
            $<$<CONFIG:Release>:-ffunction-sections>
            $<$<CONFIG:Release>:-fdata-sections>)
endif()

# Main application executable target.
set(LICASA_APPLICATION_SOURCES
        src/app/app_constants.h
        src/media/android_motion_photo.cpp
        src/media/android_motion_photo.h
        src/export/animation_export.cpp
        src/export/animation_export.h
        src/io/atomic_file_copy.cpp
        src/io/atomic_file_copy.h
        src/io/byte_range_device.cpp
        src/io/byte_range_device.h
        src/io/external_file_identity.h
        src/imaging/codec_plugins.cpp
        src/imaging/codec_plugins.h
        src/platform/graphics_backend.cpp
        src/platform/graphics_backend.h
        src/imaging/image_animation.cpp
        src/imaging/image_animation.h
        src/media/live_photo_pairing.cpp
        src/media/live_photo_pairing.h
        src/media/motion/motion_photo_backend_interface.h
        src/media/motion/motion_photo_frame.h
        src/export/motion_photo_export.cpp
        src/export/motion_photo_export.h
        src/export/motion_photo_frame_save_coordinator.cpp
        src/export/motion_photo_frame_save_coordinator.h
        src/media/motion/motion_photo_playback_policy.h
        src/media/motion/motion_photo_session.cpp
        src/media/motion/motion_photo_session.h
        src/platform/native_window_ops.cpp
        src/platform/native_window_ops.h
        src/media/photo_asset_probe.cpp
        src/media/photo_asset_probe.h
        src/media/preferred_cover_frame_coordinator.cpp
        src/media/preferred_cover_frame_coordinator.h
        src/media/preferred_cover_frame_store.cpp
        src/media/preferred_cover_frame_store.h
        src/platform/single_instance.cpp
        src/platform/single_instance.h
        src/platform/single_instance_endpoint.cpp
        src/platform/single_instance_endpoint.h
        src/app/window_manager.cpp
        src/app/window_manager.h
        src/app/window_media_state.cpp
        src/app/window_media_state.h
)
qt_add_executable(licasa src/main.cpp ${LICASA_APPLICATION_SOURCES})
if(LICASA_ENABLE_MOTION_PLAYBACK)
    # Keep the already-qualified player implementation reusable by both its
    # direct integration tests and the private runtime module. The main viewer
    # never links this target. PIC is required when the static implementation is
    # folded into the demand-loaded module below.
    add_library(licasa_motion_playback STATIC
            src/media/motion/motion_photo_playback.cpp
            src/media/motion/motion_photo_playback.h
            src/media/motion/motion_photo_frame.h
            src/media/motion/motion_photo_playback_policy.h)
    target_include_directories(licasa_motion_playback PUBLIC src)
    target_link_libraries(licasa_motion_playback PUBLIC Qt6::Core Qt6::Gui Qt6::Multimedia)
    set_target_properties(licasa_motion_playback PROPERTIES POSITION_INDEPENDENT_CODE ON)
    licasa_enable_warnings(licasa_motion_playback)

    # Stage 2A: the only target that links MotionPhotoPlayback into the viewer
    # runtime is this private module. MotionPhotoSession finds it with QLibrary
    # only after the user requests playback.
    add_library(licasa_motion_photo_backend MODULE
            src/media/motion/motion_photo_backend_plugin.cpp
            src/media/motion/motion_photo_backend_interface.h
            src/media/motion/motion_photo_frame.h
            src/media/motion/motion_photo_playback_policy.h
            src/io/byte_range_device.cpp
            src/io/byte_range_device.h)
    target_include_directories(licasa_motion_photo_backend PRIVATE src)
    target_link_libraries(licasa_motion_photo_backend PRIVATE
            licasa_motion_playback Qt6::Core Qt6::Multimedia)
    set_target_properties(licasa_motion_photo_backend PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/plugins/motion")
    licasa_enable_warnings(licasa_motion_photo_backend)
    install(TARGETS licasa_motion_photo_backend
            LIBRARY DESTINATION "${LICASA_PLUGIN_INSTALL_DIR}/motion")

    function(licasa_enable_motion_backend_loader target_name)
        target_compile_definitions(${target_name} PRIVATE
                LICASA_MOTION_BACKEND_FILE_NAME="$<TARGET_FILE_NAME:licasa_motion_photo_backend>"
                LICASA_MOTION_BACKEND_INSTALL_RELATIVE="${LICASA_INSTALL_PLUGIN_RELATIVE}/motion")
        add_dependencies(${target_name} licasa_motion_photo_backend)
    endfunction()
endif()

# QML module:
# - embeds resources
# - enables cached compilation/qmlcachegen
# - generates module metadata
#
# Item 12 export presentation is deliberately URL-loaded only after explicit
# user intent. Keep these files embedded and qmllint-visible, but do not add
# them to the module type table or compile them into the executable QML cache.
# This preserves qrc:/Licasa/qml/... loading while keeping ordinary still
# startup free of export-only qmlcache/type-registration work.
set_source_files_properties(
        qml/MotionExportDialog.qml
        qml/AnimationExportDialog.qml
        qml/TemporalExportCoordinator.qml
        PROPERTIES
        QT_QML_SKIP_CACHEGEN TRUE
        QT_QML_SKIP_QMLDIR_ENTRY TRUE)

function(licasa_add_qml target_name)
    # Qt 6.4's generated type registration includes this QML-exposed header by
    # basename. Other first-party includes stay qualified from the src root.
    target_include_directories(${target_name} PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src/media/motion")
    set(import_scan_option)
    if(QT6_IS_SHARED_LIBS_BUILD)
        # Shared Qt resolves QML plugins at runtime. Skipping the static-plugin
        # import scan also prevents qmlimportscanner from recursively parsing
        # unrelated, deliberately-invalid Qt parser/fuzz fixtures in build/deps.
        set(import_scan_option NO_IMPORT_SCAN)
    endif()
    qt_add_qml_module(${target_name}
            URI Licasa
            VERSION 1.0
            ${import_scan_option}
            RESOURCE_PREFIX /
            OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/${target_name}-qml
            DEPENDENCIES
            QtQuick
            QtQuick.Window
            QtQuick.Dialogs
            QML_FILES
            qml/Main.qml
            qml/OpenDialog.qml
            qml/SaveDialog.qml
            qml/FrameSaveDialog.qml
            qml/MotionExportDialog.qml
            qml/AnimationExportDialog.qml
            qml/TemporalExportCoordinator.qml
            qml/FullscreenSideMenu.qml
            qml/EditorSidePanel.qml
            qml/EditorAdjustContent.qml
            qml/EditorCropContent.qml
            qml/EditorExportContent.qml
            qml/EditorSession.qml
            qml/CropOverlay.qml
            qml/ImageViewport.qml
            qml/InfoOverlays.qml
            qml/OverlayControls.qml
            qml/NotificationCenter.qml
            qml/TemporalControls.qml
            qml/ViewerTemporalControls.qml
            qml/WindowedMoveController.qml
            qml/WindowedZoomController.qml
            qml/components/HeaderButton.qml
            qml/components/ActionChip.qml
            qml/components/SectionCard.qml
            qml/components/AdjustRow.qml
            qml/components/ToggleRow.qml
            qml/components/ValueRow.qml
            qml/components/InteractiveScrollBar.qml
            qml/components/RoundIconButton.qml
            SOURCES
            src/media/motion/motion_photo_frame_surface.cpp
            src/media/motion/motion_photo_frame_surface.h
            RESOURCES
            qml/ViewerMath.js
            qml/FileUrls.js
            qml/MotionPhotoFrameSurfaceHost.qml
            assets/icons/settings.svg
            assets/icons/edit.svg
            assets/icons/exit-fullscreen.svg
            assets/icons/close.svg
            assets/icons/notifications.svg
    )
endfunction()
licasa_add_qml(licasa)
qt_add_resources(licasa "licasa_app_icon" PREFIX "/" FILES assets/licasa.png)

# Link only the Qt modules actually used by this app.
target_link_libraries(licasa PRIVATE
        licasa_image_services
        Qt6::Core
        Qt6::Gui
        Qt6::Network
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickDialogs2
)

# On X11, query the physical button state while the window manager owns an
# interactive move. Floating gestures also need the input shape extension;
# they never change the visible bounding region.
if(UNIX AND NOT APPLE)
    find_package(X11 QUIET)
    if(X11_FOUND)
        target_compile_definitions(licasa PRIVATE LICASA_HAVE_X11_POINTER_QUERY=1)
        target_link_libraries(licasa PRIVATE X11::X11)
        if(X11_Xext_FOUND)
            target_compile_definitions(licasa PRIVATE LICASA_HAVE_X11_SHAPE=1)
            target_link_libraries(licasa PRIVATE ${X11_Xext_LIB})
        endif()
    endif()
endif()

# Enable IPO/LTO only when requested and supported by the current toolchain.
if(LICASA_ENABLE_IPO AND LTO_SUPPORTED)
    set_property(TARGET licasa PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()

# Let CMake supply the toolchain's standard optimization and NDEBUG flags. Add
# only dead-section elimination here.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(licasa PRIVATE
            $<$<CONFIG:Release>:-ffunction-sections>
            $<$<CONFIG:Release>:-fdata-sections>
    )
    target_link_options(licasa PRIVATE
            $<$<CONFIG:Release>:-Wl,--gc-sections>
            $<$<CONFIG:Release>:-Wl,-O1>
    )
endif()
licasa_enable_warnings(licasa)
if(LICASA_ENABLE_MOTION_PLAYBACK)
    licasa_enable_motion_backend_loader(licasa)
endif()

if(LICASA_BUILD_DIAGNOSTICS)
    qt_add_executable(licasa_diagnostics
            tools/diagnostics_main.cpp
            tools/diagnostics_common.cpp
            tools/diagnostics_raw_behavior.cpp
            tools/diagnostics_motion_controls.cpp
            tools/diagnostics_viewer_measurement.cpp
            ${LICASA_APPLICATION_SOURCES})
    target_include_directories(licasa_diagnostics PRIVATE src)
    target_link_libraries(licasa_diagnostics PRIVATE licasa_image_services Qt6::Core Qt6::Gui Qt6::Network
            Qt6::Qml Qt6::Quick Qt6::QuickDialogs2)
    if(X11_FOUND)
        target_compile_definitions(licasa_diagnostics PRIVATE LICASA_HAVE_X11_POINTER_QUERY=1)
        target_link_libraries(licasa_diagnostics PRIVATE X11::X11)
    endif()
    licasa_add_qml(licasa_diagnostics)
    licasa_enable_warnings(licasa_diagnostics)
    if(LICASA_ENABLE_MOTION_PLAYBACK)
        licasa_enable_motion_backend_loader(licasa_diagnostics)
    endif()
    if(LICASA_ENABLE_IPO AND LTO_SUPPORTED)
        set_property(TARGET licasa_diagnostics PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    endif()
    install(TARGETS licasa_diagnostics RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

    # AVIF_STAGE4_PACKAGE_ANIMATION_V1
    qt_add_executable(licasa_animation_probe tools/animation_probe_main.cpp)
    target_link_libraries(licasa_animation_probe PRIVATE Qt6::Core Qt6::Gui)
    licasa_enable_warnings(licasa_animation_probe)
    install(TARGETS licasa_animation_probe RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# Install the executable into the standard binary directory.
install(TARGETS licasa
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Keep desktop associations aligned with the features in this build. Optional
# families are added only after their installed-package decode gate exists.
set(LICASA_DESKTOP_MIME_TYPES "image/bmp;image/gif;image/jpeg;image/png;image/svg+xml;image/vnd.microsoft.icon;image/x-portable-bitmap;image/x-portable-graymap;image/x-portable-pixmap;image/x-xbitmap;image/x-xpixmap;")
if(LICASA_ENABLE_HEIF)
    # HEIF stills are already qualified. Sequence MIME advertisement is enabled
    # together with its Stage-3 installed-package and clean-runtime gate below.
    string(APPEND LICASA_DESKTOP_MIME_TYPES
            "image/heic;image/heif;image/heic-sequence;image/heif-sequence;")
endif()
if(LICASA_ENABLE_JXL)
    string(APPEND LICASA_DESKTOP_MIME_TYPES "image/jxl;")
endif()
if(LICASA_ENABLE_RAW)
    # DNG/phone RAW has completed the pinned real-camera, sanitizer, package
    # and clean-runtime gates. Other LibRaw extensions remain reader capability
    # only until they receive equivalent real-camera qualification.
    string(APPEND LICASA_DESKTOP_MIME_TYPES "image/x-adobe-dng;")
endif()
if(LICASA_ENABLE_AVIF)
    # AVIF still-image support completed the Release, sanitizer, corpus,
    # performance, lifecycle, installed-package and clean-runtime gates.
    string(APPEND LICASA_DESKTOP_MIME_TYPES "image/avif;")
endif()
if(LICASA_ENABLE_APNG)
    # APNG completed correctness/security, sanitizer, common-path performance,
    # lifecycle, installed-package and locked-down clean-runtime qualification.
    string(APPEND LICASA_DESKTOP_MIME_TYPES "image/apng;")
endif()
configure_file(packaging/licasa.desktop.in generated/licasa.desktop @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/generated/licasa.desktop"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
)

# Install the application icon for desktop launchers and icon themes.
install(FILES assets/licasa.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/pixmaps
)
install(FILES assets/licasa.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/512x512/apps
)

# One-click local deployment for CLion. Selecting this target builds the active
# CMake profile first, then installs that exact build into ~/.local without sudo.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(LICASA_LOCAL_DEPLOY_TARGETS licasa)
    if(LICASA_BUILD_DIAGNOSTICS)
        list(APPEND LICASA_LOCAL_DEPLOY_TARGETS licasa_diagnostics licasa_animation_probe)
    endif()
    if(LICASA_ENABLE_HEIF)
        list(APPEND LICASA_LOCAL_DEPLOY_TARGETS licasa_heif licasa_heif_plugin)
    endif()
    if(LICASA_ENABLE_JXL)
        list(APPEND LICASA_LOCAL_DEPLOY_TARGETS licasa_jxl licasa_jxl_plugin)
    endif()
    if(LICASA_ENABLE_RAW)
        list(APPEND LICASA_LOCAL_DEPLOY_TARGETS licasa_raw licasa_raw_plugin)
    endif()
    if(LICASA_ENABLE_AVIF)
        list(APPEND LICASA_LOCAL_DEPLOY_TARGETS licasa_avif licasa_avif_plugin)
    endif()
    if(LICASA_ENABLE_APNG)
        list(APPEND LICASA_LOCAL_DEPLOY_TARGETS licasa_apng licasa_apng_plugin)
    endif()
    if(LICASA_ENABLE_QOI)
        list(APPEND LICASA_LOCAL_DEPLOY_TARGETS licasa_qoi_plugin)
    endif()
    if(LICASA_ENABLE_JP2)
        list(APPEND LICASA_LOCAL_DEPLOY_TARGETS licasa_jp2_plugin)
    endif()

    add_custom_target(licasa_deploy_local
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/deploy_local.py
                    --build-dir ${CMAKE_BINARY_DIR}
                    --prefix "$ENV{HOME}/.local"
            DEPENDS ${LICASA_LOCAL_DEPLOY_TARGETS}
            USES_TERMINAL
            COMMENT "Build the active Licasa profile and deploy it to ~/.local")

    add_custom_target(licasa_deploy_local_run
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/deploy_local.py
                    --build-dir ${CMAKE_BINARY_DIR}
                    --prefix "$ENV{HOME}/.local"
                    --launch
            DEPENDS ${LICASA_LOCAL_DEPLOY_TARGETS}
            USES_TERMINAL
            COMMENT "Build, deploy and launch the active Licasa profile")
endif()

if(BUILD_TESTING)
    include(cmake/LicasaTests.cmake)
endif()
