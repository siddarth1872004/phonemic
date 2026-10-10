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

project(PhoneMic VERSION 0.3.0 LANGUAGES C CXX)

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
option(PHONEMIC_BUILD_TOOLS "Build phonemic-receiver / phonemic-softphone"        ON)
option(PHONEMIC_BUILD_TESTS "Build the unit/integration test suite"               ON)
option(PHONEMIC_WITH_RNNOISE "Voice Focus noise removal (RNNoise)"                ON)
option(PHONEMIC_WITH_WEB    "Built-in HTTPS server for the browser mic (mbedTLS)" ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

include(cmake/Dependencies.cmake)

add_subdirectory(shared)
add_subdirectory(pc-app)

if(PHONEMIC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(pc-app/tests)
endif()
