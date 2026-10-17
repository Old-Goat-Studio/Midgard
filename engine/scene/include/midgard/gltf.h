// SPDX-License-Identifier: Apache-2.0
// Loading meshes from glTF binary (.glb) files, read with cgltf.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "midgard/render/mesh_data.h"

namespace midgard {

// All triangle primitives of all meshes in the file, merged into one mesh in model space
// (node transforms are not applied). Missing normals default to +Y. Throws AssetError.
render::MeshData load_glb(std::span<const uint8_t> bytes);

// A mesh as a glTF binary: positions, normals, uvs, tangents and 32-bit indices (what load_glb
// reads back exactly).
std::vector<uint8_t> save_glb(const render::MeshData& mesh);

}  // namespace midgard
