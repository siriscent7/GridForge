cmake_minimum_required(VERSION 3.20)

if(APPLE AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum supported macOS version")
endif()

if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Build native Apple Silicon binaries")
endif()

project(GridForge VERSION 0.1.0 LANGUAGES CXX)

find_package(Threads REQUIRED)

if(APPLE)
    option(GRIDFORGE_ENABLE_METAL "Build the optional Apple Metal backend" ON)
else()
    option(GRIDFORGE_ENABLE_METAL "Build the optional Apple Metal backend" OFF)
endif()

if(GRIDFORGE_ENABLE_METAL AND NOT APPLE)
    message(FATAL_ERROR "GRIDFORGE_ENABLE_METAL=ON requires macOS and the Apple Metal/Foundation frameworks.")
endif()

option(GRIDFORGE_BUILD_TESTS "Build GridForge tests" ON)

add_library(gridforge src/runtime.cpp)
add_library(GridForge::gridforge ALIAS gridforge)
target_include_directories(gridforge
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(gridforge PUBLIC cxx_std_20)
target_link_libraries(gridforge PRIVATE Threads::Threads)
set_target_properties(gridforge PROPERTIES CXX_EXTENSIONS OFF)

if(GRIDFORGE_ENABLE_METAL)
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 20)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
    set(CMAKE_OBJCXX_EXTENSIONS OFF)

    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/vector_add.metal" GRIDFORGE_VECTOR_ADD_SHADER)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
    configure_file(src/metal_shader.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/metal_shader.hpp" @ONLY)
    target_sources(gridforge PRIVATE src/metal_bridge.mm)
    set_source_files_properties(src/metal_bridge.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_include_directories(gridforge PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_compile_definitions(gridforge PRIVATE GRIDFORGE_HAS_METAL=1)
    target_link_libraries(gridforge PRIVATE "-framework Metal" "-framework Foundation")
endif()

add_executable(gridforge_demo src/main.cpp)
target_link_libraries(gridforge_demo PRIVATE GridForge::gridforge)
target_compile_features(gridforge_demo PRIVATE cxx_std_20)

add_executable(gridforge_async_demo src/async_main.cpp)
target_link_libraries(gridforge_async_demo PRIVATE GridForge::gridforge)
target_compile_features(gridforge_async_demo PRIVATE cxx_std_20)

add_executable(gridforge_dependency_demo src/dependency_main.cpp)
target_link_libraries(gridforge_dependency_demo PRIVATE GridForge::gridforge)
target_compile_features(gridforge_dependency_demo PRIVATE cxx_std_20)

add_executable(gridforge_bench src/bench_main.cpp)
target_link_libraries(gridforge_bench PRIVATE GridForge::gridforge)
target_compile_features(gridforge_bench PRIVATE cxx_std_20)

set(GRIDFORGE_GIT_REVISION "unavailable")
set(GRIDFORGE_GIT_DIRTY "unknown")
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE GRIDFORGE_GIT_REVISION OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET RESULT_VARIABLE GRIDFORGE_GIT_REVISION_RESULT)
    if(NOT GRIDFORGE_GIT_REVISION_RESULT EQUAL 0)
        set(GRIDFORGE_GIT_REVISION "unavailable")
    endif()
    execute_process(COMMAND "${GIT_EXECUTABLE}" status --porcelain
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE GRIDFORGE_GIT_STATUS OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET RESULT_VARIABLE GRIDFORGE_GIT_STATUS_RESULT)
    if(GRIDFORGE_GIT_STATUS_RESULT EQUAL 0)
        if(GRIDFORGE_GIT_STATUS STREQUAL "")
            set(GRIDFORGE_GIT_DIRTY "false")
        else()
            set(GRIDFORGE_GIT_DIRTY "true")
        endif()
    endif()
endif()
target_compile_definitions(gridforge_bench PRIVATE
    GRIDFORGE_GIT_REVISION="${GRIDFORGE_GIT_REVISION}"
    GRIDFORGE_GIT_DIRTY="${GRIDFORGE_GIT_DIRTY}"
    GRIDFORGE_BUILD_CONFIG="$<CONFIG>")

if(GRIDFORGE_BUILD_TESTS)
    include(CTest)
    add_executable(gridforge_tests tests/test_runtime.cpp)
    target_link_libraries(gridforge_tests PRIVATE GridForge::gridforge)
    target_compile_features(gridforge_tests PRIVATE cxx_std_20)
    target_include_directories(gridforge_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

    add_executable(gridforge_async_tests tests/test_async.cpp)
    target_link_libraries(gridforge_async_tests PRIVATE GridForge::gridforge)
    target_compile_features(gridforge_async_tests PRIVATE cxx_std_20)
    target_include_directories(gridforge_async_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

    add_executable(gridforge_async_metal_tests tests/test_async_metal.cpp)
    target_link_libraries(gridforge_async_metal_tests PRIVATE GridForge::gridforge)
    target_compile_features(gridforge_async_metal_tests PRIVATE cxx_std_20)
    target_include_directories(gridforge_async_metal_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

    add_executable(gridforge_dependency_tests tests/test_dependencies.cpp)
    target_link_libraries(gridforge_dependency_tests PRIVATE GridForge::gridforge)
    target_compile_features(gridforge_dependency_tests PRIVATE cxx_std_20)
    target_include_directories(gridforge_dependency_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

    add_executable(gridforge_dependency_metal_tests tests/test_dependencies_metal.cpp)
    target_link_libraries(gridforge_dependency_metal_tests PRIVATE GridForge::gridforge)
    target_compile_features(gridforge_dependency_metal_tests PRIVATE cxx_std_20)

    add_executable(gridforge_bench_tests tests/test_bench.cpp)
    target_link_libraries(gridforge_bench_tests PRIVATE GridForge::gridforge)
    target_compile_features(gridforge_bench_tests PRIVATE cxx_std_20)
    target_include_directories(gridforge_bench_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

    add_test(NAME gridforge_cpu COMMAND gridforge_tests --backend cpu)
    add_test(NAME gridforge_metal COMMAND gridforge_tests --backend metal)
    add_test(NAME gridforge_async_cpu COMMAND gridforge_async_tests)
    add_test(NAME gridforge_async_metal COMMAND gridforge_async_metal_tests)
    add_test(NAME gridforge_dependencies_cpu COMMAND gridforge_dependency_tests)
    add_test(NAME gridforge_dependencies_metal COMMAND gridforge_dependency_metal_tests)
    add_dependencies(gridforge_bench_tests gridforge_bench)
    add_test(NAME gridforge_bench_tests COMMAND gridforge_bench_tests "$<TARGET_FILE:gridforge_bench>")
    set_tests_properties(gridforge_metal PROPERTIES SKIP_RETURN_CODE 77)
    set_tests_properties(gridforge_async_metal PROPERTIES SKIP_RETURN_CODE 77)
    set_tests_properties(gridforge_dependencies_metal PROPERTIES SKIP_RETURN_CODE 77)
    set_tests_properties(gridforge_cpu gridforge_metal gridforge_async_cpu gridforge_async_metal gridforge_dependencies_cpu gridforge_dependencies_metal gridforge_bench_tests PROPERTIES TIMEOUT 90)
endif()