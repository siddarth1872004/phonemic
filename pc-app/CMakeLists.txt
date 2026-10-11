# ---- phonemic_core: everything except the window ----------------------------
add_library(phonemic_core STATIC
  core/src/audio_out.cpp
  core/src/denoise.cpp
  core/src/dsp.cpp
  core/src/jitter_buffer.cpp
  core/src/miniaudio_impl.c
  core/src/net.cpp
  core/src/opus_codec.cpp
  core/src/playout.cpp
  core/src/process.cpp
  core/src/receiver.cpp
  core/src/settings.cpp
  core/src/usb.cpp
  core/src/util.cpp
  core/src/virtual_mic.cpp
  core/src/voice_fx.cpp
  core/src/websocket.cpp
  ipc/ring_feeder.cpp
)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_sources(phonemic_core PRIVATE core/src/pulse_ctl.cpp)
  target_link_libraries(phonemic_core PRIVATE ${CMAKE_DL_LIBS})
endif()
target_include_directories(phonemic_core PUBLIC core/include ipc)
target_link_libraries(phonemic_core PUBLIC phonemic_shared opus PRIVATE miniaudio)
target_compile_definitions(phonemic_core PUBLIC PHONEMIC_VERSION="${PROJECT_VERSION}")

if(PHONEMIC_WITH_RNNOISE)
  target_link_libraries(phonemic_core PRIVATE rnnoise)
  target_compile_definitions(phonemic_core PRIVATE PHONEMIC_HAVE_RNNOISE)
endif()

if(PHONEMIC_WITH_WEB)
  # Bake web-client/ into the binary so the app is a single file.
  set(_web_dir ${PROJECT_SOURCE_DIR}/web-client)
  set(_web_files ${_web_dir}/index.html ${_web_dir}/app.js ${_web_dir}/pcm-worklet.js ${_web_dir}/style.css)
  set(_web_cpp ${CMAKE_CURRENT_BINARY_DIR}/web_assets.cpp)
  add_custom_command(
    OUTPUT ${_web_cpp}
    COMMAND ${CMAKE_COMMAND} -DOUT=${_web_cpp} -DDIR=${_web_dir}
            -DFILES=index.html,app.js,pcm-worklet.js,style.css
            -P ${PROJECT_SOURCE_DIR}/cmake/EmbedFiles.cmake
    DEPENDS ${_web_files} ${PROJECT_SOURCE_DIR}/cmake/EmbedFiles.cmake
    COMMENT "Embedding web client"
    VERBATIM)
  target_sources(phonemic_core PRIVATE core/src/web_server.cpp ${_web_cpp})
  target_link_libraries(phonemic_core PRIVATE mbedtls mbedx509 mbedcrypto PUBLIC qrcodegen)
  target_compile_definitions(phonemic_core PUBLIC PHONEMIC_HAVE_WEB)
endif()

if(WIN32)
  target_link_libraries(phonemic_core PUBLIC ws2_32 iphlpapi bcrypt winmm)
  target_compile_definitions(phonemic_core PUBLIC _WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN)
  target_sources(phonemic_core PRIVATE ipc/driver_feed_win.cpp)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(phonemic_core PUBLIC Threads::Threads)
endif()

if(MSVC)
  target_compile_options(phonemic_core PRIVATE /W4 /utf-8 /wd4244 /wd4267)
else()
  target_compile_options(phonemic_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# ---- the desktop app --------------------------------------------------------------
if(PHONEMIC_BUILD_GUI)
  add_executable(PhoneMic WIN32 MACOSX_BUNDLE gui/main.cpp gui/ui.cpp gui/app.cpp)
  target_link_libraries(PhoneMic PRIVATE phonemic_core imgui)
  if(WIN32)
    target_sources(PhoneMic PRIVATE gui/phonemic.rc)
  endif()
  if(MSVC)
    target_compile_options(PhoneMic PRIVATE /utf-8)
  endif()
endif()

# ---- command-line tools ------------------------------------------------------------
if(PHONEMIC_BUILD_TOOLS)
  add_executable(phonemic-receiver tools/receiver_main.cpp)
  target_link_libraries(phonemic-receiver PRIVATE phonemic_core)
  add_executable(phonemic-softphone tools/softphone_main.cpp)
  target_link_libraries(phonemic-softphone PRIVATE phonemic_core)
  # Plays a tone into a virtual mic and checks it comes back (CI, and support).
  add_executable(phonemic-vmic-check tools/vmic_check_main.cpp)
  target_link_libraries(phonemic-vmic-check PRIVATE phonemic_core miniaudio)
  if(WIN32)
    # Creates the PhoneMic device and installs the driver (install-driver.ps1).
    add_executable(phonemic-driver tools/driver_setup_win.cpp)
    target_link_libraries(phonemic-driver PRIVATE phonemic_core setupapi newdev cfgmgr32)
    if(MINGW)
      target_link_options(phonemic-driver PRIVATE -municode)
    endif()
  endif()
endif()

# ---- macOS virtual microphone (Core Audio plug-in) ---------------------------------
if(APPLE)
  add_subdirectory(driver-macos)
endif()
