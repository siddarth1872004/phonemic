add_executable(phonemic-tests
  test_main.cpp
  test_wire.cpp
  test_jitter.cpp
  test_voice_fx.cpp
  test_dsp.cpp
  test_playout.cpp
  test_denoise.cpp
  test_receiver.cpp
  test_crypto.cpp
  test_usb.cpp
  test_wifi.cpp
  test_persistent_mic.cpp
  test_settings.cpp
  test_opus.cpp
  test_bluetooth.cpp
)
target_link_libraries(phonemic-tests PRIVATE phonemic_core opus)
if(MSVC)
  target_compile_options(phonemic-tests PRIVATE /utf-8)
endif()
add_test(NAME phonemic-tests COMMAND phonemic-tests)

# The shared protocol header must stay valid C99 (it is compiled by C code).
add_executable(phonemic-wire-c99 test_wire_c99.c)
target_link_libraries(phonemic-wire-c99 PRIVATE phonemic_shared)
set_target_properties(phonemic-wire-c99 PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
add_test(NAME phonemic-wire-c99 COMMAND phonemic-wire-c99)
