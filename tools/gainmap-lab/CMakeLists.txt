cmake_minimum_required(VERSION 3.21)
project(LicasaGainmapLab LANGUAGES CXX)

find_package(Qt6 6.4 REQUIRED COMPONENTS Gui Test Concurrent)
find_path(UHDR_INCLUDE_DIR NAMES ultrahdr_api.h REQUIRED)
find_library(UHDR_LIBRARY NAMES uhdr REQUIRED)
get_filename_component(LICASA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

enable_testing()
add_executable(licasa_gainmap_lab_tests tst_gainmap.cpp jpeg_gainmap.cpp
    "${LICASA_ROOT}/src/io/byte_range_device.cpp"
    "${LICASA_ROOT}/src/media/android_motion_photo.cpp"
    "${LICASA_ROOT}/src/media/live_photo_pairing.cpp"
    "${LICASA_ROOT}/src/media/photo_asset_probe.cpp")
set_target_properties(licasa_gainmap_lab_tests PROPERTIES AUTOMOC ON)
target_compile_features(licasa_gainmap_lab_tests PRIVATE cxx_std_17)
target_include_directories(licasa_gainmap_lab_tests PRIVATE "${LICASA_ROOT}/src" "${UHDR_INCLUDE_DIR}")
target_compile_definitions(licasa_gainmap_lab_tests PRIVATE
    GAINMAP_FIXTURE="${LICASA_ROOT}/tests/test-assets/formats/hdr/jpeg-gainmap-dual.jpg"
    GAINMAP_P3_FIXTURE="${LICASA_ROOT}/tests/test-assets/formats/hdr/jpeg-gainmap-p3.jpg"
    GAINMAP_RGB_FIXTURE="${LICASA_ROOT}/tests/test-assets/formats/hdr/jpeg-gainmap-rgb.jpg")
target_link_libraries(licasa_gainmap_lab_tests PRIVATE Qt6::Gui Qt6::Test "${UHDR_LIBRARY}")
add_test(NAME gainmap_lab COMMAND licasa_gainmap_lab_tests)
# The generated maximum-dimension rows encode and decode 16 MP photos.
set_tests_properties(gainmap_lab PROPERTIES TIMEOUT 240)

add_executable(licasa_gainmap_coordinator_tests tst_request_coordinator.cpp
    request_coordinator.cpp jpeg_gainmap.cpp)
set_target_properties(licasa_gainmap_coordinator_tests PROPERTIES AUTOMOC ON)
target_compile_features(licasa_gainmap_coordinator_tests PRIVATE cxx_std_17)
target_include_directories(licasa_gainmap_coordinator_tests PRIVATE "${LICASA_ROOT}/src" "${UHDR_INCLUDE_DIR}")
target_compile_definitions(licasa_gainmap_coordinator_tests PRIVATE
    GAINMAP_FIXTURE="${LICASA_ROOT}/tests/test-assets/formats/hdr/jpeg-gainmap-dual.jpg")
target_link_libraries(licasa_gainmap_coordinator_tests PRIVATE Qt6::Gui Qt6::Test "${UHDR_LIBRARY}")
add_test(NAME gainmap_coordinator COMMAND licasa_gainmap_coordinator_tests)
set_tests_properties(gainmap_coordinator PROPERTIES TIMEOUT 30)

add_executable(licasa_gainmap_async_tests tst_async_decoder.cpp async_decoder.cpp
    ../hdr-probe/gainmap_photo.cpp
    request_coordinator.cpp jpeg_gainmap.cpp)
set_target_properties(licasa_gainmap_async_tests PROPERTIES AUTOMOC ON)
target_compile_features(licasa_gainmap_async_tests PRIVATE cxx_std_17)
target_include_directories(licasa_gainmap_async_tests PRIVATE "${LICASA_ROOT}/src" "${UHDR_INCLUDE_DIR}")
target_compile_definitions(licasa_gainmap_async_tests PRIVATE
    GAINMAP_FIXTURE="${LICASA_ROOT}/tests/test-assets/formats/hdr/jpeg-gainmap-dual.jpg")
target_link_libraries(licasa_gainmap_async_tests PRIVATE Qt6::Gui Qt6::Test Qt6::Concurrent "${UHDR_LIBRARY}")
add_test(NAME gainmap_async COMMAND licasa_gainmap_async_tests)
# The camera rows decode two retained photos ten times when their files are set.
set_tests_properties(gainmap_async PROPERTIES TIMEOUT 120)

add_executable(licasa_gainmap_budget_tests tst_resource_budget.cpp async_decoder.cpp
    request_coordinator.cpp jpeg_gainmap.cpp)
set_target_properties(licasa_gainmap_budget_tests PROPERTIES AUTOMOC ON)
target_compile_features(licasa_gainmap_budget_tests PRIVATE cxx_std_17)
target_include_directories(licasa_gainmap_budget_tests PRIVATE "${LICASA_ROOT}/src" "${UHDR_INCLUDE_DIR}")
target_compile_definitions(licasa_gainmap_budget_tests PRIVATE
    GAINMAP_FIXTURE="${LICASA_ROOT}/tests/test-assets/formats/hdr/jpeg-gainmap-dual.jpg")
target_link_libraries(licasa_gainmap_budget_tests PRIVATE Qt6::Gui Qt6::Test Qt6::Concurrent "${UHDR_LIBRARY}")
add_test(NAME gainmap_budget COMMAND licasa_gainmap_budget_tests)
set_tests_properties(gainmap_budget PROPERTIES TIMEOUT 30)
