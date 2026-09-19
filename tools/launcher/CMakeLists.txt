cmake_minimum_required(VERSION 3.20)
project(launcher LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT: the launcher must start even on machines without the VC++ redist.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

add_executable(demo-dx WIN32 launcher.cpp launcher.rc)

target_compile_options(demo-dx PRIVATE /W3 /utf-8)

set_target_properties(demo-dx PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}"
)
