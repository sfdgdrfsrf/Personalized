cmake_minimum_required(VERSION 3.22)
project(Personalized LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ─── Dobby (inline hooking framework for ARM64) ───
# Expected at vendor/dobby — cloned in CI or as submodule
if(EXISTS "${CMAKE_SOURCE_DIR}/vendor/dobby/CMakeLists.txt")
    set(DOBBY_DEBUG OFF CACHE BOOL "" FORCE)
    add_subdirectory(vendor/dobby)
else()
    message(STATUS "Dobby not at vendor/dobby — attempting FetchContent")
    include(FetchContent)
    FetchContent_Declare(dobby
        GIT_REPOSITORY https://github.com/jmpews/Dobby.git
        GIT_TAG        master
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(dobby)
endif()

# ─── Main mod shared library ───
add_library(Personalized SHARED
    src/Main.cpp
    src/SeedManager.cpp
    src/RandomMapper.cpp
)

target_include_directories(Personalized PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

# Dobby header path (may vary depending on how it was brought in)
if(TARGET dobby)
    target_link_libraries(Personalized PRIVATE dobby)
else()
    # FetchContent puts headers in the fetch content dir
    target_include_directories(Personalized PRIVATE
        ${dobby_SOURCE_DIR}/include
    )
    target_link_libraries(Personalized PRIVATE dobby)
endif()

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
