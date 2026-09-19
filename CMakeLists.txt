cmake_minimum_required(VERSION 3.20)
project(TriangleBinDX LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W3 /permissive- /utf-8)
endif()

set(TB_SOURCES
    src/main.cpp
    src/renderer_common.cpp
    src/renderer_dx11.cpp
    src/renderer_dx12.cpp
    src/sdl2_atomic_shim.c
    deps/imgui/imgui.cpp
    deps/imgui/imgui_draw.cpp
    deps/imgui/imgui_tables.cpp
    deps/imgui/imgui_widgets.cpp
    deps/imgui/backends/imgui_impl_dx11.cpp
    deps/imgui/backends/imgui_impl_dx12.cpp
    deps/imgui/backends/imgui_impl_sdl2.cpp
)

# Map the MSVC target platform to the matching per-architecture SDL2 library
# folder (the SDL2-devel-VC package lays out libs under lib/<arch>). For the
# Visual Studio multi-config generator CMAKE_VS_PLATFORM_NAME is reliable; fall
# back to CMAKE_SYSTEM_PROCESSOR for single-config generators.
if(CMAKE_VS_PLATFORM_NAME STREQUAL "Win32" OR CMAKE_SYSTEM_PROCESSOR MATCHES "^(i[3-6]86|x86)$")
    set(SDL2_ARCH x86)
elseif(CMAKE_VS_PLATFORM_NAME STREQUAL "ARM64" OR CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64")
    set(SDL2_ARCH arm64)
elseif(CMAKE_VS_PLATFORM_NAME STREQUAL "ARM64EC")
    set(SDL2_ARCH arm64ec)
elseif(CMAKE_VS_PLATFORM_NAME STREQUAL "ARM" OR CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM|arm)$")
    set(SDL2_ARCH arm)
else()
    set(SDL2_ARCH x64)
endif()

add_executable(demo-dx ${TB_SOURCES})

set_target_properties(demo-dx PROPERTIES
    WIN32_EXECUTABLE TRUE
    LINK_OPTIONS "/ENTRY:mainCRTStartup"
)

target_include_directories(demo-dx PRIVATE
    src
    deps/imgui
    deps/imgui/backends
    deps/glm
    deps/SDL2/include
)

if(SDL2_ARCH STREQUAL "arm64")
    # SDL2's shared library cannot be linked for MSVC ARM64 (missing _Interlocked*
    # intrinsics). Link the statically-built SDL2 instead; sdl2_atomic_shim.c
    # provides the missing symbols by forwarding to the kernel32 API.
    target_link_libraries(demo-dx PRIVATE
        d3d11
        d3d12
        dxgi
        d3dcompiler
        "${CMAKE_CURRENT_SOURCE_DIR}/deps/SDL2/lib/arm64/SDL2-static.lib"
        user32 winmm imm32 version shell32 ole32 oleaut32 setupapi dinput8
    )
else()
    target_link_libraries(demo-dx PRIVATE
        d3d11
        d3d12
        dxgi
        d3dcompiler
        "${CMAKE_CURRENT_SOURCE_DIR}/deps/SDL2/lib/${SDL2_ARCH}/SDL2.lib"
        user32
    )
endif()

set_target_properties(demo-dx PROPERTIES
    # Keep each per-architecture build output inside its own build directory so
    # the four configurations never overwrite one another.
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}"
)
