cmake_minimum_required(VERSION 3.24)

project(Project360 VERSION 0.1.0 LANGUAGES CXX)

option(PROJECT360_BUILD_TESTS "Build Project360 tests" ON)
option(PROJECT360_BUILD_GUI "Build the Qt Widgets frontend when Qt 6 is available" ON)
option(PROJECT360_BUILD_BENCHMARKS "Build reproducible Project360 benchmarks" OFF)

add_library(project360_core
    src/common/log.cpp
    src/cpu/decoder.cpp
    src/cpu/interpreter.cpp
    src/gpu/xenos/pm4_decoder.cpp
    src/gpu/xenos/pm4_draw.cpp
    src/gpu/xenos/pm4_registers.cpp
    src/gpu/xenos/shader_alu.cpp
    src/gpu/xenos/shader_execution.cpp
    src/gpu/xenos/vertex_fetch.cpp
    src/kernel/guest_executor.cpp
    src/kernel/guest_pool.cpp
    src/kernel/kernel_dispatcher.cpp
    src/kernel/guest_thread_context.cpp
    src/loader/xex_parser.cpp
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

if(PROJECT360_BUILD_BENCHMARKS)
    add_executable(project360_kernel_memory_bench tools/kernel_memory_bench.cpp)
    target_link_libraries(project360_kernel_memory_bench PRIVATE project360_core)
endif()

if(PROJECT360_BUILD_GUI)
    find_package(Qt6 6.5 COMPONENTS Widgets QUIET)
    if(Qt6_FOUND)
        qt_standard_project_setup()

        qt_add_library(project360_qt_frontend STATIC
            src/frontend/qt/game_library_model.cpp
            src/frontend/qt/game_library_model.hpp
        )
        target_include_directories(project360_qt_frontend PUBLIC src src/frontend/qt)
        target_link_libraries(project360_qt_frontend PUBLIC Qt6::Widgets project360_frontend_core)
        target_compile_features(project360_qt_frontend PUBLIC cxx_std_20)
        if(MSVC)
            target_compile_options(project360_qt_frontend PRIVATE /W4 /permissive- /EHsc /utf-8)
        else()
            target_compile_options(project360_qt_frontend PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
        endif()

        qt_add_executable(project360_gui
            src/frontend/qt/main.cpp
            src/frontend/qt/main_window.cpp
            src/frontend/qt/main_window.hpp
        )
        target_link_libraries(project360_gui PRIVATE project360_qt_frontend)
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
    find_package(Threads REQUIRED)
    add_executable(project360_tests tests/cpu/cpu_tests.cpp)
    target_link_libraries(project360_tests PRIVATE project360_core Threads::Threads)
    add_test(NAME project360_tests COMMAND project360_tests)

    add_executable(project360_gpu_tests tests/gpu/pm4_decoder_tests.cpp)
    target_link_libraries(project360_gpu_tests PRIVATE project360_core)
    add_test(NAME project360_gpu_tests COMMAND project360_gpu_tests)

    add_executable(project360_memory_tests tests/memory/memory_tests.cpp)
    target_link_libraries(project360_memory_tests PRIVATE project360_core)
    add_test(NAME project360_memory_tests COMMAND project360_memory_tests)

    add_executable(project360_kernel_tests tests/kernel/kernel_dispatcher_tests.cpp)
    target_link_libraries(project360_kernel_tests PRIVATE project360_core)
    add_test(NAME project360_kernel_tests COMMAND project360_kernel_tests)

    add_executable(project360_loader_tests tests/loader/xex_parser_tests.cpp)
    target_link_libraries(project360_loader_tests PRIVATE project360_core)
    add_test(NAME project360_loader_tests COMMAND project360_loader_tests)

    add_executable(project360_library_tests tests/frontend/library_store_tests.cpp)
    target_link_libraries(project360_library_tests PRIVATE project360_frontend_core)
    add_test(NAME project360_library_tests COMMAND project360_library_tests)

    if(PROJECT360_BUILD_GUI AND Qt6_FOUND)
        add_executable(project360_qt_model_tests tests/frontend/qt_model_tests.cpp)
        target_link_libraries(project360_qt_model_tests PRIVATE project360_qt_frontend Qt6::Core)
        target_compile_features(project360_qt_model_tests PRIVATE cxx_std_20)
        if(MSVC)
            target_compile_options(project360_qt_model_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
        else()
            target_compile_options(project360_qt_model_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
        endif()
        add_test(NAME project360_qt_model_tests COMMAND project360_qt_model_tests)

        add_executable(project360_qt_window_tests
            tests/frontend/qt_window_tests.cpp
            src/frontend/qt/main_window.cpp
            src/frontend/qt/main_window.hpp
        )
        target_link_libraries(project360_qt_window_tests PRIVATE project360_qt_frontend Qt6::Widgets)
        target_compile_features(project360_qt_window_tests PRIVATE cxx_std_20)
        if(MSVC)
            target_compile_options(project360_qt_window_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
        else()
            target_compile_options(project360_qt_window_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
        endif()
        add_test(NAME project360_qt_window_tests COMMAND project360_qt_window_tests)
        set_tests_properties(project360_qt_window_tests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        )
    endif()
endif()
