# The protocol header shared verbatim with the Android native engine
# (phone-app/native compiles the same file).
add_library(phonemic_shared INTERFACE)
target_include_directories(phonemic_shared INTERFACE include)
