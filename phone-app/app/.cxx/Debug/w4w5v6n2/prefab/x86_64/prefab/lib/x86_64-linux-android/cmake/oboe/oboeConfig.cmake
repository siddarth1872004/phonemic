if(NOT TARGET oboe::oboe)
add_library(oboe::oboe SHARED IMPORTED)
set_target_properties(oboe::oboe PROPERTIES
    IMPORTED_LOCATION "C:/Users/Siddarth/.gradle/caches/transforms-4/ebdf4c73c2f268f3b01ebe757135409b/transformed/oboe-1.9.0/prefab/modules/oboe/libs/android.x86_64/liboboe.so"
    INTERFACE_INCLUDE_DIRECTORIES "C:/Users/Siddarth/.gradle/caches/transforms-4/ebdf4c73c2f268f3b01ebe757135409b/transformed/oboe-1.9.0/prefab/modules/oboe/include"
    INTERFACE_LINK_LIBRARIES ""
)
endif()

