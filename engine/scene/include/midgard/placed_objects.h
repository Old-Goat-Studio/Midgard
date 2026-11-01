// SPDX-License-Identifier: Apache-2.0
// Objects placed over the ground by a world type's rules (milestone 5.11): the placements
// (midgard/terrain.h's Terrain::placements_in) near a point, kept in the scene as instances of
// their object templates, so they are drawn, lit and solid like any other instance. They are
// made as the point comes within each rule's distance, a few tiles at a time, nearest first,
// and taken away once well beyond it. They are the game's own (ids "~placed:..."): never saved
// or shown in the editor's lists.
#pragma once

#include <functional>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include <glm/glm.hpp>

#include "midgard/scene.h"
#include "midgard/terrain.h"

namespace midgard {

class PlacedObjects {
public:
    explicit PlacedObjects(const Terrain& terrain) : terrain_(terrain) {}

    // Places what is missing near `around` (at most `max_tiles` tiles this time) and takes away
    // what is well beyond it. `added` is told each new instance (after it is in the scene);
    // `removing` each one about to go.
    void update(Scene& scene, const glm::dvec3& around, int max_tiles,
                const std::function<void(entt::entity)>& added = {},
                const std::function<void(entt::entity)>& removing = {});
    // Takes them all away (the terrain or its rules changed).
    void clear(Scene& scene, const std::function<void(entt::entity)>& removing = {});
    size_t count() const;  // instances in the scene
    size_t tiles() const { return tiles_.size(); }

    static bool is_placed(const std::string& entity_id);  // "~placed:..."

private:
    using Tile = std::tuple<int, int, int, int>;  // face, level, x, y
    double reach() const;                         // the furthest rule's distance
    Terrain terrain_;
    std::map<Tile, std::vector<std::string>> tiles_;
};

}  // namespace midgard
