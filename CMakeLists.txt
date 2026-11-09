cmake_minimum_required(VERSION 3.21)
project(M720Mapper VERSION 2.0.0 LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Build with MSVC x64 and a Windows SDK that includes C++/WinRT headers.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_options(/W4 /permissive- /utf-8 /EHsc)
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

add_executable(M720Mapper WIN32 src/main.cpp src/core.cpp)
target_link_libraries(M720Mapper PRIVATE BluetoothApis Setupapi User32 Shell32 Advapi32 WindowsApp)

include(CTest)
if(BUILD_TESTING)
    add_executable(protocol_tests tests/protocol_tests.cpp)
    target_include_directories(protocol_tests PRIVATE src)
    add_test(NAME protocol_tests COMMAND protocol_tests)
endif()
