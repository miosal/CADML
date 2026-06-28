# CADML — declarative parametric CAD markup language. Apache-2.0.
#
# One project, two targets selected by toolchain:
#   cmake --preset default   → native libraries + CLI tools + tests
#   cmake --preset wasm      → the WebAssembly module (cadml.js / .wasm)
#
# All third-party deps are fetched + built from source (cmake/CadmlDependencies.cmake);
# no system packages are required. See docs/ for everything else.

cmake_minimum_required(VERSION 3.24)

project(cadml
    # Software/implementation version (semver). Independent of the CADML
    # *language* spec version, which is 0.1 (see docs/spec/language.md).
    VERSION      0.1.1
    DESCRIPTION  "CADML - declarative parametric CAD markup language"
    HOMEPAGE_URL "https://github.com/miosal/cadml"
    LANGUAGES    CXX C
)

set(CMAKE_CXX_STANDARD          20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

# Default tests + CLI tools ON for a standalone build, OFF when this
# project is pulled in via FetchContent / add_subdirectory.
option(CADML_BUILD_TESTS "Build the unit tests"       ${PROJECT_IS_TOP_LEVEL})
option(CADML_BUILD_TOOLS "Build the native CLI tools" ${PROJECT_IS_TOP_LEVEL})

# Set the WASM size/exception flags before deps are fetched so the fetched
# deps inherit them.
if (EMSCRIPTEN)
    add_compile_options(-fwasm-exceptions -Oz)
    add_link_options(-fwasm-exceptions -Oz)
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/CadmlDependencies.cmake)

# Shared strict-warning policy for internal targets.
function(cadml_strict_warnings target)
    if (MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wno-unused-parameter)
    endif()
endfunction()

# ── WebAssembly target ──────────────────────────────────────────────────
if (EMSCRIPTEN)
    add_subdirectory(src/wasm)
    return()
endif()

# ── Native target ───────────────────────────────────────────────────────
if (MSVC)
    add_compile_options(/utf-8)
endif()

# Default output layout (a parent project including us via FetchContent can
# override by setting these first).
if (NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()
if (NOT CMAKE_LIBRARY_OUTPUT_DIRECTORY)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
endif()
if (NOT CMAKE_ARCHIVE_OUTPUT_DIRECTORY)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
endif()

if (CADML_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(src/cadml)
add_subdirectory(src/cadml_compile)
add_subdirectory(src/engine)

if (CADML_BUILD_TOOLS)
    add_subdirectory(src/cli)
endif()

if (CADML_BUILD_TESTS)
    add_subdirectory(tests)
endif()

message(STATUS "CADML ${PROJECT_VERSION} (native) — tests=${CADML_BUILD_TESTS} tools=${CADML_BUILD_TOOLS}")
