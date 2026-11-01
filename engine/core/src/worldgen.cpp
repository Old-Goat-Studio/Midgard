// SPDX-License-Identifier: Apache-2.0
#include "midgard/worldgen.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "midgard/det.h"
#include "midgard/hash.h"

namespace midgard {

namespace {

using json = nlohmann::json;
using Op = WorldGraph::Op;

constexpr double kTwoPi = 6.283185307179586;
constexpr double kDegrees = 57.29577951308232;  // 180 / pi
constexpr double kFar = 1e30;

double wrap01(double u) {
    return u - std::floor(u);
}

double smooth01(double t) {
    t = std::min(std::max(t, 0.0), 1.0);
    return t * t * (3.0 - 2.0 * t);
}

const std::map<std::string, Op>& op_names() {
    static const std::map<std::string, Op> names = {
        {"noise", Op::Noise},       {"ridged", Op::Ridged},
        {"constant", Op::Constant}, {"input", Op::Input},
        {"add", Op::Add},           {"multiply", Op::Multiply},
        {"min", Op::Min},           {"max", Op::Max},
        {"subtract", Op::Subtract}, {"divide", Op::Divide},
        {"lerp", Op::Lerp},         {"abs", Op::Abs},
        {"clamp", Op::Clamp},       {"smoothstep", Op::Smoothstep},
        {"curve", Op::Curve},       {"terrace", Op::Terrace},
        {"smooth", Op::Smooth},     {"erode", Op::Erode}};
    return names;
}

// How many inputs each kind of node takes: exactly n, or at least 2 (-2).
int inputs_of(Op op) {
    switch (op) {
        case Op::Noise:
        case Op::Ridged:
        case Op::Constant:
        case Op::Input:
            return 0;
        case Op::Add:
        case Op::Multiply:
        case Op::Min:
        case Op::Max:
            return -2;
        case Op::Subtract:
        case Op::Divide:
            return 2;
        case Op::Lerp:
            return 3;
        default:
            return 1;
    }
}

struct Builder {
    const json& nodes;
    std::vector<WorldGraph::Node> out;
    std::map<std::string, int> index;  // node id -> position in out
    std::map<std::string, int> state;  // 1 visiting, 2 done
    std::vector<std::string> problems;

    double number(const json& n, const std::string& id, const char* key, double fallback,
                  double low, double high, bool required = false) {
        if (!n.contains(key)) {
            if (required) problems.push_back("node '" + id + "': \"" + key + "\" is needed");
            return fallback;
        }
        if (!n.at(key).is_number()) {
            problems.push_back("node '" + id + "': \"" + key + "\" is a number");
            return fallback;
        }
        const double v = n.at(key).get<double>();
        if (!(v >= low && v <= high) || !std::isfinite(v)) {
            problems.push_back("node '" + id + "': \"" + key + "\" is from " + json(low).dump() +
                               " to " + json(high).dump());
            return fallback;
        }
        return v;
    }

    // An input: a number becomes a constant node.
    int input(const json& item, const std::string& from, int depth) {
        if (item.is_number()) {
            WorldGraph::Node c;
            c.op = Op::Constant;
            c.a = item.get<double>();
            out.push_back(c);
            return int(out.size()) - 1;
        }
        if (!item.is_string()) {
            problems.push_back("node '" + from + "': inputs are node ids or numbers");
            return -1;
        }
        return build(item.get<std::string>(), depth);
    }

    int build(const std::string& id, int depth) {
        if (const auto it = index.find(id); it != index.end()) return it->second;
        if (!nodes.contains(id)) {
            problems.push_back("no node '" + id + "'");
            return -1;
        }
        if (state[id] == 1) {
            problems.push_back("node '" + id + "' depends on itself (a loop)");
            return -1;
        }
        state[id] = 1;
        const json& n = nodes.at(id);
        WorldGraph::Node node;
        const std::string op_name = n.is_object() && n.contains("op") && n.at("op").is_string()
                                        ? n.at("op").get<std::string>()
                                        : std::string();
        const auto op = op_names().find(op_name);
        if (op == op_names().end()) {
            problems.push_back("node '" + id + "': no such node \"" + op_name + "\"");
            state[id] = 2;
            return -1;
        }
        node.op = op->second;
        const bool filter = node.op == Op::Smooth || node.op == Op::Erode;
        if (filter && depth >= kMaxFilterDepth)
            problems.push_back("node '" + id + "': smooth and erode nest at most " +
                               std::to_string(kMaxFilterDepth) + " deep");
        // Inputs first (so they come before the node).
        const json in = n.contains("in") ? n.at("in") : json::array();
        if (!in.is_array()) {
            problems.push_back("node '" + id + "': \"in\" is a list");
        } else {
            const int wanted = inputs_of(node.op);
            if ((wanted >= 0 && int(in.size()) != wanted) || (wanted < 0 && int(in.size()) < 2))
                problems.push_back(
                    "node '" + id + "': " + op_name + " takes " +
                    (wanted < 0 ? std::string("two or more inputs")
                                : std::to_string(wanted) + (wanted == 1 ? " input" : " inputs")));
            for (const json& item : in)
                node.in.push_back(input(item, id, depth + (filter ? 1 : 0)));
        }
        switch (node.op) {
            case Op::Noise:
            case Op::Ridged:
                node.a = number(n, id, "size", 500.0, 1e-3, 1e7, true);
                node.octaves = int(number(n, id, "octaves", 6, 1, 12));
                if (n.contains("salt")) {
                    if (n.at("salt").is_string())
                        node.salt = det::tag(n.at("salt").get<std::string>().c_str());
                    else
                        problems.push_back("node '" + id + "': \"salt\" is a name");
                }
                break;
            case Op::Constant:
                node.a = number(n, id, "value", 0.0, -1e9, 1e9, true);
                break;
            case Op::Input: {
                const std::string name = n.value("name", std::string());
                if (name == "latitude")
                    node.input = WorldGraph::InputName::Latitude;
                else if (name == "edge_distance")
                    node.input = WorldGraph::InputName::EdgeDistance;
                else if (name == "end_distance")
                    node.input = WorldGraph::InputName::EndDistance;
                else
                    problems.push_back("node '" + id +
                                       "': inputs are latitude, edge_distance or end_distance");
                break;
            }
            case Op::Clamp:
                node.a = number(n, id, "min", -1e30, -1e30, 1e30);
                node.b = number(n, id, "max", 1e30, -1e30, 1e30);
                break;
            case Op::Smoothstep:
                node.a = number(n, id, "from", 0.0, -1e9, 1e9, true);
                node.b = number(n, id, "to", 1.0, -1e9, 1e9, true);
                if (node.a == node.b)
                    problems.push_back("node '" + id + "': \"from\" and \"to\" differ");
                break;
            case Op::Curve: {
                const json points = n.value("points", json::array());
                if (!points.is_array() || points.size() < 2) {
                    problems.push_back("node '" + id + "': a curve has two points or more");
                    break;
                }
                for (const json& p : points) {
                    if (!p.is_array() || p.size() != 2 || !p[0].is_number() || !p[1].is_number()) {
                        problems.push_back("node '" + id + "': points are [x, y]");
                        break;
                    }
                    const glm::dvec2 q(p[0].get<double>(), p[1].get<double>());
                    if (!node.points.empty() && !(q.x > node.points.back().x))
                        problems.push_back("node '" + id + "': the points' x increase");
                    node.points.push_back(q);
                }
                break;
            }
            case Op::Terrace:
                node.a = number(n, id, "step", 10.0, 1e-3, 1e6, true);
                node.b = number(n, id, "sharpness", 1.0, 0.0, 1.0);
                break;
            case Op::Smooth:
                node.a = number(n, id, "radius", 10.0, 0.01, 2000.0, true);
                break;
            case Op::Erode:
                node.a = number(n, id, "radius", 10.0, 0.01, 2000.0, true);
                node.b = number(n, id, "strength", 0.5, 0.0, 1.0);
                break;
            default:
                break;
        }
        out.push_back(node);
        state[id] = 2;
        index[id] = int(out.size()) - 1;
        if (int(out.size()) > kMaxGraphNodes && problems.empty())
            problems.push_back("a graph has at most " + std::to_string(kMaxGraphNodes) + " nodes");
        return index[id];
    }
};

}  // namespace

glm::dvec3 graph_noise_position(const Shape& s, const GraphPlace& p) {
    if (s.kind() == Shape::Kind::FlatCylinder) {
        // On the circle it would be if it were curved, so its edges meet (as Terrain does).
        const glm::dvec2 size = s.face_size();
        const double r = size.x / kTwoPi;
        const double angle = kTwoPi * wrap01(p.u);
        return {r * det::sin(angle), -(r * det::cos(angle)), (p.v - 0.5) * size.y};
    }
    return s.position({p.face, p.u, p.v, 0.0});
}

GraphPlace graph_offset(const Shape& s, const GraphPlace& p, double east, double north) {
    const glm::dvec2 size = s.face_size();
    switch (s.kind()) {
        case Shape::Kind::BoundedArea:
        case Shape::Kind::FlatCylinder:
        case Shape::Kind::CurvedCylinder:
            return {p.face, p.u + east / size.x, p.v + north / size.y};
        default: {
            // Along a frame from the point's direction (not its face's u and v, which turn at
            // a face's edge), back onto the sphere.
            const glm::dvec3 d = cube_direction(p.face, p.u, p.v);
            const glm::dvec3 axis =
                std::abs(d.y) < 0.9 ? glm::dvec3(0.0, 1.0, 0.0) : glm::dvec3(1.0, 0.0, 0.0);
            glm::dvec3 e(axis.y * d.z - axis.z * d.y, axis.z * d.x - axis.x * d.z,
                         axis.x * d.y - axis.y * d.x);
            const double el = det::sqrt(e.x * e.x + e.y * e.y + e.z * e.z);
            e = glm::dvec3(e.x / el, e.y / el, e.z / el);
            const glm::dvec3 n(d.y * e.z - d.z * e.y, d.z * e.x - d.x * e.z, d.x * e.y - d.y * e.x);
            const double r = s.radius();
            const glm::dvec3 q(d.x * r + e.x * east + n.x * north,
                               d.y * r + e.y * east + n.y * north,
                               d.z * r + e.z * east + n.z * north);
            const double ql = det::sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
            const SurfacePoint sp = cube_surface({q.x / ql, q.y / ql, q.z / ql});
            return {sp.face, sp.u, sp.v};
        }
    }
}

std::vector<std::string> graph_problems(const json& j) {
    if (!j.is_object() || !j.contains("nodes") || !j.at("nodes").is_object())
        return {"a graph is {\"nodes\": {...}, \"output\": id}"};
    if (!j.contains("output") || !j.at("output").is_string())
        return {"a graph's \"output\" is a node id"};
    Builder b{j.at("nodes"), {}, {}, {}, {}};
    b.build(j.at("output").get<std::string>(), 0);
    return b.problems;
}

WorldGraph WorldGraph::from_json(const json& j) {
    const std::vector<std::string> problems = graph_problems(j);
    if (!problems.empty()) throw std::runtime_error("world graph: " + problems.front());
    Builder b{j.at("nodes"), {}, {}, {}, {}};
    WorldGraph g;
    g.output_ = b.build(j.at("output").get<std::string>(), 0);
    g.nodes_ = std::move(b.out);
    return g;
}

bool WorldGraph::uses_neighbours() const {
    for (const Node& n : nodes_)
        if (n.op == Op::Smooth || n.op == Op::Erode) return true;
    return false;
}

double WorldGraph::evaluate(const Shape& shape, std::uint64_t seed, const GraphPlace& place) const {
    std::vector<double> memo(nodes_.size(), 0.0);
    std::vector<char> done(nodes_.size(), 0);
    return eval(output_, shape, seed, place, memo, done);
}

double WorldGraph::around(int node, const Shape& shape, std::uint64_t seed, const GraphPlace& p,
                          double radius) const {
    const GraphPlace places[4] = {
        graph_offset(shape, p, radius, 0.0), graph_offset(shape, p, -radius, 0.0),
        graph_offset(shape, p, 0.0, radius), graph_offset(shape, p, 0.0, -radius)};
    double sum = 0.0;
    for (const GraphPlace& q : places) {
        std::vector<double> memo(nodes_.size(), 0.0);
        std::vector<char> done(nodes_.size(), 0);
        sum = sum + eval(node, shape, seed, q, memo, done);
    }
    return sum * 0.25;
}

double WorldGraph::eval(int i, const Shape& shape, std::uint64_t seed, const GraphPlace& p,
                        std::vector<double>& memo, std::vector<char>& done) const {
    if (done[size_t(i)]) return memo[size_t(i)];
    const Node& n = nodes_[size_t(i)];
    const auto in = [&](size_t k) { return eval(n.in[k], shape, seed, p, memo, done); };
    double r = 0.0;
    switch (n.op) {
        case Op::Noise:
        case Op::Ridged: {
            const glm::dvec3 q = graph_noise_position(shape, p);
            const double noise =
                double(det::fbm(seed ^ n.salt, q.x / n.a, q.y / n.a, q.z / n.a, n.octaves)) /
                65536.0;
            r = n.op == Op::Noise ? noise : 1.0 - std::abs(noise);
            break;
        }
        case Op::Constant:
            r = n.a;
            break;
        case Op::Input: {
            const glm::dvec2 size = shape.face_size();
            switch (n.input) {
                case InputName::Latitude:
                    if (shape.kind() == Shape::Kind::SphereOutside ||
                        shape.kind() == Shape::Kind::SphereInside) {
                        const glm::dvec3 d = cube_direction(p.face, p.u, p.v);
                        r = det::atan2(d.y, det::sqrt(d.x * d.x + d.z * d.z)) * kDegrees;
                    }
                    break;
                case InputName::EdgeDistance:
                    r = shape.kind() == Shape::Kind::BoundedArea
                            ? std::min(std::min(p.u * size.x, (1.0 - p.u) * size.x),
                                       std::min(p.v * size.y, (1.0 - p.v) * size.y))
                            : kFar;
                    break;
                case InputName::EndDistance:
                    r = shape.kind() == Shape::Kind::FlatCylinder ||
                                shape.kind() == Shape::Kind::CurvedCylinder
                            ? std::min(p.v * size.y, (1.0 - p.v) * size.y)
                            : kFar;
                    break;
            }
            break;
        }
        case Op::Add:
            r = in(0);
            for (size_t k = 1; k < n.in.size(); ++k) r = r + in(k);
            break;
        case Op::Multiply:
            r = in(0);
            for (size_t k = 1; k < n.in.size(); ++k) r = r * in(k);
            break;
        case Op::Min:
            r = in(0);
            for (size_t k = 1; k < n.in.size(); ++k) r = std::min(r, in(k));
            break;
        case Op::Max:
            r = in(0);
            for (size_t k = 1; k < n.in.size(); ++k) r = std::max(r, in(k));
            break;
        case Op::Subtract:
            r = in(0) - in(1);
            break;
        case Op::Divide: {
            const double b = in(1);
            r = b == 0.0 ? 0.0 : in(0) / b;
            break;
        }
        case Op::Lerp: {
            const double a = in(0), b = in(1), t = in(2);
            r = a + (b - a) * t;
            break;
        }
        case Op::Abs:
            r = std::abs(in(0));
            break;
        case Op::Clamp:
            r = std::min(std::max(in(0), n.a), n.b);
            break;
        case Op::Smoothstep:
            r = smooth01((in(0) - n.a) / (n.b - n.a));
            break;
        case Op::Curve: {
            const double x = in(0);
            if (x <= n.points.front().x) {
                r = n.points.front().y;
            } else if (x >= n.points.back().x) {
                r = n.points.back().y;
            } else {
                size_t k = 0;
                while (!(x < n.points[k + 1].x)) ++k;
                const glm::dvec2 a = n.points[k], b = n.points[k + 1];
                r = a.y + (b.y - a.y) * ((x - a.x) / (b.x - a.x));
            }
            break;
        }
        case Op::Terrace: {
            const double s = in(0) / n.a;
            const double k = std::floor(s);
            const double f = s - k;
            const double g = f * f * (3.0 - 2.0 * f);
            r = (k + (f + (g - f) * n.b)) * n.a;
            break;
        }
        case Op::Smooth:
            r = around(n.in[0], shape, seed, p, n.a);
            break;
        case Op::Erode: {
            const double x = in(0);
            r = x - n.b * std::max(0.0, x - around(n.in[0], shape, seed, p, n.a));
            break;
        }
    }
    memo[size_t(i)] = r;
    done[size_t(i)] = 1;
    return r;
}

std::uint64_t worldgen_fingerprint() {
    // tools/reference/worldgen.py's fingerprint(): its "everything" graph, bodies and places.
    const WorldGraph graph = WorldGraph::from_json(json::parse(
        R"({"output": "out", "nodes": {"n": {"op": "noise", "size": 300, "octaves": 4, "salt": "base"}, "r": {"op": "ridged", "size": 900, "octaves": 3, "salt": "ridges"}, "lat": {"op": "input", "name": "latitude"}, "edge": {"op": "input", "name": "edge_distance"}, "end": {"op": "input", "name": "end_distance"}, "near_edge": {"op": "smoothstep", "in": ["edge"], "from": 0, "to": 400}, "near_end": {"op": "smoothstep", "in": ["end"], "from": 0, "to": 300}, "cold": {"op": "curve", "in": ["abs_lat"], "points": [[0, 0], [45, 10], [90, 40]]}, "abs_lat": {"op": "abs", "in": ["lat"]}, "land": {"op": "lerp", "in": ["n", "r", 0.4]}, "scaled": {"op": "multiply", "in": [50, "land", "near_edge", "near_end"]}, "stepped": {"op": "terrace", "in": ["scaled"], "step": 6, "sharpness": 0.7}, "smoothed": {"op": "smooth", "in": ["stepped"], "radius": 12}, "worn": {"op": "erode", "in": ["smoothed"], "radius": 20, "strength": 0.6}, "sum": {"op": "add", "in": ["worn", "cold", 3]}, "low": {"op": "min", "in": ["sum", 70]}, "high": {"op": "max", "in": ["low", -25]}, "clamped": {"op": "clamp", "in": ["high"], "min": -20, "max": 65}, "diff": {"op": "subtract", "in": ["clamped", "n"]}, "out": {"op": "divide", "in": ["diff", 1.5]}}})"));
    const Shape shapes[] = {
        Shape::bounded_area(3000.0, 2000.0, 9.81), Shape::flat_cylinder(6000.0, 5000.0, 9.81),
        Shape::curved_cylinder(1000.0, 5000.0, 9.81), Shape::sphere_outside(3000.0, 9.81),
        Shape::sphere_inside(2500.0, 9.81)};
    const std::vector<GraphPlace> places = {
        {0, 0.5, 0.5}, {0, 0.013, 0.97}, {0, 0.731, 0.204}, {0, 0.999, 0.5}, {0, 0.25, 0.0005}};
    std::vector<GraphPlace> sphere_places;
    for (int f = 0; f < 6; ++f) sphere_places.push_back({f, 0.37, 0.61});
    sphere_places.push_back({2, 0.0, 0.0});
    sphere_places.push_back({4, 1.0, 0.5});
    Fnv1a64 h;
    for (const Shape& shape : shapes) {
        const bool sphere =
            shape.kind() == Shape::Kind::SphereOutside || shape.kind() == Shape::Kind::SphereInside;
        for (const GraphPlace& p : sphere ? sphere_places : places) {
            const double value = graph.evaluate(shape, 1234, p);
            std::uint64_t bits = 0;
            std::memcpy(&bits, &value, 8);
            unsigned char b[8];
            for (int i = 0; i < 8; ++i) b[i] = static_cast<unsigned char>(bits >> (8 * i));
            h.add(b, 8);
        }
    }
    return h.value();
}

}  // namespace midgard
