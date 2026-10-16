// SPDX-License-Identifier: Apache-2.0
// A body: one place to stand on (roadmap §7, "Terms"), with its shape, size, seed, gravity and
// where it sits in its room. Generated bodies arrive with world types (Phase 5.11); until then
// test bodies come from settings:
//
//   {"id": "test_planet", "shape": "sphere_outside", "radius": 1000, "gravity": 9.81,
//    "seed": 829174332, "origin": [0, 0, 0]}
//
// Shapes and their sizes: "bounded_area" (width, length), "flat_cylinder" (circumference,
// length), "curved_cylinder" (radius, length), "sphere_outside" and "sphere_inside" (radius).
// Sizes are checked against the roadmap's limits (§7, "Size and precision").
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

#include "midgard/shape.h"

namespace midgard {

class BodyError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Body {
    std::string id;
    Shape shape;
    std::uint64_t seed = 0;
    glm::dvec3 origin{0.0};  // where the shape's centre is in the room

    glm::dvec3 position(const SurfacePoint& s) const { return origin + shape.position(s); }
    SurfacePoint surface(const glm::dvec3& p) const { return shape.surface(p - origin); }
    glm::dvec3 up(const glm::dvec3& p) const { return shape.up(p - origin); }
    glm::dvec3 gravity(const glm::dvec3& p) const { return shape.gravity(p - origin); }
    SurfaceFrame frame(const glm::dvec3& p) const { return shape.frame(p - origin); }
};

// Reads a body from JSON (above). Throws BodyError naming the problem.
Body body_from_json(const nlohmann::json& j);

const char* shape_name(Shape::Kind kind);  // "sphere_outside" and so on

}  // namespace midgard
