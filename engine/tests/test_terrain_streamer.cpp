// SPDX-License-Identifier: Apache-2.0
// Streaming terrain (midgard/terrain_streamer.h): the chunks chosen cover the ground once, no
// two touching chunks are more than a level apart, edges meeting coarser chunks are marked,
// detail follows the camera, a flat-wrap cylinder goes round, and fast travel never leaves
// holes or lets memory grow.
#include <doctest/doctest.h>

#include <map>
#include <tuple>

#include <nlohmann/json.hpp>

#include "midgard/terrain_streamer.h"

using json = nlohmann::json;

namespace {

midgard::Body body(const char* text) {
    return midgard::body_from_json(json::parse(text));
}

const char* kArea = R"({"id": "a", "shape": "bounded_area", "width": 6000, "length": 4000,
                        "seed": 7})";
const char* kStrip = R"({"id": "s", "shape": "flat_cylinder", "circumference": 6000,
                         "length": 5000, "seed": 8})";
const char* kHabitat = R"({"id": "h", "shape": "curved_cylinder", "radius": 1000,
                           "length": 5000, "seed": 9})";

// The area of the face a chunk covers, in grid points at the finest level, squared.
double coverage(const midgard::Terrain& t, const std::vector<midgard::TerrainLeaf>& leaves) {
    double sum = 0.0;
    for (const midgard::TerrainLeaf& l : leaves) {
        const double side = double(std::int64_t(1) << (t.max_level() - l.key.level));
        sum += side * side;
    }
    return sum;
}

// Every chosen chunk's neighbours are at most one level coarser or finer, and an edge is marked
// coarser exactly when the chunk there is its parent's neighbour.
void check_balanced(const midgard::Terrain& t, const std::vector<midgard::TerrainLeaf>& leaves) {
    auto less = [](const midgard::ChunkKey& a, const midgard::ChunkKey& b) {
        return std::tie(a.face, a.level, a.x, a.y) < std::tie(b.face, b.level, b.x, b.y);
    };
    std::map<midgard::ChunkKey, int, decltype(less)> level(less);
    for (const midgard::TerrainLeaf& l : leaves) level[l.key] = l.key.level;
    const auto covering = [&](midgard::ChunkKey k) -> std::optional<midgard::ChunkKey> {
        while (true) {
            if (level.contains(k)) return k;
            if (k.level == 0) return std::nullopt;
            k = t.parent(k);
        }
    };
    for (const midgard::TerrainLeaf& l : leaves) {
        for (int e = 0; e < 4; ++e) {
            const auto n = t.neighbour(l.key, midgard::ChunkEdge(e));
            if (!n) continue;
            const auto c = covering(*n);
            if (!c) {
                CHECK_FALSE(l.coarser[size_t(e)]);
                continue;
            }
            CHECK(c->level >= l.key.level - 1);
            CHECK(l.coarser[size_t(e)] == (c->level == l.key.level - 1));
        }
    }
}

}  // namespace

TEST_CASE("terrain chunks: the ground covered once, finer near the camera, balanced") {
    midgard::TerrainStreamer s(body(kArea), {});
    const midgard::Terrain& t = s.terrain();
    s.set_range(1e9);  // everything
    const std::vector<midgard::TerrainLeaf> leaves = s.choose({0.0, 20.0, 0.0});
    // The whole face, once: the leaves' areas add up to the face's.
    const double side = double(std::int64_t(1) << t.max_level());
    CHECK(coverage(t, leaves) == doctest::Approx(t.roots_u() * t.roots_v() * side * side));
    check_balanced(t, leaves);
    // Finest under the camera, coarse far away.
    int under = -1, far = 99;
    for (const midgard::TerrainLeaf& l : leaves) {
        const glm::dvec3 o = t.chunk_origin(l.key);
        if (glm::length(glm::dvec2(o.x, o.z)) < 70.0) under = std::max(under, l.key.level);
        if (std::abs(o.x) > 2500.0) far = std::min(far, l.key.level);
    }
    CHECK(under == t.max_level());
    CHECK(far <= 1);
    // Out of range: not drawn.
    s.set_range(1500.0);
    CHECK(s.choose({0.0, 20.0, 0.0}).size() < leaves.size());
}

TEST_CASE("terrain chunks on a flat-wrap cylinder go round: nearer copies are chosen") {
    midgard::TerrainStreamer s(body(kStrip), {});
    s.set_range(2500.0);
    // Near the east edge (x = 3000), the west edge's chunks are drawn one circumference east.
    const std::vector<midgard::TerrainLeaf> leaves = s.choose({2900.0, 20.0, 0.0});
    bool wrapped = false;
    for (const midgard::TerrainLeaf& l : leaves)
        if (l.key.x == 0) {
            CHECK(l.wrap == 1);
            wrapped = true;
        }
    CHECK(wrapped);
    check_balanced(s.terrain(), leaves);
}

TEST_CASE("terrain chunks on a curved cylinder: the far side is always chosen") {
    midgard::TerrainStreamer s(body(kHabitat), {});
    s.set_range(100.0);  // short, but the sky overhead is the far side of the world
    const std::vector<midgard::TerrainLeaf> leaves = s.choose({0.0, -998.0, 0.0});
    bool overhead = false;
    for (const midgard::TerrainLeaf& l : leaves) {
        const glm::dvec3 o = s.terrain().chunk_origin(l.key);
        if (o.y > 800.0) overhead = true;
    }
    CHECK(overhead);
    check_balanced(s.terrain(), leaves);
}

TEST_CASE("streaming: fast travel never leaves holes, and memory stays bounded") {
    midgard::JobSystem jobs(4);
    midgard::TerrainStreamer s(body(kArea), {}, &jobs);
    s.set_range(2000.0);
    s.set_cache_limit(300);
    s.update({0.0, 20.0, 0.0}, nullptr, true);  // the first frame waits for its chunks
    REQUIRE(s.stats().complete);
    const size_t first = s.drawn().size();
    CHECK(first > 0);
    // 300 m a frame across the map and back, without waiting.
    size_t most = 0;
    for (int frame = 0; frame < 60; ++frame) {
        const double x =
            -2800.0 + (frame % 30) * 190.0 * (frame < 30 ? 1 : -1) + (frame < 30 ? 0 : 5600.0);
        s.update({x, 30.0, 400.0 * std::sin(frame * 0.3)}, nullptr);
        const midgard::TerrainStreamStats st = s.stats();
        // Kept beyond what's wanted now and what's drawn: the cache, and jobs left behind.
        most = std::max(most, st.cached - std::min(st.cached, st.wanted + st.drawn));
        // Always something whole to draw: the last complete choice until the new one is ready.
        CHECK(st.drawn > 0);
        CHECK(coverage(s.terrain(), s.drawn()) > 0.0);
    }
    CHECK(most <= 300 + 32);  // the limit, and at most the jobs in flight
    // Stopping still: everything wanted arrives.
    for (int frame = 0; frame < 200 && !s.stats().complete; ++frame)
        s.update({1000.0, 30.0, 0.0}, nullptr);
    s.update({1000.0, 30.0, 0.0}, nullptr, true);
    CHECK(s.stats().complete);
    check_balanced(s.terrain(), s.drawn());
}
