// SPDX-License-Identifier: Apache-2.0
// World generation graphs (midgard/worldgen.h, milestone 5.11): against the shared table
// (schema/tests/worldgen_cases.json, from tools/reference/worldgen.py) to the bit, hand-worked
// cases for every node, the 5.9 ground as a graph, what the cook refuses, and seams.
#include <doctest/doctest.h>

#include <cinttypes>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "midgard/det.h"
#include "midgard/worldgen.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

json table() {
    std::ifstream in(fs::path(MIDGARD_SCHEMA_DIR) / "tests" / "worldgen_cases.json");
    REQUIRE(in);
    return json::parse(in);
}

midgard::Shape shape_named(const json& b) {
    const std::string s = b.at("shape").get<std::string>();
    if (s == "bounded_area")
        return midgard::Shape::bounded_area(b.at("width"), b.at("length"), 9.81);
    if (s == "flat_cylinder")
        return midgard::Shape::flat_cylinder(b.at("circumference"), b.at("length"), 9.81);
    if (s == "curved_cylinder")
        return midgard::Shape::curved_cylinder(b.at("radius"), b.at("length"), 9.81);
    if (s == "sphere_outside") return midgard::Shape::sphere_outside(b.at("radius"), 9.81);
    return midgard::Shape::sphere_inside(b.at("radius"), 9.81);
}

// One node's value, fed by constants: {"op": ..., more} with "in" numbers.
double one(const char* node_json,
           const midgard::Shape& shape = midgard::Shape::bounded_area(3000, 2000, 9.81),
           midgard::GraphPlace place = {0, 0.5, 0.5}) {
    const json g = {{"output", "x"}, {"nodes", {{"x", json::parse(node_json)}}}};
    return midgard::WorldGraph::from_json(g).evaluate(shape, 7, place);
}

std::string hex(std::uint64_t v) {
    char out[17];
    std::snprintf(out, sizeof(out), "%016" PRIx64, v);
    return out;
}

}  // namespace

TEST_CASE("world graphs match the shared table to the bit") {
    const json t = table();
    const std::uint64_t seed = t.at("seed").get<std::uint64_t>();
    int cases = 0;
    for (const json& c : t.at("cases")) {
        CAPTURE(c.dump());
        const midgard::WorldGraph g =
            midgard::WorldGraph::from_json(t.at("graphs").at(c.at("graph").get<std::string>()));
        const midgard::Shape shape =
            shape_named(t.at("bodies").at(c.at("body").get<std::string>()));
        const json& p = c.at("place");
        CHECK(g.evaluate(shape, seed, {p[0].get<int>(), p[1].get<double>(), p[2].get<double>()}) ==
              c.at("height").get<double>());
        ++cases;
    }
    CHECK(cases == 62);
    CHECK(hex(midgard::worldgen_fingerprint()) == t.at("fingerprint").get<std::string>());
    CHECK(midgard::worldgen_fingerprint() == midgard::kWorldGraphFingerprint);
}

TEST_CASE("every node, worked by hand") {
    CHECK(one(R"({"op": "constant", "value": 4.5})") == 4.5);
    CHECK(one(R"({"op": "add", "in": [1, 2, 3.5]})") == 6.5);
    CHECK(one(R"({"op": "multiply", "in": [2, 3, -1.5]})") == -9.0);
    CHECK(one(R"({"op": "min", "in": [4, -2, 7]})") == -2.0);
    CHECK(one(R"({"op": "max", "in": [4, -2, 7]})") == 7.0);
    CHECK(one(R"({"op": "subtract", "in": [10, 4]})") == 6.0);
    CHECK(one(R"({"op": "divide", "in": [10, 4]})") == 2.5);
    CHECK(one(R"({"op": "divide", "in": [10, 0]})") == 0.0);  // not infinity
    CHECK(one(R"({"op": "lerp", "in": [2, 10, 0.25]})") == 4.0);
    CHECK(one(R"({"op": "abs", "in": [-3]})") == 3.0);
    CHECK(one(R"({"op": "clamp", "in": [12], "min": 0, "max": 10})") == 10.0);
    CHECK(one(R"({"op": "clamp", "in": [-1], "min": 0, "max": 10})") == 0.0);
    CHECK(one(R"({"op": "smoothstep", "in": [5], "from": 0, "to": 10})") == 0.5);
    CHECK(one(R"({"op": "smoothstep", "in": [2.5], "from": 0, "to": 10})") == 0.15625);
    CHECK(one(R"({"op": "smoothstep", "in": [20], "from": 0, "to": 10})") == 1.0);
    // A curve: straight between its points, flat beyond them.
    const char* curve = R"({"op": "curve", "in": [%g], "points": [[0, 0], [10, 20], [30, 25]]})";
    const auto at = [&](double x) {
        char text[200];
        std::snprintf(text, sizeof(text), curve, x);
        return one(text);
    };
    CHECK(at(-5) == 0.0);
    CHECK(at(5) == 10.0);
    CHECK(at(10) == 20.0);
    CHECK(at(20) == 22.5);
    CHECK(at(99) == 25.0);
    // Terraces: sharpness 0 is no change; 1 the smooth step's.
    CHECK(one(R"({"op": "terrace", "in": [13], "step": 5, "sharpness": 0})") ==
          doctest::Approx(13.0));
    CHECK(one(R"({"op": "terrace", "in": [13], "step": 5, "sharpness": 1})") ==
          doctest::Approx(13.24));  // s 2.6: 2 + 0.648 steps
    CHECK(one(R"({"op": "terrace", "in": [10], "step": 5, "sharpness": 1})") == 10.0);
    // Inputs: latitude on a sphere's top face is 90; its equator 0; elsewhere 0.
    const midgard::Shape planet = midgard::Shape::sphere_outside(1000, 9.81);
    CHECK(one(R"({"op": "input", "name": "latitude"})", planet, {2, 0.5, 0.5}) ==
          doctest::Approx(90.0));
    CHECK(one(R"({"op": "input", "name": "latitude"})", planet, {0, 0.5, 0.5}) ==
          doctest::Approx(0.0));
    CHECK(one(R"({"op": "input", "name": "latitude"})") == 0.0);  // flat ground
    // Edges and ends, in metres; none on other shapes.
    CHECK(one(R"({"op": "input", "name": "edge_distance"})") == 1000.0);  // 3000 x 2000
    CHECK(one(R"({"op": "input", "name": "edge_distance"})",
              midgard::Shape::bounded_area(3000, 2000, 9.81),
              {0, 0.1, 0.5}) == doctest::Approx(300.0));
    const midgard::Shape habitat = midgard::Shape::curved_cylinder(1000, 5000, 9.81);
    CHECK(one(R"({"op": "input", "name": "end_distance"})", habitat, {0, 0.3, 0.9}) ==
          doctest::Approx(500.0));
    CHECK(one(R"({"op": "input", "name": "end_distance"})") == 1e30);
    CHECK(one(R"({"op": "input", "name": "edge_distance"})", habitat) == 1e30);
    // Smooth and erode of a constant leave it.
    CHECK(one(R"({"op": "smooth", "in": [7], "radius": 30})") == 7.0);
    CHECK(one(R"({"op": "erode", "in": [7], "radius": 30, "strength": 1})") == 7.0);
}

TEST_CASE("the 5.9 ground as a graph: base + height_scale * noise, exactly") {
    const midgard::WorldGraph classic =
        midgard::WorldGraph::from_json(table().at("graphs").at("classic"));
    for (const midgard::Shape& shape : {midgard::Shape::bounded_area(3000, 2000, 9.81),
                                        midgard::Shape::flat_cylinder(6000, 5000, 9.81),
                                        midgard::Shape::curved_cylinder(1000, 5000, 9.81),
                                        midgard::Shape::sphere_outside(3000, 9.81)})
        for (const midgard::GraphPlace p :
             {midgard::GraphPlace{0, 0.31, 0.77}, midgard::GraphPlace{0, 0.999, 0.02}}) {
            const glm::dvec3 q = midgard::graph_noise_position(shape, p);
            const double expected =
                8.0 +
                60.0 * (double(midgard::det::fbm(42, q.x / 500.0, q.y / 500.0, q.z / 500.0, 6)) /
                        65536.0);
            CHECK(classic.evaluate(shape, 42, p) == expected);
        }
}

TEST_CASE("what the cook refuses in a graph") {
    const auto problems = [](const char* text) {
        return midgard::graph_problems(json::parse(text));
    };
    CHECK(problems(R"({"output": "a", "nodes": {"a": {"op": "constant", "value": 1}}})").empty());
    for (const char* bad :
         {R"({"nodes": {}})",                                                   // no output
          R"({"output": "a", "nodes": {}})",                                    // no such node
          R"({"output": "a", "nodes": {"a": {"op": "magic"}}})",                // no such op
          R"({"output": "a", "nodes": {"a": {"op": "add", "in": ["a", 1]}}})",  // a loop
          R"({"output": "a", "nodes": {"a": {"op": "add", "in": [1]}}})",       // one input
          R"({"output": "a", "nodes": {"a": {"op": "lerp", "in": [1, 2]}}})",   // two of three
          R"({"output": "a", "nodes": {"a": {"op": "noise", "size": 0}}})",     // size 0
          R"({"output": "a", "nodes": {"a": {"op": "noise", "size": 9, "octaves": 13}}})",
          R"({"output": "a", "nodes": {"a": {"op": "input", "name": "height"}}})",
          R"({"output": "a", "nodes": {"a": {"op": "curve", "in": [1], "points": [[0, 0]]}}})",
          R"({"output": "a", "nodes": {"a": {"op": "curve", "in": [1],
                                                "points": [[0, 0], [0, 1]]}}})",  // x not rising
          R"({"output": "a", "nodes": {"a": {"op": "smoothstep", "in": [1], "from": 2,
                                                "to": 2}}})",
          R"({"output": "a", "nodes": {"a": {"op": "erode", "in": [1], "radius": 5,
                                                "strength": 2}}})",
          R"({"output": "a", "nodes": {"a": {"op": "terrace", "in": [1], "step": 0}}})",
          // Smooth inside smooth inside smooth inside smooth: too much work per point.
          R"({"output": "a", "nodes": {
                 "a": {"op": "smooth", "in": ["b"], "radius": 5},
                 "b": {"op": "smooth", "in": ["c"], "radius": 5},
                 "c": {"op": "smooth", "in": ["d"], "radius": 5},
                 "d": {"op": "smooth", "in": [1], "radius": 5}}})"}) {
        CAPTURE(bad);
        CHECK_FALSE(problems(bad).empty());
        CHECK_THROWS(midgard::WorldGraph::from_json(json::parse(bad)));
    }
}

TEST_CASE("a smoothed graph is the same on a cube face's edge from both faces") {
    // Its neighbours are found along a frame from the point's own direction, so a point that two
    // faces share gives the same value from either.
    const midgard::WorldGraph g =
        midgard::WorldGraph::from_json(table().at("graphs").at("everything"));
    const midgard::Shape planet = midgard::Shape::sphere_outside(3000, 9.81);
    for (int f = 0; f < 6; ++f)
        for (int e = 0; e < 4; ++e)
            for (const double t : {0.25, 0.5, 0.8125}) {
                const midgard::CubeLink link = midgard::cube_link(f, e);
                const auto on_edge = [](int edge, double s) {
                    return edge == 0   ? glm::dvec2(0.0, s)
                           : edge == 1 ? glm::dvec2(1.0, s)
                           : edge == 2 ? glm::dvec2(s, 0.0)
                                       : glm::dvec2(s, 1.0);
                };
                const glm::dvec2 a = on_edge(e, t),
                                 b = on_edge(link.edge, link.reversed ? 1 - t : t);
                CAPTURE(f);
                CAPTURE(e);
                CHECK(g.evaluate(planet, 99, {f, a.x, a.y}) ==
                      g.evaluate(planet, 99, {link.face, b.x, b.y}));
            }
}
