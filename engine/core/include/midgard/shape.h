// SPDX-License-Identifier: Apache-2.0
// World shapes: where a point on a world's surface is, which way is up, and how strong gravity
// is there (roadmap §7, "World shapes"). Rendering, physics, AI, placement and networking ask
// this layer; nothing assumes "y is up".
//
// A surface point is (face, u, v, height): u and v run from 0 to 1 across a face, and height is
// in metres along "up". Flat shapes and cylinders have one face; spheres have six, the faces of
// a cube projected onto the sphere (a "cube-sphere"), with a tangent warp that evens out their
// area.
//
//   bounded area        x = (u - 1/2) W, z = (v - 1/2) L, y = height; up is +Y
//   flat-wrap cylinder  the same, with u wrapping (x repeats every C metres)
//   curved cylinder     the inside of a tube along Z: u goes round (angle 2 pi u, 0 at the
//                       bottom), v along; height points to the axis; spin gravity g r / R outwards
//   sphere outside      the cube face's direction times (R + height); gravity to the centre
//   sphere inside       times (R - height); up points to the centre, gravity outwards
//
// Positions come from midgard/det.h's operations in a fixed order, so they are bit-identical on
// every target, and points on a power-of-two grid that lie on a seam (the cylinder's join, a
// cube edge or corner) come out bit-identical from every face that shares them. The Python twin
// is tools/reference/shapes.py; schema/tests/shape_*.json hold both to the same results.
#pragma once

#include <cstdint>
#include <optional>

#include <glm/glm.hpp>

namespace midgard {

struct SurfacePoint {
    int face = 0;
    double u = 0.0, v = 0.0;  // 0..1 across the face
    double height = 0.0;      // metres along up
};

// The local directions at a point: up, east (increasing u) and north (cross(up, east)), so
// east x north = up. On a sphere, east follows the face's u direction, so it turns at a face's
// edge (up never does).
struct SurfaceFrame {
    glm::dvec3 up, east, north;
};

class Shape {
public:
    enum class Kind { BoundedArea, FlatCylinder, CurvedCylinder, SphereOutside, SphereInside };

    static Shape bounded_area(double width, double length, double gravity);
    static Shape flat_cylinder(double circumference, double length, double gravity);
    static Shape curved_cylinder(double radius, double length, double surface_gravity);
    static Shape sphere_outside(double radius, double gravity);
    static Shape sphere_inside(double radius, double gravity);

    Kind kind() const { return kind_; }
    int faces() const;
    // A face's extent in metres along u and along v (at height 0; on a sphere, the arc length
    // of the face's middle line).
    glm::dvec2 face_size() const;
    double radius() const;  // cylinders and spheres
    double surface_gravity() const { return gravity_; }

    // Surface point to 3D, and back (the inverse puts a point on a seam on the lowest-numbered
    // face that has it).
    glm::dvec3 position(const SurfacePoint& s) const;
    SurfacePoint surface(const glm::dvec3& p) const;

    glm::dvec3 up(const glm::dvec3& p) const;       // unit length
    glm::dvec3 gravity(const glm::dvec3& p) const;  // m/s^2, along -up
    SurfaceFrame frame(const glm::dvec3& p) const;

private:
    Shape(Kind kind, double size_u, double size_v, double gravity)
        : kind_(kind), size_u_(size_u), size_v_(size_v), gravity_(gravity) {}
    Kind kind_;
    double size_u_, size_v_;  // width/circumference/radius, and length
    double gravity_;
};

// A hash over positions, up and gravity on a 17 x 17 grid on every face of five test shapes
// (those of schema/tests/shape_fingerprint.json): kShapeFingerprint on every target. The
// self-test checks it on devices.
std::uint64_t shape_fingerprint();
inline constexpr std::uint64_t kShapeFingerprint = 0xca94b76690b1e465ULL;

// The cube-sphere's mapping of a face point to a unit direction, and back.
glm::dvec3 cube_direction(int face, double u, double v);
SurfacePoint cube_surface(const glm::dvec3& direction);  // height 0
// The (u, v) a direction has on one face's plane, continued past the face's edges (so u or v
// may be outside 0..1): cube_direction's inverse on that face, for a direction less than 90
// degrees from the face's middle. Navigation (milestone 5.12) extends a face's frame across
// its edges this way. (Not on the bit-identical path: it may differ in the last bits.)
glm::dvec2 cube_face_uv(int face, const glm::dvec3& direction);

// How the cube's faces join. A face's edges are numbered 0 (u = 0), 1 (u = 1), 2 (v = 0) and
// 3 (v = 1). Across an edge lies `face`, joined there by its edge `edge`; the coordinate along
// the shared edge (v for edges 0 and 1, u for 2 and 3) runs the other way on that face if
// `reversed`, so a point at t along one is at 1 - t along the other.
struct CubeLink {
    int face = 0;
    int edge = 0;
    bool reversed = false;
};
CubeLink cube_link(int face, int edge);
// A face point just past one of its edges (u or v a little outside 0..1, not both), as the same
// point on the face across that edge: exact for points on a power-of-two grid, so a grid point
// found either way is bit-identical. Points inside the face come back as they are.
SurfacePoint cube_across(const SurfacePoint& s);
// A direction as a point in `face`'s u and v, carried on past the face's edges (u or v outside
// 0 to 1) for directions less than 90 degrees from the face's middle (none otherwise): the
// inverse of cube_direction there. A brush stroke that begins on one face carries on across an
// edge in that face's terms.
std::optional<glm::dvec2> cube_face_point(int face, const glm::dvec3& direction);

}  // namespace midgard
