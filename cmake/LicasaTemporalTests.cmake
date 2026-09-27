# Temporal session tests. Included before aggregate benches
# so every enabled executable participates in their build dependencies.

add_executable(licasa_motion_session_tests
        tests/tst_motion_photo_session.cpp
        src/media/motion/motion_photo_session.cpp
        src/media/motion/motion_photo_backend_interface.h
        src/media/motion/motion_photo_playback_policy.h)
target_include_directories(licasa_motion_session_tests PRIVATE src)
target_link_libraries(licasa_motion_session_tests PRIVATE Qt6::Core Qt6::Test)
if(LICASA_ENABLE_MOTION_PLAYBACK)
    target_compile_definitions(licasa_motion_session_tests PRIVATE
            LICASA_MOTION_BACKEND_ENABLED=1)
    licasa_enable_motion_backend_loader(licasa_motion_session_tests)
endif()
licasa_enable_warnings(licasa_motion_session_tests)
list(APPEND LICASA_TEST_TARGETS licasa_motion_session_tests)
list(APPEND LICASA_CORE_TEST_TARGETS licasa_motion_session_tests)
add_test(NAME licasa_motion_session_tests COMMAND licasa_motion_session_tests)
set_tests_properties(licasa_motion_session_tests PROPERTIES
        TIMEOUT 30
        LABELS "core;motion-photo;session;security;lifecycle;fast")
if(LICASA_ENABLE_MOTION_PLAYBACK)
    set_property(TEST licasa_motion_session_tests APPEND PROPERTY ENVIRONMENT
            "QT_MEDIA_BACKEND=ffmpeg;QT_PLUGIN_PATH=${CMAKE_CURRENT_BINARY_DIR}/plugins")
endif()
