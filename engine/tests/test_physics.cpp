// SPDX-License-Identifier: Apache-2.0
#include <doctest/doctest.h>

#include <algorithm>
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

// ---- the character -------------------------------------------------------------------------
namespace {

// Steps the world with a character walking at `walk` for a while.
void walk_for(World& world, Character& c, double seconds, const glm::vec3& walk,
              float jump = 0.0f) {
    for (double t = 0.0; t < seconds - 1e-9; t += kStep) {
        world.step(kStep, [&] { c.step(walk, jump); });
        jump = 0.0f;  // one press
    }
}

BodyId add_box(World& world, glm::dvec3 at, glm::vec3 size, glm::quat turn = {1, 0, 0, 0}) {
    BodyDesc d;
    d.collider.size = size;
    d.position = at;
    d.rotation = turn;
    return world.add(d);
}

}  // namespace

TEST_CASE("the character lands, walks, stops at walls, climbs steps and jumps") {
    World world;
    add_box(world, {0, -0.5, 0}, {60, 1, 60});
    CharacterDesc desc;  // radius 0.3, 1 m tall: its middle stands 0.5 m above the ground
    desc.position = {0, 2, 0};
    Character c(world, desc);

    walk_for(world, c, 1.5, {0, 0, 0});
    CHECK(c.on_ground());
    CHECK(std::abs(c.position().y - 0.5) < 0.03);

    // 4 m/s along +x for a second; the upward part of the walk is ignored.
    walk_for(world, c, 1.0, {4, 3, 0});
    CHECK(c.position().x == doctest::Approx(4.0).epsilon(0.03));
    CHECK(std::abs(c.position().y - 0.5) < 0.03);

    // A wall at x = 8 (its face at 7.5): stopped a radius short of it, not through it.
    add_box(world, {8, 1, 0}, {1, 2, 4});
    walk_for(world, c, 2.0, {4, 0, 0});
    CHECK(c.position().x == doctest::Approx(7.2).epsilon(0.01));

    // Steps: 0.25 m is walked up, 0.5 m is a wall.
    c.set_position({0, 0.5, 10});
    add_box(world, {3, 0.125, 10}, {2, 0.25, 4});
    add_box(world, {8, 0.25, 10}, {2, 0.5, 4});
    walk_for(world, c, 1.5, {2, 0, 0});  // to x = 3, on the low step
    CHECK(c.position().y == doctest::Approx(0.75).epsilon(0.05));
    walk_for(world, c, 3.0, {2, 0, 0});  // on towards the high one
    CHECK(c.position().x < 7.0 + 0.05);  // its face is at 7: stopped there
    CHECK(c.position().y < 0.8);

    // Jumping at 5 m/s rises about 1.27 m (v^2 / 2g), then lands again.
    c.set_position({0, 0.5, -10});
    walk_for(world, c, 0.5, {0, 0, 0});
    double highest = 0.0;
    world.step(kStep, [&] { c.step({0, 0, 0}, 5.0f); });
    for (int i = 0; i < 120; ++i) {
        world.step(kStep, [&] { c.step({0, 0, 0}); });
        highest = std::max(highest, c.position().y);
    }
    CHECK(highest - 0.5 == doctest::Approx(1.27).epsilon(0.08));
    CHECK(c.on_ground());
}

TEST_CASE("the character stands on gentle slopes and can't climb steep ones") {
    World world;
    add_box(world, {0, -0.5, 0}, {60, 1, 60});
    // Ramps rising towards +x: 25 degrees (walkable) and 65 degrees (steeper than 50: a wall).
    const auto ramp = [&](double z, float degrees) {
        const glm::quat turn = glm::angleAxis(glm::radians(degrees), glm::vec3(0, 0, 1));
        add_box(world, {6, 0, z}, {8, 0.5, 4}, turn);
    };
    ramp(0, 25.0f);
    ramp(10, 65.0f);
    CharacterDesc desc;
    desc.position = {7.0, 2.5, 0};  // above the middle of the gentle ramp
    Character c(world, desc);
    walk_for(world, c, 1.0, {0, 0, 0});
    REQUIRE(c.on_ground());
    const glm::dvec3 resting = c.position();
    walk_for(world, c, 2.0, {0, 0, 0});
    CHECK(glm::length(c.position() - resting) < 0.02);  // no sliding down
    walk_for(world, c, 1.0, {2, 0, 0});
    CHECK(c.position().y > resting.y + 0.5);  // walked on up it

    c.set_position({1.0, 0.5, 10});
    walk_for(world, c, 3.0, {2, 0, 0});
    CHECK(c.position().y < 1.0);  // the steep ramp stopped it near the bottom
}

TEST_CASE("the character walks all the way round a planet and along inside a cylinder") {
    struct Case {
        const char* name;
        Body body;
    };
    for (const Case& t :
         {Case{"planet", make_body("planet", Shape::sphere_outside(1000, 9.81))},
          Case{"cylinder", make_body("tube", Shape::curved_cylinder(1000, 5000, 9.81))}}) {
        CAPTURE(std::string(t.name));
        World world(t.body);
        if (t.body.shape.kind() == Shape::Kind::SphereOutside) {
            BodyDesc ground;
            ground.collider.kind = Collider::Kind::Sphere;
            ground.collider.radius = 1000.0f;
            world.add(ground);
        } else {
            world.add([&] {
                BodyDesc d;
                d.collider = ground_mesh(t.body.shape, 512, 4);  // 12 m chords, 2 cm sag
                return d;
            }());
        }
        const SurfacePoint start{0, 0.5, 0.5, 0.5};
        CharacterDesc desc;
        desc.position = t.body.position(start);
        Character c(world, desc);
        // Walk "east" (round the planet's equator, round the cylinder), keeping the heading
        // level as "up" turns underfoot.
        glm::dvec3 heading = t.body.frame(desc.position).east;
        const double speed = 20.0;
        double turned = 0.0;  // radians round the centre (or the axis)
        glm::dvec3 last = c.position();
        int steps = 0, airborne = 0;
        double lowest = 1e9, highest = -1e9;
        while (turned < 2.0 * 3.14159265358979 && steps < 30000) {
            const glm::dvec3 up = t.body.up(c.position());
            heading = glm::normalize(heading - up * glm::dot(heading, up));
            world.step(kStep, [&] { c.step(glm::vec3(heading * speed)); });
            ++steps;
            const glm::dvec3 now = c.position();
            glm::dvec3 a = last, b = now;
            if (t.body.shape.kind() == Shape::Kind::CurvedCylinder) a.z = b.z = 0.0;
            turned +=
                std::acos(std::clamp(glm::dot(glm::normalize(a), glm::normalize(b)), -1.0, 1.0));
            last = now;
            if (!c.on_ground()) ++airborne;
            if (steps > 60) {
                const double h = t.body.surface(now).height;
                lowest = std::min(lowest, h);
                highest = std::max(highest, h);
            }
        }
        // Round in 2 pi R / 20 m/s = 314 s (18850 steps), give or take a few per cent.
        CHECK(steps == doctest::Approx(18850).epsilon(0.05));
        CHECK(glm::length(c.position() - desc.position) < 50.0);  // back where it began
        CHECK(lowest > 0.45);                                     // never sank into the ground
        CHECK(highest < 0.65);  // nor left it (flat chords and 2 cm padding aside)
        CHECK(airborne < 30);   // on its feet all the way (half a second in all, at most)
    }
}
