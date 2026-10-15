// SPDX-License-Identifier: Apache-2.0
// Loading meshes from glTF binary (.glb) files, read with cgltf.
#pragma once

#include <cstdint>
#include <span>

#include "midgard/render/mesh_data.h"

namespace midgard {

// All triangle primitives of all meshes in the file, merged into one mesh in model space
// (node transforms are not applied). Missing normals default to +Y. Throws AssetError.
render::MeshData load_glb(std::span<const uint8_t> bytes);

}  // namespace midgard
