// SPDX-License-Identifier: Apache-2.0
#include "midgard/world_map.h"

#include <algorithm>
#include <cmath>

#include "midgard/det.h"

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

}  // namespace

SurfacePoint world_map_place(const Terrain& terrain, int width, int height, int x, int y) {
    const double fx = (double(x) + 0.5) / double(width);
    const double fy = (double(y) + 0.5) / double(height);
    const Shape& shape = terrain.body().shape;
    if (shape.kind() == Shape::Kind::SphereOutside || shape.kind() == Shape::Kind::SphereInside) {
        const double longitude = (fx * 2.0 - 1.0) * kPi;
        const double latitude = (0.5 - fy) * kPi;
        const glm::dvec3 d(std::cos(latitude) * std::sin(longitude), std::sin(latitude),
                           std::cos(latitude) * std::cos(longitude));
        return cube_surface(d);
    }
    return {0, fx, fy, 0.0};
}

std::vector<std::uint8_t> world_map(const Terrain& terrain, int width, int height) {
    width = std::clamp(width, 1, 4096);
    height = std::clamp(height, 1, 4096);
    const TerrainSettings& s = terrain.settings();
    std::vector<double> heights(size_t(width) * size_t(height));
    std::vector<SurfacePoint> places(heights.size());
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const SurfacePoint p = world_map_place(terrain, width, height, x, y);
            places[size_t(y) * width + x] = p;
            heights[size_t(y) * width + x] = terrain.height(p.face, p.u, p.v);
        }
    // Metres a pixel, along each axis, for the shading.
    const glm::dvec2 face = terrain.body().shape.face_size();
    const bool sphere = terrain.body().shape.kind() == Shape::Kind::SphereOutside ||
                        terrain.body().shape.kind() == Shape::Kind::SphereInside;
    const double across = sphere ? 4.0 * face.x / double(width) : face.x / double(width);
    const double down = sphere ? 2.0 * face.x / double(height) : face.y / double(height);
    const auto h_at = [&](int x, int y) {
        x = std::clamp(x, 0, width - 1);
        y = std::clamp(y, 0, height - 1);
        return heights[size_t(y) * width + x];
    };

    std::vector<std::uint8_t> out(size_t(width) * size_t(height) * 4);
    const glm::vec3 deep(0.05f, 0.12f, 0.28f), shallow(0.16f, 0.38f, 0.52f);
    const glm::vec3 sand(0.74f, 0.68f, 0.50f), rock(0.45f, 0.43f, 0.40f), snow(0.94f, 0.95f, 0.97f);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const SurfacePoint& p = places[size_t(y) * width + x];
            const double h = heights[size_t(y) * width + x];
            const float above = float(h - s.sea_level);
            glm::vec3 c;
            if (above < 0.0f) {
                c = lerp3(shallow, deep, -above / 30.0f);
            } else {
                const glm::vec3 b = terrain.biome_at(p.face, p.u, p.v);
                const float moisture = std::clamp(b.x, 0.0f, 1.0f);
                c = lerp3(glm::vec3(0.55f, 0.50f, 0.28f), glm::vec3(0.27f, 0.42f, 0.16f), moisture);
                const int biome = terrain.biome_index(p.face, p.u, p.v, h);
                if (biome >= 0) {
                    const Biome& bi = s.biomes[size_t(biome)];
                    c = lerp3(bi.dry_color, bi.color, moisture);
                }
                c = lerp3(c, sand, 1.0f - smooth(1.0f, 2.5f, above));
                // Rock where it's steep, snow where it's cold or high.
                const double gx = (h_at(x + 1, y) - h_at(x - 1, y)) / (2.0 * across);
                const double gy = (h_at(x, y + 1) - h_at(x, y - 1)) / (2.0 * down);
                const float slope = float(std::sqrt(gx * gx + gy * gy));
                c = lerp3(c, rock, smooth(0.6f, 1.0f, slope));
                const float cold =
                    s.climate
                        ? 1.0f - smooth(-1.0f, 1.0f,
                                        float(terrain.temperature_at(p.face, p.u, p.v, h)))
                        : smooth(float(s.snow_line) - 3.0f, float(s.snow_line) + 3.0f, float(h));
                c = lerp3(c, snow, cold);
                // Light from the north-west.
                const float shade = float(std::clamp(1.0 - (gx - gy) * 0.8, 0.55, 1.35));
                c *= shade;
            }
            std::uint8_t* px = &out[(size_t(y) * width + x) * 4];
            px[0] = std::uint8_t(std::clamp(c.r, 0.0f, 1.0f) * 255.0f + 0.5f);
            px[1] = std::uint8_t(std::clamp(c.g, 0.0f, 1.0f) * 255.0f + 0.5f);
            px[2] = std::uint8_t(std::clamp(c.b, 0.0f, 1.0f) * 255.0f + 0.5f);
            px[3] = 255;
        }
    return out;
}

}  // namespace midgard
