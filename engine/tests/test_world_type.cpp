// SPDX-License-Identifier: Apache-2.0
// World types (midgard/world_type.h, milestone 5.11): recipes and the worlds that name them, as
// JSON; what the cook refuses; and which shapes and sizes a recipe suits.
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "midgard/world_map.h"
#include "midgard/world_type.h"

using json = nlohmann::json;

TEST_CASE("a world type's recipe from JSON and back") {
    const json j = json::parse(R"({"shapes": ["sphere_outside", "sphere_inside"],
        "size": [1000, 8000], "gravity": 7.5, "atmosphere": "env_earth",
        "calendar": "cal_standard", "terrain": {"height_scale": 120, "sea_level": 4,
        "wall": "barrier"}})");
    const midgard::WorldRecipe r = midgard::recipe_from_json(j);
    CHECK(r.shapes.size() == 2);
    CHECK(*r.size_min == 1000.0);
    CHECK(*r.size_max == 8000.0);
    CHECK(*r.gravity == 7.5);
    CHECK(r.atmosphere == "env_earth");
    CHECK(r.terrain.height_scale == 120.0);
    CHECK(r.terrain.wall == midgard::WallLook::Barrier);
    CHECK(midgard::recipe_from_json(midgard::recipe_to_json(r)) == r);
    CHECK(midgard::recipe_problems(r).empty());
    // An empty recipe is the default ground, for any shape and size.
    const midgard::WorldRecipe any = midgard::recipe_from_json(json::object());
    CHECK(any.shapes.empty());
    CHECK(any.terrain == midgard::TerrainSettings{});
    // What can't be read.
    for (const char* bad :
         {R"({"shapes": ["cube"]})", R"({"size": [1000]})", R"({"gravity": "high"})",
          R"({"terrain": {"seed": 4}})", R"({"atmosphere": 3})", "[]"})
        CHECK_THROWS(midgard::recipe_from_json(json::parse(bad)));
    // What the cook refuses.
    CHECK_FALSE(midgard::recipe_problems(
                    midgard::recipe_from_json(json::parse(R"({"size": [5000, 1000]})")))
                    .empty());
    CHECK_FALSE(
        midgard::recipe_problems(midgard::recipe_from_json(json::parse(R"({"gravity": 0})")))
            .empty());
    CHECK_FALSE(midgard::recipe_problems(
                    midgard::recipe_from_json(json::parse(R"({"terrain": {"octaves": 0}})")))
                    .empty());
}

TEST_CASE("a world names a world type's version and its seed") {
    const midgard::WorldRef w = midgard::world_ref_from_json(
        json::parse(R"({"world_type": "wt_moon", "version": 3, "seed": 42})"));
    CHECK(w.world_type == "wt_moon");
    CHECK(w.version == 3);
    CHECK(*w.seed == 42);
    CHECK(midgard::world_ref_from_json(midgard::world_ref_to_json(w)) == w);
    CHECK(midgard::is_world_ref(json::parse(R"({"world_type": "wt_moon"})")));
    CHECK_FALSE(midgard::is_world_ref(json::parse(R"({"height_scale": 60})")));  // from before
    CHECK(midgard::world_ref_from_json(json::parse(R"({"world_type": "a"})")).version == 1);
    CHECK_THROWS(midgard::world_ref_from_json(json::parse(R"({"world_type": "a", "version": 0})")));
    // The recipe's settings belong to the world type, not the scene.
    CHECK_THROWS(
        midgard::world_ref_from_json(json::parse(R"({"world_type": "a", "height_scale": 60})")));
    // The terrain made: the recipe's, with the world's seed.
    midgard::WorldRecipe r;
    r.terrain.height_scale = 80.0;
    const midgard::TerrainSettings t = midgard::world_terrain(r, w);
    CHECK(t.height_scale == 80.0);
    CHECK(*t.seed == 42);
}

TEST_CASE("a world type suits some shapes and sizes") {
    const midgard::WorldRecipe moons = midgard::recipe_from_json(
        json::parse(R"({"shapes": ["sphere_outside"], "size": [1000, 3000]})"));
    const auto body = [](const char* text) { return midgard::body_from_json(json::parse(text)); };
    CHECK(midgard::recipe_misfits(moons,
                                  body(R"({"id": "m", "shape": "sphere_outside", "radius": 2000})"))
              .empty());
    CHECK(midgard::recipe_misfits(moons,
                                  body(R"({"id": "m", "shape": "sphere_outside", "radius": 5000})"))
              .size() == 1);  // too big
    CHECK(midgard::recipe_misfits(moons, body(R"({"id": "h", "shape": "curved_cylinder",
                                                   "radius": 2000, "length": 5000})"))
              .size() == 1);  // not a planet
    // The size: a bounded area's width, a flat cylinder's circumference, or the radius.
    CHECK(midgard::world_size(body(R"({"id": "a", "shape": "bounded_area", "width": 3000,
                                       "length": 2000})")) == 3000.0);
    CHECK(midgard::world_size(body(R"({"id": "s", "shape": "flat_cylinder",
                                       "circumference": 6000, "length": 5000})")) == 6000.0);
    CHECK(midgard::world_size(body(R"({"id": "h", "shape": "sphere_inside",
                                       "radius": 2500})")) == 2500.0);
}

TEST_CASE("a recipe carries its graph, and the cook checks it") {
    const midgard::WorldRecipe r = midgard::recipe_from_json(json::parse(
        R"({"graph": {"output": "h", "nodes": {"h": {"op": "constant", "value": 3}}}})"));
    CHECK_FALSE(r.graph.empty());
    CHECK(midgard::recipe_problems(r).empty());
    CHECK(midgard::recipe_from_json(midgard::recipe_to_json(r)) == r);
    const midgard::WorldRecipe bad = midgard::recipe_from_json(
        json::parse(R"({"graph": {"output": "h", "nodes": {"h": {"op": "magic"}}}})"));
    REQUIRE(midgard::recipe_problems(bad).size() == 1);
    CHECK(midgard::recipe_problems(bad)[0].find("graph") != std::string::npos);
    // None: the terrain settings' hills, as in milestone 5.9.
    CHECK(midgard::recipe_from_json(json::object()).graph.empty());
}

TEST_CASE("a world's map: projections for each shape, coloured by the ground") {
    using midgard::MapProjection;
    const auto body = [](const char* text) { return midgard::body_from_json(json::parse(text)); };
    midgard::TerrainSettings s;
    s.seed = 7;
    // A bounded area: from above, the pixel's middle across and down the face, in proportion.
    const midgard::Terrain area(
        body(R"({"id": "a", "shape": "bounded_area", "width": 4000, "length": 2000})"), s);
    CHECK(midgard::map_projections(midgard::Shape::Kind::BoundedArea) ==
          std::vector<MapProjection>{MapProjection::TopDown});
    CHECK(midgard::map_aspect(area, MapProjection::TopDown) == 2.0);
    const auto corner = midgard::world_map_place(area, 64, 32, 0, 0, MapProjection::TopDown);
    REQUIRE(corner);
    CHECK(corner->face == 0);
    CHECK(corner->u == doctest::Approx(0.5 / 64));
    CHECK(corner->v == doctest::Approx(0.5 / 32));
    const std::vector<std::uint8_t> map = midgard::world_map(area, 64, 32);
    REQUIRE(map.size() == 64 * 32 * 4);
    CHECK(map == midgard::world_map(area, 64, 32, MapProjection::TopDown));  // its usual one
    // Its ocean round the edge is blue and opaque.
    CHECK(map[2] > map[0]);
    CHECK(map[3] == 255);
    CHECK_THROWS_AS(midgard::world_map(area, 8, 8, MapProjection::CubeFaces),
                    std::invalid_argument);
    // A cylinder unrolled: round it across, along it down.
    const midgard::Terrain habitat(
        body(R"({"id": "h", "shape": "curved_cylinder", "radius": 1000, "length": 5000})"), s);
    CHECK(midgard::map_aspect(habitat, MapProjection::Unrolled) ==
          doctest::Approx(2.0 * 3.141592653589793 * 1000.0 / 5000.0));
    // A sphere: latitude and longitude, the +Y pole along the top row.
    const midgard::Terrain planet(body(R"({"id": "p", "shape": "sphere_outside", "radius": 2000})"),
                                  s);
    const auto top =
        midgard::world_map_place(planet, 64, 32, 10, 0, MapProjection::Equirectangular);
    CHECK(midgard::cube_direction(top->face, top->u, top->v).y > 0.99);
    const auto middle =
        midgard::world_map_place(planet, 64, 32, 32, 16, MapProjection::Equirectangular);
    CHECK(std::abs(midgard::cube_direction(middle->face, middle->u, middle->v).y) < 0.06);
    // The cube's cross: each face's middle in its cell, the corners off the map.
    const auto at = [&](int x, int y) {
        return midgard::world_map_place(planet, 400, 300, x, y, MapProjection::CubeFaces);
    };
    CHECK_FALSE(at(10, 10));
    CHECK_FALSE(at(390, 290));
    const glm::dvec3 middles[6] = {{-1, 0, 0}, {0, 0, 1}, {1, 0, 0},
                                   {0, 0, -1}, {0, 1, 0}, {0, -1, 0}};
    const int cells[6][2] = {{0, 1}, {1, 1}, {2, 1}, {3, 1}, {1, 0}, {1, 2}};
    for (int i = 0; i < 6; ++i) {
        const auto p = at(cells[i][0] * 100 + 50, cells[i][1] * 100 + 50);
        REQUIRE(p);
        CHECK(glm::dot(midgard::cube_direction(p->face, p->u, p->v), middles[i]) > 0.999);
    }
    // Neighbouring cells meet: the +Z cell's right edge is the +X cell's left edge.
    const auto left = at(199, 150), right = at(200, 150);
    CHECK(glm::distance(midgard::cube_direction(left->face, left->u, left->v),
                        midgard::cube_direction(right->face, right->u, right->v)) < 0.02);
    const std::vector<std::uint8_t> cross =
        midgard::world_map(planet, 40, 30, MapProjection::CubeFaces);
    CHECK(cross[3] == 0);                                 // a corner: transparent
    CHECK(cross[(size_t(15) * 40 + 15) * 4 + 3] == 255);  // +Z's cell
}
