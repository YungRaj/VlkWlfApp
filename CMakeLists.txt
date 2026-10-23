cmake_minimum_required(VERSION 3.18)
project(VkWlfApp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_program(WAYLAND_SCANNER_EXECUTABLE NAMES wayland-scanner REQUIRED)

# Where to find xdg-shell.xml / linux-dmabuf-unstable-v1.xml.
# Override with -DPROTOCOL_DIR=... or $VKWLF_PROTOCOL_DIR; defaults to the
# system wayland-protocols package by default.
if(NOT DEFINED PROTOCOL_DIR AND DEFINED ENV{VKWLF_PROTOCOL_DIR})
  set(PROTOCOL_DIR "$ENV{VKWLF_PROTOCOL_DIR}")
endif()
if(NOT DEFINED PROTOCOL_DIR)
  set(PROTOCOL_DIR "/usr/share/wayland-protocols")
endif()
if(NOT EXISTS "${PROTOCOL_DIR}/xdg-shell.xml")
  set(PROTOCOL_DIR "/usr/share/wayland-protocols")
  set(XDG_SHELL_XML "${PROTOCOL_DIR}/stable/xdg-shell/xdg-shell.xml")
  set(LINUX_DMABUF_XML "${PROTOCOL_DIR}/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml")
else()
  set(XDG_SHELL_XML "${PROTOCOL_DIR}/xdg-shell.xml")
  set(LINUX_DMABUF_XML "${PROTOCOL_DIR}/linux-dmabuf-unstable-v1.xml")
endif()

set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GEN_DIR}")

# Generate server and client headers + protocol implementation files
add_custom_command(
  OUTPUT
    "${GEN_DIR}/xdg-shell-server-protocol.h"
    "${GEN_DIR}/xdg-shell-client-protocol.h"
    "${GEN_DIR}/xdg-shell-protocol.c"
    "${GEN_DIR}/linux-dmabuf-unstable-v1-server-protocol.h"
    "${GEN_DIR}/linux-dmabuf-unstable-v1-client-protocol.h"
    "${GEN_DIR}/linux-dmabuf-unstable-v1-protocol.c"
  COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" server-header "${XDG_SHELL_XML}" "${GEN_DIR}/xdg-shell-server-protocol.h"
  COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" client-header "${XDG_SHELL_XML}" "${GEN_DIR}/xdg-shell-client-protocol.h"
  COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" private-code "${XDG_SHELL_XML}" "${GEN_DIR}/xdg-shell-protocol.c"
  COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" server-header "${LINUX_DMABUF_XML}" "${GEN_DIR}/linux-dmabuf-unstable-v1-server-protocol.h"
  COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" client-header "${LINUX_DMABUF_XML}" "${GEN_DIR}/linux-dmabuf-unstable-v1-client-protocol.h"
  COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" private-code "${LINUX_DMABUF_XML}" "${GEN_DIR}/linux-dmabuf-unstable-v1-protocol.c"
  DEPENDS "${XDG_SHELL_XML}" "${LINUX_DMABUF_XML}"
  COMMENT "Generating Wayland protocol headers and code for xdg-shell and linux-dmabuf-unstable-v1"
)

set(PROTOCOL_SRCS
  "${GEN_DIR}/xdg-shell-protocol.c"
  "${GEN_DIR}/linux-dmabuf-unstable-v1-protocol.c"
  "${GEN_DIR}/xdg-shell-server-protocol.h"
  "${GEN_DIR}/linux-dmabuf-unstable-v1-server-protocol.h"
)

set(VKWLF_CORE_SRCS
  src/wayland_server_engine.cpp
  src/vk_renderer.cpp
  src/input_handler.cpp
  ${PROTOCOL_SRCS}
)

if(ANDROID)
  # When cross-compiling for Android NDK, build libwayland-server from source.
  # Override with -DWAYLAND_TP_DIR=... -DLIBFFI_TP_DIR=... (or the matching
  # VKWLF_WAYLAND_SRC / VKWLF_LIBFFI_SRC env vars). No defaults: these must be set.
  if(NOT DEFINED WAYLAND_TP_DIR)
    if(DEFINED ENV{VKWLF_WAYLAND_SRC})
      set(WAYLAND_TP_DIR "$ENV{VKWLF_WAYLAND_SRC}")
    else()
      set(WAYLAND_TP_DIR "$ENV{HOME}/src/wayland")
    endif()
  endif()
  if(NOT DEFINED LIBFFI_TP_DIR)
    if(DEFINED ENV{VKWLF_LIBFFI_SRC})
      set(LIBFFI_TP_DIR "$ENV{VKWLF_LIBFFI_SRC}")
    else()
      set(LIBFFI_TP_DIR "$ENV{HOME}/src/libffi")
    endif()
  endif()
  if(NOT EXISTS "${WAYLAND_TP_DIR}/src/wayland-server.c")
    message(FATAL_ERROR "libwayland sources not found at ${WAYLAND_TP_DIR}; set -DWAYLAND_TP_DIR or VKWLF_WAYLAND_SRC")
  endif()
  add_custom_command(
    OUTPUT
      "${GEN_DIR}/wayland-server-protocol.h"
      "${GEN_DIR}/wayland-server-protocol-core.h"
      "${GEN_DIR}/wayland-client-protocol.h"
      "${GEN_DIR}/wayland-client-protocol-core.h"
      "${GEN_DIR}/wayland-protocol.c"
    COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" server-header "${WAYLAND_TP_DIR}/protocol/wayland.xml" "${GEN_DIR}/wayland-server-protocol.h"
    COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" -c server-header "${WAYLAND_TP_DIR}/protocol/wayland.xml" "${GEN_DIR}/wayland-server-protocol-core.h"
    COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" client-header "${WAYLAND_TP_DIR}/protocol/wayland.xml" "${GEN_DIR}/wayland-client-protocol.h"
    COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" -c client-header "${WAYLAND_TP_DIR}/protocol/wayland.xml" "${GEN_DIR}/wayland-client-protocol-core.h"
    COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" private-code "${WAYLAND_TP_DIR}/protocol/wayland.xml" "${GEN_DIR}/wayland-protocol.c"
    # Some libwayland checkouts include the generated headers as
    # "third_party/wayland/protocol/...": mirror that layout inside GEN_DIR.
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${GEN_DIR}/third_party/wayland/protocol"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${GEN_DIR}/wayland-server-protocol.h" "${GEN_DIR}/third_party/wayland/protocol/wayland-server-protocol.h"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${GEN_DIR}/wayland-server-protocol-core.h" "${GEN_DIR}/third_party/wayland/protocol/wayland-server-protocol-core.h"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${GEN_DIR}/wayland-client-protocol.h" "${GEN_DIR}/third_party/wayland/protocol/wayland-client-protocol.h"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${GEN_DIR}/wayland-client-protocol-core.h" "${GEN_DIR}/third_party/wayland/protocol/wayland-client-protocol-core.h"
    DEPENDS "${WAYLAND_TP_DIR}/protocol/wayland.xml"
  )

  add_library(wayland_server_static STATIC
    "${WAYLAND_TP_DIR}/src/connection.c"
    "${WAYLAND_TP_DIR}/src/event-loop.c"
    "${WAYLAND_TP_DIR}/src/wayland-os.c"
    "${WAYLAND_TP_DIR}/src/wayland-server.c"
    "${WAYLAND_TP_DIR}/src/wayland-client.c"
    "${WAYLAND_TP_DIR}/src/wayland-shm.c"
    "${WAYLAND_TP_DIR}/src/wayland-util.c"
    "${GEN_DIR}/wayland-protocol.c"
    "${GEN_DIR}/wayland-server-protocol.h"
    "${GEN_DIR}/wayland-client-protocol.h"
    "${LIBFFI_TP_DIR}/src/prep_cif.c"
    "${LIBFFI_TP_DIR}/src/types.c"
    "${LIBFFI_TP_DIR}/src/raw_api.c"
    "${LIBFFI_TP_DIR}/src/java_raw_api.c"
    "${LIBFFI_TP_DIR}/src/closures.c"
    "${LIBFFI_TP_DIR}/src/x86/ffi64.c"
    "${LIBFFI_TP_DIR}/src/x86/unix64.S"
    "${LIBFFI_TP_DIR}/src/x86/ffiw64.c"
    "${LIBFFI_TP_DIR}/src/x86/win64.S"
  )
  enable_language(ASM)
  target_include_directories(wayland_server_static PUBLIC
    "${GEN_DIR}"
    "${GEN_DIR}"
    "${WAYLAND_TP_DIR}"
    "${WAYLAND_TP_DIR}/src"
    "${WAYLAND_TP_DIR}/protocol"
    "${LIBFFI_TP_DIR}/include"
    "${LIBFFI_TP_DIR}/k8-linux"
    "${LIBFFI_TP_DIR}/k8-linux/include"
    "${LIBFFI_TP_DIR}/src/x86"
  )

  add_library(vkwlf_jni SHARED
    ${VKWLF_CORE_SRCS}
    src/main.cpp
  )
  target_include_directories(vkwlf_jni PRIVATE include "${GEN_DIR}")
  target_link_options(vkwlf_jni PRIVATE "-Wl,-z,max-page-size=16384" "-Wl,-z,common-page-size=16384")
  target_link_libraries(vkwlf_jni PRIVATE
    wayland_server_static
    android
    nativewindow
    vulkan
    log
  )

  add_executable(vkwlf_compositor
    ${VKWLF_CORE_SRCS}
    src/main.cpp
  )
  target_include_directories(vkwlf_compositor PRIVATE include "${GEN_DIR}")
  target_link_options(vkwlf_compositor PRIVATE "-Wl,-z,max-page-size=16384" "-Wl,-z,common-page-size=16384")
  target_link_libraries(vkwlf_compositor PRIVATE
    wayland_server_static
    android
    nativewindow
    vulkan
    log
  )

  add_executable(vkwlf_gpu_client
    tests/weston_simple_shm_client.cpp
    "${GEN_DIR}/xdg-shell-protocol.c"
    "${GEN_DIR}/linux-dmabuf-unstable-v1-protocol.c"
    "${GEN_DIR}/xdg-shell-client-protocol.h"
    "${GEN_DIR}/linux-dmabuf-unstable-v1-client-protocol.h"
  )
  set_target_properties(vkwlf_gpu_client PROPERTIES OUTPUT_NAME "libvkwlf_gpu_client.so")
  target_include_directories(vkwlf_gpu_client PRIVATE include "${GEN_DIR}")
  target_link_options(vkwlf_gpu_client PRIVATE "-pie" "-Wl,-z,max-page-size=16384" "-Wl,-z,common-page-size=16384")
  target_link_libraries(vkwlf_gpu_client PRIVATE
    wayland_server_static
    android
    nativewindow
    vulkan
    log
  )
else()
  add_library(vkwlf_jni SHARED
    ${VKWLF_CORE_SRCS}
    src/main.cpp
  )
  target_include_directories(vkwlf_jni PRIVATE include "${GEN_DIR}")
  target_link_libraries(vkwlf_jni PRIVATE
    wayland-server
    vulkan
    pthread
  )

  add_executable(vkwlf_compositor
    ${VKWLF_CORE_SRCS}
    src/main.cpp
  )
  target_include_directories(vkwlf_compositor PRIVATE include "${GEN_DIR}")
  target_link_libraries(vkwlf_compositor PRIVATE
    wayland-server
    vulkan
    pthread
  )

  add_executable(vkwlf_guest_proxy
    ${VKWLF_CORE_SRCS}
    src/vkwlf_guest_proxy.cpp
  )
  target_include_directories(vkwlf_guest_proxy PRIVATE include "${GEN_DIR}")
  target_link_libraries(vkwlf_guest_proxy PRIVATE
    wayland-server
    vulkan
    pthread
  )

  add_executable(weston-simple-shm
    tests/weston_simple_shm_client.cpp
    "${GEN_DIR}/xdg-shell-protocol.c"
    "${GEN_DIR}/linux-dmabuf-unstable-v1-protocol.c"
    "${GEN_DIR}/xdg-shell-client-protocol.h"
    "${GEN_DIR}/linux-dmabuf-unstable-v1-client-protocol.h"
  )
  target_include_directories(weston-simple-shm PRIVATE include "${GEN_DIR}")
  target_link_libraries(weston-simple-shm PRIVATE
    wayland-client
    pthread
  )
endif()
