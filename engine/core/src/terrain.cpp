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

int roots_along(double size) {
    return std::max(1, int(std::lround(size / kRootSize)));
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
                     {"spacing", t.spacing}};
    if (t.seed) j["seed"] = *t.seed;
    return j;
}

std::vector<std::string> terrain_problems(const TerrainSettings& t) {
    std::vector<std::string> out;
    const auto finite = [](double v) { return std::isfinite(v); };
    if (!finite(t.base) || !finite(t.height_scale) || !finite(t.sea_level) ||
        !finite(t.ocean_depth) || !finite(t.wall_height))
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

Terrain::Terrain(const Body& body, const TerrainSettings& settings)
    : body_(body), settings_(settings), seed_(settings.seed.value_or(body.seed)) {
    const glm::dvec2 size = body_.shape.face_size();
    roots_u_ = roots_along(size.x);
    roots_v_ = roots_along(size.y);
    const double root = std::max(size.x / roots_u_, size.y / roots_v_);
    const double spacing = std::max(settings_.spacing, 0.25);
    while (max_level_ < kMaxTerrainLevel &&
           root / (double(std::int64_t(1) << max_level_) * kChunkQuads) > spacing)
        ++max_level_;
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

double Terrain::height(int face, double u, double v) const {
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

glm::dvec3 Terrain::chunk_origin(const ChunkKey& k) const {
    return position(k.face, u_at(k, kChunkQuads / 2), v_at(k, kChunkQuads / 2), 0.0);
}

ChunkMesh Terrain::mesh(const ChunkKey& k, const std::array<bool, 4>& coarser) const {
    constexpr int n = kChunkQuads;
    constexpr int g = n + 3;  // with a border of one point all round, for normals
    std::vector<glm::dvec3> p(size_t(g) * g);
    ChunkMesh out;
    out.heights.resize(size_t(n + 1) * (n + 1));
    for (int j = -1; j <= n + 1; ++j) {
        const double v = v_at(k, j);
        for (int i = -1; i <= n + 1; ++i) {
            const double u = u_at(k, i);
            const double h = height(k.face, u, v);
            p[size_t(j + 1) * g + size_t(i + 1)] = position(k.face, u, v, h);
            if (i >= 0 && i <= n && j >= 0 && j <= n) out.heights[size_t(j) * (n + 1) + i] = h;
        }
    }
    const auto at = [&](int i, int j) -> glm::dvec3& { return p[size_t(j + 1) * g + (i + 1)]; };
    std::vector<glm::dvec3> normal(size_t(n + 1) * (n + 1));
    for (int j = 0; j <= n; ++j)
        for (int i = 0; i <= n; ++i)
            normal[size_t(j) * (n + 1) + i] = glm::normalize(
                glm::cross(at(i, j + 1) - at(i, j - 1), at(i + 1, j) - at(i - 1, j)));
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
            const glm::dvec2 size = body_.shape.face_size();
            out.uvs.emplace_back(float(u_at(k, i) * size.x / 16.0),
                                 float(v_at(k, j) * size.y / 16.0));
        }
    out.indices.reserve(size_t(n) * n * 6);
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            const std::uint32_t a = std::uint32_t(j * (n + 1) + i), b = a + (n + 1), c = a + 1,
                                d = b + 1;
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
