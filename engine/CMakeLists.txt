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
    scene/src/validate.cpp
)
target_include_directories(midgard_scene PUBLIC scene/include PRIVATE "${CGLTF_INCLUDE_DIR}")
target_link_libraries(midgard_scene
    PUBLIC midgard_core midgard_render midgard_input EnTT::EnTT glm::glm nlohmann_json::nlohmann_json
    PRIVATE miniz::miniz unofficial::sqlite3::sqlite3 midgard_options
)

# ---- link: the editor's live link (TCP, length-prefixed JSON) --------------------------------
add_library(midgard_link STATIC link/src/link.cpp)
target_include_directories(midgard_link PUBLIC link/include)
target_link_libraries(midgard_link PUBLIC nlohmann_json::nlohmann_json PRIVATE midgard_options)
find_package(Threads REQUIRED)
target_link_libraries(midgard_link PUBLIC Threads::Threads)
if(WIN32)
    target_link_libraries(midgard_link PRIVATE ws2_32)
endif()

# ---- Python ------------------------------------------------------------------------------
# Two Pythons are involved. The host's Python (the project's .venv if there is one) runs build
# tools and provides pybind11 (pip install pybind11); _midgard is built for it. The game's own
# CPython (tools/setup_python.py, in .tools/python/) is what the runtime embeds for scripts.
set(MIDGARD_PYBIND11_DIR "")
if(NOT CMAKE_CROSSCOMPILING AND (MIDGARD_BUILD_PYTHON OR MIDGARD_SCRIPTING))
    if(NOT DEFINED Python_EXECUTABLE AND EXISTS "${PROJECT_SOURCE_DIR}/.venv")
        set(Python_ROOT_DIR "${PROJECT_SOURCE_DIR}/.venv")
    endif()
    find_package(Python 3.12 COMPONENTS Interpreter Development.Module)
    if(Python_FOUND)
        execute_process(COMMAND "${Python_EXECUTABLE}" -m pybind11 --cmakedir
                        OUTPUT_VARIABLE MIDGARD_PYBIND11_DIR
                        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    endif()
    if(MIDGARD_PYBIND11_DIR)
        file(TO_CMAKE_PATH "${MIDGARD_PYBIND11_DIR}" MIDGARD_PYBIND11_DIR)  # Windows backslashes
        set(PYBIND11_FINDPYTHON ON)
        find_package(pybind11 CONFIG REQUIRED PATHS "${MIDGARD_PYBIND11_DIR}" NO_DEFAULT_PATH)
    endif()
elseif(CMAKE_CROSSCOMPILING AND MIDGARD_SCRIPTING)
    # Cross-compiling (Android): the host's Python only runs tools and has pybind11's headers.
    # Searched in PATH order (the .venv first): the toolchain's search roots would otherwise find
    # the system's Python rather than the one pybind11 was installed into.
    find_program(Python_EXECUTABLE NAMES python3 python NAMES_PER_DIR
                 PATHS "${PROJECT_SOURCE_DIR}/.venv/bin" "${PROJECT_SOURCE_DIR}/.venv/Scripts"
                       ENV PATH
                 NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
    if(Python_EXECUTABLE)
        execute_process(COMMAND "${Python_EXECUTABLE}" -m pybind11 --cmakedir
                        OUTPUT_VARIABLE MIDGARD_PYBIND11_DIR
                        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    endif()
    if(MIDGARD_PYBIND11_DIR)
        file(TO_CMAKE_PATH "${MIDGARD_PYBIND11_DIR}" MIDGARD_PYBIND11_DIR)
        set(PYBIND11_NOPYTHON ON)  # headers only: the game's Python is set up below
        find_package(pybind11 CONFIG REQUIRED PATHS "${MIDGARD_PYBIND11_DIR}" NO_DEFAULT_PATH
                     NO_CMAKE_FIND_ROOT_PATH)
    endif()
endif()

# ---- script: the game's embedded CPython ----------------------------------------------------
if(MIDGARD_SCRIPTING)
    set(MIDGARD_PY_FOLDER "python")  # python-build-standalone's layout
    if(ANDROID)
        set(MIDGARD_PY_TARGET "android-arm64")
        set(MIDGARD_PY_FOLDER "prefix")  # python.org's Android build
    elseif(WIN32)
        set(MIDGARD_PY_TARGET "windows-x86_64")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
        set(MIDGARD_PY_TARGET "linux-arm64")
    else()
        set(MIDGARD_PY_TARGET "linux-x86_64")
    endif()
    file(GLOB MIDGARD_GAME_PYTHON LIST_DIRECTORIES true
         "${PROJECT_SOURCE_DIR}/.tools/python/*-${MIDGARD_PY_TARGET}/${MIDGARD_PY_FOLDER}")
    if(MIDGARD_GAME_PYTHON AND TARGET pybind11::headers)
        list(GET MIDGARD_GAME_PYTHON -1 MIDGARD_GAME_PYTHON)
        add_library(midgard_python INTERFACE)
        if(WIN32)
            file(GLOB MIDGARD_PY_LIB "${MIDGARD_GAME_PYTHON}/libs/python3[0-9]*.lib")
            target_include_directories(midgard_python INTERFACE "${MIDGARD_GAME_PYTHON}/include")
        elseif(ANDROID)
            file(GLOB MIDGARD_PY_LIB "${MIDGARD_GAME_PYTHON}/lib/libpython3.*.so")
            file(GLOB MIDGARD_PY_INCLUDE LIST_DIRECTORIES true
                 "${MIDGARD_GAME_PYTHON}/include/python3.*")
            target_include_directories(midgard_python INTERFACE ${MIDGARD_PY_INCLUDE})
        else()
            file(GLOB MIDGARD_PY_LIB "${MIDGARD_GAME_PYTHON}/lib/libpython3.*.so")
            file(GLOB MIDGARD_PY_INCLUDE LIST_DIRECTORIES true
                 "${MIDGARD_GAME_PYTHON}/include/python3.*")
            target_include_directories(midgard_python INTERFACE ${MIDGARD_PY_INCLUDE})
        endif()
        target_link_libraries(midgard_python INTERFACE ${MIDGARD_PY_LIB})

        add_library(midgard_script STATIC script/src/script.cpp script/src/script_system.cpp)
        target_include_directories(midgard_script PUBLIC script/include)
        target_link_libraries(midgard_script
            PUBLIC midgard_python midgard_scene midgard_input
            PRIVATE pybind11::headers midgard_options)
        message(STATUS "Scripting: CPython from ${MIDGARD_GAME_PYTHON}")
    elseif(MIDGARD_REQUIRE_PYTHON)
        message(FATAL_ERROR "Scripting needs the game's CPython (python tools/setup_python.py "
                            "--target ${MIDGARD_PY_TARGET}) and pybind11 (pip install pybind11). "
                            "Found: CPython folder '${MIDGARD_GAME_PYTHON}', host Python "
                            "'${Python_EXECUTABLE}', pybind11 '${MIDGARD_PYBIND11_DIR}'.")
    else()
        message(STATUS "Scripting: off (run python tools/setup_python.py, and pip install "
                       "pybind11, to embed Python in the runtime)")
    endif()
endif()

if(MIDGARD_BUILD_PROGRAMS)
    find_package(SDL3 CONFIG REQUIRED)
    if(ANDROID)
        # SDL's Java activity loads the game as libmain.so (SDL is linked in statically).
        add_library(midgard_runtime SHARED runtime/main.cpp)
        set_target_properties(midgard_runtime PROPERTIES OUTPUT_NAME main)
        target_link_options(midgard_runtime PRIVATE $<$<CONFIG:Release>:-s>)
    else()
        add_executable(midgard_runtime runtime/main.cpp
                       $<$<PLATFORM_ID:Windows>:runtime/runtime.rc>)
    endif()
    target_link_libraries(midgard_runtime PRIVATE
        midgard_core midgard_render midgard_scene midgard_link SDL3::SDL3 Vulkan::Headers
        midgard_options)
    target_include_directories(midgard_runtime PRIVATE ${Stb_INCLUDE_DIR})  # the window icon
    if(ANDROID)
        target_link_libraries(midgard_runtime PRIVATE miniz::miniz)  # unpacking Python
    endif()
    if(WIN32)
        # A windowed program (no console behind the game). It attaches to the terminal it was
        # started from, so --version still prints there.
        set_target_properties(midgard_runtime PROPERTIES WIN32_EXECUTABLE ON)
    endif()
    if(TARGET midgard_script)
        target_link_libraries(midgard_runtime PRIVATE midgard_script)
        target_compile_definitions(midgard_runtime PRIVATE MIDGARD_HAS_SCRIPTING=1)
        # The game's Python goes beside the runtime: python314.dll next to it on Windows (where
        # it can't be delay-loaded), python/lib/libpython3.14.so through the rpath on Linux, and
        # on Android libpython3.14.so beside libmain.so (in the APK) plus a python.zip asset.
        if(NOT WIN32 AND NOT ANDROID)
            set_target_properties(midgard_runtime PROPERTIES
                BUILD_RPATH "\$ORIGIN/python/lib" INSTALL_RPATH "\$ORIGIN/python/lib")
        endif()
        add_custom_command(TARGET midgard_runtime POST_BUILD
            COMMAND "${Python_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/setup_python.py" runtime
                    --target "${MIDGARD_PY_TARGET}" --out "$<TARGET_FILE_DIR:midgard_runtime>"
            COMMENT "Adding the game's Python beside the runtime"
            VERBATIM)
    endif()

    # The dedicated server is headless: no renderer, no SDL. Android never hosts.
    if(NOT ANDROID)
        add_executable(midgard_server server/main.cpp)
        target_link_libraries(midgard_server PRIVATE midgard_core midgard_options)
    endif()
endif()

# ---- _midgard: the engine as a Python module for the editor (roadmap §2, channel 3) ---------
# Built for the host's Python (see "Python" above); skipped without pybind11.
if(MIDGARD_BUILD_PYTHON AND NOT CMAKE_CROSSCOMPILING)
    if(MIDGARD_PYBIND11_DIR)
        pybind11_add_module(_midgard bindings/module.cpp bindings/viewport.cpp)
        target_link_libraries(_midgard PRIVATE midgard_scene midgard_link volk::volk
                              Vulkan::Headers midgard_options)
        if(UNIX)
            target_link_libraries(_midgard PRIVATE ${CMAKE_DL_LIBS})
        endif()
        message(STATUS "_midgard: building for Python ${Python_VERSION} (${Python_EXECUTABLE})")
    elseif(MIDGARD_REQUIRE_PYTHON)
        message(FATAL_ERROR "_midgard needs Python 3.12+ with pybind11 (pip install pybind11)")
    else()
        message(STATUS "_midgard: skipped (needs Python 3.12+ with pybind11: pip install pybind11)")
    endif()
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
        tests/test_link.cpp
    )
    target_link_libraries(midgard_tests PRIVATE
        midgard_core midgard_render midgard_scene midgard_link midgard_options doctest::doctest
        unofficial::sqlite3::sqlite3 miniz::miniz nlohmann_json::nlohmann_json)
    target_compile_definitions(midgard_tests PRIVATE
        MIDGARD_SCHEMA_DIR="${PROJECT_SOURCE_DIR}/schema")
    if(NOT CMAKE_CROSSCOMPILING)
        add_test(NAME midgard_tests COMMAND midgard_tests)
    endif()
    if(TARGET _midgard)
        add_test(NAME python_module
                 COMMAND "${Python_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_bindings.py"
                         "$<TARGET_FILE_DIR:_midgard>" "${PROJECT_SOURCE_DIR}/schema"
                         "${MIDGARD_VERSION}")
    endif()
endif()
