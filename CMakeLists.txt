cmake_minimum_required(VERSION 3.22)
project(Personalized LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ─── Main mod shared library ───
# No external dependencies — uses only NDK system libraries:
#   - MiniHook: our own ARM64 inline hooker (in Main.cpp)
#   - dlsym: runtime symbol resolution
#   - android/log.h: logging to logcat
#   - sys/mman.h: mprotect for code modification

add_library(Personalized SHARED
    src/Main.cpp
    src/SeedManager.cpp
    src/RandomMapper.cpp
)

target_include_directories(Personalized PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

# Android system libraries
target_link_libraries(Personalized PRIVATE
    log     # __android_log_print
    dl      # dlopen, dlsym
)

# ─── Android-specific compilation ───
if(ANDROID)
    target_compile_options(Personalized PRIVATE
        -fPIC
        -O2
        -ffunction-sections
        -fdata-sections
        -fexceptions
        -frtti
        -fno-stack-protector
        -fvisibility=hidden
        -fvisibility-inlines-hidden
        -Wno-everything
    )
    target_link_options(Personalized PRIVATE
        -Wl,--gc-sections
        -Wl,--icf=all
        -Wl,--hash-style=gnu
        -Wl,-z,max-page-size=16384
        -Wl,--allow-shlib-undefined
    )
    target_link_libraries(Personalized PRIVATE
        android
        EGL
        GLESv3
        GLESv2
    )
endif()

# ─── Strip release builds ───
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set_target_properties(Personalized PROPERTIES
        LINK_FLAGS "-Wl,-s"
    )
endif()

# ─── Debug defines ───
target_compile_definitions(Personalized PRIVATE
    $<$<CONFIG:Debug>:DEBUG>
    $<$<CONFIG:Debug>:_DEBUG>
    $<$<CONFIG:Release>:NDEBUG>
)
