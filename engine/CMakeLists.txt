# SPDX-License-Identifier: Apache-2.0
find_package(nlohmann_json CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(VulkanHeaders CONFIG REQUIRED)
find_package(volk CONFIG REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(Stb REQUIRED)

# Warnings, and strict floating point so procedural generation gives identical results on every
# target (see the roadmap, §7 "Procedural generation").
add_library(midgard_options INTERFACE)
if(MSVC)
    target_compile_options(midgard_options INTERFACE /W4 /permissive- /utf-8 /fp:precise /Zc:__cplusplus)
    # Standard C functions (getenv, sscanf) are portable; MSVC's "_s" replacements are not.
    target_compile_definitions(midgard_options INTERFACE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(midgard_options INTERFACE -Wall -Wextra -Wpedantic -ffp-contract=off)
endif()

configure_file(core/version.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/midgard/version.h" @ONLY)

# ---- core: packages and data, no graphics (shared by the runtime, server and tools) ----------
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

# ---- shaders: GLSL compiled to SPIR-V at build time and embedded as C arrays ----------------
# glslangValidator is a host tool (vcpkg "host" dependency), so this also works when
# cross-compiling for Android.
file(GLOB GLSLANG_TOOL_DIRS LIST_DIRECTORIES true "${VCPKG_INSTALLED_DIR}/*/tools/glslang")
find_program(GLSLANG_VALIDATOR glslangValidator HINTS ${GLSLANG_TOOL_DIRS} REQUIRED)
set(SHADER_OUT "${CMAKE_CURRENT_BINARY_DIR}/generated/shaders")
file(MAKE_DIRECTORY "${SHADER_OUT}")
set(SHADER_HEADERS)
foreach(stage vert frag)
    set(src "${CMAKE_CURRENT_SOURCE_DIR}/render/shaders/mesh.${stage}")
    set(out "${SHADER_OUT}/mesh_${stage}.h")
    add_custom_command(
        OUTPUT "${out}"
        COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.1 --vn mesh_${stage} -o "${out}" "${src}"
        DEPENDS "${src}"
        COMMENT "Compiling shader mesh.${stage}")
    list(APPEND SHADER_HEADERS "${out}")
endforeach()
add_custom_target(midgard_shaders DEPENDS ${SHADER_HEADERS})

# ---- render: the Vulkan renderer -----------------------------------------------------------
add_library(midgard_render STATIC
    render/src/renderer.cpp
    render/src/mesh_data.cpp
    render/src/third_party_impl.cpp
)
add_dependencies(midgard_render midgard_shaders)
target_include_directories(midgard_render
    PUBLIC render/include
    PRIVATE "${SHADER_OUT}" ${Stb_INCLUDE_DIR}
)
target_compile_definitions(midgard_render PUBLIC GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_FORCE_RADIANS)
target_link_libraries(midgard_render
    PUBLIC glm::glm
    PRIVATE volk::volk Vulkan::Headers GPUOpen::VulkanMemoryAllocator midgard_options
)

# ---- input: the actions layer (platform independent) ----------------------------------------
add_library(midgard_input STATIC input/src/input.cpp)
target_include_directories(midgard_input PUBLIC input/include)
target_link_libraries(midgard_input PRIVATE unofficial::sqlite3::sqlite3 midgard_options)

# ---- scene: game.pak, glTF meshes and the EnTT scene built from the database ----------------
find_package(miniz CONFIG REQUIRED)
find_package(EnTT CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_path(CGLTF_INCLUDE_DIR cgltf.h REQUIRED)
add_library(midgard_scene STATIC
    scene/src/pak.cpp
    scene/src/gltf.cpp
    scene/src/scene.cpp
    scene/src/game.cpp
    scene/src/third_person.cpp
)
target_include_directories(midgard_scene PUBLIC scene/include PRIVATE "${CGLTF_INCLUDE_DIR}")
target_link_libraries(midgard_scene
    PUBLIC midgard_core midgard_render midgard_input EnTT::EnTT glm::glm
    PRIVATE miniz::miniz unofficial::sqlite3::sqlite3 nlohmann_json::nlohmann_json midgard_options
)

if(MIDGARD_BUILD_PROGRAMS)
    find_package(SDL3 CONFIG REQUIRED)
    add_executable(midgard_runtime runtime/main.cpp)
    target_link_libraries(midgard_runtime PRIVATE
        midgard_core midgard_render midgard_scene SDL3::SDL3 Vulkan::Headers midgard_options)
    if(WIN32)
        # A windowed program (no console behind the game). It attaches to the terminal it was
        # started from, so --version still prints there.
        set_target_properties(midgard_runtime PROPERTIES WIN32_EXECUTABLE ON)
    endif()

    # The dedicated server is headless: no renderer, no SDL.
    add_executable(midgard_server server/main.cpp)
    target_link_libraries(midgard_server PRIVATE midgard_core midgard_options)
endif()

if(MIDGARD_BUILD_TESTS)
    find_package(doctest CONFIG REQUIRED)
    add_executable(midgard_tests
        tests/test_main.cpp
        tests/test_manifest.cpp
        tests/test_package.cpp
        tests/test_mesh_data.cpp
        tests/test_scene.cpp
        tests/test_input.cpp
    )
    target_link_libraries(midgard_tests PRIVATE
        midgard_core midgard_render midgard_scene midgard_options doctest::doctest
        unofficial::sqlite3::sqlite3 miniz::miniz nlohmann_json::nlohmann_json)
    target_compile_definitions(midgard_tests PRIVATE
        MIDGARD_SCHEMA_DIR="${PROJECT_SOURCE_DIR}/schema")
    if(NOT CMAKE_CROSSCOMPILING)
        add_test(NAME midgard_tests COMMAND midgard_tests)
    endif()
endif()
