// SPDX-License-Identifier: Apache-2.0
#include "midgard/body.h"

#include <optional>

#include <nlohmann/json.hpp>

namespace midgard {

namespace {

// The roadmap's size limits (§7, "Size and precision"), in metres.
constexpr double kSphereRadius[2] = {1'000.0, 20'000.0};
constexpr double kCylinderRadius[2] = {1'000.0, 5'000.0};
constexpr double kCylinderLength[2] = {5'000.0, 50'000.0};
// A flat-wrap cylinder's circumference: about 2 pi times the cylinder radii.
constexpr double kCircumference[2] = {6'000.0, 32'000.0};
constexpr double kAreaSide[2] = {1.0, 200'000.0};

double number(const nlohmann::json& j, const char* key, const std::string& id) {
    if (!j.contains(key) || !j.at(key).is_number())
        throw BodyError("body '" + id + "' needs a number for \"" + key + "\"");
    return j.at(key).get<double>();
}

double sized(const nlohmann::json& j, const char* key, const double (&range)[2],
             const std::string& id) {
    const double v = number(j, key, id);
    if (!(v >= range[0] && v <= range[1]))
        throw BodyError("body '" + id + "': " + key + " " + std::to_string(v) + " m is outside " +
                        std::to_string(range[0]) + " to " + std::to_string(range[1]) + " m");
    return v;
}

}  // namespace

const char* shape_name(Shape::Kind kind) {
    switch (kind) {
        case Shape::Kind::BoundedArea:
            return "bounded_area";
        case Shape::Kind::FlatCylinder:
            return "flat_cylinder";
        case Shape::Kind::CurvedCylinder:
            return "curved_cylinder";
        case Shape::Kind::SphereOutside:
            return "sphere_outside";
        case Shape::Kind::SphereInside:
            return "sphere_inside";
    }
    return "?";
}

Body body_from_json(const nlohmann::json& j) {
    if (!j.is_object()) throw BodyError("a body must be a JSON object");
    const std::string id = j.value("id", std::string("body"));
    const std::string kind = j.value("shape", std::string());
    double gravity = 9.81;
    if (j.contains("gravity")) {
        gravity = number(j, "gravity", id);
        if (!(gravity > 0.0 && gravity <= 50.0))
            throw BodyError("body '" + id + "': gravity must be above 0 and at most 50 m/s^2");
    }
    std::optional<Shape> shape;
    if (kind == "bounded_area")
        shape = Shape::bounded_area(sized(j, "width", kAreaSide, id),
                                    sized(j, "length", kAreaSide, id), gravity);
    else if (kind == "flat_cylinder")
        shape = Shape::flat_cylinder(sized(j, "circumference", kCircumference, id),
                                     sized(j, "length", kCylinderLength, id), gravity);
    else if (kind == "curved_cylinder")
        shape = Shape::curved_cylinder(sized(j, "radius", kCylinderRadius, id),
                                       sized(j, "length", kCylinderLength, id), gravity);
    else if (kind == "sphere_outside")
        shape = Shape::sphere_outside(sized(j, "radius", kSphereRadius, id), gravity);
    else if (kind == "sphere_inside")
        shape = Shape::sphere_inside(sized(j, "radius", kSphereRadius, id), gravity);
    else
        throw BodyError("body '" + id + "': unknown shape \"" + kind +
                        "\" (bounded_area, flat_cylinder, curved_cylinder, sphere_outside or "
                        "sphere_inside)");
    Body body{id, *shape};
    if (j.contains("seed")) {
        if (!j.at("seed").is_number_integer())
            throw BodyError("body '" + id + "': the seed must be a whole number");
        body.seed = j.at("seed").get<std::uint64_t>();
    }
    if (j.contains("origin")) {
        const nlohmann::json& o = j.at("origin");
        if (!o.is_array() || o.size() != 3 || !o[0].is_number() || !o[1].is_number() ||
            !o[2].is_number())
            throw BodyError("body '" + id + "': origin needs 3 numbers");
        body.origin = {o[0].get<double>(), o[1].get<double>(), o[2].get<double>()};
    }
    return body;
}

}  // namespace midgard
