# SPDX-License-Identifier: Apache-2.0
find_package(nlohmann_json CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)

# Warnings, and strict floating point so procedural generation gives identical results on every
# target (see the roadmap, §7 "Procedural generation").
add_library(midgard_options INTERFACE)
if(MSVC)
    target_compile_options(midgard_options INTERFACE /W4 /permissive- /utf-8 /fp:precise /Zc:__cplusplus)
else()
    target_compile_options(midgard_options INTERFACE -Wall -Wextra -Wpedantic -ffp-contract=off)
endif()

configure_file(core/version.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/midgard/version.h" @ONLY)

add_library(midgard_core STATIC
    core/src/manifest.cpp
    core/src/package.cpp
)
target_include_directories(midgard_core PUBLIC
    core/include
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
)
target_link_libraries(midgard_core
    PUBLIC nlohmann_json::nlohmann_json
    PRIVATE unofficial::sqlite3::sqlite3 midgard_options
)

if(MIDGARD_BUILD_PROGRAMS)
    add_executable(midgard_runtime runtime/main.cpp)
    target_link_libraries(midgard_runtime PRIVATE midgard_core midgard_options)

    add_executable(midgard_server server/main.cpp)
    target_link_libraries(midgard_server PRIVATE midgard_core midgard_options)
endif()

if(MIDGARD_BUILD_TESTS)
    find_package(doctest CONFIG REQUIRED)
    add_executable(midgard_tests
        tests/test_main.cpp
        tests/test_manifest.cpp
        tests/test_package.cpp
    )
    target_link_libraries(midgard_tests PRIVATE
        midgard_core midgard_options doctest::doctest unofficial::sqlite3::sqlite3)
    target_compile_definitions(midgard_tests PRIVATE
        MIDGARD_SCHEMA_DIR="${PROJECT_SOURCE_DIR}/schema")
    if(NOT CMAKE_CROSSCOMPILING)
        add_test(NAME midgard_tests COMMAND midgard_tests)
    endif()
endif()
