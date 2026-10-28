cmake_minimum_required(VERSION 3.16)
project(nanoAudio VERSION 2.5.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(nanoAudioDriver SHARED
    nanoAudioDriver.h
    nanoAudioDriver.cpp
    nanoAudioConfig.h
    nanoAudioConfig.cpp
    nanoAudioDriver.def
)

target_compile_definitions(nanoAudioDriver PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_link_libraries(nanoAudioDriver PRIVATE
    ole32
    uuid
    avrt
    winmm
    user32
    gdi32
    advapi32
    comctl32
    ksuser
)

if(MSVC)
    target_compile_options(nanoAudioDriver PRIVATE /W4 /O2 /EHsc)
endif()
