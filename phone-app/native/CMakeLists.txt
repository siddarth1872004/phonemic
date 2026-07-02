cmake_minimum_required(VERSION 3.22.1)
project(phonemic LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Oboe is delivered as a Prefab package by the com.google.oboe:oboe AAR.
find_package(oboe REQUIRED CONFIG)

add_library(phonemic SHARED
    jni_bridge.cpp
    audio_engine.cpp
)

target_link_libraries(phonemic
    PRIVATE
        oboe::oboe
        android
        log
)

# NOTE (single source of truth): the 18-byte packet header is currently built
# inline in audio_engine.cpp. Phase 1 should replace that with the C header
# generated from the /protocol Rust crate via cbindgen, so the phone and PC can
# never drift. Tracked in docs/ARCHITECTURE.md.
