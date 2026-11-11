// SPDX-License-Identifier: Apache-2.0
#include "midgard/footsteps.h"

#include <algorithm>
#include <cmath>

namespace midgard {

const std::vector<std::string>& footstep_surfaces() {
    static const std::vector<std::string> names = {"grass", "sand", "rock",  "snow",
                                                   "water", "wood", "stone", "metal"};
    return names;
}

std::string terrain_ground(const Terrain& terrain, const Body& body, const glm::dvec3& point) {
    const TerrainSettings& s = terrain.settings();
    const SurfacePoint p = body.surface(point);
    const double h = terrain.height(p.face, p.u, p.v);
    const double above = h - s.sea_level;
    if (above < -0.2) return "water";
    // The slope, from points a metre or so away across the face.
    const glm::dvec3 at = terrain.position(p.face, p.u, p.v, h);
    const double across = std::max(glm::distance(terrain.position(p.face, 0.0, p.v, 0.0),
                                                 terrain.position(p.face, 1.0, p.v, 0.0)),
                                   1.0);
    const double step = 1.0 / across;
    const double u2 = p.u + step, v2 = p.v + step;
    const glm::dvec3 eu = terrain.position(p.face, u2, p.v, terrain.height(p.face, u2, p.v)) - at;
    const glm::dvec3 ev = terrain.position(p.face, p.u, v2, terrain.height(p.face, p.u, v2)) - at;
    const glm::dvec3 up = body.up(at);
    glm::dvec3 n = glm::cross(eu, ev);
    n = glm::length(n) > 1e-12 ? glm::normalize(n) : up;
    const double slope = 1.0 - std::abs(glm::dot(n, up));  // 0 flat, 1 a wall
    const double painted = terrain.biome_at(p.face, p.u, p.v).z;
    // As terrain.frag lays them, each over the one before (at their midpoints).
    std::string ground = "grass";
    if (above < 1.75) ground = "sand";
    if (slope > 0.30) ground = "rock";
    if (painted < -0.3) ground = "sand";
    if (painted > 0.3) ground = "rock";
    // A paint layer (6.0b): a path or paving, stepped on as stone.
    if (std::abs(terrain.biome_at(p.face, p.u, p.v).w) > 0.3) ground = "rock";
    const bool cold =
        s.climate ? terrain.temperature_at(p.face, p.u, p.v, h) < 0.0 : h > s.snow_line;
    if (cold && slope < 0.525 && above >= 0.0) ground = "snow";
    return ground;
}

}  // namespace midgard
