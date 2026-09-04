cmake_minimum_required(VERSION 3.21)
project(dlss5-gfn-overlay LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "This project targets Windows. Cross-compile with cmake/mingw-w64-x86_64.cmake.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Capture backend.
#   wgc  - Windows Graphics Capture. Captures the one window and nothing else,
#          follows it between monitors, never sees the rest of the desktop.
#          Needs C++/WinRT, so MSVC only.
#   dxgi - Desktop Duplication. Duplicates the monitor the window is on and
#          crops to it. Builds on every toolchain, including MinGW.
set(GFN_CAPTURE_BACKEND "auto" CACHE STRING "Capture backend: auto, wgc or dxgi")
set_property(CACHE GFN_CAPTURE_BACKEND PROPERTY STRINGS auto wgc dxgi)

if(GFN_CAPTURE_BACKEND STREQUAL "auto")
    if(MSVC)
        set(GFN_CAPTURE_BACKEND "wgc")
    else()
        set(GFN_CAPTURE_BACKEND "dxgi")
    endif()
endif()

if(GFN_CAPTURE_BACKEND STREQUAL "wgc" AND NOT MSVC)
    message(FATAL_ERROR "The wgc backend needs C++/WinRT, which only MSVC provides. Use -DGFN_CAPTURE_BACKEND=dxgi.")
endif()

message(STATUS "Capture backend: ${GFN_CAPTURE_BACKEND}")

set(GFN_SOURCES
    src/main.cpp
    src/app.cpp
    src/config.cpp
    src/d3d.cpp
    src/hotkey.cpp
    src/log.cpp
    src/presenter.cpp
    src/stats.cpp
    src/worker.cpp
)

if(GFN_CAPTURE_BACKEND STREQUAL "wgc")
    list(APPEND GFN_SOURCES src/capture.cpp)
else()
    list(APPEND GFN_SOURCES src/capture_dxgi.cpp)
endif()

add_executable(dlss5-gfn-overlay ${GFN_SOURCES})
target_include_directories(dlss5-gfn-overlay PRIVATE src)

target_compile_definitions(dlss5-gfn-overlay PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

target_link_libraries(dlss5-gfn-overlay PRIVATE
    d3d11
    dxgi
    d3dcompiler
    dwmapi
    gdi32
    user32
    ole32
)

if(GFN_CAPTURE_BACKEND STREQUAL "wgc")
    target_compile_definitions(dlss5-gfn-overlay PRIVATE GFN_CAPTURE_WGC)
    target_link_libraries(dlss5-gfn-overlay PRIVATE windowsapp runtimeobject)
endif()

if(MSVC)
    target_compile_options(dlss5-gfn-overlay PRIVATE /W4 /permissive- /EHsc)
    set_property(TARGET dlss5-gfn-overlay PROPERTY VS_GLOBAL_WindowsTargetPlatformVersion "10.0.19041.0")
else()
    target_compile_options(dlss5-gfn-overlay PRIVATE -Wall -Wextra)
    # Ship one file: no libstdc++, libgcc or winpthread DLL alongside the exe.
    target_link_options(dlss5-gfn-overlay PRIVATE
        -static-libgcc -static-libstdc++
        -Wl,-Bstatic -lstdc++ -lwinpthread -Wl,-Bdynamic)
endif()

install(TARGETS dlss5-gfn-overlay RUNTIME DESTINATION .)
install(FILES config.example.ini DESTINATION .)
