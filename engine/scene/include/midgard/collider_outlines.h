// SPDX-License-Identifier: Apache-2.0
// Outlines of a scene's physics shapes for debug drawing (F4 in the game, View ▸ Show Colliders
// in the editor). They come from the components alone, so the editor draws them without running
// physics, and in the game they follow the entities as physics moves them.
//
// Colours: static scenery grey-green, dynamic bodies orange, kinematic ones blue, triggers
// yellow, and the player's capsule cyan.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "midgard/render/mesh_data.h"
#include "midgard/render/renderer.h"
#include "midgard/scene.h"

namespace midgard {

// A mesh's data by content id, or nullptr if it isn't loaded (its collider is then left out).
using MeshLookup = std::function<const render::MeshData*(const std::string& asset)>;

// The lines, relative to `origin` (the camera's), at most about max_lines of them.
std::vector<render::DebugLine> collider_outlines(const Scene& scene, const glm::dvec3& origin,
                                                 const MeshLookup& mesh,
                                                 size_t max_lines = 200'000);

}  // namespace midgard
