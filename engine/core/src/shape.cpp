// SPDX-License-Identifier: Apache-2.0
// The order of operations here is the specification (tools/reference/shapes.py repeats it).
#include "midgard/shape.h"

#include <cmath>
#include <stdexcept>

#include "midgard/det.h"
#include "midgard/hash.h"

namespace midgard {

namespace {

// Cube faces as (axis, sign) for the normal n, the u direction t and the v direction b, with
// t x b = n.
struct AxisSign {
    int axis;
    double sign;
};
struct Face {
    AxisSign n, t, b;
};
constexpr Face kFaces[6] = {
    {{0, 1.0}, {2, -1.0}, {1, 1.0}},   // +X
    {{0, -1.0}, {2, 1.0}, {1, 1.0}},   // -X
    {{1, 1.0}, {0, 1.0}, {2, -1.0}},   // +Y
    {{1, -1.0}, {0, 1.0}, {2, 1.0}},   // -Y
    {{2, 1.0}, {0, 1.0}, {1, 1.0}},    // +Z
    {{2, -1.0}, {0, -1.0}, {1, 1.0}},  // -Z
};

constexpr double kQuarterPi = 0.7853981633974483;
constexpr double kFourOverPi = 1.2732395447351628;
constexpr double kTwoPi = 6.283185307179586;

// -0.0 becomes 0.0, so equal points have equal bits.
double zero(double x) {
    // Not x + 0.0: some compilers (MSVC) drop the addition as a no-op, keeping -0.0.
    return x == 0.0 ? 0.0 : x;
}
glm::dvec3 zero(const glm::dvec3& p) {
    return {zero(p.x), zero(p.y), zero(p.z)};
}

// [-1, 1] to the cube face's coordinate. The edges are exactly +-1 (tan(pi/4) rounds).
double warp(double a) {
    if (a == 1.0) return 1.0;
    if (a == -1.0) return -1.0;
    return det::tan(kQuarterPi * a);
}

double unwarp(double w) {
    if (w >= 1.0) return 1.0;
    if (w <= -1.0) return -1.0;
    return det::atan(w) * kFourOverPi;
}

double wrap01(double u) {
    return u - std::floor(u);
}

bool spherical(Shape::Kind k) {
    return k == Shape::Kind::SphereOutside || k == Shape::Kind::SphereInside;
}

}  // namespace

glm::dvec3 cube_direction(int face, double u, double v) {
    if (face < 0 || face > 5) throw std::out_of_range("cube faces are 0 to 5");
    const Face& f = kFaces[face];
    double c[3] = {0.0, 0.0, 0.0};
    c[f.n.axis] = f.n.sign;
    c[f.t.axis] = f.t.sign * warp(2.0 * u - 1.0);
    c[f.b.axis] = f.b.sign * warp(2.0 * v - 1.0);
    const double length = det::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
    return {c[0] / length, c[1] / length, c[2] / length};
}

SurfacePoint cube_surface(const glm::dvec3& d) {
    double best = -1.0;
    int face = 0;
    for (int f = 0; f < 6; ++f) {
        const double value = d[kFaces[f].n.axis] * kFaces[f].n.sign;
        if (value > best) {
            best = value;
            face = f;
        }
    }
    const Face& f = kFaces[face];
    const double scale = d[f.n.axis] * f.n.sign;
    const double wu = d[f.t.axis] * f.t.sign / scale;
    const double wv = d[f.b.axis] * f.b.sign / scale;
    return {face, (unwarp(wu) + 1.0) * 0.5, (unwarp(wv) + 1.0) * 0.5, 0.0};
}

Shape Shape::bounded_area(double width, double length, double gravity) {
    return {Kind::BoundedArea, width, length, gravity};
}
Shape Shape::flat_cylinder(double circumference, double length, double gravity) {
    return {Kind::FlatCylinder, circumference, length, gravity};
}
Shape Shape::curved_cylinder(double radius, double length, double surface_gravity) {
    return {Kind::CurvedCylinder, radius, length, surface_gravity};
}
Shape Shape::sphere_outside(double radius, double gravity) {
    return {Kind::SphereOutside, radius, 0.0, gravity};
}
Shape Shape::sphere_inside(double radius, double gravity) {
    return {Kind::SphereInside, radius, 0.0, gravity};
}

int Shape::faces() const {
    return spherical(kind_) ? 6 : 1;
}

double Shape::radius() const {
    return kind_ == Kind::BoundedArea || kind_ == Kind::FlatCylinder ? 0.0 : size_u_;
}

glm::dvec2 Shape::face_size() const {
    switch (kind_) {
        case Kind::CurvedCylinder:
            return {kTwoPi * size_u_, size_v_};
        case Kind::SphereOutside:
        case Kind::SphereInside:
            return {kTwoPi / 4.0 * size_u_, kTwoPi / 4.0 * size_u_};
        default:
            return {size_u_, size_v_};
    }
}

glm::dvec3 Shape::position(const SurfacePoint& s) const {
    switch (kind_) {
        case Kind::BoundedArea:
        case Kind::FlatCylinder: {
            const double u = kind_ == Kind::FlatCylinder ? wrap01(s.u) : s.u;
            return zero(glm::dvec3((u - 0.5) * size_u_, s.height, (s.v - 0.5) * size_v_));
        }
        case Kind::CurvedCylinder: {
            const double angle = kTwoPi * wrap01(s.u);
            const double r = size_u_ - s.height;
            return zero(
                glm::dvec3(r * det::sin(angle), -(r * det::cos(angle)), (s.v - 0.5) * size_v_));
        }
        default: {
            const glm::dvec3 d = cube_direction(s.face, s.u, s.v);
            const double r = kind_ == Kind::SphereOutside ? size_u_ + s.height : size_u_ - s.height;
            return zero(glm::dvec3(d.x * r, d.y * r, d.z * r));
        }
    }
}

SurfacePoint Shape::surface(const glm::dvec3& p) const {
    switch (kind_) {
        case Kind::BoundedArea:
        case Kind::FlatCylinder: {
            double u = p.x / size_u_ + 0.5;
            if (kind_ == Kind::FlatCylinder) u = wrap01(u);
            return {0, u, p.z / size_v_ + 0.5, p.y};
        }
        case Kind::CurvedCylinder: {
            double u = det::atan2(p.x, -p.y) / kTwoPi;
            if (u < 0.0) u = u + 1.0;
            return {0, u, p.z / size_v_ + 0.5, size_u_ - det::sqrt(p.x * p.x + p.y * p.y)};
        }
        default: {
            const double r = det::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
            SurfacePoint s = cube_surface({p.x / r, p.y / r, p.z / r});
            s.height = kind_ == Kind::SphereOutside ? r - size_u_ : size_u_ - r;
            return s;
        }
    }
}

glm::dvec3 Shape::up(const glm::dvec3& p) const {
    switch (kind_) {
        case Kind::BoundedArea:
        case Kind::FlatCylinder:
            return {0.0, 1.0, 0.0};
        case Kind::CurvedCylinder: {
            const double r = det::sqrt(p.x * p.x + p.y * p.y);
            if (r == 0.0) return {0.0, 1.0, 0.0};  // on the axis "up" is undefined
            return zero(glm::dvec3(-p.x / r, -p.y / r, 0.0));
        }
        default: {
            const double r = det::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
            const double s = kind_ == Kind::SphereOutside ? 1.0 : -1.0;
            return zero(glm::dvec3(s * p.x / r, s * p.y / r, s * p.z / r));
        }
    }
}

glm::dvec3 Shape::gravity(const glm::dvec3& p) const {
    if (kind_ == Kind::CurvedCylinder) {  // spin gravity: g r / R, away from the axis
        const double scale = gravity_ / size_u_;
        return zero(glm::dvec3(p.x * scale, p.y * scale, 0.0));
    }
    const glm::dvec3 u = up(p);
    return zero(glm::dvec3(-u.x * gravity_, -u.y * gravity_, -u.z * gravity_));
}

SurfaceFrame Shape::frame(const glm::dvec3& p) const {
    const glm::dvec3 u = up(p);
    glm::dvec3 east{1.0, 0.0, 0.0};
    if (kind_ == Kind::CurvedCylinder) {
        // The direction of increasing angle: d/dtheta of (sin, -cos).
        const double r = det::sqrt(p.x * p.x + p.y * p.y);
        east = r == 0.0 ? glm::dvec3(1.0, 0.0, 0.0) : glm::dvec3(-p.y / r, p.x / r, 0.0);
    } else if (spherical(kind_)) {
        const Face& f = kFaces[surface(p).face];
        glm::dvec3 t{0.0, 0.0, 0.0};
        t[f.t.axis] = f.t.sign;
        east = glm::normalize(t - u * glm::dot(t, u));
    }
    return {u, east, glm::cross(u, east)};
}

std::uint64_t shape_fingerprint() {
    const Shape shapes[5] = {
        Shape::bounded_area(2000.0, 3000.0, 9.81), Shape::flat_cylinder(6283.0, 5000.0, 9.81),
        Shape::curved_cylinder(1000.0, 5000.0, 9.81), Shape::sphere_outside(1000.0, 9.81),
        Shape::sphere_inside(1500.0, 7.5)};
    Fnv1a64 hash;  // little-endian doubles, as on every target Midgard runs on
    for (const Shape& s : shapes)
        for (int f = 0; f < s.faces(); ++f)
            for (int j = 0; j < 17; ++j)
                for (int i = 0; i < 17; ++i) {
                    const glm::dvec3 p = s.position({f, i / 16.0, j / 16.0, 2.5});
                    const glm::dvec3 values[3] = {p, s.up(p), s.gravity(p)};
                    for (const glm::dvec3& v : values) {
                        const double xyz[3] = {v.x, v.y, v.z};
                        hash.add(xyz, sizeof(xyz));
                    }
                }
    return hash.value();
}

}  // namespace midgard
