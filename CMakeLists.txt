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
        concurrency_tests
        tests/concurrency_test.cpp
        minikv.concurrency
    )
    minikv_add_test(record_tests tests/record_test.cpp minikv.record)
    minikv_add_test(
        storage_log_tests
        tests/storage_log_test.cpp
        minikv.storage_log
    )
endif()
