cmake_minimum_required(VERSION 3.24)

project(Project360 VERSION 0.1.0 LANGUAGES CXX)

option(PROJECT360_BUILD_TESTS "Build Project360 tests" ON)
option(PROJECT360_BUILD_GUI "Build the Qt Widgets frontend when Qt 6 is available" ON)

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

add_library(project360_frontend_core
    src/frontend/game_library_store.cpp
)
target_include_directories(project360_frontend_core PUBLIC src)
target_compile_features(project360_frontend_core PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(project360_frontend_core PRIVATE /W4 /permissive- /EHsc /utf-8)
else()
    target_compile_options(project360_frontend_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
endif()

add_executable(project360 src/app/main.cpp)
target_link_libraries(project360 PRIVATE project360_core)

if(PROJECT360_BUILD_GUI)
    find_package(Qt6 6.5 COMPONENTS Widgets QUIET)
    if(Qt6_FOUND)
        qt_standard_project_setup()
        qt_add_executable(project360_gui
            src/frontend/qt/main.cpp
            src/frontend/qt/main_window.cpp
            src/frontend/qt/main_window.hpp
        )
        target_link_libraries(project360_gui PRIVATE Qt6::Widgets project360_frontend_core)
        target_compile_features(project360_gui PRIVATE cxx_std_20)
        if(MSVC)
            target_compile_options(project360_gui PRIVATE /W4 /permissive- /EHsc /utf-8)
        else()
            target_compile_options(project360_gui PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
        endif()
    else()
        message(STATUS "Qt 6.5+ Widgets not found; skipping optional project360_gui target")
    endif()
endif()

if(PROJECT360_BUILD_TESTS)
    enable_testing()
    add_executable(project360_tests tests/cpu/cpu_tests.cpp)
    target_link_libraries(project360_tests PRIVATE project360_core)
    add_test(NAME project360_tests COMMAND project360_tests)

    add_executable(project360_library_tests tests/frontend/library_store_tests.cpp)
    target_link_libraries(project360_library_tests PRIVATE project360_frontend_core)
    add_test(NAME project360_library_tests COMMAND project360_library_tests)
endif()
