// SPDX-License-Identifier: Apache-2.0
// A world's map (milestone 5.11): the whole of its ground as an image, coloured by the sea's
// depth, the ground's biome (or the 5.9 grass and rock) and its snow, shaded by its slopes, in
// one projection per shape:
//   bounded area        from above (x across, z down)
//   flat or curved cylinder   unrolled: round it across, along it down
//   sphere outside or inside  latitude and longitude (equirectangular): longitude across,
//                             the +Y pole at the top
// The editor's World Type editor shows it; the game's map screen (Phase 6) will too.
#pragma once

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include "midgard/terrain.h"

namespace midgard {

// width x height pixels, RGBA8, top row first.
std::vector<std::uint8_t> world_map(const Terrain& terrain, int width, int height);

// Where a map pixel's middle is on the world (face, u, v): the projection above.
SurfacePoint world_map_place(const Terrain& terrain, int width, int height, int x, int y);

}  // namespace midgard
