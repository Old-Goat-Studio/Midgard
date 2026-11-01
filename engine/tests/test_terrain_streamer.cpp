// SPDX-License-Identifier: Apache-2.0
// Streaming terrain (midgard/terrain_streamer.h): the chunks chosen cover the ground once, no
// two touching chunks are more than a level apart, edges meeting coarser chunks are marked,
// detail follows the camera, a flat-wrap cylinder goes round, and fast travel never leaves
// holes or lets memory grow.
#include <doctest/doctest.h>

#include <algorithm>
#include <map>
#include <tuple>
#include <vector>

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
            if (k.level == t.min_level()) return std::nullopt;
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

TEST_CASE("terrain chunks on a cube-sphere: every face covered once, balanced across face edges") {
    for (const char* text : {R"({"id": "w", "shape": "sphere_outside", "radius": 3000, "seed": 11,
              "origin": [0, -3000, 0]})",
                             R"({"id": "h", "shape": "sphere_inside", "radius": 2500, "seed": 12,
              "origin": [0, 2500, 0]})"}) {
        CAPTURE(text);
        midgard::TerrainStreamer s(body(text), {});
        const midgard::Terrain& t = s.terrain();
        s.set_range(1e9);
        const double side = double(std::int64_t(1) << t.max_level());
        // From near a face edge, near a cube corner, and from the start.
        const glm::dvec3 c = t.body().origin;
        const double r = t.body().shape.radius();
        for (const glm::dvec3 d : {glm::dvec3(0.0, -1.0, 0.94), glm::dvec3(1.0, 1.0, -1.0),
                                   glm::dvec3(0.0, 1.0, 0.0), glm::dvec3(0.0, -1.0, 0.0)}) {
            const glm::dvec3 eye =
                t.ground(c + glm::normalize(d) * r) + t.body().up(c + glm::normalize(d) * r) * 30.0;
            const std::vector<midgard::TerrainLeaf> leaves = s.choose(eye);
            CHECK(coverage(t, leaves) ==
                  doctest::Approx(6.0 * t.roots_u() * t.roots_v() * side * side));
            check_balanced(t, leaves);
        }
    }
}

namespace {

// The world positions along a chunk mesh's edge (0 -u, 1 +u, 2 -v, 3 +v), in grid order.
std::vector<glm::dvec3> edge_points(const midgard::ChunkMesh& m, int edge) {
    constexpr int n = midgard::kChunkQuads;
    std::vector<glm::dvec3> out;
    for (int s = 0; s <= n; ++s) {
        const int i = edge == 0 ? 0 : edge == 1 ? n : s;
        const int j = edge == 2 ? 0 : edge == 3 ? n : s;
        out.push_back(m.origin + glm::dvec3(m.positions[size_t(j) * (n + 1) + size_t(i)]));
    }
    return out;
}

double distance_to_polyline(const glm::dvec3& p, const std::vector<glm::dvec3>& line) {
    double best = 1e30;
    for (size_t i = 0; i + 1 < line.size(); ++i) {
        const glm::dvec3 a = line[i], b = line[i + 1];
        const double t = std::clamp(glm::dot(p - a, b - a) / glm::dot(b - a, b - a), 0.0, 1.0);
        best = std::min(best, glm::length(p - (a + (b - a) * t)));
    }
    return best;
}

// Every chosen chunk's edge lies on its neighbour's edge (same level, or one coarser), as drawn.
void check_no_cracks(const midgard::Terrain& t, const std::vector<midgard::TerrainLeaf>& leaves) {
    auto less = [](const midgard::ChunkKey& a, const midgard::ChunkKey& b) {
        return std::tie(a.face, a.level, a.x, a.y) < std::tie(b.face, b.level, b.x, b.y);
    };
    std::map<midgard::ChunkKey, midgard::TerrainLeaf, decltype(less)> by_key(less);
    for (const midgard::TerrainLeaf& l : leaves) by_key[l.key] = l;
    std::map<midgard::ChunkKey, midgard::ChunkMesh, decltype(less)> meshes(less);
    const auto mesh = [&](const midgard::TerrainLeaf& l) -> const midgard::ChunkMesh& {
        auto it = meshes.find(l.key);
        if (it == meshes.end()) it = meshes.emplace(l.key, t.mesh(l.key, l.coarser)).first;
        return it->second;
    };
    double worst = 0.0;
    int pairs = 0;
    for (const midgard::TerrainLeaf& l : leaves) {
        for (int e = 0; e < 4; ++e) {
            const auto n = t.neighbour(l.key, midgard::ChunkEdge(e));
            if (!n) continue;
            midgard::ChunkKey c = *n;
            while (!by_key.contains(c) && c.level > t.min_level()) c = t.parent(c);
            if (!by_key.contains(c)) continue;  // finer there: it is checked against this one
            // The neighbour's edge facing this chunk.
            int back = -1;
            for (int f = 0; f < 4; ++f) {
                const auto m = t.neighbour(c, midgard::ChunkEdge(f));
                if (!m) continue;
                midgard::ChunkKey a = l.key;
                while (a.level > m->level) a = t.parent(a);
                if (*m == a) back = f;
            }
            REQUIRE(back >= 0);
            const std::vector<glm::dvec3> theirs = edge_points(mesh(by_key[c]), back);
            for (const glm::dvec3& p : edge_points(mesh(l), e))
                worst = std::max(worst, distance_to_polyline(p, theirs));
            ++pairs;
        }
    }
    CAPTURE(pairs);
    CHECK(worst < 1e-3);
}

}  // namespace

TEST_CASE("terrain chunks as drawn meet with no cracks, on every shape") {
    const auto run = [](const char* text, const std::vector<glm::dvec3>& directions) {
        CAPTURE(text);
        midgard::TerrainStreamer s(body(text), {});
        const midgard::Terrain& t = s.terrain();
        const glm::dvec3 c = t.body().origin;
        const double r = t.body().shape.kind() == midgard::Shape::Kind::BoundedArea
                             ? 0.0
                             : t.body().shape.radius();
        for (const glm::dvec3& d : directions) {
            CAPTURE(d);
            const glm::dvec3 on = r > 0.0 ? c + glm::normalize(d) * r : d;
            const glm::dvec3 eye = t.ground(on) + t.body().up(on) * 40.0;
            check_no_cracks(t, s.choose(eye));
        }
    };
    run(kArea, {glm::dvec3(0.0), glm::dvec3(1200.0, 0.0, -900.0)});
    run(R"({"id": "h", "shape": "sphere_inside", "radius": 2500, "seed": 12,
            "origin": [0, 2500, 0]})",
        {glm::dvec3(0.0, -1.0, 0.94), glm::dvec3(0.3, -1.0, 0.2)});
    run(R"({"id": "w", "shape": "sphere_outside", "radius": 3000, "seed": 11,
            "origin": [0, -3000, 0]})",
        {glm::dvec3(1.0, 1.0, -1.0), glm::dvec3(1.0, 0.86, 0.12)});
}

TEST_CASE("a sphere chunk's up is encoded in its axis's frame, clear of the octahedral fold") {
    for (const char* text :
         {R"({"id": "w", "shape": "sphere_outside", "radius": 3000, "seed": 11})",
          R"({"id": "h", "shape": "sphere_inside", "radius": 2500, "seed": 12})"}) {
        CAPTURE(text);
        const midgard::Terrain t(body(text), {});
        for (int f = 0; f < 6; ++f)
            for (int y = 0; y < t.roots_v(); ++y)
                for (int x = 0; x < t.roots_u(); ++x) {
                    const midgard::ChunkKey k{f, 0, x, y};
                    const int frame = midgard::up_frame(t.body().up(t.chunk_origin(k)));
                    const midgard::ChunkMesh m = t.mesh(k);
                    for (const glm::vec3& up : m.ups) {
                        const glm::vec3 local = midgard::in_up_frame(up, frame);
                        CHECK(local.z > 0.5f);  // the upper half: no fold anywhere in the chunk
                        CHECK(glm::length(local) == doctest::Approx(1.0f));
                    }
                }
    }
}

TEST_CASE("the far side of a hollow world comes in a few coarse chunks, at every size") {
    for (const double radius : {1000.0, 2500.0, 8000.0, 20000.0}) {
        CAPTURE(radius);
        const std::string text = R"({"id": "h", "shape": "sphere_inside", "radius": )" +
                                 std::to_string(radius) + R"(, "seed": 12, "origin": [0, )" +
                                 std::to_string(radius) + ", 0]}";
        midgard::TerrainStreamer s(body(text.c_str()), {});
        const midgard::Terrain& t = s.terrain();
        CHECK(t.chunks_u(t.min_level()) == 1);  // the coarsest: one chunk a face
        const glm::dvec3 eye = t.ground(glm::dvec3(0.0)) + glm::dvec3(0.0, 2.0, 0.0);
        const std::vector<midgard::TerrainLeaf> leaves = s.choose(eye);
        // All of it is overhead, so all of it is drawn: every face, once, balanced.
        const double side = std::ldexp(1.0, t.max_level() - t.min_level());
        CHECK(coverage(t, leaves) == doctest::Approx(6.0 * side * side));
        check_balanced(t, leaves);
        int far = 0, coarsest_far = 99;
        for (const midgard::TerrainLeaf& l : leaves)
            if (glm::length(t.chunk_origin(l.key) - eye) > radius) {
                ++far;
                coarsest_far = std::min(coarsest_far, l.key.level);
            }
        MESSAGE("radius ", radius, ": ", leaves.size(), " chunks, ", far,
                " of them more than a radius away, the coarsest at level ", coarsest_far);
        // However big the world, the far side stays a few coarse chunks, and the whole view a
        // bounded number (the frame's cost: render.terrain_hollow_phone in midgard_bench).
        CHECK(far <= 200);
        CHECK(leaves.size() <= 1000);
    }
}
