cmake_minimum_required(VERSION 3.22.1)
project(phonemic LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Oboe is delivered as a Prefab package by the com.google.oboe:oboe AAR.
find_package(oboe REQUIRED CONFIG)

# libopus (every frame is Opus), the same pinned version the desktop app uses.
include(FetchContent)
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

# The wire protocol is the SAME header the desktop app compiles
# (../../shared) — one implementation, so the phone and PC cannot drift.
set(PHONEMIC_SHARED ${CMAKE_CURRENT_SOURCE_DIR}/../../shared)

add_library(phonemic SHARED
    jni_bridge.cpp
    audio_engine.cpp
    ${PHONEMIC_SHARED}/third_party/monocypher/monocypher.c
)

target_include_directories(phonemic PRIVATE
    ${PHONEMIC_SHARED}/include
    ${PHONEMIC_SHARED}/third_party/monocypher
)

target_compile_options(phonemic PRIVATE -Wall -Wextra -Wno-unused-parameter
    $<$<CONFIG:Release>:-O2>)
# Small APK: drop unused code (Gradle strips the symbols).
target_compile_options(opus PRIVATE -ffunction-sections -fdata-sections)
target_compile_options(phonemic PRIVATE -ffunction-sections -fdata-sections)
target_link_options(phonemic PRIVATE -Wl,--gc-sections)

target_link_libraries(phonemic
    PRIVATE
        oboe::oboe
        opus
        android
        log
)
