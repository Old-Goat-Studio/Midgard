// SPDX-License-Identifier: Apache-2.0
// Terrain (midgard/terrain.h): heights and chunk hashes against the shared table
// (schema/tests/terrain_chunks.json, from tools/reference/terrain.py), no cracks where detail
// levels meet or across a cylinder's join, and the settings the cook refuses.
#include <doctest/doctest.h>

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "midgard/terrain.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

json table() {
    std::ifstream in(fs::path(MIDGARD_SCHEMA_DIR) / "tests" / "terrain_chunks.json");
    REQUIRE(in);
    return json::parse(in);
}

midgard::Terrain terrain(const json& t, const std::string& body, const std::string& settings) {
    json b = t.at("bodies").at(body);
    b["id"] = body;
    return midgard::Terrain(midgard::body_from_json(b),
                            midgard::terrain_from_json(t.at("settings").at(settings)));
}

std::string hex(std::uint64_t v) {
    char out[17];
    std::snprintf(out, sizeof(out), "%016" PRIx64, v);
    return out;
}

midgard::Terrain area() {
    return midgard::Terrain(
        midgard::body_from_json(json::parse(
            R"({"id": "a", "shape": "bounded_area", "width": 3000, "length": 2000, "seed": 7})")),
        {});
}

midgard::Terrain habitat() {
    return midgard::Terrain(midgard::body_from_json(json::parse(
                                R"({"id": "h", "shape": "curved_cylinder", "radius": 1000,
                                    "length": 5000, "seed": 9})")),
                            {});
}

constexpr int kN = midgard::kChunkQuads;

glm::dvec3 world(const midgard::ChunkMesh& m, int i, int j) {
    return m.origin + glm::dvec3(m.positions[size_t(j) * (kN + 1) + size_t(i)]);
}

}  // namespace

TEST_CASE("terrain heights and chunk hashes match the shared table") {
    const json t = table();
    for (const auto& [name, layout] : t.at("layout").items()) {
        const midgard::Terrain tr = terrain(t, name, "default");
        CHECK(tr.roots_u() == layout[0].get<int>());
        CHECK(tr.roots_v() == layout[1].get<int>());
        CHECK(tr.max_level() == layout[2].get<int>());
    }
    int hashes = 0, heights = 0;
    for (const json& c : t.at("cases")) {
        CAPTURE(c.dump());
        const midgard::Terrain tr =
            terrain(t, c.at("body").get<std::string>(), c.at("settings").get<std::string>());
        if (c.contains("hash")) {
            const json& k = c.at("key");
            const midgard::ChunkKey key{k[0].get<int>(), k[1].get<int>(), k[2].get<int>(),
                                        k[3].get<int>()};
            CHECK(tr.valid(key));
            CHECK(hex(tr.chunk_hash(key)) == c.at("hash").get<std::string>());
            ++hashes;
        } else {
            CHECK(tr.height(0, c.at("u").get<double>(), c.at("v").get<double>()) ==
                  c.at("height").get<double>());
            ++heights;
        }
    }
    CHECK(hashes == 24);
    CHECK(heights == 48);
    CHECK(hex(midgard::terrain_fingerprint()) == t.at("fingerprint").get<std::string>());
    CHECK(midgard::terrain_fingerprint() == midgard::kTerrainFingerprint);
}

TEST_CASE("a chunk's mesh: its grid, its heights, and normals that point up") {
    const midgard::Terrain tr = area();
    const midgard::ChunkMesh m = tr.mesh({0, 2, 5, 3});
    constexpr size_t side = kN + 1;
    REQUIRE(m.positions.size() == side * side);
    CHECK(m.indices.size() == size_t(kN) * kN * 6);
    for (size_t i = 0; i < m.positions.size(); ++i) {
        CHECK(double(m.positions[i].y) == doctest::Approx(m.heights[i]).epsilon(1e-4));
        CHECK(m.normals[i].y > 0.2f);  // up, more or less (hills, not cliffs)
    }
    // Triangles turn counter-clockwise seen from above: their normals point up.
    for (size_t t = 0; t < m.indices.size(); t += 3) {
        const glm::vec3 a = m.positions[m.indices[t]], b = m.positions[m.indices[t + 1]],
                        c = m.positions[m.indices[t + 2]];
        CHECK(glm::cross(b - a, c - a).y > 0.0f);
    }
}

TEST_CASE("no cracks: neighbours share edge points exactly, at one level and across two") {
    const midgard::Terrain tr = area();
    // Same level: the east edge of one is the west edge of the next, point for point.
    const midgard::ChunkMesh a = tr.mesh({0, 2, 4, 3}), b = tr.mesh({0, 2, 5, 3});
    for (int j = 0; j <= kN; ++j) {
        CHECK(a.heights[size_t(j) * (kN + 1) + kN] == b.heights[size_t(j) * (kN + 1)]);
        CHECK(glm::length(world(a, kN, j) - world(b, 0, j)) < 1e-3);
        CHECK(glm::length(glm::dvec3(a.normals[size_t(j) * (kN + 1) + kN] -
                                     b.normals[size_t(j) * (kN + 1)])) < 1e-4);
    }
    // A fine chunk beside a coarse one: its edge points lie on the coarse edge, with no gap.
    // The fine chunk (level 3, x 10, y 6) is the lower half along v of the part of the face
    // east of the coarse chunk (level 2, x 4, y 3): fine rows 0..n are coarse rows 0..n/2.
    const midgard::ChunkMesh f = tr.mesh({0, 3, 10, 6}, {true, false, false, false});
    const midgard::ChunkMesh c = tr.mesh({0, 2, 4, 3});
    for (int j = 0; j <= kN; ++j) {
        const int cj = j / 2;
        const glm::dvec3 lo = world(c, kN, cj), hi = world(c, kN, std::min(cj + 1, kN));
        const glm::dvec3 on_edge = j % 2 == 0 ? lo : (lo + hi) * 0.5;
        CHECK(glm::length(world(f, 0, j) - on_edge) < 1e-3);
    }
}

TEST_CASE("a curved cylinder's join: the last chunk round meets the first exactly") {
    const midgard::Terrain tr = habitat();
    for (int level : {0, 2}) {
        const int last = (tr.roots_u() << level) - 1;
        const midgard::ChunkKey east{0, level, last, 1}, first{0, level, 0, 1};
        REQUIRE(tr.neighbour(east, midgard::kEast) == first);
        REQUIRE(tr.neighbour(first, midgard::kWest) == east);
        const midgard::ChunkMesh a = tr.mesh(east), b = tr.mesh(first);
        for (int j = 0; j <= kN; ++j) {
            CHECK(a.heights[size_t(j) * (kN + 1) + kN] == b.heights[size_t(j) * (kN + 1)]);
            CHECK(glm::length(world(a, kN, j) - world(b, 0, j)) < 1e-3);
        }
    }
    // A bounded area doesn't wrap: nothing is past its edges.
    CHECK_FALSE(area().neighbour({0, 0, 0, 0}, midgard::kWest).has_value());
}

TEST_CASE("terrain shapes the edges: ocean round a bounded area, walls at a cylinder's ends") {
    const midgard::Terrain a = area();
    CHECK(a.height(0, 0.0, 0.5) == doctest::Approx(-30.0));  // the sea floor at the edge
    CHECK(a.height(0, 0.5, 0.0) == doctest::Approx(-30.0));
    const midgard::Terrain h = habitat();
    CHECK(h.height(0, 0.3, 0.0) == doctest::Approx(120.0));  // up to the wall
    CHECK(h.height(0, 0.3, 1.0) == doctest::Approx(120.0));
}

TEST_CASE("terrain settings from JSON, and those the cook refuses") {
    const midgard::TerrainSettings t = midgard::terrain_from_json(json::parse(
        R"({"seed": 5, "height_scale": 90, "octaves": 4, "wall": "barrier", "spacing": 1})"));
    CHECK(t.seed == 5u);
    CHECK(t.height_scale == 90.0);
    CHECK(t.wall == midgard::WallLook::Barrier);
    CHECK(midgard::terrain_problems(t).empty());
    const json back = midgard::terrain_to_json(t);
    CHECK(back.at("wall") == "barrier");
    CHECK(midgard::terrain_from_json(back).octaves == 4);
    CHECK_THROWS(midgard::terrain_from_json(json::parse(R"({"wall": "hedge"})")));
    CHECK_THROWS(midgard::terrain_from_json(json::parse(R"({"octaves": 2.5})")));
    midgard::TerrainSettings bad;
    bad.octaves = 20;
    bad.spacing = 0.01;
    bad.feature_size = 0.0;
    CHECK(midgard::terrain_problems(bad).size() == 3);
    // A finer spacing means more levels.
    midgard::TerrainSettings fine;
    fine.spacing = 0.5;
    const midgard::Body b = midgard::body_from_json(
        json::parse(R"({"id": "a", "shape": "bounded_area", "width": 3000, "length": 2000})"));
    CHECK(midgard::Terrain(b, fine).max_level() == midgard::Terrain(b, {}).max_level() + 2);
}

TEST_CASE("water over a chunk: at sea level, its depth at each point, none over dry land") {
    const midgard::Terrain tr = area();
    // At the map's edge the sea floor is 30 m down: water everywhere.
    const auto sea = tr.water_mesh({0, 2, 0, 3});
    REQUIRE(sea.has_value());
    const midgard::ChunkMesh ground = tr.mesh({0, 2, 0, 3});
    for (size_t i = 0; i < sea->positions.size(); ++i) {
        const glm::dvec3 p = sea->origin + glm::dvec3(sea->positions[i]);
        CHECK(p.y == doctest::Approx(0.0).epsilon(1e-6));  // the sea level
        CHECK(sea->heights[i] == doctest::Approx(0.0 - ground.heights[i]));
        CHECK(sea->normals[i].y == doctest::Approx(1.0f));
    }
    CHECK(sea->heights[0] == doctest::Approx(30.0));  // the corner: the ocean's depth
    // Somewhere the ground is above the sea all over: no water.
    midgard::TerrainSettings high;
    high.base = 500.0;
    high.ocean_band = 0.0;
    const midgard::Terrain dry(area().body(), high);
    CHECK_FALSE(dry.water_mesh({0, 2, 5, 3}).has_value());
}
