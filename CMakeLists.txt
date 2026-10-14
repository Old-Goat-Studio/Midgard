# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.25)

# One VERSION file for the whole repository (editor and engine).
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" MIDGARD_VERSION)
string(STRIP "${MIDGARD_VERSION}" MIDGARD_VERSION)

project(Midgard VERSION ${MIDGARD_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MIDGARD_BUILD_PROGRAMS "Build midgard_runtime and midgard_server" ON)
option(MIDGARD_BUILD_TESTS "Build the engine tests" ON)

if(MIDGARD_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(engine)
