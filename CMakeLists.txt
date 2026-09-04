cmake_minimum_required(VERSION 3.21)
project(dlss5-gfn-overlay LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "This project targets Windows only: it uses Windows Graphics Capture, D3D11 and DXGI.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(dlss5-gfn-overlay
    src/main.cpp
    src/app.cpp
    src/capture.cpp
    src/config.cpp
    src/d3d.cpp
    src/hotkey.cpp
    src/log.cpp
    src/presenter.cpp
    src/stats.cpp
    src/worker.cpp
)

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
    windowsapp   # C++/WinRT: Windows.Graphics.Capture
    runtimeobject
)

if(MSVC)
    target_compile_options(dlss5-gfn-overlay PRIVATE /W4 /permissive- /await:strict /EHsc)
    # C++/WinRT headers need a recent enough Windows SDK; 10.0.19041 carries
    # IsBorderRequired and CreateFreeThreaded.
    set_property(TARGET dlss5-gfn-overlay PROPERTY VS_GLOBAL_WindowsTargetPlatformVersion "10.0.19041.0")
endif()

install(TARGETS dlss5-gfn-overlay RUNTIME DESTINATION .)
install(FILES config.example.ini DESTINATION .)
