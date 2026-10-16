// SPDX-License-Identifier: Apache-2.0
// The world's surface as a mesh: the ground of a scene's body (its shape at height 0), fine
// enough to stay within a few centimetres of the true curve, for drawing and as a mesh collider.
// It's the built-in mesh asset "builtin:surface"; terrain replaces it from milestone 5.9.
#pragma once

#include <optional>

#include "midgard/body.h"
#include "midgard/render/mesh_data.h"

namespace midgard {

inline constexpr const char* kBuiltinSurface = "builtin:surface";

// The surface of `body` in room space (place it with an identity transform), its triangles
// facing "up" into the world and its normals along up. Without a body: flat ground, 1 km square
// at y = 0.
render::MeshData surface_mesh(const std::optional<Body>& body);

}  // namespace midgard
