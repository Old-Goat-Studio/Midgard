// SPDX-License-Identifier: Apache-2.0
#include "midgard/terrain.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "midgard/det.h"
#include "midgard/hash.h"

namespace midgard {

namespace {

constexpr double kTwoPi = 6.283185307179586;

double wrap01(double u) {
    return u - std::floor(u);
}

double smooth01(double t) {
    t = std::min(std::max(t, 0.0), 1.0);
    return t * t * (3.0 - 2.0 * t);
}

bool wraps(Shape::Kind kind) {
    return kind == Shape::Kind::FlatCylinder || kind == Shape::Kind::CurvedCylinder;
}

bool sphere(Shape::Kind kind) {
    return kind == Shape::Kind::SphereOutside || kind == Shape::Kind::SphereInside;
}

int roots_along(double size) {
    return std::max(1, int(std::lround(size / kRootSize)));
}

// A sphere's faces have a power of two of roots a side, the nearest to kRootSize metres each
// (in proportion), so every grid point is a power-of-two fraction of the face and the points on
// face edges come out bit-identical from both faces.
int sphere_roots(double size) {
    constexpr double kSqrt2 = 1.4142135623730951;
    int roots = 1;
    while (roots < (1 << 16) && 2.0 * roots <= size / kRootSize * kSqrt2) roots *= 2;
    return roots;
}

}  // namespace

const char* wall_look_name(WallLook look) {
    switch (look) {
        case WallLook::Cliff:
            return "cliff";
        case WallLook::Fortification:
            return "fortification";
        case WallLook::Barrier:
            return "barrier";
    }
    return "cliff";
}

TerrainSettings terrain_from_json(const nlohmann::json& j) {
    if (!j.is_object()) throw std::runtime_error("terrain settings are a JSON object");
    TerrainSettings t;
    const auto number = [&](const char* key, double& into) {
        if (!j.contains(key)) return;
        if (!j.at(key).is_number())
            throw std::runtime_error(std::string("terrain: \"") + key + "\" is a number");
        into = j.at(key).get<double>();
    };
    if (j.contains("seed")) {
        if (!j.at("seed").is_number_integer())
            throw std::runtime_error("terrain: the seed is a whole number");
        t.seed = j.at("seed").get<std::uint64_t>();
    }
    number("base", t.base);
    number("height_scale", t.height_scale);
    number("feature_size", t.feature_size);
    if (j.contains("octaves")) {
        if (!j.at("octaves").is_number_integer())
            throw std::runtime_error("terrain: \"octaves\" is a whole number");
        t.octaves = j.at("octaves").get<int>();
    }
    number("sea_level", t.sea_level);
    number("ocean_band", t.ocean_band);
    number("ocean_depth", t.ocean_depth);
    number("end_ramp", t.end_ramp);
    number("wall_height", t.wall_height);
    number("spacing", t.spacing);
    number("snow_line", t.snow_line);
    if (j.contains("wall")) {
        const std::string wall = j.at("wall").is_string() ? j.at("wall").get<std::string>() : "";
        if (wall == "cliff")
            t.wall = WallLook::Cliff;
        else if (wall == "fortification")
            t.wall = WallLook::Fortification;
        else if (wall == "barrier")
            t.wall = WallLook::Barrier;
        else
            throw std::runtime_error(
                "terrain: \"wall\" is \"cliff\", \"fortification\" or \"barrier\"");
    }
    return t;
}

nlohmann::json terrain_to_json(const TerrainSettings& t) {
    nlohmann::json j{{"base", t.base},
                     {"height_scale", t.height_scale},
                     {"feature_size", t.feature_size},
                     {"octaves", t.octaves},
                     {"sea_level", t.sea_level},
                     {"ocean_band", t.ocean_band},
                     {"ocean_depth", t.ocean_depth},
                     {"end_ramp", t.end_ramp},
                     {"wall_height", t.wall_height},
                     {"wall", wall_look_name(t.wall)},
                     {"spacing", t.spacing},
                     {"snow_line", t.snow_line}};
    if (t.seed) j["seed"] = *t.seed;
    return j;
}

std::vector<std::string> terrain_problems(const TerrainSettings& t) {
    std::vector<std::string> out;
    const auto finite = [](double v) { return std::isfinite(v); };
    if (!finite(t.base) || !finite(t.height_scale) || !finite(t.sea_level) ||
        !finite(t.ocean_depth) || !finite(t.wall_height) || !finite(t.snow_line))
        out.push_back("terrain: heights are numbers");
    if (!(t.height_scale >= 0.0 && t.height_scale <= 5000.0))
        out.push_back("terrain: the height scale is from 0 to 5000 m");
    if (!(t.feature_size >= 1.0 && t.feature_size <= 100000.0))
        out.push_back("terrain: the feature size is from 1 to 100,000 m");
    if (t.octaves < 1 || t.octaves > 12) out.push_back("terrain: octaves are from 1 to 12");
    if (!(t.ocean_band >= 0.0)) out.push_back("terrain: the ocean band can't be negative");
    if (!(t.ocean_depth >= 0.0)) out.push_back("terrain: the ocean's depth can't be negative");
    if (!(t.end_ramp >= 0.0)) out.push_back("terrain: the end ramp can't be negative");
    if (!(t.spacing >= 0.25 && t.spacing <= 64.0))
        out.push_back("terrain: the grid spacing is from 0.25 to 64 m");
    return out;
}

const char* stamp_tool_name(StampTool tool) {
    switch (tool) {
        case StampTool::Raise:
            return "raise";
        case StampTool::Lower:
            return "lower";
        case StampTool::Smooth:
            return "smooth";
        case StampTool::Flatten:
            return "flatten";
        case StampTool::Paint:
            return "paint";
    }
    return "raise";
}

const char* paint_layer_name(PaintLayer layer) {
    switch (layer) {
        case PaintLayer::Clear:
            return "clear";
        case PaintLayer::Lush:
            return "lush";
        case PaintLayer::Dry:
            return "dry";
        case PaintLayer::Sand:
            return "sand";
        case PaintLayer::Rock:
            return "rock";
    }
    return "clear";
}

TerrainStamp stamp_from_json(const nlohmann::json& j) {
    if (!j.is_object()) throw std::runtime_error("a terrain stamp is a JSON object");
    TerrainStamp s;
    const std::string tool = j.value("tool", std::string("raise"));
    bool known = false;
    for (StampTool t : {StampTool::Raise, StampTool::Lower, StampTool::Smooth, StampTool::Flatten,
                        StampTool::Paint})
        if (tool == stamp_tool_name(t)) {
            s.tool = t;
            known = true;
        }
    if (!known) throw std::runtime_error("terrain stamp: no tool '" + tool + "'");
    const std::string paint = j.value("paint", std::string("lush"));
    known = false;
    for (PaintLayer l :
         {PaintLayer::Clear, PaintLayer::Lush, PaintLayer::Dry, PaintLayer::Sand, PaintLayer::Rock})
        if (paint == paint_layer_name(l)) {
            s.paint = l;
            known = true;
        }
    if (!known) throw std::runtime_error("terrain stamp: no paint '" + paint + "'");
    const auto number = [&](const char* key, double& into) {
        if (!j.contains(key)) return;
        if (!j.at(key).is_number())
            throw std::runtime_error(std::string("terrain stamp: '") + key + "' is a number");
        into = j.at(key).get<double>();
    };
    number("radius", s.radius);
    number("strength", s.strength);
    number("height", s.height);
    s.face = j.value("face", 0);
    if (!j.contains("points") || !j.at("points").is_array())
        throw std::runtime_error("terrain stamp: 'points' is a list of [u, v]");
    for (const nlohmann::json& p : j.at("points")) {
        if (!p.is_array() || p.size() != 2 || !p[0].is_number() || !p[1].is_number())
            throw std::runtime_error("terrain stamp: a point is [u, v]");
        s.points.emplace_back(p[0].get<double>(), p[1].get<double>());
    }
    return s;
}

nlohmann::json stamp_to_json(const TerrainStamp& s) {
    nlohmann::json points = nlohmann::json::array();
    for (const glm::dvec2& p : s.points) points.push_back({p.x, p.y});
    nlohmann::json j{{"tool", stamp_tool_name(s.tool)},
                     {"radius", s.radius},
                     {"strength", s.strength},
                     {"face", s.face},
                     {"points", points}};
    if (s.tool == StampTool::Paint) j["paint"] = paint_layer_name(s.paint);
    if (s.tool == StampTool::Flatten) j["height"] = s.height;
    return j;
}

std::vector<std::string> stamp_problems(const TerrainStamp& s) {
    std::vector<std::string> out;
    if (!(s.radius > 0.0 && s.radius <= 2000.0))
        out.push_back("terrain stamp: the radius is above 0 and at most 2000 m");
    const bool metres = s.tool == StampTool::Raise || s.tool == StampTool::Lower;
    if (metres ? !(s.strength >= 0.0 && s.strength <= 1000.0)
               : !(s.strength >= 0.0 && s.strength <= 1.0))
        out.push_back(metres ? "terrain stamp: raising and lowering is 0 to 1000 m"
                             : "terrain stamp: the strength is 0 to 1");
    if (s.points.empty()) out.push_back("terrain stamp: a stroke has a point at least");
    for (const glm::dvec2& p : s.points)
        if (!std::isfinite(p.x) || !std::isfinite(p.y)) {
            out.push_back("terrain stamp: points are numbers");
            break;
        }
    if (!std::isfinite(s.height)) out.push_back("terrain stamp: the height is a number");
    return out;
}

Terrain::Terrain(const Body& body, const TerrainSettings& settings,
                 std::vector<TerrainStamp> stamps)
    : body_(body),
      settings_(settings),
      stamps_(std::move(stamps)),
      seed_(settings.seed.value_or(body.seed)) {
    const glm::dvec2 size = body_.shape.face_size();
    if (sphere(body_.shape.kind())) {
        roots_u_ = roots_v_ = sphere_roots(size.x);
    } else {
        roots_u_ = roots_along(size.x);
        roots_v_ = roots_along(size.y);
    }
    const double root = std::max(size.x / roots_u_, size.y / roots_v_);
    const double spacing = std::max(settings_.spacing, 0.25);
    while (max_level_ < kMaxTerrainLevel &&
           root / (double(std::int64_t(1) << max_level_) * kChunkQuads) > spacing)
        ++max_level_;
    for (const TerrainStamp& s : stamps_) bounds_.push_back(stamp_bounds(s));
}

std::int64_t Terrain::points_u(int level) const {
    return std::int64_t(roots_u_) * (std::int64_t(1) << level) * kChunkQuads;
}

std::int64_t Terrain::points_v(int level) const {
    return std::int64_t(roots_v_) * (std::int64_t(1) << level) * kChunkQuads;
}

bool Terrain::valid(const ChunkKey& k) const {
    return k.face >= 0 && k.face < body_.shape.faces() && k.level >= 0 && k.level <= max_level_ &&
           k.x >= 0 && k.y >= 0 && std::int64_t(k.x) < std::int64_t(roots_u_) << k.level &&
           std::int64_t(k.y) < std::int64_t(roots_v_) << k.level;
}

std::array<ChunkKey, 4> Terrain::children(const ChunkKey& k) const {
    const int l = k.level + 1, x = k.x * 2, y = k.y * 2;
    return {ChunkKey{k.face, l, x, y}, ChunkKey{k.face, l, x + 1, y}, ChunkKey{k.face, l, x, y + 1},
            ChunkKey{k.face, l, x + 1, y + 1}};
}

ChunkKey Terrain::parent(const ChunkKey& k) const {
    return {k.face, std::max(k.level - 1, 0), k.x / 2, k.y / 2};
}

std::optional<ChunkKey> Terrain::neighbour(const ChunkKey& k, ChunkEdge edge) const {
    ChunkKey n = k;
    const int across_u = roots_u_ << k.level, across_v = roots_v_ << k.level;
    switch (edge) {
        case kWest:
            n.x -= 1;
            break;
        case kEast:
            n.x += 1;
            break;
        case kSouth:
            n.y -= 1;
            break;
        case kNorth:
            n.y += 1;
            break;
    }
    const bool off = n.x < 0 || n.x >= across_u || n.y < 0 || n.y >= across_v;
    if (off && sphere(body_.shape.kind())) {
        // Onto the face across that edge (square faces: across_u == across_v).
        const CubeLink link = cube_link(k.face, int(edge));
        int along = edge < kSouth ? k.y : k.x;
        if (link.reversed) along = across_u - 1 - along;
        const int in = link.edge % 2 == 0 ? 0 : across_u - 1;
        n.face = link.face;
        n.x = link.edge < kSouth ? in : along;
        n.y = link.edge < kSouth ? along : in;
        return n;
    }
    if (n.x < 0 || n.x >= across_u) {
        if (!wraps(body_.shape.kind())) return std::nullopt;
        n.x = (n.x + across_u) % across_u;
    }
    if (n.y < 0 || n.y >= across_v) return std::nullopt;
    return n;
}

glm::dvec3 Terrain::noise_position(int face, double u, double v) const {
    const Shape& s = body_.shape;
    if (s.kind() == Shape::Kind::FlatCylinder) {
        // On the circle it would be if it were curved, so its edges meet.
        const glm::dvec2 size = s.face_size();
        const double r = size.x / kTwoPi;
        const double angle = kTwoPi * wrap01(u);
        return {r * det::sin(angle), -(r * det::cos(angle)), (v - 0.5) * size.y};
    }
    return s.position({face, u, v, 0.0});
}

Terrain::StampBounds Terrain::stamp_bounds(const TerrainStamp& s) const {
    const glm::dvec2 size = body_.shape.face_size();
    StampBounds b{1e300, -1e300, 1e300, -1e300};
    for (const glm::dvec2& p : s.points) {
        b.u0 = std::min(b.u0, p.x);
        b.u1 = std::max(b.u1, p.x);
        b.v0 = std::min(b.v0, p.y);
        b.v1 = std::max(b.v1, p.y);
    }
    const double ru = s.radius / size.x, rv = s.radius / size.y;
    b.u0 -= ru;
    b.u1 += ru;
    b.v0 -= rv;
    b.v1 += rv;
    if (wraps(body_.shape.kind())) {  // round the cylinder: any u
        b.u0 = -1e300;
        b.u1 = 1e300;
    }
    return b;
}

bool Terrain::touches(const ChunkKey& k, const TerrainStamp& s) const {
    if (s.face != k.face) return false;
    const StampBounds b = stamp_bounds(s);
    const double u0 = u_at(k, -1), u1 = u_at(k, kChunkQuads + 1);
    const double v0 = v_at(k, -1), v1 = v_at(k, kChunkQuads + 1);
    return b.u0 <= u1 && b.u1 >= u0 && b.v0 <= v1 && b.v1 >= v0;
}

double Terrain::stamp_weight(const TerrainStamp& s, int face, double u, double v) const {
    if (s.face != face || s.points.empty() || !(s.radius > 0.0)) return 0.0;
    const glm::dvec2 size = body_.shape.face_size();
    const bool round = wraps(body_.shape.kind());
    // The stroke's points in metres from (u, v) (round a cylinder, the nearer way).
    const auto metres = [&](const glm::dvec2& p) {
        double du = p.x - u;
        if (round) du = du - std::floor(du + 0.5);
        return glm::dvec2(du * size.x, (p.y - v) * size.y);
    };
    glm::dvec2 a = metres(s.points[0]);
    double best = a.x * a.x + a.y * a.y;  // the distance squared to the stroke's line
    for (size_t i = 1; i < s.points.size(); ++i) {
        const glm::dvec2 b = metres(s.points[i]);
        const double ex = b.x - a.x, ey = b.y - a.y;
        const double len2 = ex * ex + ey * ey;
        double t = len2 > 0.0 ? -(a.x * ex + a.y * ey) / len2 : 0.0;
        t = std::min(std::max(t, 0.0), 1.0);
        const double cx = a.x + ex * t, cy = a.y + ey * t;
        best = std::min(best, cx * cx + cy * cy);
        a = b;
    }
    return smooth01(1.0 - det::sqrt(best) / s.radius);
}

double Terrain::height(int face, double u, double v) const {
    double h = base_height(face, u, v);
    for (size_t i = 0; i < stamps_.size(); ++i) {
        const TerrainStamp& s = stamps_[i];
        const StampBounds& b = bounds_[i];
        if (s.tool == StampTool::Paint || s.face != face || u < b.u0 || u > b.u1 || v < b.v0 ||
            v > b.v1)
            continue;
        const double w = stamp_weight(s, face, u, v);
        if (w <= 0.0) continue;
        switch (s.tool) {
            case StampTool::Raise:
                h = h + s.strength * w;
                break;
            case StampTool::Lower:
                h = h - s.strength * w;
                break;
            case StampTool::Flatten:
                h = h + (s.height - h) * (w * s.strength);
                break;
            case StampTool::Smooth: {
                const glm::dvec2 size = body_.shape.face_size();
                const double du = 0.5 * s.radius / size.x, dv = 0.5 * s.radius / size.y;
                const double average =
                    (base_height(face, u + du, v) + base_height(face, u - du, v) +
                     base_height(face, u, v + dv) + base_height(face, u, v - dv)) *
                    0.25;
                h = h + (average - h) * (w * s.strength);
                break;
            }
            case StampTool::Paint:
                break;
        }
    }
    return h;
}

std::optional<glm::dvec3> Terrain::raycast(const glm::dvec3& origin, const glm::dvec3& direction,
                                           double distance) const {
    const glm::dvec3 d = glm::normalize(direction);
    if (above_ground(origin) < 0.0) return std::nullopt;  // starting under the ground
    double t = 0.0;
    while (t < distance) {
        const double next = std::min(t + std::max(0.5, t * 0.01), distance);
        if (above_ground(origin + d * next) < 0.0) {  // crossed: narrow it down
            double lo = t, hi = next;
            for (int i = 0; i < 30; ++i) {
                const double mid = 0.5 * (lo + hi);
                (above_ground(origin + d * mid) < 0.0 ? hi : lo) = mid;
            }
            return origin + d * hi;
        }
        t = next;
    }
    return std::nullopt;
}

double Terrain::base_height(int face, double u, double v) const {
    const TerrainSettings& t = settings_;
    const glm::dvec3 q = noise_position(face, u, v);
    const double f = t.feature_size;
    const det::Fixed n = det::fbm(seed_, q.x / f, q.y / f, q.z / f, t.octaves);
    double h = t.base + t.height_scale * (double(n) / 65536.0);
    const Shape::Kind kind = body_.shape.kind();
    const glm::dvec2 size = body_.shape.face_size();
    if (kind == Shape::Kind::BoundedArea) {
        if (t.ocean_band > 0.0) {
            const double d = std::min(std::min(u * size.x, (1.0 - u) * size.x),
                                      std::min(v * size.y, (1.0 - v) * size.y));
            const double s = smooth01((d - t.ocean_band) / t.ocean_band);
            h = -t.ocean_depth + (h + t.ocean_depth) * s;
        }
    } else if (wraps(kind)) {
        if (t.end_ramp > 0.0) {
            const double e = std::min(v * size.y, (1.0 - v) * size.y);
            const double s = smooth01(1.0 - e / t.end_ramp);
            h = h + (t.wall_height - h) * s;
        }
    }
    return h;
}

glm::dvec3 Terrain::ground(const glm::dvec3& p) const {
    const SurfacePoint s = body_.surface(p);
    glm::dvec3 g = position(s.face, s.u, s.v, height(s.face, s.u, s.v));
    if (body_.shape.kind() == Shape::Kind::FlatCylinder) {  // the copy nearest the point
        const double c = body_.shape.face_size().x;
        g.x += std::round((p.x - g.x) / c) * c;
    }
    return g;
}

double Terrain::above_ground(const glm::dvec3& p) const {
    const SurfacePoint s = body_.surface(p);
    return s.height - height(s.face, s.u, s.v);
}

glm::dvec3 Terrain::position(int face, double u, double v, double h) const {
    const Shape& s = body_.shape;
    if (s.kind() == Shape::Kind::BoundedArea || s.kind() == Shape::Kind::FlatCylinder) {
        const glm::dvec2 size = s.face_size();
        return body_.origin + glm::dvec3((u - 0.5) * size.x, h, (v - 0.5) * size.y);
    }
    return body_.position({face, u, v, h});
}

double Terrain::u_at(const ChunkKey& k, int i) const {
    return double(std::int64_t(k.x) * kChunkQuads + i) / double(points_u(k.level));
}

double Terrain::v_at(const ChunkKey& k, int j) const {
    return double(std::int64_t(k.y) * kChunkQuads + j) / double(points_v(k.level));
}

glm::vec3 Terrain::biome_at(int face, double u, double v) const {
    const glm::dvec3 q = noise_position(face, u, v);
    const double moist = double(det::fbm(seed_ ^ det::tag("moisture"), q.x / 1500.0, q.y / 1500.0,
                                         q.z / 1500.0, 4)) /
                         65536.0;
    const double detail =
        double(det::gradient_noise(seed_ ^ det::tag("detail"), q.x / 7.0, q.y / 7.0, q.z / 7.0)) /
        65536.0;
    const double natural = std::clamp(0.5 + 0.7 * moist, 0.0, 1.0);
    double moisture = natural, ground = 0.0;
    for (size_t i = 0; i < stamps_.size(); ++i) {
        const TerrainStamp& s = stamps_[i];
        const StampBounds& b = bounds_[i];
        if (s.tool != StampTool::Paint || s.face != face || u < b.u0 || u > b.u1 || v < b.v0 ||
            v > b.v1)
            continue;
        const double w = stamp_weight(s, face, u, v) * std::clamp(s.strength, 0.0, 1.0);
        if (w <= 0.0) continue;
        switch (s.paint) {
            case PaintLayer::Lush:
                moisture += (1.0 - moisture) * w;
                break;
            case PaintLayer::Dry:
                moisture -= moisture * w;
                break;
            case PaintLayer::Sand:
                ground += (-1.0 - ground) * w;
                break;
            case PaintLayer::Rock:
                ground += (1.0 - ground) * w;
                break;
            case PaintLayer::Clear:
                moisture += (natural - moisture) * w;
                ground -= ground * w;
                break;
        }
    }
    return {float(moisture), float(std::clamp(detail, -1.0, 1.0)), float(ground)};
}

bool Terrain::flipped(const ChunkKey& k) const {
    // Across the chunk's middle: from v's step to u's, crossed, against up there.
    const double u = u_at(k, kChunkQuads / 2), v = v_at(k, kChunkQuads / 2);
    const double du = 1.0 / double(points_u(k.level)), dv = 1.0 / double(points_v(k.level));
    const glm::dvec3 p = position(k.face, u, v, 0.0);
    const glm::dvec3 c =
        glm::cross(position(k.face, u, v + dv, 0.0) - p, position(k.face, u + du, v, 0.0) - p);
    return glm::dot(c, body_.up(p)) < 0.0;
}

glm::dvec3 Terrain::chunk_origin(const ChunkKey& k) const {
    return position(k.face, u_at(k, kChunkQuads / 2), v_at(k, kChunkQuads / 2), 0.0);
}

ChunkMesh Terrain::mesh(const ChunkKey& k, const std::array<bool, 4>& coarser) const {
    constexpr int n = kChunkQuads;
    constexpr int g = n + 3;  // with a border of one point all round, for normals
    std::vector<glm::dvec3> p(size_t(g) * g);
    ChunkMesh out;
    out.heights.resize(size_t(n + 1) * (n + 1));
    const bool round = sphere(body_.shape.kind());
    for (int j = -1; j <= n + 1; ++j) {
        const double v = v_at(k, j);
        for (int i = -1; i <= n + 1; ++i) {
            const double u = u_at(k, i);
            // On a sphere, the border past a face's edge is the face across it (the same grid
            // points, so normals match there). A cube corner's border point is never used.
            SurfacePoint s{k.face, u, v, 0.0};
            if (round && (u < 0.0 || u > 1.0) != (v < 0.0 || v > 1.0)) s = cube_across(s);
            const double h = height(s.face, s.u, s.v);
            p[size_t(j + 1) * g + size_t(i + 1)] = position(s.face, s.u, s.v, h);
            if (i >= 0 && i <= n && j >= 0 && j <= n) out.heights[size_t(j) * (n + 1) + i] = h;
        }
    }
    const auto at = [&](int i, int j) -> glm::dvec3& { return p[size_t(j + 1) * g + (i + 1)]; };
    // Which way round the grid turns, seen from above: u then v turn clockwise on a flat map
    // (x then z, seen from +y), but the other way on the outside of a sphere.
    const bool flip = flipped(k);
    std::vector<glm::dvec3> normal(size_t(n + 1) * (n + 1));
    for (int j = 0; j <= n; ++j)
        for (int i = 0; i <= n; ++i) {
            const glm::dvec3 c =
                glm::cross(at(i, j + 1) - at(i, j - 1), at(i + 1, j) - at(i - 1, j));
            normal[size_t(j) * (n + 1) + i] = glm::normalize(flip ? -c : c);
        }
    // Onto a coarser neighbour's edge: the points between its points move to the midpoint.
    const auto stitch = [&](int i0, int j0, int di, int dj) {
        for (int s = 1; s < n; s += 2) {
            const int i = i0 + di * s, j = j0 + dj * s;
            at(i, j) = (at(i - di, j - dj) + at(i + di, j + dj)) * 0.5;
            glm::dvec3& nm = normal[size_t(j) * (n + 1) + i];
            nm = glm::normalize(normal[size_t(j - dj) * (n + 1) + (i - di)] +
                                normal[size_t(j + dj) * (n + 1) + (i + di)]);
        }
    };
    if (coarser[kWest]) stitch(0, 0, 0, 1);
    if (coarser[kEast]) stitch(n, 0, 0, 1);
    if (coarser[kSouth]) stitch(0, 0, 1, 0);
    if (coarser[kNorth]) stitch(0, n, 1, 0);

    out.origin = chunk_origin(k);
    out.positions.reserve(size_t(n + 1) * (n + 1));
    out.normals.reserve(size_t(n + 1) * (n + 1));
    for (int j = 0; j <= n; ++j)
        for (int i = 0; i <= n; ++i) {
            out.positions.push_back(glm::vec3(at(i, j) - out.origin));
            out.normals.push_back(glm::vec3(normal[size_t(j) * (n + 1) + i]));
            out.ups.push_back(glm::vec3(body_.up(at(i, j))));
            out.biome.push_back(biome_at(k.face, u_at(k, i), v_at(k, j)));
            const glm::dvec2 size = body_.shape.face_size();
            out.uvs.emplace_back(float(u_at(k, i) * size.x / 16.0),
                                 float(v_at(k, j) * size.y / 16.0));
        }
    out.indices.reserve(size_t(n) * n * 6);
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            const std::uint32_t a = std::uint32_t(j * (n + 1) + i), b = a + (n + 1), c = a + 1,
                                d = b + 1;
            if (flip)
                out.indices.insert(out.indices.end(), {a, d, b, a, c, d});
            else
                out.indices.insert(out.indices.end(), {a, b, d, a, d, c});
        }
    return out;
}

std::optional<ChunkMesh> Terrain::water_mesh(const ChunkKey& k,
                                             const std::array<bool, 4>& coarser) const {
    constexpr int n = kChunkQuads;
    const double sea = settings_.sea_level;
    ChunkMesh out;
    out.origin = chunk_origin(k);
    std::vector<glm::dvec3> p(size_t(n + 1) * (n + 1));
    out.heights.resize(p.size());
    bool wet = false;
    const glm::dvec2 size = body_.shape.face_size();
    for (int j = 0; j <= n; ++j) {
        const double v = v_at(k, j);
        for (int i = 0; i <= n; ++i) {
            const double u = u_at(k, i);
            const size_t at = size_t(j) * (n + 1) + i;
            out.heights[at] = sea - height(k.face, u, v);
            wet = wet || out.heights[at] > 0.0;
            p[at] = position(k.face, u, v, sea);
            out.uvs.emplace_back(float(u * size.x / 16.0), float(v * size.y / 16.0));
        }
    }
    if (!wet) return std::nullopt;
    const bool flip = flipped(k);
    const auto stitch = [&](int i0, int j0, int di, int dj) {
        for (int s = 1; s < n; s += 2) {
            const size_t at = size_t(j0 + dj * s) * (n + 1) + (i0 + di * s);
            const size_t lo = size_t(j0 + dj * (s - 1)) * (n + 1) + (i0 + di * (s - 1));
            const size_t hi = size_t(j0 + dj * (s + 1)) * (n + 1) + (i0 + di * (s + 1));
            p[at] = (p[lo] + p[hi]) * 0.5;
            out.heights[at] = (out.heights[lo] + out.heights[hi]) * 0.5;
        }
    };
    if (coarser[kWest]) stitch(0, 0, 0, 1);
    if (coarser[kEast]) stitch(n, 0, 0, 1);
    if (coarser[kSouth]) stitch(0, 0, 1, 0);
    if (coarser[kNorth]) stitch(0, n, 1, 0);
    for (const glm::dvec3& q : p) {
        out.positions.push_back(glm::vec3(q - out.origin));
        out.normals.push_back(glm::vec3(body_.up(q)));
    }
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            const std::uint32_t a = std::uint32_t(j * (n + 1) + i), b = a + (n + 1), c = a + 1,
                                d = b + 1;
            if (flip)
                out.indices.insert(out.indices.end(), {a, d, b, a, c, d});
            else
                out.indices.insert(out.indices.end(), {a, b, d, a, d, c});
        }
    return out;
}

std::uint64_t Terrain::chunk_hash(const ChunkKey& k) const {
    constexpr int n = kChunkQuads;
    Fnv1a64 h;
    for (const std::int32_t v :
         {std::int32_t(k.face), std::int32_t(k.level), std::int32_t(k.x), std::int32_t(k.y)}) {
        unsigned char b[4];
        for (int i = 0; i < 4; ++i) b[i] = static_cast<unsigned char>(std::uint32_t(v) >> (8 * i));
        h.add(b, 4);
    }
    for (int j = 0; j <= n; ++j) {
        const double v = v_at(k, j);
        for (int i = 0; i <= n; ++i) {
            const double height_here = height(k.face, u_at(k, i), v);
            std::uint64_t bits = 0;
            std::memcpy(&bits, &height_here, 8);
            unsigned char b[8];
            for (int c = 0; c < 8; ++c) b[c] = static_cast<unsigned char>(bits >> (8 * c));
            h.add(b, 8);
        }
    }
    return h.value();
}

std::uint64_t terrain_fingerprint() {
    // The shapes and chunks of tools/reference/terrain.py's fingerprint().
    const auto body = [](const char* json_text) {
        return body_from_json(nlohmann::json::parse(json_text));
    };
    const Body bodies[] = {
        body(R"({"id": "area", "shape": "bounded_area", "width": 3000, "length": 2000,
                 "seed": 7})"),
        body(R"({"id": "strip", "shape": "flat_cylinder", "circumference": 6000,
                 "length": 5000, "seed": 8})"),
        body(R"({"id": "habitat", "shape": "curved_cylinder", "radius": 1000, "length": 5000,
                 "seed": 9})"),
        body(R"({"id": "planet", "shape": "sphere_outside", "radius": 1000, "seed": 10})"),
    };
    Fnv1a64 all;
    for (const Body& b : bodies) {
        const Terrain t(b, TerrainSettings{});
        for (const ChunkKey& k : {ChunkKey{0, 0, 0, 0}, ChunkKey{0, 2, 3, 1}}) {
            const std::uint64_t c = t.chunk_hash(k);
            unsigned char bytes[8];
            for (int i = 0; i < 8; ++i) bytes[i] = static_cast<unsigned char>(c >> (8 * i));
            all.add(bytes, 8);
        }
    }
    return all.value();
}

}  // namespace midgard
