// SPDX-License-Identifier: Apache-2.0
#include "midgard/compass.h"

#include <cmath>

#include "midgard/world_map.h"

namespace midgard {
namespace {

constexpr double kDegrees = 180.0 / 3.14159265358979323846;

glm::dvec3 flat(const glm::dvec3& v, const glm::dvec3& up) {
    return v - up * glm::dot(v, up);
}

}  // namespace

Bearing bearing_at(const std::optional<Body>& body, const glm::dvec3& p) {
    Bearing b;
    if (!body) return b;
    const Shape::Kind kind = body->shape.kind();
    b.up = glm::normalize(body->up(p));
    glm::dvec3 north;
    if (kind == Shape::Kind::SphereOutside || kind == Shape::Kind::SphereInside) {
        north = flat(glm::dvec3(0.0, 1.0, 0.0), b.up);
        if (glm::dot(north, north) < 1e-12) north = flat(glm::dvec3(0.0, 0.0, -1.0), b.up);
    } else {
        north = flat(body->frame(p).north, b.up);
    }
    b.north = glm::normalize(north);
    b.east = glm::normalize(glm::cross(b.north, b.up));
    return b;
}

double heading_of(const Bearing& b, const glm::dvec3& direction, double fallback) {
    const double n = glm::dot(direction, b.north), e = glm::dot(direction, b.east);
    if (n * n + e * e < 1e-12) return fallback;
    double h = std::atan2(e, n) * kDegrees;
    if (h < 0.0) h += 360.0;
    return h >= 360.0 ? 0.0 : h;
}

glm::dvec2 east_north(const Bearing& b, const glm::dvec3& from, const glm::dvec3& target) {
    const glm::dvec3 d = target - from;
    return {glm::dot(d, b.east), glm::dot(d, b.north)};
}

std::vector<std::uint8_t> local_map(const Terrain& terrain, const glm::dvec3& centre, double radius,
                                    int size) {
    size = std::max(size, 2);
    const Body& body = terrain.body();
    const Bearing b = bearing_at(body, centre);
    const double step = 2.0 * radius / double(size);
    std::vector<std::optional<SurfacePoint>> places(size_t(size) * size);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const double east = (x + 0.5) * step - radius;
            const double north = radius - (y + 0.5) * step;  // (north up)
            SurfacePoint s = body.surface(centre + b.east * east + b.north * north);
            // (past a bounded area's edge, or a cylinder's ends: nothing there)
            const bool beyond_u =
                body.shape.kind() == Shape::Kind::BoundedArea && (s.u < 0.0 || s.u > 1.0);
            const bool beyond_v = (body.shape.kind() == Shape::Kind::BoundedArea ||
                                   body.shape.kind() == Shape::Kind::FlatCylinder ||
                                   body.shape.kind() == Shape::Kind::CurvedCylinder) &&
                                  (s.v < 0.0 || s.v > 1.0);
            if (!beyond_u && !beyond_v) places[size_t(y) * size + x] = s;
        }
    return map_pixels(terrain, size, size, places, step, step);
}

}  // namespace midgard
