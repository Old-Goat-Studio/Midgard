// SPDX-License-Identifier: Apache-2.0
#include "midgard/placed_objects.h"

#include <algorithm>
#include <cmath>
#include <set>

#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include "midgard/scene_physics.h"

namespace midgard {

namespace {

constexpr const char* kPrefix = "~placed:";

// The turn that takes unit vector a to unit vector b (half a turn about x if they're opposite).
glm::quat from_to(const glm::vec3& a, const glm::vec3& b) {
    const float w = 1.0f + glm::dot(a, b);
    if (w < 1e-6f) return glm::quat(0.0f, 1.0f, 0.0f, 0.0f);
    const glm::vec3 axis = glm::cross(a, b);
    return glm::normalize(glm::quat(w, axis.x, axis.y, axis.z));
}

std::string tile_name(int face, int level, int x, int y) {
    return std::string(kPrefix) + std::to_string(face) + ":" + std::to_string(level) + ":" +
           std::to_string(x) + ":" + std::to_string(y) + ":";
}

}  // namespace

bool PlacedObjects::is_placed(const std::string& entity_id) {
    return entity_id.rfind(kPrefix, 0) == 0;
}

double PlacedObjects::reach() const {
    double out = 0.0;
    for (const PlacementRule& r : terrain_.settings().placements) out = std::max(out, r.distance);
    return out;
}

size_t PlacedObjects::count() const {
    size_t n = 0;
    for (const auto& [tile, ids] : tiles_) n += ids.size();
    return n;
}

void PlacedObjects::clear(Scene& scene, const std::function<void(entt::entity)>& removing) {
    for (const auto& [tile, ids] : tiles_)
        for (const std::string& id : ids) {
            if (removing) removing(scene.find(id));
            scene.remove_entity(id);
        }
    tiles_.clear();
}

void PlacedObjects::update(Scene& scene, const glm::dvec3& around, int max_tiles,
                           const std::function<void(entt::entity)>& added,
                           const std::function<void(entt::entity)>& removing) {
    const double reach_m = reach();
    if (reach_m <= 0.0) return;
    const Terrain& t = terrain_;
    const int level = t.placement_level();
    const double side = t.body().shape.face_size().x / double(t.chunks_u(level));
    const auto distance = [&](const ChunkKey& k) {
        return std::max(0.0, glm::length(t.chunk_origin(k) - around) - 0.75 * side);
    };

    // The tiles within reach: out from the one under the point through its neighbours (across
    // a sphere's face edges and a cylinder's join too).
    const SurfacePoint s = t.body().surface(around);
    const auto clamp_index = [](double f, std::int64_t n) {
        return int(std::clamp<std::int64_t>(std::int64_t(std::floor(f * double(n))), 0, n - 1));
    };
    const ChunkKey start{s.face, level, clamp_index(s.u, t.chunks_u(level)),
                         clamp_index(s.v, t.chunks_v(level))};
    std::vector<std::pair<double, ChunkKey>> wanted;
    std::set<Tile> seen{{start.face, start.level, start.x, start.y}};
    std::vector<ChunkKey> todo{start};
    while (!todo.empty()) {
        const ChunkKey k = todo.back();
        todo.pop_back();
        const double d = distance(k);
        if (d > reach_m) continue;
        wanted.emplace_back(d, k);
        for (int e = 0; e < 4; ++e) {
            const std::optional<ChunkKey> n = t.neighbour(k, ChunkEdge(e));
            if (n && seen.insert({n->face, n->level, n->x, n->y}).second) todo.push_back(*n);
        }
    }

    // Well beyond reach: taken away.
    for (auto it = tiles_.begin(); it != tiles_.end();) {
        const auto [face, l, x, y] = it->first;
        if (distance({face, l, x, y}) > 1.5 * reach_m) {
            for (const std::string& id : it->second) {
                if (removing) removing(scene.find(id));
                scene.remove_entity(id);
            }
            it = tiles_.erase(it);
        } else {
            ++it;
        }
    }

    // New tiles, nearest first, a few at a time.
    std::sort(wanted.begin(), wanted.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    int made = 0;
    for (const auto& [d, k] : wanted) {
        const Tile key{k.face, k.level, k.x, k.y};
        if (tiles_.contains(key)) continue;
        if (made++ >= max_tiles) break;
        std::vector<std::string>& ids = tiles_[key];
        const std::vector<TerrainPlacement> placed = t.placements_in(k);
        for (size_t i = 0; i < placed.size(); ++i) {
            const TerrainPlacement& p = placed[i];
            const PlacementRule& rule = t.settings().placements[size_t(p.rule)];
            if (d > rule.distance || !scene.objects.contains(rule.object)) continue;
            // Upright along its up, turned about it.
            const glm::quat lean = from_to(glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(p.up));
            const glm::quat turn =
                glm::angleAxis(float(glm::radians(p.yaw)), glm::vec3(0.0f, 1.0f, 0.0f));
            const glm::vec3 rotation = euler_degrees(lean * turn);
            const float scale = float(p.scale);
            const nlohmann::json components = {
                {"transform",
                 {{"position", {p.position.x, p.position.y, p.position.z}},
                  {"rotation", {rotation.x, rotation.y, rotation.z}},
                  {"scale", {scale, scale, scale}}}},
                {"object", {{"id", rule.object}}}};
            const std::string id = tile_name(k.face, k.level, k.x, k.y) + std::to_string(i);
            const entt::entity e = scene.put_entity(id, rule.object, "", components);
            ids.push_back(id);
            if (added) added(e);
        }
    }
}

}  // namespace midgard
