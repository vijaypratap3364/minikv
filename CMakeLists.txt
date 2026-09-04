cmake_minimum_required(VERSION 3.20)

project(
    MiniKV
    VERSION 0.1.0
    DESCRIPTION "A small educational persistent key-value storage engine"
    LANGUAGES CXX
)

find_package(Threads REQUIRED)

option(
    MINIKV_ENABLE_TSAN
    "Instrument MiniKV and its tests with ThreadSanitizer"
    OFF
)
option(
    MINIKV_BUILD_BENCHMARKS
    "Build the dependency-free MiniKV benchmark runner"
    OFF
)
option(
    MINIKV_ENABLE_GPROF
    "Instrument MiniKV and benchmark targets for GNU gprof"
    OFF
)
if(MINIKV_ENABLE_TSAN)
    if(WIN32 OR NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(
            FATAL_ERROR
            "MINIKV_ENABLE_TSAN requires GCC or Clang on a non-Windows host"
        )
    endif()
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()
if(MINIKV_ENABLE_GPROF)
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        message(FATAL_ERROR "MINIKV_ENABLE_GPROF requires GCC")
    endif()
    add_compile_options(-pg)
    add_link_options(-pg)
endif()

function(minikv_enable_warnings target_name)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE /W4 /permissive-)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

add_library(
    minikv
    src/minikv.cpp
    src/platform_sync.cpp
    src/record.cpp
    src/segmented_storage.cpp
    src/storage_log.cpp
    src/version.cpp
)
add_library(MiniKV::minikv ALIAS minikv)

target_compile_features(minikv PUBLIC cxx_std_20)
target_link_libraries(minikv PUBLIC Threads::Threads)
target_include_directories(
    minikv
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
set_target_properties(minikv PROPERTIES CXX_EXTENSIONS OFF)
minikv_enable_warnings(minikv)

add_executable(minikv_demo tools/minikv_demo.cpp)
target_link_libraries(minikv_demo PRIVATE MiniKV::minikv)
set_target_properties(minikv_demo PROPERTIES CXX_EXTENSIONS OFF)
minikv_enable_warnings(minikv_demo)

if(MINIKV_BUILD_BENCHMARKS)
    execute_process(
        COMMAND git rev-parse --short=12 HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE MINIKV_BENCHMARK_GIT_SHA
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(NOT MINIKV_BENCHMARK_GIT_SHA)
        set(MINIKV_BENCHMARK_GIT_SHA "unknown")
    endif()

    add_executable(minikv_benchmark benchmarks/minikv_benchmark.cpp)
    target_link_libraries(minikv_benchmark PRIVATE MiniKV::minikv)
    target_compile_definitions(
        minikv_benchmark
        PRIVATE
            MINIKV_BENCHMARK_BUILD_TYPE="$<CONFIG>"
            MINIKV_BENCHMARK_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
            MINIKV_BENCHMARK_GIT_SHA="${MINIKV_BENCHMARK_GIT_SHA}"
            MINIKV_BENCHMARK_OS="${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_VERSION}"
    )
    set_target_properties(
        minikv_benchmark PROPERTIES CXX_EXTENSIONS OFF
    )
    minikv_enable_warnings(minikv_benchmark)
endif()

include(CTest)
if(BUILD_TESTING)
    function(minikv_add_test target_name source_file test_name)
        add_executable(${target_name} ${source_file})
        target_link_libraries(${target_name} PRIVATE MiniKV::minikv)
        target_include_directories(
            ${target_name}
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/src
                ${CMAKE_CURRENT_SOURCE_DIR}/tests
        )
        set_target_properties(${target_name} PROPERTIES CXX_EXTENSIONS OFF)
        minikv_enable_warnings(${target_name})

        add_test(NAME ${test_name} COMMAND ${target_name})
    endfunction()

    minikv_add_test(minikv_tests tests/minikv_test.cpp minikv.api)
    minikv_add_test(
        compaction_tests
        tests/compaction_test.cpp
        minikv.compaction
    )
    minikv_add_test(
        concurrency_tests
        tests/concurrency_test.cpp
        minikv.concurrency
    )
    minikv_add_test(record_tests tests/record_test.cpp minikv.record)
    minikv_add_test(
        segmented_storage_tests
        tests/segmented_storage_test.cpp
        minikv.segmented_storage
    )
    minikv_add_test(
        storage_log_tests
        tests/storage_log_test.cpp
        minikv.storage_log
    )
endif()
