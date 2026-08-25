cmake_minimum_required(VERSION 3.20)

project(
    MiniKV
    VERSION 0.1.0
    DESCRIPTION "A small educational persistent key-value storage engine"
    LANGUAGES CXX
)

function(minikv_enable_warnings target_name)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE /W4 /permissive-)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

add_library(minikv src/version.cpp)
add_library(MiniKV::minikv ALIAS minikv)

target_compile_features(minikv PUBLIC cxx_std_20)
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
    add_executable(minikv_tests tests/version_test.cpp)
    target_link_libraries(minikv_tests PRIVATE MiniKV::minikv)
    set_target_properties(minikv_tests PROPERTIES CXX_EXTENSIONS OFF)
    minikv_enable_warnings(minikv_tests)

    add_test(NAME minikv.version COMMAND minikv_tests)
endif()
