# ============================================================================
#  VectorDB — a local-first vector database in C++20.
#
#  Build layout
#  ------------
#    vectordb        static library  : all engine code (core, index, storage, db)
#    vectordb_cli    executable      : thin CLI, installed as `vectordb`
#    vectordb_tests  executable      : GoogleTest unit + integration suites
#    vectordb_bench  executable      : benchmark driver
#
#  Configure with a preset so the vcpkg toolchain is picked up automatically:
#    cmake --preset release && cmake --build --preset release
#
#  See learnings/20-build-and-dependencies/01-cmake.md for a guided tour.
# ============================================================================
cmake_minimum_required(VERSION 3.24)

project(VectorDB
    VERSION 0.1.0
    DESCRIPTION "A local-first vector database with brute-force and HNSW indexes"
    LANGUAGES CXX
)

# ---------------------------------------------------------------------------
# Language level. C++20 is required, not merely preferred: the codebase uses
# concepts, std::span, designated initialisers and <bit>.
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile_commands.json so clangd / clang-tidy work out of the box.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default to Release when the caller did not choose. Benchmarking a Debug
# build and reporting the numbers as "performance" is a documented no-no.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    message(STATUS "No build type specified; defaulting to Release")
endif()

# All binaries land in <build>/bin so scripts and tests can find them.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(VECTORDB_BUILD_TESTS      "Build the test suites"        ON)
option(VECTORDB_BUILD_BENCHMARKS "Build the benchmark driver"   ON)
option(VECTORDB_ENABLE_SIMD      "Enable SIMD distance kernels" ON)
option(VECTORDB_WERROR           "Treat warnings as errors"     OFF)

# ---------------------------------------------------------------------------
# Dependencies (provided by vcpkg in manifest mode)
# ---------------------------------------------------------------------------
find_package(spdlog          CONFIG REQUIRED)
find_package(nlohmann_json   CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# Warning set shared by every target we own (never applied to dependencies).
# ---------------------------------------------------------------------------
add_library(vectordb_warnings INTERFACE)
target_compile_options(vectordb_warnings INTERFACE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:
        -Wall -Wextra -Wpedantic
        -Wshadow -Wconversion -Wsign-conversion
        -Wnon-virtual-dtor -Woverloaded-virtual
        -Wcast-align -Wunused -Wnull-dereference
        -Wdouble-promotion -Wformat=2
    >
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)
if(VECTORDB_WERROR)
    target_compile_options(vectordb_warnings INTERFACE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Werror>
        $<$<CXX_COMPILER_ID:MSVC>:/WX>
    )
endif()

# ---------------------------------------------------------------------------
# The engine library
# ---------------------------------------------------------------------------
add_library(vectordb STATIC)
add_library(VectorDB::vectordb ALIAS vectordb)

target_include_directories(vectordb
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(vectordb
    PUBLIC
        spdlog::spdlog
    PRIVATE
        nlohmann_json::nlohmann_json
        unofficial::sqlite3::sqlite3
        vectordb_warnings
)

if(VECTORDB_ENABLE_SIMD)
    target_compile_definitions(vectordb PUBLIC VECTORDB_ENABLE_SIMD=1)
endif()

# Release builds: optimise hard, but never at the cost of IEEE correctness.
# -ffast-math would let the compiler reassociate float sums, which changes
# distance results and would break the scalar-vs-SIMD equivalence tests.
target_compile_options(vectordb PRIVATE
    $<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:GNU,Clang,AppleClang>>:-O3>
)

add_subdirectory(src)

# ---------------------------------------------------------------------------
# CLI executable
# ---------------------------------------------------------------------------
add_executable(vectordb_cli)
set_target_properties(vectordb_cli PROPERTIES OUTPUT_NAME vectordb)
target_link_libraries(vectordb_cli PRIVATE vectordb vectordb_warnings)
add_subdirectory(src/cli)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(VECTORDB_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Benchmarks
# ---------------------------------------------------------------------------
if(VECTORDB_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

# ---------------------------------------------------------------------------
# Configuration summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "VectorDB ${PROJECT_VERSION}")
message(STATUS "  build type ...... ${CMAKE_BUILD_TYPE}")
message(STATUS "  compiler ........ ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  target arch ..... ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "  tests ........... ${VECTORDB_BUILD_TESTS}")
message(STATUS "  benchmarks ...... ${VECTORDB_BUILD_BENCHMARKS}")
message(STATUS "  simd ............ ${VECTORDB_ENABLE_SIMD}")
message(STATUS "")
