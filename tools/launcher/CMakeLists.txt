cmake_minimum_required(VERSION 3.20)
project(launcher LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT: the launcher must start even on machines without the VC++ redist.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

# The launcher embeds the three per-architecture builds and the SDL2 DLLs they
# need as resources. Their absolute paths are resolved here at configure time
# rather than being hard-coded in the .rc file, so the repository stays
# machine-independent.
get_filename_component(TB_PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

set(TB_PAYLOAD_VARS
    TB_X64_EXE
    TB_X64_SDL2
    TB_X86_EXE
    TB_X86_SDL2
    TB_ARM64_EXE
)

set(TB_PAYLOAD_PATHS
    "${TB_PROJECT_ROOT}/build-x64/demo-dx.exe"
    "${TB_PROJECT_ROOT}/deps/SDL2/lib/x64/SDL2.dll"
    "${TB_PROJECT_ROOT}/build-x86/demo-dx.exe"
    "${TB_PROJECT_ROOT}/deps/SDL2/lib/x86/SDL2.dll"
    "${TB_PROJECT_ROOT}/build-arm64/demo-dx.exe"
)

foreach(_var _path IN ZIP_LISTS TB_PAYLOAD_VARS TB_PAYLOAD_PATHS)
    if(NOT EXISTS "${_path}")
        message(FATAL_ERROR
            "Launcher payload not found: ${_path}\n"
            "Build the x64, x86 and ARM64 configurations first -- see the "
            "Launcher section of README.md.")
    endif()
    file(TO_NATIVE_PATH "${_path}" _native)
    string(REPLACE "\\" "\\\\" _escaped "${_native}")
    set(${_var}_RC "${_escaped}")
endforeach()

configure_file(launcher.rc.in launcher.rc @ONLY)

add_executable(demo-dx WIN32 launcher.cpp "${CMAKE_CURRENT_BINARY_DIR}/launcher.rc")

target_compile_options(demo-dx PRIVATE /W3 /utf-8)

set_target_properties(demo-dx PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}"
)
