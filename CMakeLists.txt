# PhoneMic — desktop app (C++), shared protocol, tools and tests.
#
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build --config Release --parallel
#   ctest --test-dir build -C Release --output-on-failure
#
# Third-party code is fetched at configure time (pinned tags, see
# cmake/Dependencies.cmake); nothing has to be installed except a C++17 compiler
# and, on Linux, the X11/GL headers for the window (see docs/BUILDING.md).
cmake_minimum_required(VERSION 3.21)

# macOS: run on 11 (Big Sur) and later, not only on the build machine's version.
# Must be set before project().
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")

project(PhoneMic VERSION 0.4.0 LANGUAGES C CXX)
if(APPLE)
  enable_language(OBJCXX)  # IOBluetooth (pc-app/core/src/bluetooth_mac.mm)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Ship a single self-contained .exe on Windows (no VC++ redistributable needed).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(PHONEMIC_BUILD_GUI   "Build the PhoneMic desktop window (Dear ImGui + GLFW)" ON)
option(PHONEMIC_BUILD_TOOLS "Build phonemic-receiver / phonemic-vmic-check"      ON)
option(PHONEMIC_BUILD_TESTS "Build the unit/integration test suite"               ON)
option(PHONEMIC_WITH_RNNOISE "Voice Focus noise removal (RNNoise)"                ON)
option(PHONEMIC_WAYLAND     "Linux: native Wayland window as well as X11 (needs wayland-scanner + headers)" OFF)
option(PHONEMIC_PORTABLE    "Linux: link libstdc++/libgcc statically (one binary for every distro)" OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(PHONEMIC_PORTABLE AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_link_options(-static-libstdc++ -static-libgcc)
endif()

# Small binaries: drop unused code and symbols from release builds (MSVC's
# Release defaults, /Gy + /OPT:REF + /OPT:ICF, already do this).
if(NOT MSVC)
  add_compile_options($<$<CONFIG:Release,MinSizeRel>:-ffunction-sections> $<$<CONFIG:Release,MinSizeRel>:-fdata-sections>)
  if(APPLE)
    add_link_options($<$<CONFIG:Release,MinSizeRel>:-Wl,-dead_strip>)
  else()
    add_link_options($<$<CONFIG:Release,MinSizeRel>:-Wl,--gc-sections> $<$<CONFIG:Release,MinSizeRel>:-s>)
  endif()
endif()

include(cmake/Dependencies.cmake)

add_subdirectory(shared)
add_subdirectory(pc-app)

if(PHONEMIC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(pc-app/tests)
endif()

# ---- Linux install + packages (AppImage / .deb / .rpm / .tar.gz) ---------------
# Only the "phonemic" component is packaged: the fetched libraries' own install
# rules (headers, static libs) stay out.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  include(GNUInstallDirs)
  set(_pkg ${PROJECT_SOURCE_DIR}/packaging/linux)
  if(PHONEMIC_BUILD_GUI)
    install(PROGRAMS $<TARGET_FILE:PhoneMic> DESTINATION ${CMAKE_INSTALL_BINDIR} RENAME phonemic COMPONENT phonemic)
  endif()
  if(PHONEMIC_BUILD_TOOLS)
    install(TARGETS phonemic-receiver phonemic-vmic-check
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT phonemic)
  endif()
  install(FILES ${_pkg}/phonemic.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications COMPONENT phonemic)
  install(FILES ${_pkg}/phonemic.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps
          COMPONENT phonemic)
  install(FILES ${_pkg}/io.github.siddarth1872004.phonemic.metainfo.xml
          DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo COMPONENT phonemic)
  install(FILES LICENSE-MIT LICENSE-APACHE README.md DESTINATION ${CMAKE_INSTALL_DOCDIR} COMPONENT phonemic)

  set(CPACK_PACKAGE_NAME phonemic)
  set(CPACK_PACKAGE_VENDOR "PhoneMic")
  set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Use your phone as a Bluetooth microphone for your PC")
  set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/siddarth1872004/phonemic")
  set(CPACK_PACKAGE_CONTACT "PhoneMic <https://github.com/siddarth1872004/phonemic/issues>")
  set(CPACK_PACKAGING_INSTALL_PREFIX /usr)
  set(CPACK_COMPONENTS_ALL phonemic)
  set(CPACK_COMPONENTS_GROUPING ALL_COMPONENTS_IN_ONE)
  set(CPACK_DEB_COMPONENT_INSTALL ON)
  set(CPACK_RPM_COMPONENT_INSTALL ON)
  set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
  # The binaries need only glibc, libGL and X11 at link time; the sound server
  # (libpulse), D-Bus (libdbus, to reach BlueZ) and Wayland are loaded at run
  # time when present.
  set(CPACK_DEBIAN_PACKAGE_DEPENDS "libc6 (>= 2.28), libgl1, libx11-6")
  set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "libpulse0, pipewire-pulse | pulseaudio, bluez, libdbus-1-3")
  set(CPACK_DEBIAN_PACKAGE_SECTION sound)
  # Normally asked from dpkg, which the (RPM-based) release build container lacks.
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE amd64)
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE arm64)
  endif()
  set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
  set(CPACK_RPM_PACKAGE_LICENSE "MIT OR Apache-2.0")
  set(CPACK_RPM_PACKAGE_GROUP "Applications/Multimedia")
  set(CPACK_RPM_PACKAGE_REQUIRES "glibc >= 2.28")
  set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
  set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
      /usr/share/applications /usr/share/icons /usr/share/icons/hicolor /usr/share/icons/hicolor/scalable
      /usr/share/icons/hicolor/scalable/apps /usr/share/metainfo)
  include(CPack)
endif()
