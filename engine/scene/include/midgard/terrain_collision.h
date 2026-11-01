// SPDX-License-Identifier: Apache-2.0
// Terrain collision (milestone 5.9): solid ground for the finest chunks round each player.
//
// Jolt's heightfield can't follow a curved surface, so a curved cylinder's chunks are mesh
// shapes; a bounded area's and a flat-wrap cylinder's are heightfields (both split each square
// along the same diagonal as the drawn chunks, so what the player walks on is what is drawn).
// Chunks are built as the player comes near and removed once they are well behind.
#pragma once

#include <cstdint>
#include <map>
#include <tuple>
#include <vector>

#include <glm/glm.hpp>

#include "midgard/physics.h"
#include "midgard/terrain.h"

namespace midgard {

// The user value of terrain bodies (what a raycast that hits the ground returns).
inline constexpr std::uint64_t kTerrainUser = ~std::uint64_t(0) - 1;

class TerrainCollision {
public:
    explicit TerrainCollision(const Terrain& terrain) : terrain_(terrain) {}

    // Keeps bodies for the finest chunks within `radius` metres of each point (the players),
    // building those that are missing and removing those beyond twice the radius.
    void update(physics::World& world, const std::vector<glm::dvec3>& around, double radius = 70.0);
    // Removes every terrain body (the world is about to go, or the terrain changed).
    void clear(physics::World& world);
    size_t bodies() const { return bodies_.size(); }

    // The collider and placement of one chunk (wrap: circumferences along x, flat cylinders).
    physics::BodyDesc body_desc(const ChunkKey& key, int wrap) const;

private:
    struct Placed {
        ChunkKey key;
        int wrap;
        bool operator<(const Placed& o) const {
            return std::tie(key.face, key.level, key.x, key.y, wrap) <
                   std::tie(o.key.face, o.key.level, o.key.x, o.key.y, o.wrap);
        }
    };
    std::vector<Placed> chunks_near(const glm::dvec3& p, double radius) const;

    Terrain terrain_;
    std::map<Placed, physics::BodyId> bodies_;
};

}  // namespace midgard
