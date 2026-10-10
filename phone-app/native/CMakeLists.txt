cmake_minimum_required(VERSION 3.22.1)
project(phonemic LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Oboe is delivered as a Prefab package by the com.google.oboe:oboe AAR.
find_package(oboe REQUIRED CONFIG)

# The wire protocol and crypto are the SAME files the desktop app compiles
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

target_link_libraries(phonemic
    PRIVATE
        oboe::oboe
        android
        log
)
