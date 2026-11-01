// SPDX-License-Identifier: Apache-2.0
#include "midgard/terrain_collision.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace midgard {

std::vector<TerrainCollision::Placed> TerrainCollision::chunks_near(const glm::dvec3& p,
                                                                    double radius) const {
    const Terrain& t = terrain_;
    const Body& body = t.body();
    const Shape::Kind kind = body.shape.kind();
    const int level = t.max_level();
    const std::int64_t across_u = std::int64_t(t.roots_u()) << level;
    const std::int64_t across_v = std::int64_t(t.roots_v()) << level;
    const glm::dvec2 size = body.shape.face_size();
    const double chunk_u = size.x / double(across_u), chunk_v = size.y / double(across_v);
    const SurfacePoint s = body.surface(p);
    // A flat-wrap cylinder: which copy along x the point is in.
    int wrap = 0;
    if (kind == Shape::Kind::FlatCylinder)
        wrap = int(std::floor((p.x - body.origin.x) / size.x + 0.5));
    const auto cu = std::int64_t(std::floor(s.u * double(across_u)));
    const auto cv = std::int64_t(std::floor(s.v * double(across_v)));
    const auto reach_u = std::int64_t(std::ceil(radius / chunk_u));
    const auto reach_v = std::int64_t(std::ceil(radius / chunk_v));
    const bool wraps = kind == Shape::Kind::FlatCylinder || kind == Shape::Kind::CurvedCylinder;
    std::vector<Placed> out;
    for (std::int64_t y = cv - reach_v; y <= cv + reach_v; ++y) {
        if (y < 0 || y >= across_v) continue;
        for (std::int64_t x = cu - reach_u; x <= cu + reach_u; ++x) {
            std::int64_t kx = x;
            int w = wrap;
            if (kx < 0 || kx >= across_u) {
                if (!wraps) continue;
                const std::int64_t turns = kx < 0 ? -1 : 1;
                kx -= turns * across_u;
                if (kind == Shape::Kind::FlatCylinder) w += int(turns);
            }
            out.push_back({ChunkKey{s.face, level, int(kx), int(y)}, w});
        }
    }
    return out;
}

physics::BodyDesc TerrainCollision::body_desc(const ChunkKey& key, int wrap) const {
    const Terrain& t = terrain_;
    const Body& body = t.body();
    const ChunkMesh m = t.mesh(key);
    physics::BodyDesc d;
    d.motion = physics::Motion::Static;
    d.user = kTerrainUser;
    d.position = m.origin;
    if (body.shape.kind() == Shape::Kind::FlatCylinder)
        d.position.x += wrap * body.shape.face_size().x;
    if (body.shape.kind() == Shape::Kind::BoundedArea ||
        body.shape.kind() == Shape::Kind::FlatCylinder) {
        const glm::dvec2 size = body.shape.face_size();
        d.collider.kind = physics::Collider::Kind::Heightfield;
        d.collider.samples = kChunkQuads + 1;
        d.collider.heights.reserve(m.heights.size());
        for (const double h : m.heights) d.collider.heights.push_back(float(h));
        d.collider.spacing = float(size.x / double(t.points_u(key.level)));
        d.collider.spacing_z = float(size.y / double(t.points_v(key.level)));
    } else {
        d.collider.kind = physics::Collider::Kind::Mesh;
        d.collider.points = m.positions;
        d.collider.triangles = m.indices;
    }
    return d;
}

void TerrainCollision::update(physics::World& world, const std::vector<glm::dvec3>& around,
                              double radius) {
    std::set<Placed> wanted, kept;
    for (const glm::dvec3& p : around) {
        for (const Placed& c : chunks_near(p, radius)) wanted.insert(c);
        for (const Placed& c : chunks_near(p, 2.0 * radius)) kept.insert(c);
    }
    for (auto it = bodies_.begin(); it != bodies_.end();) {
        if (!kept.contains(it->first)) {
            world.remove(it->second);
            it = bodies_.erase(it);
        } else {
            ++it;
        }
    }
    for (const Placed& c : wanted)
        if (!bodies_.contains(c)) bodies_.emplace(c, world.add(body_desc(c.key, c.wrap)));
}

void TerrainCollision::clear(physics::World& world) {
    for (const auto& [placed, id] : bodies_) world.remove(id);
    bodies_.clear();
}

}  // namespace midgard
