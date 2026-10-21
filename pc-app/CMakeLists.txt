# ---- phonemic_core: everything except the window ----------------------------
add_library(phonemic_core STATIC
  core/src/audio_out.cpp
  core/src/bluetooth.cpp
  core/src/denoise.cpp
  core/src/default_mic.cpp
  core/src/dsp.cpp
  core/src/jitter_buffer.cpp
  core/src/miniaudio_impl.c
  core/src/opus_codec.cpp
  core/src/playout.cpp
  core/src/process.cpp
  core/src/receiver.cpp
  core/src/settings.cpp
  core/src/util.cpp
  core/src/virtual_mic.cpp
  core/src/voice_fx.cpp
  ipc/ring_feeder.cpp
)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_sources(phonemic_core PRIVATE core/src/pulse_ctl.cpp core/src/bluetooth_linux.cpp)
  target_link_libraries(phonemic_core PRIVATE ${CMAKE_DL_LIBS})
endif()
target_include_directories(phonemic_core PUBLIC core/include ipc)
target_link_libraries(phonemic_core PUBLIC phonemic_shared opus PRIVATE miniaudio)
target_compile_definitions(phonemic_core PUBLIC PHONEMIC_VERSION="${PROJECT_VERSION}")

if(PHONEMIC_WITH_RNNOISE)
  target_link_libraries(phonemic_core PRIVATE rnnoise)
  target_compile_definitions(phonemic_core PRIVATE PHONEMIC_HAVE_RNNOISE)
endif()

if(WIN32)
  target_link_libraries(phonemic_core PUBLIC ws2_32 bthprops winmm setupapi ole32)
  target_compile_definitions(phonemic_core PUBLIC _WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN)
  target_sources(phonemic_core PRIVATE ipc/driver_feed_win.cpp core/src/bluetooth_win.cpp)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(phonemic_core PUBLIC Threads::Threads)
endif()

if(APPLE)
  target_sources(phonemic_core PRIVATE core/src/bluetooth_mac.mm)
  set_source_files_properties(core/src/bluetooth_mac.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
  target_link_libraries(phonemic_core PUBLIC "-framework CoreFoundation" "-framework CoreAudio"
                                             "-framework Foundation" "-framework IOBluetooth")
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
  if(APPLE)
    # Declares Bluetooth use (NSBluetoothAlwaysUsageDescription): required for IOBluetooth.
    set_target_properties(PhoneMic PROPERTIES MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/gui/Info.plist.in)
  endif()
  if(MSVC)
    target_compile_options(PhoneMic PRIVATE /utf-8)
  endif()
endif()

# ---- command-line tools ------------------------------------------------------------
if(PHONEMIC_BUILD_TOOLS)
  add_executable(phonemic-receiver tools/receiver_main.cpp)
  target_link_libraries(phonemic-receiver PRIVATE phonemic_core)
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
  # Ship the driver inside PhoneMic.app so the app's "Install" button finds it.
  if(TARGET PhoneMic)
    add_dependencies(PhoneMic PhoneMicAudio)
    add_custom_command(TARGET PhoneMic POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_BUNDLE_CONTENT_DIR:PhoneMic>/Resources
      COMMAND ${CMAKE_COMMAND} -E rm -rf $<TARGET_BUNDLE_CONTENT_DIR:PhoneMic>/Resources/PhoneMic.driver
      # ditto keeps permissions and the code signature intact.
      COMMAND ditto $<TARGET_BUNDLE_DIR:PhoneMicAudio> $<TARGET_BUNDLE_CONTENT_DIR:PhoneMic>/Resources/PhoneMic.driver
      COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/driver-macos/install-driver.sh
              $<TARGET_BUNDLE_CONTENT_DIR:PhoneMic>/Resources/install-driver.sh
      VERBATIM)
  endif()
endif()
