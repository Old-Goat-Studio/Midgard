// SPDX-License-Identifier: Apache-2.0
// Which way is north, and which way the player faces, on every world shape (milestone 6.0: the
// HUD's compass and minimap).
//
//   flat ground, bounded area, flat-wrap cylinder   north is -Z (the shape's own north)
//   curved cylinder                                 north is along the tube (the shape's north)
//   sphere outside or inside                        north points towards the +Y pole (the pole
//                                                   the world's latitude is measured from), so
//                                                   it never jumps at the cube's face edges
// At a sphere's poles, where every way is south (or north), the frame falls back to -Z.
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include <glm/glm.hpp>

#include "midgard/body.h"
#include "midgard/terrain.h"

namespace midgard {

// The compass's directions at a point: unit up, north and east (north x east = -up, so east is
// on the right facing north, seen from above).
struct Bearing {
    glm::dvec3 up{0.0, 1.0, 0.0};
    glm::dvec3 north{0.0, 0.0, -1.0};
    glm::dvec3 east{1.0, 0.0, 0.0};
};

// At p on the world (none: flat ground, +Y up).
Bearing bearing_at(const std::optional<Body>& body, const glm::dvec3& p);

// The heading of a direction, in degrees: 0 north, 90 east, 180 south, 270 west (0 to 360). A
// direction straight up or down has the heading of `fallback`.
double heading_of(const Bearing& b, const glm::dvec3& direction, double fallback = 0.0);

// Where `target` is from `from`, in metres east and north along the ground (the tangent plane
// at `from`: right for anything a minimap shows).
glm::dvec2 east_north(const Bearing& b, const glm::dvec3& from, const glm::dvec3& target);

// The ground around `centre` as a north-up map (as the world map colours it: midgard/
// world_map.h): size x size pixels, RGBA8, top row first, `radius` metres from its middle to
// each edge.
std::vector<std::uint8_t> local_map(const Terrain& terrain, const glm::dvec3& centre, double radius,
                                    int size);

}  // namespace midgard
