# ozz-animation isn't in vcpkg: this port builds its runtime and offline libraries (no tools,
# samples, tests or FBX/glTF importers: Midgard imports glTF itself) and writes the CMake
# package ozz doesn't install, with the targets ozz::base, ozz::animation and
# ozz::animation_offline.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO guillaumeblanc/ozz-animation
    REF "${VERSION}"
    SHA512 85e6c1b5e693c2f044815b92cb0896fb7f7d495d91cb9ff55aef4615483c33c3e4ef7232f84aa6539c92aed0e9d842fcc448712166a0eab2b252525196caff90
    HEAD_REF master
)

if(VCPKG_CRT_LINKAGE STREQUAL "dynamic")
    set(OZZ_RT_DLL ON)
else()
    set(OZZ_RT_DLL OFF)
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        --compile-no-warning-as-error
        -Dozz_build_tools=OFF
        -Dozz_build_fbx=OFF
        -Dozz_build_gltf=OFF
        -Dozz_build_data=OFF
        -Dozz_build_samples=OFF
        -Dozz_build_howtos=OFF
        -Dozz_build_tests=OFF
        -Dozz_build_postfix=OFF
        -Dozz_build_msvc_rt_dll=${OZZ_RT_DLL}
)
vcpkg_cmake_install()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include" "${CURRENT_PACKAGES_DIR}/debug/share")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/share/doc")

# The CMake package (ozz installs none).
set(OZZ_CONFIG "${CURRENT_PACKAGES_DIR}/share/ozz-animation/ozz-animation-config.cmake")
file(WRITE "${OZZ_CONFIG}" [[
# Written by Midgard's vcpkg overlay port (vcpkg-overlays/ozz-animation).
get_filename_component(_ozz_prefix "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
foreach(_ozz_lib IN ITEMS base animation animation_offline)
    if(NOT TARGET ozz::${_ozz_lib})
        add_library(ozz::${_ozz_lib} STATIC IMPORTED)
        find_library(_ozz_release NAMES ozz_${_ozz_lib} PATHS "${_ozz_prefix}/lib" NO_DEFAULT_PATH
                     NO_CACHE)
        find_library(_ozz_debug NAMES ozz_${_ozz_lib} PATHS "${_ozz_prefix}/debug/lib"
                     NO_DEFAULT_PATH NO_CACHE)
        set_target_properties(ozz::${_ozz_lib} PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${_ozz_prefix}/include"
            IMPORTED_LOCATION "${_ozz_release}"
            IMPORTED_CONFIGURATIONS "RELEASE")
        if(_ozz_debug)
            set_property(TARGET ozz::${_ozz_lib} APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
            set_target_properties(ozz::${_ozz_lib} PROPERTIES IMPORTED_LOCATION_DEBUG "${_ozz_debug}")
        endif()
        unset(_ozz_release)
        unset(_ozz_debug)
    endif()
endforeach()
set_property(TARGET ozz::animation APPEND PROPERTY INTERFACE_LINK_LIBRARIES ozz::base)
set_property(TARGET ozz::animation_offline APPEND PROPERTY INTERFACE_LINK_LIBRARIES ozz::animation)
set(ozz-animation_FOUND TRUE)
]])

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE.md")
