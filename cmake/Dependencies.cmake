# Third-party dependencies, fetched at configure time and pinned to exact tags /
# commits so builds are reproducible. Each one is small and has a reason:
#
#   miniaudio  — audio output on WASAPI / PulseAudio / ALSA / CoreAudio, with
#                built-in sample-rate + channel conversion (single header).
#   imgui+glfw — the native desktop window (no web view, no Electron).
#   opus       — compression + loss concealment over the Bluetooth link.
#   rnnoise    — Voice Focus noise removal (the 2018 model, bundled weights).
include(FetchContent)
set(FETCHCONTENT_QUIET ON)

# Populate without add_subdirectory() (for header-only / hand-built deps).
macro(phonemic_fetch name repo tag)
  FetchContent_Declare(${name}
    GIT_REPOSITORY ${repo}
    GIT_TAG        ${tag}
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  do-not-add-subdirectory)
  FetchContent_MakeAvailable(${name})
endmacro()

# ---- miniaudio -------------------------------------------------------------
phonemic_fetch(miniaudio https://github.com/mackron/miniaudio.git 0.11.21)
add_library(miniaudio INTERFACE)
target_include_directories(miniaudio SYSTEM INTERFACE ${miniaudio_SOURCE_DIR})
if(UNIX AND NOT APPLE)
  target_link_libraries(miniaudio INTERFACE ${CMAKE_DL_LIBS} pthread m)
endif()
if(APPLE)
  target_link_libraries(miniaudio INTERFACE "-framework CoreFoundation" "-framework CoreAudio" "-framework AudioToolbox")
endif()

# ---- opus --------------------------------------------------------------------
# Optional compression (~48 kbps instead of 768 kbps PCM) with real packet-loss
# concealment and in-band FEC. Plain CMake build; the DNN extras stay off.
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(opus
  GIT_REPOSITORY https://github.com/xiph/opus.git
  GIT_TAG        v1.5.2
  GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(opus)

# ---- rnnoise -----------------------------------------------------------------
if(PHONEMIC_WITH_RNNOISE)
  # v0.1.1: the last release that ships the trained model in the source tree.
  FetchContent_Declare(rnnoise
    GIT_REPOSITORY https://github.com/xiph/rnnoise.git
    GIT_TAG        6cbfd53eb348a8d394e0757b4025c6ded34eb2b6
    SOURCE_SUBDIR  do-not-add-subdirectory)
  FetchContent_MakeAvailable(rnnoise)

  # rnnoise uses C99 variable-length arrays, which MSVC does not support.
  # Rewrite the four VLAs as alloca() into a build-dir copy (all compilers).
  set(_rn_src ${CMAKE_BINARY_DIR}/rnnoise-patched)
  file(MAKE_DIRECTORY ${_rn_src})
  foreach(f denoise.c rnn.c rnn_data.c rnn_reader.c pitch.c kiss_fft.c celt_lpc.c)
    file(READ ${rnnoise_SOURCE_DIR}/src/${f} _c)
    string(REPLACE "opus_val16 xx[n];" "opus_val16 *xx = (opus_val16*)alloca(sizeof(opus_val16)*(n));" _c "${_c}")
    string(REPLACE "opus_val16 x_lp4[len>>2];" "opus_val16 *x_lp4 = (opus_val16*)alloca(sizeof(opus_val16)*(len>>2));" _c "${_c}")
    string(REPLACE "opus_val16 y_lp4[lag>>2];" "opus_val16 *y_lp4 = (opus_val16*)alloca(sizeof(opus_val16)*(lag>>2));" _c "${_c}")
    string(REPLACE "opus_val32 xcorr[max_pitch>>1];" "opus_val32 *xcorr = (opus_val32*)alloca(sizeof(opus_val32)*(max_pitch>>1));" _c "${_c}")
    string(REPLACE "opus_val32 yy_lookup[maxperiod+1];" "opus_val32 *yy_lookup = (opus_val32*)alloca(sizeof(opus_val32)*(maxperiod+1));" _c "${_c}")
    file(WRITE ${_rn_src}/${f}.tmp "${_c}")
    file(COPY_FILE ${_rn_src}/${f}.tmp ${_rn_src}/${f} ONLY_IF_DIFFERENT)
  endforeach()
  file(WRITE ${_rn_src}/rnnoise_compat.h
"#pragma once
#ifdef _WIN32
#include <malloc.h>
#else
#include <alloca.h>
#endif
")
  add_library(rnnoise STATIC
    ${_rn_src}/denoise.c ${_rn_src}/rnn.c ${_rn_src}/rnn_data.c ${_rn_src}/rnn_reader.c
    ${_rn_src}/pitch.c ${_rn_src}/kiss_fft.c ${_rn_src}/celt_lpc.c)
  target_include_directories(rnnoise PUBLIC ${rnnoise_SOURCE_DIR}/include
                                     PRIVATE ${rnnoise_SOURCE_DIR}/src ${_rn_src})
  target_compile_definitions(rnnoise PRIVATE _USE_MATH_DEFINES)
  if(MSVC)
    target_compile_options(rnnoise PRIVATE /FI${_rn_src}/rnnoise_compat.h /wd4244 /wd4305 /wd4267 /wd4018)
  else()
    target_compile_options(rnnoise PRIVATE -include ${_rn_src}/rnnoise_compat.h -w)
    target_link_libraries(rnnoise PRIVATE m)
  endif()
  set_target_properties(rnnoise PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# ---- GUI: GLFW + Dear ImGui ---------------------------------------------------
if(PHONEMIC_BUILD_GUI)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
  # X11 always (also runs on Wayland desktops through XWayland). Native Wayland
  # is opt-in (PHONEMIC_WAYLAND, on for release builds) because it needs
  # wayland-scanner and the wayland/xkbcommon headers; GLFW loads those
  # libraries at run time, so the binary still starts without them.
  set(GLFW_BUILD_WAYLAND ${PHONEMIC_WAYLAND} CACHE BOOL "" FORCE)
  FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    TRUE)
  FetchContent_MakeAvailable(glfw)

  phonemic_fetch(imgui https://github.com/ocornut/imgui.git v1.91.5)
  find_package(OpenGL REQUIRED)
  add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp)
  target_include_directories(imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
  target_link_libraries(imgui PUBLIC glfw OpenGL::GL)
endif()
