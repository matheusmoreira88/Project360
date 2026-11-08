cmake_minimum_required(VERSION 3.24)

project(Project360 VERSION 0.1.0 LANGUAGES CXX)

option(PROJECT360_BUILD_TESTS "Build Project360 tests" ON)

add_library(project360_core
    src/common/log.cpp
    src/cpu/decoder.cpp
    src/cpu/interpreter.cpp
    src/memory/guest_memory.cpp
)
target_include_directories(project360_core PUBLIC src)
target_compile_features(project360_core PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(project360_core PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(project360_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
endif()

add_executable(project360 src/app/main.cpp)
target_link_libraries(project360 PRIVATE project360_core)

if(PROJECT360_BUILD_TESTS)
    enable_testing()
    add_executable(project360_tests tests/cpu/cpu_tests.cpp)
    target_link_libraries(project360_tests PRIVATE project360_core)
    add_test(NAME project360_tests COMMAND project360_tests)
endif()
