# PhoneMic.driver — the macOS virtual microphone (a Core Audio AudioServerPlugIn
# loaded by coreaudiod; no kernel extension). Install with install-driver.sh or
# the button in the app.
add_library(PhoneMicAudio MODULE PhoneMicAudio.c)
set(_plist ${CMAKE_CURRENT_BINARY_DIR}/Info.plist)
configure_file(Info.plist.in ${_plist} @ONLY)
set_target_properties(PhoneMicAudio PROPERTIES
  BUNDLE TRUE
  BUNDLE_EXTENSION driver
  OUTPUT_NAME PhoneMic
  MACOSX_BUNDLE_INFO_PLIST ${_plist}
  C_VISIBILITY_PRESET hidden
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
target_compile_options(PhoneMicAudio PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)
target_link_libraries(PhoneMicAudio PRIVATE "-framework CoreFoundation" "-framework CoreAudio")
# Apple silicon refuses to run unsigned code: sign ad hoc (enough for a plug-in
# the user installs on their own Mac).
add_custom_command(TARGET PhoneMicAudio POST_BUILD
  COMMAND codesign --force --sign - --timestamp=none $<TARGET_BUNDLE_DIR:PhoneMicAudio>
  VERBATIM)

# Ship the driver inside PhoneMic.app so the app's "Install" button can find it.
if(TARGET PhoneMic)
  add_dependencies(PhoneMic PhoneMicAudio)
  add_custom_command(TARGET PhoneMic POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_BUNDLE_CONTENT_DIR:PhoneMic>/Resources
    COMMAND ${CMAKE_COMMAND} -E copy_directory $<TARGET_BUNDLE_DIR:PhoneMicAudio>
            $<TARGET_BUNDLE_CONTENT_DIR:PhoneMic>/Resources/PhoneMic.driver
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/install-driver.sh
            $<TARGET_BUNDLE_CONTENT_DIR:PhoneMic>/Resources/install-driver.sh
    VERBATIM)
endif()
