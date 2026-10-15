# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.25)

# One VERSION file for the whole repository (editor and engine).
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" MIDGARD_VERSION)
string(STRIP "${MIDGARD_VERSION}" MIDGARD_VERSION)

project(Midgard VERSION ${MIDGARD_VERSION} LANGUAGES CXX)
if(WIN32)
    enable_language(RC)  # the runtime's icon resource
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# The engine libraries also go into the _midgard Python module (a shared library).
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(MIDGARD_BUILD_PROGRAMS "Build midgard_runtime and midgard_server" ON)
option(MIDGARD_BUILD_TESTS "Build the engine tests" ON)
option(MIDGARD_BUILD_PYTHON "Build the _midgard Python module for the editor (needs pybind11)" ON)
option(MIDGARD_SCRIPTING "Embed CPython in the runtime for game scripts (tools/setup_python.py)" ON)
option(MIDGARD_REQUIRE_PYTHON
       "Fail instead of skipping _midgard or scripting when their Python parts are missing (CI)" OFF)

if(MIDGARD_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(engine)
