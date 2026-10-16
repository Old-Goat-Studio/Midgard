// SPDX-License-Identifier: Apache-2.0
#include <doctest/doctest.h>

#include <cmath>
#include <string>
#include <vector>

#include "midgard/body.h"
#include "midgard/physics.h"

using namespace midgard;
using namespace midgard::physics;

namespace {

// The ground of a curved shape as a static mesh, its triangles facing "up" (into the world):
// cells_u x cells_v cells per face, small enough that the flat triangles stay within a few
// centimetres of the curved surface.
Collider ground_mesh(const Shape& shape, int cells_u, int cells_v) {
    Collider c;
    c.kind = Collider::Kind::Mesh;
    for (int f = 0; f < shape.faces(); ++f) {
        const auto base = static_cast<std::uint32_t>(c.points.size());
        for (int j = 0; j <= cells_v; ++j)
            for (int i = 0; i <= cells_u; ++i)
                c.points.push_back(
                    glm::vec3(shape.position({f, double(i) / cells_u, double(j) / cells_v, 0.0})));
        for (int j = 0; j < cells_v; ++j)
            for (int i = 0; i < cells_u; ++i) {
                const std::uint32_t a = base + std::uint32_t(j * (cells_u + 1) + i);
                const std::uint32_t b = a + 1, d = a + std::uint32_t(cells_u + 1), e = d + 1;
                const auto add = [&](std::uint32_t p, std::uint32_t q, std::uint32_t r) {
                    const glm::vec3 n =
                        glm::cross(c.points[q] - c.points[p], c.points[r] - c.points[p]);
                    const glm::vec3 mid = (c.points[p] + c.points[q] + c.points[r]) / 3.0f;
                    const bool facing_up = glm::dot(glm::dvec3(n), shape.up(glm::dvec3(mid))) > 0.0;
                    c.triangles.insert(c.triangles.end(),
                                       {p, facing_up ? q : r, facing_up ? r : q});
                };
                add(a, b, e);
                add(a, e, d);
            }
    }
    return c;
}

struct TestWorld {
    std::string name;
    Body body;
};

Body make_body(const char* id, Shape shape) {
    return Body{id, shape, 0, glm::dvec3(0.0)};
}

std::vector<TestWorld> test_worlds() {
    return {
        {"bounded area", make_body("area", Shape::bounded_area(200, 200, 9.81))},
        {"flat-wrap cylinder", make_body("flat", Shape::flat_cylinder(200, 200, 9.81))},
        {"curved cylinder", make_body("tube", Shape::curved_cylinder(50, 100, 9.81))},
        {"sphere outside", make_body("planet", Shape::sphere_outside(50, 9.81))},
        {"sphere inside", make_body("hollow", Shape::sphere_inside(50, 9.81))},
    };
}

// The world's ground: a slab for the flat shapes, a sphere for a planet, meshes inside.
void add_ground(World& world, const Body& body) {
    BodyDesc ground;
    switch (body.shape.kind()) {
        case Shape::Kind::BoundedArea:
        case Shape::Kind::FlatCylinder:
            ground.collider.size = {200.0f, 1.0f, 200.0f};
            ground.position = {0.0, -0.5, 0.0};
            break;
        case Shape::Kind::SphereOutside:
            ground.collider.kind = Collider::Kind::Sphere;
            ground.collider.radius = float(body.shape.radius());
            break;
        case Shape::Kind::CurvedCylinder:  // 2.5 m chords around, 2 cm below the curve at most
            ground.collider = ground_mesh(body.shape, 128, 8);
            break;
        case Shape::Kind::SphereInside:  // 3 m cells on each cube face
            ground.collider = ground_mesh(body.shape, 24, 24);
            break;
    }
    world.add(ground);
}

// Where to drop things: a few points on each face.
std::vector<SurfacePoint> drop_points(const Shape& shape) {
    std::vector<SurfacePoint> out;
    for (int f = 0; f < shape.faces(); ++f)
        for (const glm::dvec2 uv : {glm::dvec2(0.5, 0.5), glm::dvec2(0.3, 0.7)})
            out.push_back({f, uv.x, uv.y, 3.0});
    return out;
}

// Runs the world for a while, a frame at a time, as a game does.
void run(World& world, double seconds) {
    for (double t = 0.0; t < seconds - 1e-9; t += kStep) world.step(kStep);
}

}  // namespace

TEST_CASE("objects fall and settle towards the ground on every shape") {
    for (const TestWorld& t : test_worlds()) {
        CAPTURE(t.name);
        World world(t.body);
        add_ground(world, t.body);
        struct Dropped {
            BodyId id;
            SurfacePoint start;
        };
        std::vector<Dropped> dropped;
        for (const SurfacePoint& s : drop_points(t.body.shape)) {
            BodyDesc box;
            box.motion = Motion::Dynamic;
            box.position = t.body.position(s);
            // Upright on the surface, so it lands flat.
            const SurfaceFrame f = t.body.frame(box.position);
            box.rotation =
                glm::quat_cast(glm::mat3(glm::vec3(f.east), glm::vec3(f.up), -glm::vec3(f.north)));
            dropped.push_back({world.add(box), s});
        }

        // A third of a second in: falling along -up, without drifting sideways.
        run(world, 0.3);
        for (const Dropped& d : dropped) {
            const BodyState s = world.state(d.id);
            const SurfacePoint now = t.body.surface(s.position);
            CHECK(now.height < 3.0 - 0.3);
            const glm::dvec3 drift =
                s.position - t.body.position({now.face, d.start.u, d.start.v, now.height});
            CHECK(glm::length(drift) < 0.05);
        }

        // Five seconds in: resting on the ground, where it was dropped.
        run(world, 4.7);
        for (const Dropped& d : dropped) {
            const BodyState s = world.state(d.id);
            const SurfacePoint now = t.body.surface(s.position);
            CHECK(std::abs(now.height - 0.5) < 0.12);  // half the box, give or take the mesh
            CHECK(glm::length(s.velocity) < 0.05f);
            const glm::dvec3 start = t.body.position({d.start.face, d.start.u, d.start.v, 0.5});
            CHECK(glm::length(s.position - start) < 0.5);
        }
    }
}

TEST_CASE("the same scene gives the same result every run") {
    const auto simulate = [] {
        World world(make_body("area", Shape::bounded_area(200, 200, 9.81)));
        add_ground(world, world.body().value());
        std::vector<BodyId> ids;
        // A tumbling pile: boxes and balls dropped onto each other, some spinning.
        for (int i = 0; i < 40; ++i) {
            BodyDesc d;
            d.motion = Motion::Dynamic;
            d.collider.kind = i % 3 == 0 ? Collider::Kind::Sphere : Collider::Kind::Box;
            d.position = {(i % 4) * 0.6 - 0.9, 1.0 + i * 0.7, (i % 5) * 0.5 - 1.0};
            d.rotation = glm::angleAxis(0.3f * float(i), glm::normalize(glm::vec3(1, 2, 3)));
            d.surface.bounciness = 0.2f;
            ids.push_back(world.add(d));
        }
        run(world, 3.0);
        std::vector<glm::dvec3> out;
        for (BodyId id : ids) out.push_back(world.state(id).position);
        return out;
    };
    const auto a = simulate(), b = simulate();
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) CHECK(a[i] == b[i]);  // bit for bit
}

TEST_CASE("bodies: kinematic movers, layers, forces and bad colliders") {
    World world;  // no body: flat gravity
    CHECK(world.gravity({0, 0, 0}) == kFlatGravity);
    BodyDesc floor;
    floor.collider.size = {20, 1, 20};
    floor.position = {0, -0.5, 0};
    world.add(floor);

    // A ball on layer 3, which doesn't collide with layer 0 (the floor): it falls through.
    world.set_collides(0, 3, false);
    CHECK_FALSE(world.collides(3, 0));
    BodyDesc ghost;
    ghost.motion = Motion::Dynamic;
    ghost.collider.kind = Collider::Kind::Sphere;
    ghost.position = {3, 1, 0};
    ghost.layer = 3;
    const BodyId ghost_id = world.add(ghost);

    // A kinematic platform moves where it's told, in one step.
    BodyDesc platform;
    platform.motion = Motion::Kinematic;
    platform.position = {-3, 1, 0};
    const BodyId platform_id = world.add(platform);
    world.move_to(platform_id, {-3, 2, 0}, glm::quat(1, 0, 0, 0));

    // An impulse sends a ball sideways (1 kg m/s on a 2 kg ball: 0.5 m/s).
    BodyDesc ball;
    ball.motion = Motion::Dynamic;
    ball.collider.kind = Collider::Kind::Sphere;
    ball.position = {0, 0.5, 0};
    ball.mass = 2.0f;
    const BodyId ball_id = world.add(ball);
    world.add_impulse(ball_id, {1, 0, 0});
    CHECK(world.state(ball_id).velocity.x == doctest::Approx(0.5));

    run(world, 1.0);
    CHECK(world.state(ghost_id).position.y < -2.0);
    CHECK(world.state(platform_id).position.y == doctest::Approx(2.0));
    CHECK(world.state(ball_id).position.x > 0.2);

    // A long stall is caught up a quarter of a second at most (the world slows down instead).
    CHECK(world.step(10.0) == 15);

    // Removing a body.
    const std::size_t before = world.body_count();
    world.remove(ghost_id);
    CHECK(world.body_count() == before - 1);

    // Malformed colliders are refused with a reason.
    BodyDesc bad;
    bad.collider.size = {0, 1, 1};
    CHECK_THROWS_AS(world.add(bad), PhysicsError);
    bad.collider = {};
    bad.collider.kind = Collider::Kind::Mesh;
    bad.motion = Motion::Dynamic;
    CHECK_THROWS_AS(world.add(bad), PhysicsError);
    bad.layer = 16;
    CHECK_THROWS_AS(world.add(bad), PhysicsError);
}
