// SPDX-License-Identifier: Apache-2.0
// Terrain (midgard/terrain.h): heights and chunk hashes against the shared table
// (schema/tests/terrain_chunks.json, from tools/reference/terrain.py), no cracks where detail
// levels meet or across a cylinder's join, and the settings the cook refuses.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

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

TEST_CASE("a chunk's material data: up at each point, moisture and detail in range") {
    const midgard::ChunkMesh m = habitat().mesh({0, 1, 3, 2});
    REQUIRE(m.ups.size() == m.positions.size());
    REQUIRE(m.biome.size() == m.positions.size());
    for (size_t i = 0; i < m.ups.size(); ++i) {
        const glm::dvec3 p = m.origin + glm::dvec3(m.positions[i]);
        CHECK(glm::length(glm::dvec3(m.ups[i]) - habitat().body().up(p)) < 1e-5);
        CHECK(m.biome[i].x >= 0.0f);
        CHECK(m.biome[i].x <= 1.0f);
        CHECK(std::abs(m.biome[i].y) <= 1.0f);
    }
    // The same point from two chunks: the same biome (it comes from the surface position).
    const glm::vec4 a = habitat().biome_at(0, 0.0, 0.4), b = habitat().biome_at(0, 1.0, 0.4);
    CHECK(a == b);
    midgard::TerrainSettings t;
    t.snow_line = 40.0;
    CHECK(midgard::terrain_from_json(midgard::terrain_to_json(t)).snow_line == 40.0);
}

TEST_CASE("terrain stamps match the shared table") {
    std::ifstream in(fs::path(MIDGARD_SCHEMA_DIR) / "tests" / "terrain_stamps.json");
    REQUIRE(in);
    const json t = json::parse(in);
    std::vector<midgard::TerrainStamp> stamps;
    for (const json& s : t.at("stamps")) stamps.push_back(midgard::stamp_from_json(s));
    int checked = 0;
    for (const json& c : t.at("cases")) {
        CAPTURE(c.dump());
        json b = t.at("bodies").at(c.at("body").get<std::string>());
        b["id"] = c.at("body");
        const midgard::Terrain tr(midgard::body_from_json(b), {}, stamps);
        if (c.contains("hash")) {
            const json& k = c.at("key");
            CHECK(hex(tr.chunk_hash({k[0].get<int>(), k[1].get<int>(), k[2].get<int>(),
                                     k[3].get<int>()})) == c.at("hash").get<std::string>());
        } else {
            CHECK(tr.height(0, c.at("u").get<double>(), c.at("v").get<double>()) ==
                  c.at("height").get<double>());
        }
        ++checked;
    }
    CHECK(checked == 30);
}

TEST_CASE("beaches match the shared table") {
    std::ifstream in(fs::path(MIDGARD_SCHEMA_DIR) / "tests" / "terrain_beaches.json");
    REQUIRE(in);
    const json t = json::parse(in);
    const midgard::TerrainSettings settings = midgard::terrain_from_json(t.at("settings"));
    CHECK(settings.beach == 40.0);
    int checked = 0;
    for (const json& c : t.at("cases")) {
        CAPTURE(c.dump());
        json b = t.at("bodies").at(c.at("body").get<std::string>());
        b["id"] = c.at("body");
        const midgard::Terrain tr(midgard::body_from_json(b), settings);
        if (c.contains("hash")) {
            const json& k = c.at("key");
            CHECK(hex(tr.chunk_hash({k[0].get<int>(), k[1].get<int>(), k[2].get<int>(),
                                     k[3].get<int>()})) == c.at("hash").get<std::string>());
        } else {
            CHECK(tr.height(0, c.at("u").get<double>(), c.at("v").get<double>()) ==
                  c.at("height").get<double>());
        }
        ++checked;
    }
    CHECK(checked == 36);
}

TEST_CASE("a beach: a gentle shelf where the ground meets the sea, however steep the land") {
    const midgard::Body body = midgard::body_from_json(json::parse(
        R"({"id": "a", "shape": "bounded_area", "width": 3000, "length": 2000, "seed": 7})"));
    // How steep the ground is where it crosses the sea's level (sorted).
    const auto shore_slopes = [&](const json& settings) {
        const midgard::Terrain t(body, midgard::terrain_from_json(settings));
        std::vector<double> out;
        const double su = 1.0 / 3000.0, sv = 1.0 / 2000.0;  // a metre each way
        for (int j = 0; j < 120; ++j)
            for (int i = 0; i < 120; ++i) {
                const double u = 0.1 + 0.8 * i / 120.0, v = 0.1 + 0.8 * j / 120.0;
                if (std::abs(t.height(0, u, v)) > 0.5) continue;  // (only at the water's edge)
                const double du = t.height(0, u + su, v) - t.height(0, u - su, v);
                const double dv = t.height(0, u, v + sv) - t.height(0, u, v - sv);
                out.push_back(std::sqrt(du * du + dv * dv) / 2.0);
            }
        std::sort(out.begin(), out.end());
        return out;
    };
    const auto median = [](const std::vector<double>& v) { return v[v.size() / 2]; };
    const auto most = [](const std::vector<double>& v) { return v[v.size() * 9 / 10]; };
    // Rolling hills (80 m, 400 m across): from 1 in 3 at the water to under 1 in 14 (the
    // shelf rises 1 in 16); and more ground near the water's level.
    json hills = json::parse(R"({"base": 0, "height_scale": 80, "feature_size": 400,
                                 "octaves": 5, "ocean_band": 100})");
    const std::vector<double> before = shore_slopes(hills);
    hills["beach"] = 40;
    const std::vector<double> after = shore_slopes(hills);
    REQUIRE(before.size() > 100);
    CHECK(after.size() > 2 * before.size());
    CHECK(median(before) > 0.3);
    CHECK(median(after) < 0.07);
    CHECK(most(after) < 0.16);  // (nine in ten under 1 in 6: no hollow is left to hold a pond)
    // Steep, rough land (200 m hills 120 m across): still five times gentler at the water (the
    // shelf shapes only the ground within 16 m of the sea's level).
    json rough = json::parse(R"({"base": -5, "height_scale": 200, "feature_size": 120,
                                 "octaves": 4, "ocean_band": 100})");
    const double steep = median(shore_slopes(rough));
    rough["beach"] = 40;
    CHECK(median(shore_slopes(rough)) < steep / 5.0);
    // Far from the sea, the land is as it was; and settings without a beach save as before.
    const midgard::Terrain bare(body, midgard::terrain_from_json(json::parse(
                                          R"({"base": 0, "height_scale": 80, "feature_size": 400,
                                              "octaves": 5, "ocean_band": 100})")));
    const midgard::Terrain beach(body, midgard::terrain_from_json(hills));
    int far = 0;
    for (int i = 0; i < 400; ++i) {
        const double u = 0.1 + 0.04 * (i % 20), v = 0.1 + 0.04 * (i / 20);
        if (std::abs(bare.base_height(0, u, v)) < 60.0) continue;
        CHECK(beach.height(0, u, v) == bare.height(0, u, v));
        ++far;
    }
    CHECK(far > 0);
    // No ponds behind a beach: ground the world made above the sea stays above it.
    int dry = 0, sunk = 0;
    for (int j = 0; j < 150; ++j)
        for (int i = 0; i < 150; ++i) {
            const double u = 0.1 + 0.8 * i / 150.0, v = 0.1 + 0.8 * j / 150.0;
            if (beach.generated_height(0, u, v) <= 0.0) continue;
            ++dry;
            if (beach.height(0, u, v) < 0.0) ++sunk;
        }
    CHECK(dry > 1000);
    CHECK(sunk == 0);
    // Gentle land near the sea (a flat meadow 2 m up) stays as it was: no beach is cut in it.
    const midgard::Terrain meadow_bare(body, midgard::terrain_from_json(json::parse(
                                                 R"({"base": 2, "height_scale": 1,
                                                     "feature_size": 400, "ocean_band": 0})")));
    const midgard::Terrain meadow(body, midgard::terrain_from_json(json::parse(
                                            R"({"base": 2, "height_scale": 1, "feature_size": 400,
                                                "ocean_band": 0, "beach": 40})")));
    for (const double u : {0.3, 0.5, 0.7})
        CHECK(meadow.height(0, u, 0.5) ==
              doctest::Approx(meadow_bare.height(0, u, 0.5)).epsilon(0.02));
    CHECK(midgard::terrain_to_json(beach.settings()).at("beach") == 40.0);
    CHECK_FALSE(midgard::terrain_to_json(bare.settings()).contains("beach"));
    // From 0 to 500 m.
    midgard::TerrainSettings wrong = beach.settings();
    wrong.beach = -1.0;
    CHECK_FALSE(midgard::terrain_problems(wrong).empty());
    wrong.beach = 501.0;
    CHECK_FALSE(midgard::terrain_problems(wrong).empty());
}

TEST_CASE("sculpting: raise, lower, flatten, smooth and paint, and the strokes' reach") {
    const midgard::Terrain plain = area();
    const auto with = [&](const char* text) {
        return midgard::Terrain(plain.body(), plain.settings(),
                                {midgard::stamp_from_json(json::parse(text))});
    };
    const double u = 0.4, v = 0.45, base = plain.height(0, u, v);
    // Raise: the full height on the stroke's line, nothing beyond its radius (3000 m wide: 20 m
    // is 0.00667 of u).
    const auto raised = with(R"({"tool": "raise", "radius": 20, "strength": 5,
                                 "points": [[0.4, 0.45]]})");
    CHECK(raised.height(0, u, v) == doctest::Approx(base + 5.0));
    CHECK(raised.height(0, u + 0.008, v) == plain.height(0, u + 0.008, v));
    CHECK(raised.height(0, u + 0.003, v) > plain.height(0, u + 0.003, v));
    // Lower, along a line: the same depth anywhere on it.
    const auto ditch = with(R"({"tool": "lower", "radius": 10, "strength": 3,
                                "points": [[0.3, 0.5], [0.35, 0.5]]})");
    for (double x : {0.3, 0.31, 0.33, 0.35})
        CHECK(ditch.height(0, x, 0.5) == doctest::Approx(plain.height(0, x, 0.5) - 3.0));
    // Flatten: all the way to its height at full strength.
    const auto flat = with(R"({"tool": "flatten", "radius": 30, "strength": 1, "height": 12,
                               "points": [[0.4, 0.45]]})");
    CHECK(flat.height(0, u, v) == doctest::Approx(12.0));
    // Smooth: closer to the average round it.
    const auto smooth = with(R"({"tool": "smooth", "radius": 40, "strength": 1,
                                 "points": [[0.4, 0.45]]})");
    const double du = 20.0 / 3000.0, dv = 20.0 / 2000.0;  // (3000 m wide, 2000 long)
    const double average = (plain.height(0, u + du, v) + plain.height(0, u - du, v) +
                            plain.height(0, u, v + dv) + plain.height(0, u, v - dv)) /
                           4.0;
    CHECK(smooth.height(0, u, v) == doctest::Approx(average));
    // Paint: rock and lush where it's painted, the heights left alone.
    const auto rock = with(R"({"tool": "paint", "paint": "rock", "radius": 10, "strength": 1,
                               "points": [[0.4, 0.45]]})");
    CHECK(rock.height(0, u, v) == base);
    CHECK(rock.biome_at(0, u, v).z == doctest::Approx(1.0f));
    CHECK(plain.biome_at(0, u, v).z == 0.0f);
    const auto lush = with(R"({"tool": "paint", "paint": "lush", "radius": 10, "strength": 1,
                               "points": [[0.4, 0.45]]})");
    CHECK(lush.biome_at(0, u, v).x == doctest::Approx(1.0f));
    // The world type's paint layers (6.0b): 1 the first, -1 the second; each the last painted
    // wins over sand and rock (and they over it), and clear takes it away.
    const auto path = with(R"({"tool": "paint", "paint": "paint_1", "radius": 10, "strength": 1,
                               "points": [[0.4, 0.45]]})");
    CHECK(path.biome_at(0, u, v).w == doctest::Approx(1.0f));
    CHECK(plain.biome_at(0, u, v).w == 0.0f);
    const auto paving = with(R"({"tool": "paint", "paint": "paint_2", "radius": 10,
                                 "strength": 1, "points": [[0.4, 0.45]]})");
    CHECK(paving.biome_at(0, u, v).w == doctest::Approx(-1.0f));
    const auto both = [&](const char* first, const char* then) {
        const auto stroke = [](const char* paint) {
            return midgard::stamp_from_json(json{{"tool", "paint"},
                                                 {"paint", paint},
                                                 {"radius", 10},
                                                 {"strength", 1},
                                                 {"points", {{0.4, 0.45}}}});
        };
        const midgard::Terrain t(plain.body(), plain.settings(), {stroke(first), stroke(then)});
        return t.biome_at(0, u, v);
    };
    CHECK(both("rock", "paint_1").z == doctest::Approx(0.0f));
    CHECK(both("rock", "paint_1").w == doctest::Approx(1.0f));
    CHECK(both("paint_1", "sand").w == doctest::Approx(0.0f));
    CHECK(both("paint_2", "clear").w == doctest::Approx(0.0f));
    CHECK(std::string(midgard::paint_layer_name(midgard::PaintLayer::Paint2)) == "paint_2");
    // A stroke across a cylinder's join reaches both sides.
    const midgard::Terrain h = habitat();
    const midgard::Terrain hs(h.body(), h.settings(),
                              {midgard::stamp_from_json(json::parse(
                                  R"({"tool": "raise", "radius": 30, "strength": 4,
                                      "points": [[0.999, 0.4], [0.001, 0.4]]})"))});
    CHECK(hs.height(0, 0.0, 0.4) == doctest::Approx(h.height(0, 0.0, 0.4) + 4.0));
    CHECK(hs.height(0, 0.9995, 0.4) == doctest::Approx(h.height(0, 0.9995, 0.4) + 4.0));
}

TEST_CASE("terrain stamps: JSON, what the cook refuses, chunks they touch, and rays") {
    const midgard::TerrainStamp s = midgard::stamp_from_json(json::parse(
        R"({"tool": "flatten", "radius": 8, "strength": 0.5, "height": 3, "face": 0,
            "points": [[0.1, 0.2], [0.11, 0.2]]})"));
    CHECK(midgard::stamp_problems(s).empty());
    CHECK(midgard::stamp_from_json(midgard::stamp_to_json(s)) == s);
    CHECK_THROWS(midgard::stamp_from_json(json::parse(R"({"tool": "dig", "points": [[0, 0]]})")));
    CHECK_THROWS(midgard::stamp_from_json(json::parse(R"({"tool": "raise"})")));
    midgard::TerrainStamp bad = s;
    bad.radius = 0.0;
    bad.strength = 2.0;  // flatten is 0 to 1
    bad.points.clear();
    CHECK(midgard::stamp_problems(bad).size() == 3);
    // Which chunks a stroke touches.
    const midgard::Terrain t = area();
    CHECK(t.touches({0, 0, 0, 0}, s));
    CHECK_FALSE(t.touches({0, 0, 2, 1}, s));
    // A ray from above hits the ground under it.
    const glm::dvec3 g = t.ground({100.0, 0.0, -50.0});
    const auto hit =
        t.raycast(g + glm::dvec3(10.0, 200.0, 0.0), glm::dvec3(-10.0, -200.0, 0.0), 1000.0);
    REQUIRE(hit.has_value());
    CHECK(glm::length(*hit - g) < 0.05);
    CHECK_FALSE(t.raycast(g + glm::dvec3(0, 50, 0), {0, 1, 0}, 1000.0).has_value());  // upwards
}

namespace {

json sphere_table() {
    std::ifstream in(fs::path(MIDGARD_SCHEMA_DIR) / "tests" / "terrain_spheres.json");
    REQUIRE(in);
    return json::parse(in);
}

midgard::Terrain sphere(const json& t, const std::string& name) {
    json b = t.at("bodies").at(name);
    b["id"] = name;
    return midgard::Terrain(midgard::body_from_json(b), {});
}

// The grid point at s (0..n) along a chunk's edge, in the face's increasing u or v.
std::pair<int, int> edge_point(int edge, int s) {
    switch (edge) {
        case midgard::kWest:
            return {0, s};
        case midgard::kEast:
            return {kN, s};
        case midgard::kSouth:
            return {s, 0};
        default:
            return {s, kN};
    }
}

size_t at(int i, int j) {
    return size_t(j) * (kN + 1) + size_t(i);
}

}  // namespace

TEST_CASE("cube-sphere terrain: heights and chunk hashes on every face match the shared table") {
    const json t = sphere_table();
    for (const auto& [name, layout] : t.at("layout").items()) {
        const midgard::Terrain tr = sphere(t, name);
        CHECK(tr.roots_u() == layout[0].get<int>());
        CHECK(tr.roots_v() == layout[1].get<int>());
        CHECK(tr.max_level() == layout[2].get<int>());
    }
    int hashes = 0, heights = 0;
    for (const json& c : t.at("cases")) {
        CAPTURE(c.dump());
        const midgard::Terrain tr = sphere(t, c.at("body").get<std::string>());
        if (c.contains("hash")) {
            const json& k = c.at("key");
            const midgard::ChunkKey key{k[0].get<int>(), k[1].get<int>(), k[2].get<int>(),
                                        k[3].get<int>()};
            CHECK(tr.valid(key));
            CHECK(hex(tr.chunk_hash(key)) == c.at("hash").get<std::string>());
            ++hashes;
        } else {
            CHECK(tr.height(c.at("face").get<int>(), c.at("u").get<double>(),
                            c.at("v").get<double>()) == c.at("height").get<double>());
            ++heights;
        }
    }
    CHECK(hashes == 144);
    CHECK(heights == 48);
}

TEST_CASE("cube-sphere terrain: no cracks on any of the 12 face edges, at every level") {
    const json t = sphere_table();
    for (const std::string name : {"world", "hollow"}) {
        CAPTURE(name);
        const midgard::Terrain tr = sphere(t, name);
        int checked = 0;
        for (int level = tr.min_level(); level <= tr.max_level(); ++level) {
            const int across = int(tr.chunks_u(level));
            // Every chunk along the edge up to level 2; then the first, a middle one, the last.
            std::vector<int> along;
            if (level <= 2) {
                for (int a = 0; a < across; ++a) along.push_back(a);
            } else {
                along = {0, across / 2 - 1, across - 1};
            }
            for (int f = 0; f < 6; ++f)
                for (int e = 0; e < 4; ++e) {
                    const midgard::CubeLink link = midgard::cube_link(f, e);
                    for (const int a : along) {
                        CAPTURE(level);
                        CAPTURE(f);
                        CAPTURE(e);
                        CAPTURE(a);
                        const int in = e % 2 == 0 ? 0 : across - 1;
                        const midgard::ChunkKey k = e < 2 ? midgard::ChunkKey{f, level, in, a}
                                                          : midgard::ChunkKey{f, level, a, in};
                        const std::optional<midgard::ChunkKey> n =
                            tr.neighbour(k, midgard::ChunkEdge(e));
                        REQUIRE(n.has_value());
                        CHECK(n->face == link.face);
                        CHECK(n->level == level);
                        REQUIRE(tr.valid(*n));
                        CHECK(tr.neighbour(*n, midgard::ChunkEdge(link.edge)) == k);  // and back
                        const midgard::ChunkMesh m = tr.mesh(k), o = tr.mesh(*n);
                        for (int s = 0; s <= kN; ++s) {
                            const auto [i, j] = edge_point(e, s);
                            const auto [oi, oj] = edge_point(link.edge, link.reversed ? kN - s : s);
                            CHECK(m.heights[at(i, j)] == o.heights[at(oi, oj)]);
                            CHECK(glm::length(world(m, i, j) - world(o, oi, oj)) < 1e-3);
                            CHECK(glm::length(glm::dvec3(m.normals[at(i, j)] -
                                                         o.normals[at(oi, oj)])) < 1e-4);
                        }
                        ++checked;
                    }
                }
        }
        CHECK(checked > 24 * 3);
    }
}

TEST_CASE("cube-sphere terrain: the 8 corners are one point from all three faces") {
    const json t = sphere_table();
    for (const std::string name : {"world", "hollow"}) {
        CAPTURE(name);
        const midgard::Terrain tr = sphere(t, name);
        for (int level = tr.min_level(); level <= tr.max_level(); ++level) {
            CAPTURE(level);
            const int last = int(tr.chunks_u(level)) - 1;
            // Corner points (rounded to 10 cm, to group them) and the heights found there.
            std::map<std::vector<long long>, std::vector<double>> corners;
            std::map<std::vector<long long>, std::vector<glm::dvec3>> points;
            for (int f = 0; f < 6; ++f)
                for (const auto& [cx, cy] :
                     {std::pair{0, 0}, std::pair{1, 0}, std::pair{0, 1}, std::pair{1, 1}}) {
                    const midgard::ChunkMesh m = tr.mesh({f, level, cx * last, cy * last});
                    const int i = cx * kN, j = cy * kN;
                    const glm::dvec3 p = world(m, i, j);
                    const std::vector<long long> key{std::llround(p.x * 10.0),
                                                     std::llround(p.y * 10.0),
                                                     std::llround(p.z * 10.0)};
                    corners[key].push_back(m.heights[at(i, j)]);
                    points[key].push_back(p);
                }
            CHECK(corners.size() == 8);
            for (const auto& [where, heights] : corners) {
                CHECK(heights.size() == 3);
                for (const double h : heights) CHECK(h == heights.front());
                for (const glm::dvec3& p : points[where])
                    CHECK(glm::length(p - points[where].front()) < 1e-3);
            }
        }
    }
}

TEST_CASE("cube-sphere terrain: a fine chunk meets a coarse one across a face edge") {
    const json t = sphere_table();
    for (const std::string name : {"world", "hollow"}) {
        const midgard::Terrain tr = sphere(t, name);
        const int across = tr.roots_u() << 2;  // chunks a side at level 2
        // Every face's four edges: a fine chunk (level 2) by the edge, and across it a coarse
        // one (level 1).
        for (int f = 0; f < 6; ++f)
            for (int e = 0; e < 4; ++e) {
                CAPTURE(name);
                CAPTURE(f);
                CAPTURE(e);
                const int in = e % 2 == 0 ? 0 : across - 1;
                const midgard::ChunkKey fine =
                    e < 2 ? midgard::ChunkKey{f, 2, in, 5} : midgard::ChunkKey{f, 2, 6, in};
                const midgard::ChunkKey coarse =
                    tr.parent(*tr.neighbour(fine, midgard::ChunkEdge(e)));
                const midgard::CubeLink link = midgard::cube_link(f, e);
                std::array<bool, 4> flags{};
                flags[size_t(e)] = true;
                const midgard::ChunkMesh m = tr.mesh(fine, flags), c = tr.mesh(coarse);
                // Positions along the shared edge in level-2 grid steps from the face's start.
                const int total = across * kN;
                const int fine_start = (e < 2 ? fine.y : fine.x) * kN;
                const int coarse_start = (link.edge < 2 ? coarse.y : coarse.x) * 2 * kN;
                const auto coarse_point = [&](int ci) {
                    const auto [i, j] = edge_point(link.edge, ci);
                    return world(c, i, j);
                };
                for (int s = 0; s <= kN; ++s) {
                    CAPTURE(s);
                    const int p = fine_start + s;
                    const int local = (link.reversed ? total - p : p) - coarse_start;
                    REQUIRE(local >= 0);
                    REQUIRE(local <= 2 * kN);
                    const glm::dvec3 on_edge =
                        local % 2 == 0
                            ? coarse_point(local / 2)
                            : (coarse_point(local / 2) + coarse_point(local / 2 + 1)) * 0.5;
                    const auto [i, j] = edge_point(e, s);
                    CHECK(glm::length(world(m, i, j) - on_edge) < 1e-3);
                }
            }
    }
}

TEST_CASE("cube-sphere terrain: normals and triangles face up on every face, outside and in") {
    const json t = sphere_table();
    for (const std::string name : {"world", "hollow"}) {
        const midgard::Terrain tr = sphere(t, name);
        for (int f = 0; f < 6; ++f) {
            CAPTURE(name);
            CAPTURE(f);
            const midgard::ChunkMesh m = tr.mesh({f, 1, 2, 5});
            for (size_t i = 0; i < m.positions.size(); ++i) {
                const glm::dvec3 up = tr.body().up(m.origin + glm::dvec3(m.positions[i]));
                CHECK(glm::dot(glm::dvec3(m.normals[i]), up) > 0.2);
            }
            for (size_t i = 0; i < m.indices.size(); i += 3) {
                const glm::dvec3 a = m.origin + glm::dvec3(m.positions[m.indices[i]]),
                                 b = m.origin + glm::dvec3(m.positions[m.indices[i + 1]]),
                                 c = m.origin + glm::dvec3(m.positions[m.indices[i + 2]]);
                CHECK(glm::dot(glm::cross(b - a, c - a), tr.body().up(a)) > 0.0);
            }
        }
    }
}

TEST_CASE("finest chunks share their edge points to the bit, as floats from their middles") {
    // What the physics world collides with (origin + float offset, in double) must leave no
    // hairline gap a ray could pass through, so shared points are the same in both chunks.
    const json t = sphere_table();
    const midgard::Terrain world = sphere(t, "world");
    const midgard::Terrain flat = area();
    const auto same = [](const midgard::Terrain& tr, const midgard::ChunkKey& k, int e) {
        const std::optional<midgard::ChunkKey> n = tr.neighbour(k, midgard::ChunkEdge(e));
        REQUIRE(n.has_value());
        int back = -1;
        for (int f = 0; f < 4; ++f)
            if (tr.neighbour(*n, midgard::ChunkEdge(f)) == k) back = f;
        REQUIRE(back >= 0);
        const midgard::ChunkMesh m = tr.mesh(k), o = tr.mesh(*n);
        std::set<std::array<double, 3>> mine, theirs;
        for (int s = 0; s <= kN; ++s) {
            const auto [i, j] = edge_point(e, s);
            const auto [oi, oj] = edge_point(back, s);
            const glm::dvec3 a = m.origin + glm::dvec3(m.positions[at(i, j)]);
            const glm::dvec3 b = o.origin + glm::dvec3(o.positions[at(oi, oj)]);
            mine.insert({a.x, a.y, a.z});
            theirs.insert({b.x, b.y, b.z});
        }
        CHECK(mine == theirs);
    };
    const int level = world.max_level();
    const int last = (world.roots_u() << level) - 1;
    for (int f = 0; f < 6; ++f) {
        CAPTURE(f);
        same(world, {f, level, last / 2, last / 2}, midgard::kEast);  // inside a face
        same(world, {f, level, last / 2, last / 2}, midgard::kNorth);
        same(world, {f, level, last, 7}, midgard::kEast);  // across face edges
        same(world, {f, level, 9, 0}, midgard::kSouth);
    }
    same(flat, {0, flat.max_level(), 40, 30}, midgard::kEast);
    same(flat, {0, flat.max_level(), 40, 30}, midgard::kNorth);
}

namespace {

midgard::Terrain sculpted_planet(const json& extra = json::array()) {
    std::ifstream in(fs::path(MIDGARD_SCHEMA_DIR) / "tests" / "terrain_sphere_stamps.json");
    REQUIRE(in);
    const json t = json::parse(in);
    json b = t.at("body");
    b["id"] = "world";
    std::vector<midgard::TerrainStamp> stamps;
    for (const json& s : t.at("stamps")) stamps.push_back(midgard::stamp_from_json(s));
    for (const json& s : extra) stamps.push_back(midgard::stamp_from_json(s));
    return midgard::Terrain(midgard::body_from_json(b), {}, stamps);
}

}  // namespace

TEST_CASE("cube-sphere sculpting: heights and chunk hashes match the shared table") {
    std::ifstream in(fs::path(MIDGARD_SCHEMA_DIR) / "tests" / "terrain_sphere_stamps.json");
    REQUIRE(in);
    const json t = json::parse(in);
    const midgard::Terrain tr = sculpted_planet();
    int heights = 0, hashes = 0;
    for (const json& c : t.at("cases")) {
        CAPTURE(c.dump());
        if (c.contains("hash")) {
            const json& k = c.at("key");
            CHECK(hex(tr.chunk_hash({k[0].get<int>(), k[1].get<int>(), k[2].get<int>(),
                                     k[3].get<int>()})) == c.at("hash").get<std::string>());
            ++hashes;
        } else {
            CHECK(tr.height(c.at("face").get<int>(), c.at("u").get<double>(),
                            c.at("v").get<double>()) == c.at("height").get<double>());
            ++heights;
        }
    }
    CHECK(heights == 9);
    CHECK(hashes == 4);
    // The self-test's fingerprint (checked on devices) is the table's.
    CHECK(hex(midgard::sphere_terrain_fingerprint()) == t.at("fingerprint").get<std::string>());
    CHECK(midgard::sphere_terrain_fingerprint() == midgard::kSphereTerrainFingerprint);
}

TEST_CASE("cube-sphere sculpting: strokes reach across a face edge, which stays exact") {
    // The table's strokes cross the edge between faces 0 and 5, plus paint across it.
    const midgard::Terrain plain = sphere(sphere_table(), "world");
    const midgard::Terrain tr = sculpted_planet(json::parse(
        R"([{"tool": "paint", "paint": "rock", "radius": 40, "strength": 1, "face": 0,
             "points": [[0.99, 0.5], [1.012, 0.5]]}])"));
    // The raise along face 0's v = 0.41 reaches over onto face 5: higher there than as
    // generated, by most of its strength right by the edge.
    const midgard::SurfacePoint across = midgard::cube_across({0, 1.004, 0.41, 0.0});
    REQUIRE(across.face == 5);
    CHECK(tr.height(5, across.u, across.v) > plain.height(5, across.u, across.v) + 15.0);
    // The painted rock shows on face 5 too.
    const midgard::SurfacePoint painted = midgard::cube_across({0, 1.004, 0.5, 0.0});
    CHECK(tr.biome_at(painted.face, painted.u, painted.v).z > 0.9f);
    // Chunks either side of the edge, at every level, still share their edge points exactly.
    const midgard::CubeLink link = midgard::cube_link(0, midgard::kEast);
    for (int level = tr.min_level(); level <= tr.max_level(); ++level) {
        CAPTURE(level);
        const int across_n = int(tr.chunks_u(level));
        for (const double v : {0.41, 0.46, 0.5, 0.52, 0.605}) {
            const midgard::ChunkKey k{0, level, across_n - 1, int(v * across_n)};
            if (v == 0.41) CHECK(tr.touches(k, tr.stamps().front()));  // under the raise
            const std::optional<midgard::ChunkKey> n = tr.neighbour(k, midgard::kEast);
            REQUIRE(n.has_value());
            const midgard::ChunkMesh m = tr.mesh(k), o = tr.mesh(*n);
            for (int s = 0; s <= kN; ++s) {
                const auto [i, j] = edge_point(midgard::kEast, s);
                const auto [oi, oj] = edge_point(link.edge, link.reversed ? kN - s : s);
                CHECK(m.heights[at(i, j)] == o.heights[at(oi, oj)]);
                CHECK(m.biome[at(i, j)] == o.biome[at(oi, oj)]);
            }
        }
    }
    // A stroke on face 0 touches the chunk across the edge on face 5 (it is made again).
    const int level = tr.max_level();
    const int last = int(tr.chunks_u(level)) - 1;
    const midgard::ChunkKey by_edge{0, level, last, int(0.41 * (last + 1))};
    CHECK(tr.touches(*tr.neighbour(by_edge, midgard::kEast), tr.stamps().front()));
    CHECK_FALSE(tr.touches({0, level, 0, 0}, tr.stamps().front()));  // far away
}

TEST_CASE("a direction in a face's terms, carried on past its edges") {
    for (int f = 0; f < 6; ++f) {
        CAPTURE(f);
        for (const double u : {-0.3, -0.02, 0.0, 0.25, 0.5, 1.0, 1.04, 1.3})
            for (const double v : {-0.1, 0.4, 1.0, 1.2}) {
                CAPTURE(u);
                CAPTURE(v);
                const glm::dvec3 d = midgard::cube_direction(f, u, v);
                const std::optional<glm::dvec2> back = midgard::cube_face_point(f, d);
                REQUIRE(back.has_value());
                CHECK(back->x == doctest::Approx(u).epsilon(1e-12));
                CHECK(back->y == doctest::Approx(v).epsilon(1e-12));
            }
        // Inside the face it is cube_surface's answer; behind it, none.
        const glm::dvec3 d = midgard::cube_direction(f, 0.3, 0.7);
        const midgard::SurfacePoint s = midgard::cube_surface(d);
        CHECK(s.face == f);
        CHECK(midgard::cube_face_point(f, d)->x == doctest::Approx(s.u));
        CHECK_FALSE(midgard::cube_face_point(f, -d).has_value());
    }
}

TEST_CASE("a climate and biomes: from JSON and back, what the cook refuses, and temperatures") {
    const json j = json::parse(R"({"climate": {"temperature": 20, "latitude_cooling": 45,
        "lapse_rate": 6, "variation": 0, "season_swing": 10},
        "biomes": [{"name": "Desert", "temperature": [24, 60], "moisture": [0, 0.4],
                    "color": [0.8, 0.7, 0.4], "dry_color": [0.85, 0.75, 0.5]},
                   {"name": "Tundra", "temperature": [-40, 3]}]})");
    const midgard::TerrainSettings t = midgard::terrain_from_json(j);
    REQUIRE(t.climate.has_value());
    CHECK(t.climate->latitude_cooling == 45.0);
    REQUIRE(t.biomes.size() == 2);
    CHECK(t.biomes[0].moisture_max == 0.4);
    CHECK(t.biomes[1].color == midgard::Biome{}.color);  // the default colours
    CHECK(midgard::terrain_from_json(midgard::terrain_to_json(t)) == t);
    CHECK(midgard::terrain_problems(t).empty());
    // What the cook refuses.
    for (const char* bad :
         {R"({"climate": {"lapse_rate": -1}})", R"({"climate": {"season_swing": 500}})",
          R"({"biomes": [{"name": "b", "temperature": [10, 0]}]})",
          R"({"biomes": [{"name": "b", "moisture": [0.2, 1.5]}]})",
          R"({"biomes": [{"name": "b", "color": [2, 0, 0]}]})"}) {
        CAPTURE(bad);
        CHECK_FALSE(
            midgard::terrain_problems(midgard::terrain_from_json(json::parse(bad))).empty());
    }
    json nine = json::object();
    nine["biomes"] = json::array();
    for (int i = 0; i < 9; ++i) nine["biomes"].push_back({{"name", std::to_string(i)}});
    CHECK_FALSE(midgard::terrain_problems(midgard::terrain_from_json(nine)).empty());

    // Temperatures by hand (no variation): colder with height, and towards a planet's poles.
    const midgard::Terrain flat(midgard::body_from_json(json::parse(
                                    R"({"id": "a", "shape": "bounded_area", "width": 3000,
                                        "length": 2000})")),
                                t);
    CHECK(flat.temperature_at(0, 0.5, 0.5, 0.0) == doctest::Approx(20.0));
    CHECK(flat.temperature_at(0, 0.5, 0.5, 500.0) == doctest::Approx(17.0));  // 6 a km
    CHECK(flat.temperature_at(0, 0.5, 0.5, -40.0) == doctest::Approx(20.0));  // under the sea
    const midgard::Terrain planet(
        midgard::body_from_json(json::parse(R"({"id": "p", "shape": "sphere_outside",
                                                "radius": 3000})")),
        t);
    CHECK(planet.temperature_at(0, 0.5, 0.5, 0.0) == doctest::Approx(20.0));   // the equator
    CHECK(planet.temperature_at(2, 0.5, 0.5, 0.0) == doctest::Approx(-25.0));  // a pole
    // A chunk's mesh carries its points' temperatures only with a climate.
    const midgard::ChunkMesh m = flat.mesh({0, 2, 5, 3});
    REQUIRE(m.temperatures.size() == m.positions.size());
    CHECK(m.temperatures[0] ==
          doctest::Approx(flat.temperature_at(0, 5.0 / 12.0, 3.0 / 8.0, m.heights[0])));
    CHECK(area().mesh({0, 2, 5, 3}).temperatures.empty());
}

namespace {

midgard::TerrainSettings forest_rules() {
    return midgard::terrain_from_json(json::parse(R"({"placements": [
        {"object": "obj_tree", "spacing": 9, "chance": 0.7, "slope": [0, 25], "height": [1, 1000],
         "scale": [0.8, 1.3], "salt": "trees"},
        {"object": "obj_rock", "spacing": 20, "chance": 0.5, "slope": [5, 70], "align": 1}]})"));
}

}  // namespace

TEST_CASE("placing objects: rules as JSON, what the cook refuses, and the same at every level") {
    const midgard::TerrainSettings s = forest_rules();
    REQUIRE(s.placements.size() == 2);
    CHECK(s.placements[0].scale_max == 1.3);
    CHECK(midgard::terrain_from_json(midgard::terrain_to_json(s)) == s);
    CHECK(midgard::terrain_problems(s).empty());
    for (const char* bad : {R"({"placements": [{"spacing": 9}]})",  // which object
                            R"({"placements": [{"object": "o", "spacing": 0.1}]})",
                            R"({"placements": [{"object": "o", "chance": 1.5}]})",
                            R"({"placements": [{"object": "o", "slope": [40, 10]}]})",
                            R"({"placements": [{"object": "o", "scale": [0, 1]}]})",
                            R"({"placements": [{"object": "o", "distance": 5000}]})"}) {
        CAPTURE(bad);
        CHECK_FALSE(
            midgard::terrain_problems(midgard::terrain_from_json(json::parse(bad))).empty());
    }

    const midgard::Terrain t(
        midgard::body_from_json(json::parse(
            R"({"id": "a", "shape": "bounded_area", "width": 3000, "length": 2000, "seed": 7})")),
        s);
    const int level = t.placement_level();
    const double side = 3000.0 / double(t.chunks_u(level));
    CHECK(side >= 64.0);
    CHECK(side < 128.0 + 1e-9);
    // The same objects whichever tiles hold them: a tile's are its four children's together.
    const midgard::ChunkKey tile{0, level, 12, 9};
    std::vector<midgard::TerrainPlacement> parent = t.placements_in(t.parent(tile));
    std::vector<midgard::TerrainPlacement> children;
    for (const midgard::ChunkKey& c : t.children(t.parent(tile)))
        for (const midgard::TerrainPlacement& p : t.placements_in(c)) children.push_back(p);
    const auto key = [](const midgard::TerrainPlacement& p) {
        return std::tuple(p.rule, p.u, p.v);
    };
    std::set<std::tuple<int, double, double>> a, b;
    for (const auto& p : parent) a.insert(key(p));
    for (const auto& p : children) b.insert(key(p));
    CHECK(a == b);
    CHECK(a.size() > 20);
    // Each where its rule allows: on the ground, its slope in range, upright or leaning.
    for (const midgard::TerrainPlacement& p : parent) {
        const midgard::PlacementRule& r = s.placements[size_t(p.rule)];
        CHECK(std::abs(t.above_ground(p.position)) < 1e-6);
        CHECK(p.scale >= r.scale_min);
        CHECK(p.scale <= r.scale_max);
        CHECK(glm::length(p.up) == doctest::Approx(1.0));
        if (r.align == 0.0) CHECK(p.up == t.body().up(p.position));
    }
    // Same rules on every target: the fingerprint (also in the self-test).
    char fp[17];
    std::snprintf(fp, sizeof(fp), "%016" PRIx64, midgard::placement_fingerprint());
    MESSAGE("placement fingerprint ", fp);
    CHECK(midgard::placement_fingerprint() == midgard::kPlacementFingerprint);
}

TEST_CASE("placing objects by biome, and on a planet") {
    midgard::TerrainSettings s = midgard::terrain_from_json(json::parse(R"({
        "climate": {"temperature": 15, "variation": 4},
        "biomes": [{"name": "Wet", "moisture": [0.5, 1]}, {"name": "Dry", "moisture": [0, 0.5]}],
        "placements": [{"object": "obj_reed", "spacing": 6, "biomes": ["Wet"], "height": [-1e6, 1e6],
                        "slope": [0, 90]}]})"));
    const midgard::Terrain t(
        midgard::body_from_json(json::parse(
            R"({"id": "a", "shape": "bounded_area", "width": 3000, "length": 2000, "seed": 7})")),
        s);
    int placed = 0;
    for (int x = 0; x < 6; ++x)
        for (const midgard::TerrainPlacement& p :
             t.placements_in({0, t.placement_level(), 10 + x, 8})) {
            CHECK(t.biome_index(p.face, p.u, p.v, t.height(p.face, p.u, p.v)) == 0);  // Wet
            ++placed;
        }
    CHECK(placed > 0);
    // A planet: placements on the ground of any face.
    const midgard::Terrain planet(
        midgard::body_from_json(
            json::parse(R"({"id": "p", "shape": "sphere_outside", "radius": 3000, "seed": 11})")),
        forest_rules());
    int on_planet = 0;
    for (int f = 0; f < 6; ++f)
        for (const midgard::TerrainPlacement& p :
             planet.placements_in({f, planet.placement_level(), 3, 5})) {
            CHECK(std::abs(planet.above_ground(p.position)) < 1e-6);
            ++on_planet;
        }
    CHECK(on_planet > 0);
}

TEST_CASE("ground layers and biomes take texture sets: every map, a tile, read and written") {
    const nlohmann::json j = nlohmann::json::parse(R"({
        "ground": {
            "grass": {"textures": {"base_color": "tex:library/textures/grass/g_color.png",
                                   "normal": "tex:library/textures/grass/g_normal.png#linear",
                                   "metallic_roughness":
                                       "tex:library/textures/grass/g_mr.png#linear",
                                   "occlusion": "tex:library/textures/grass/g_ao.png#linear",
                                   "emissive": "tex:library/textures/grass/g_glow.png"},
                      "tile": 3.0},
            "rock": {"textures": {"base_color": "tex:library/textures/rock/r_color.png"}}},
        "biomes": [{"name": "Forest",
                    "texture": {"textures": {"base_color": "tex:f.png"}, "tile": 2.0},
                    "dry_texture": {"textures": {"normal": "tex:g.png#linear"}}}]})");
    const midgard::TerrainSettings t = midgard::terrain_from_json(j);
    const midgard::GroundTexture& grass = t.ground.at("grass");
    CHECK(grass.tile == 3.0);
    CHECK(grass.normal == "tex:library/textures/grass/g_normal.png#linear");
    CHECK(grass.occlusion == "tex:library/textures/grass/g_ao.png#linear");
    CHECK(grass.metallic_roughness == "tex:library/textures/grass/g_mr.png#linear");
    CHECK(grass.emissive == "tex:library/textures/grass/g_glow.png");
    CHECK(grass.textures().size() == 5);
    CHECK(t.ground.at("rock").tile == 4.0);  // (the default)
    CHECK(t.biomes[0].texture.base_color == "tex:f.png");
    CHECK(t.biomes[0].dry_texture.normal == "tex:g.png#linear");
    CHECK(midgard::ground_textures(t).size() == 8);
    CHECK(midgard::terrain_problems(t).empty());
    // Written as read.
    CHECK(midgard::terrain_from_json(midgard::terrain_to_json(t)) == t);
    // Maps switched off: kept, but not used (nor packed), and written as read.
    midgard::TerrainSettings dimmed = midgard::terrain_from_json(nlohmann::json::parse(R"({
        "ground": {"rock": {"textures": {"base_color": "tex:c.png", "emissive": "tex:e.png",
                                          "metallic_roughness": "tex:m.png#linear"},
                             "off": ["emissive", "metallic_roughness"]}}})"));
    const midgard::GroundTexture& rock = dimmed.ground.at("rock");
    CHECK(rock.emissive == "tex:e.png");
    CHECK(rock.map("emissive").empty());
    CHECK(rock.map("base_color") == "tex:c.png");
    CHECK(rock.textures() == std::vector<std::string>{"tex:c.png"});
    CHECK(midgard::ground_textures(dimmed) == std::vector<std::string>{"tex:c.png"});
    CHECK(midgard::terrain_from_json(midgard::terrain_to_json(dimmed)) == dimmed);
    CHECK_THROWS(midgard::terrain_from_json(
        nlohmann::json::parse(R"({"ground": {"rock": {"off": ["sparkle"]}}})")));
    // Refused: an unknown map; told: an unknown layer, a tile out of range.
    CHECK_THROWS(midgard::terrain_from_json(
        nlohmann::json::parse(R"({"ground": {"grass": {"textures": {"height": "tex:h.png"}}}})")));
    midgard::TerrainSettings bad = t;
    bad.ground["lava"] = grass;
    bad.ground["sand"].tile = 0.0;
    bad.ground["sand"].base_color = "tex:s.png";
    const auto problems = midgard::terrain_problems(bad);
    CHECK(std::any_of(problems.begin(), problems.end(),
                      [](const std::string& p) { return p.find("'lava'") != std::string::npos; }));
    CHECK(std::any_of(problems.begin(), problems.end(),
                      [](const std::string& p) { return p.find("tile") != std::string::npos; }));
}
