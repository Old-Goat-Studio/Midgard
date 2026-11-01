// SPDX-License-Identifier: Apache-2.0
// World generation graphs (roadmap §7, "World types"; milestone 5.11): how a world type makes its
// ground's height at a point, as a graph of nodes. A recipe's "graph":
//
//   {"output": "height",
//    "nodes": {"hills": {"op": "noise", "size": 500, "octaves": 6},
//              "height": {"op": "add", "in": [8, "scaled"]},
//              "scaled": {"op": "multiply", "in": [60, "hills"]}}}
//
// "in" lists other nodes by id, or numbers. The nodes, and exactly what each computes (in this
// order of operations: tools/reference/worldgen.py is the twin, schema/tests/worldgen_cases.json
// holds both to it):
//   noise       fbm(seed, q / size, octaves) / 65536, about -1 to 1; "salt" (a name) gives an
//               independent noise from the same seed (seed ^ tag(salt))
//   ridged      1 - |noise|: ridges where the noise crosses 0
//   constant    "value"
//   input       "name": latitude (degrees, -90 to 90 on a sphere, else 0), edge_distance (metres
//               to a bounded area's nearest edge), end_distance (metres to a cylinder's nearer
//               end); 1e30 where a shape has no edge or ends
//   add, multiply, min, max   over all of "in", left to right
//   subtract, divide          a - b, a / b (0 where b is 0)
//   lerp        a + (b - a) t
//   abs
//   clamp       min(max(x, "min"), "max")
//   smoothstep  t = (x - "from") / ("to" - "from"), clamped to 0..1, then t t (3 - 2 t)
//   curve       "points" [[x, y], ...] (x increasing), straight between them, flat beyond
//   terrace     steps "step" metres apart: s = x / step, k = floor(s), f = s - k,
//               (k + f + (f f (3 - 2 f) - f) "sharpness") step
//   smooth      the input's average at four points "radius" metres away (east, west, north,
//               south, along the ground): (((east + west) + north) + south) * 0.25
//   erode       x - "strength" max(0, x - smooth(x)): wears peaks down, keeps valleys
// q is the point's noise position (midgard/terrain.h): on the shape's surface, in metres.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

#include "midgard/body.h"

namespace midgard {

inline constexpr int kMaxGraphNodes = 256;
inline constexpr int kMaxFilterDepth = 3;  // smooth and erode nested at most this deep

// A point on a world's surface, where a graph is evaluated.
struct GraphPlace {
    int face = 0;
    double u = 0.0, v = 0.0;
};

class WorldGraph {
public:
    // Throws std::runtime_error naming what's wrong (graph_problems lists them all).
    static WorldGraph from_json(const nlohmann::json& j);
    // The height (metres, before the shape's edges shape it) at a place on a shape, from a seed.
    double evaluate(const Shape& shape, std::uint64_t seed, const GraphPlace& place) const;
    int node_count() const { return int(nodes_.size()); }
    bool uses_neighbours() const;  // a smooth or erode node: four times the work per point

    enum class Op {
        Noise,
        Ridged,
        Constant,
        Input,
        Add,
        Multiply,
        Min,
        Max,
        Subtract,
        Divide,
        Lerp,
        Abs,
        Clamp,
        Smoothstep,
        Curve,
        Terrace,
        Smooth,
        Erode
    };
    enum class InputName { Latitude, EdgeDistance, EndDistance };
    struct Node {
        Op op = Op::Constant;
        std::vector<int> in;      // earlier nodes
        double a = 0.0, b = 0.0;  // size/value/min/from/step/radius, and max/to/sharpness/strength
        int octaves = 1;
        std::uint64_t salt = 0;
        InputName input = InputName::Latitude;
        std::vector<glm::dvec2> points;
    };

private:
    double eval(int node, const Shape& shape, std::uint64_t seed, const GraphPlace& place,
                std::vector<double>& memo, std::vector<char>& done) const;
    double around(int node, const Shape& shape, std::uint64_t seed, const GraphPlace& place,
                  double radius) const;
    std::vector<Node> nodes_;  // inputs before the nodes that use them
    int output_ = 0;
};

// What the cook refuses in a graph (none: it can be used).
std::vector<std::string> graph_problems(const nlohmann::json& j);

// The noise position of a place (midgard/terrain.h's Terrain::noise_position), and a place
// `east` and `north` metres from another along the ground (a sphere's directions from the
// point's own, not its face's, so it is the same from every face).
glm::dvec3 graph_noise_position(const Shape& shape, const GraphPlace& place);
GraphPlace graph_offset(const Shape& shape, const GraphPlace& place, double east, double north);

// A graph's value at a fixed set of places on five test shapes (those of
// schema/tests/worldgen_cases.json's fingerprint): kWorldGraphFingerprint on every target.
std::uint64_t worldgen_fingerprint();
inline constexpr std::uint64_t kWorldGraphFingerprint = 0xf33aae197ab72bc2ULL;

}  // namespace midgard
