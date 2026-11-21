# Opt-in Linux Qt6.11 lab only. Pin the v1 protocol shipped with Qt6.11.2;
# its XML retains the upstream MIT license. Generate locally, never at runtime.
include_guard(GLOBAL)
enable_language(C)
find_program(LICASA_WAYLAND_SCANNER NAMES wayland-scanner REQUIRED NO_CMAKE_FIND_ROOT_PATH)
find_path(LICASA_WAYLAND_INCLUDE_DIR NAMES wayland-client.h REQUIRED)
find_library(LICASA_WAYLAND_CLIENT_LIBRARY NAMES wayland-client REQUIRED)
set(_hdr_root "${CMAKE_CURRENT_LIST_DIR}/..")
set(_hdr_protocol_dir "${CMAKE_CURRENT_BINARY_DIR}/generated/hdr-wayland")
file(MAKE_DIRECTORY "${_hdr_protocol_dir}")
set(_hdr_xml "${_hdr_root}/src/imaging/hdr/wayland/color-management-v1.xml")
add_custom_command(OUTPUT "${_hdr_protocol_dir}/color-management-client.h"
    "${_hdr_protocol_dir}/color-management-server.h" "${_hdr_protocol_dir}/color-management-protocol.c"
    COMMAND "${LICASA_WAYLAND_SCANNER}" client-header "${_hdr_xml}" "${_hdr_protocol_dir}/color-management-client.h"
    COMMAND "${LICASA_WAYLAND_SCANNER}" server-header "${_hdr_xml}" "${_hdr_protocol_dir}/color-management-server.h"
    COMMAND "${LICASA_WAYLAND_SCANNER}" private-code "${_hdr_xml}" "${_hdr_protocol_dir}/color-management-protocol.c"
    DEPENDS "${_hdr_xml}" VERBATIM)
add_library(licasa_hdr_wayland STATIC
    "${_hdr_root}/src/imaging/hdr/wayland_hdr.cpp"
    "${_hdr_protocol_dir}/color-management-protocol.c" "${_hdr_protocol_dir}/color-management-client.h")
target_compile_features(licasa_hdr_wayland PUBLIC cxx_std_17)
target_include_directories(licasa_hdr_wayland PUBLIC "${_hdr_protocol_dir}" "${LICASA_WAYLAND_INCLUDE_DIR}")
target_link_libraries(licasa_hdr_wayland PUBLIC Qt6::Quick Qt6::GuiPrivate "${LICASA_WAYLAND_CLIENT_LIBRARY}")
