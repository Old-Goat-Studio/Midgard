// SPDX-License-Identifier: Apache-2.0
#include "midgard/world_map.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace midgard {

namespace {

constexpr double kPi = 3.141592653589793;

glm::vec3 lerp3(const glm::vec3& a, const glm::vec3& b, float t) {
    return a + (b - a) * std::clamp(t, 0.0f, 1.0f);
}

float smooth(float lo, float hi, float x) {
    const float t = std::clamp((x - lo) / (hi - lo), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// The cube's cross: a cell's face, seen from outside: its middle (the face's normal), and the
// directions of the cell's right and its top.
struct CrossCell {
    int col, row;
    glm::dvec3 normal, right, top;
};
const CrossCell kCross[6] = {
    {0, 1, {-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},   // -X
    {1, 1, {0, 0, 1}, {1, 0, 0}, {0, 1, 0}},    // +Z
    {2, 1, {1, 0, 0}, {0, 0, -1}, {0, 1, 0}},   // +X
    {3, 1, {0, 0, -1}, {-1, 0, 0}, {0, 1, 0}},  // -Z
    {1, 0, {0, 1, 0}, {1, 0, 0}, {0, 0, -1}},   // +Y
    {1, 2, {0, -1, 0}, {1, 0, 0}, {0, 0, 1}},   // -Y
};

}  // namespace

std::vector<MapProjection> map_projections(Shape::Kind kind) {
    switch (kind) {
        case Shape::Kind::BoundedArea:
            return {MapProjection::TopDown};
        case Shape::Kind::FlatCylinder:
        case Shape::Kind::CurvedCylinder:
            return {MapProjection::Unrolled};
        case Shape::Kind::SphereOutside:
        case Shape::Kind::SphereInside:
            return {MapProjection::Equirectangular, MapProjection::CubeFaces};
    }
    return {};
}

const char* map_projection_name(MapProjection p) {
    switch (p) {
        case MapProjection::TopDown:
            return "top_down";
        case MapProjection::Unrolled:
            return "unrolled";
        case MapProjection::Equirectangular:
            return "equirectangular";
        case MapProjection::CubeFaces:
            return "cube_faces";
    }
    return "?";
}

std::optional<MapProjection> map_projection_from_name(const std::string& name) {
    for (MapProjection p : {MapProjection::TopDown, MapProjection::Unrolled,
                            MapProjection::Equirectangular, MapProjection::CubeFaces})
        if (name == map_projection_name(p)) return p;
    return std::nullopt;
}

double map_aspect(const Terrain& terrain, MapProjection p) {
    const glm::dvec2 face = terrain.body().shape.face_size();
    switch (p) {
        case MapProjection::Equirectangular:
            return 2.0;
        case MapProjection::CubeFaces:
            return 4.0 / 3.0;
        default:
            return face.x / face.y;
    }
}

std::optional<SurfacePoint> world_map_place(const Terrain& terrain, int width, int height, int x,
                                            int y, MapProjection p) {
    (void)terrain;
    const double fx = (double(x) + 0.5) / double(width);
    const double fy = (double(y) + 0.5) / double(height);
    switch (p) {
        case MapProjection::Equirectangular: {
            const double longitude = (fx * 2.0 - 1.0) * kPi;
            const double latitude = (0.5 - fy) * kPi;
            const glm::dvec3 d(std::cos(latitude) * std::sin(longitude), std::sin(latitude),
                               std::cos(latitude) * std::cos(longitude));
            return cube_surface(d);
        }
        case MapProjection::CubeFaces: {
            const double cx = fx * 4.0, cy = fy * 3.0;
            const int col = std::min(int(cx), 3), row = std::min(int(cy), 2);
            for (const CrossCell& c : kCross) {
                if (c.col != col || c.row != row) continue;
                const double s = (cx - col) * 2.0 - 1.0;  // -1 at the cell's left
                const double t = 1.0 - (cy - row) * 2.0;  // -1 at its bottom
                return cube_surface(glm::normalize(c.normal + s * c.right + t * c.top));
            }
            return std::nullopt;
        }
        default:
            return SurfacePoint{0, fx, fy, 0.0};
    }
}

glm::dvec2 world_map_point(const SurfacePoint& place, MapProjection p) {
    switch (p) {
        case MapProjection::Equirectangular: {
            const glm::dvec3 d = glm::normalize(cube_direction(place.face, place.u, place.v));
            const double longitude = std::atan2(d.x, d.z);
            const double latitude = std::asin(std::clamp(d.y, -1.0, 1.0));
            return {longitude / (2.0 * kPi) + 0.5, 0.5 - latitude / kPi};
        }
        case MapProjection::CubeFaces: {
            const glm::dvec3 d = cube_direction(place.face, place.u, place.v);
            const CrossCell* best = &kCross[0];
            for (const CrossCell& c : kCross)
                if (glm::dot(d, c.normal) > glm::dot(d, best->normal)) best = &c;
            const double n = glm::dot(d, best->normal);
            const double s = glm::dot(d, best->right) / n, t = glm::dot(d, best->top) / n;
            return {(best->col + (s + 1.0) * 0.5) / 4.0, (best->row + (1.0 - t) * 0.5) / 3.0};
        }
        default:
            return {place.u - std::floor(place.u), place.v};  // (a flat cylinder's u wraps)
    }
}

std::vector<std::uint8_t> world_map(const Terrain& terrain, int width, int height) {
    return world_map(terrain, width, height, map_projections(terrain.body().shape.kind())[0]);
}

std::vector<std::uint8_t> world_map(const Terrain& terrain, int width, int height,
                                    MapProjection projection) {
    const Shape& shape = terrain.body().shape;
    const std::vector<MapProjection> allowed = map_projections(shape.kind());
    if (std::find(allowed.begin(), allowed.end(), projection) == allowed.end())
        throw std::invalid_argument(std::string("a ") + shape_name(shape.kind()) + " has no " +
                                    map_projection_name(projection) + " map");
    width = std::clamp(width, 1, 4096);
    height = std::clamp(height, 1, 4096);
    const size_t count = size_t(width) * size_t(height);
    std::vector<std::optional<SurfacePoint>> places(count);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            places[size_t(y) * width + x] =
                world_map_place(terrain, width, height, x, y, projection);
    // Metres a pixel, along each axis, for the shading.
    const glm::dvec2 face = shape.face_size();
    double across = face.x / double(width), down = face.y / double(height);
    if (projection == MapProjection::Equirectangular) {
        across = 4.0 * face.x / double(width);  // round the equator
        down = 2.0 * face.x / double(height);   // pole to pole
    } else if (projection == MapProjection::CubeFaces) {
        across = 4.0 * face.x / double(width);
        down = 3.0 * face.x / double(height);
    }
    return map_pixels(terrain, width, height, places, across, down);
}

std::vector<std::uint8_t> map_pixels(const Terrain& terrain, int width, int height,
                                     const std::vector<std::optional<SurfacePoint>>& places,
                                     double across, double down) {
    const TerrainSettings& s = terrain.settings();
    const size_t count = size_t(width) * size_t(height);
    std::vector<double> heights(count, s.sea_level);
    for (size_t i = 0; i < count && i < places.size(); ++i)
        if (places[i]) heights[i] = terrain.height(places[i]->face, places[i]->u, places[i]->v);
    // A neighbour's height, or this pixel's off the map's edge or off the cross.
    const auto h_at = [&](int x, int y, double fallback) {
        if (x < 0 || y < 0 || x >= width || y >= height) return fallback;
        const size_t i = size_t(y) * width + x;
        return places[i] ? heights[i] : fallback;
    };

    std::vector<std::uint8_t> out(count * 4, 0);
    const glm::vec3 deep(0.05f, 0.12f, 0.28f), shallow(0.16f, 0.38f, 0.52f);
    const glm::vec3 sand(0.74f, 0.68f, 0.50f), rock(0.45f, 0.43f, 0.40f), snow(0.94f, 0.95f, 0.97f);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const size_t i = size_t(y) * width + x;
            if (!places[i]) continue;  // transparent
            const SurfacePoint& p = *places[i];
            const double h = heights[i];
            const float above = float(h - s.sea_level);
            glm::vec3 c;
            if (above < 0.0f) {
                c = lerp3(shallow, deep, -above / 30.0f);
            } else {
                const glm::vec4 b = terrain.biome_at(p.face, p.u, p.v);
                const float moisture = std::clamp(b.x, 0.0f, 1.0f);
                c = lerp3(glm::vec3(0.55f, 0.50f, 0.28f), glm::vec3(0.27f, 0.42f, 0.16f), moisture);
                const int biome = terrain.biome_index(p.face, p.u, p.v, h);
                if (biome >= 0) {
                    const Biome& bi = s.biomes[size_t(biome)];
                    c = lerp3(bi.dry_color, bi.color, moisture);
                }
                c = lerp3(c, sand, 1.0f - smooth(1.0f, 2.5f, above));
                // Rock where it's steep, snow where it's cold or high.
                const double gx = (h_at(x + 1, y, h) - h_at(x - 1, y, h)) / (2.0 * across);
                const double gy = (h_at(x, y + 1, h) - h_at(x, y - 1, h)) / (2.0 * down);
                const float slope = float(std::sqrt(gx * gx + gy * gy));
                c = lerp3(c, rock, smooth(0.6f, 1.0f, slope));
                const float cold =
                    s.climate
                        ? 1.0f - smooth(-1.0f, 1.0f,
                                        float(terrain.temperature_at(p.face, p.u, p.v, h)))
                        : smooth(float(s.snow_line) - 3.0f, float(s.snow_line) + 3.0f, float(h));
                c = lerp3(c, snow, cold);
                // Light from the top left.
                c *= float(std::clamp(1.0 - (gx - gy) * 0.8, 0.55, 1.35));
            }
            std::uint8_t* px = &out[i * 4];
            px[0] = std::uint8_t(std::clamp(c.r, 0.0f, 1.0f) * 255.0f + 0.5f);
            px[1] = std::uint8_t(std::clamp(c.g, 0.0f, 1.0f) * 255.0f + 0.5f);
            px[2] = std::uint8_t(std::clamp(c.b, 0.0f, 1.0f) * 255.0f + 0.5f);
            px[3] = 255;
        }
    return out;
}

}  // namespace midgard
