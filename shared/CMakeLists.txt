# The protocol + crypto headers shared verbatim with the Android native engine
# (phone-app/native/CMakeLists.txt compiles the same files).
add_library(monocypher STATIC third_party/monocypher/monocypher.c)
target_include_directories(monocypher PUBLIC third_party/monocypher)
set_target_properties(monocypher PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(phonemic_shared INTERFACE)
target_include_directories(phonemic_shared INTERFACE include)
target_link_libraries(phonemic_shared INTERFACE monocypher)
