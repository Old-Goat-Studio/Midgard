// SPDX-License-Identifier: Apache-2.0
// Physics: one world per room, built on Jolt Physics (double-precision build, so positions are
// exact across worlds thousands of kilometres wide). Jolt's own gravity is off; each step every
// moving body gets the gravity of the world's shape where it is (midgard/body.h), so things fall
// towards a planet's centre, outwards inside a hollow world or a spinning cylinder, and down on
// flat ground.
//
// The world steps at a fixed 60 steps a second whatever the frame rate, so the same scene with
// the same inputs gives the same result every run on one machine.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "midgard/body.h"

namespace midgard::physics {

class PhysicsError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

inline constexpr double kStep = 1.0 / 60.0;                 // seconds per physics step
inline constexpr int kLayers = 16;                          // collision layers 0 to 15
inline constexpr glm::dvec3 kFlatGravity{0.0, -9.81, 0.0};  // a world without a body

enum class Motion {
    Static,     // never moves (ground, walls)
    Dynamic,    // moved by forces, gravity and collisions
    Kinematic,  // moved by the game (doors, platforms); pushes dynamic bodies, never pushed back
};

// A collision shape, in the body's own space and units (the body's scale applies).
struct Collider {
    enum class Kind { Box, Sphere, Capsule, Convex, Mesh, Heightfield };
    Kind kind = Kind::Box;
    glm::vec3 size{1.0f};    // box: full size along x, y and z
    float radius = 0.5f;     // sphere and capsule
    float height = 1.0f;     // capsule: the whole height along y, ends included
    glm::vec3 offset{0.0f};  // where the shape's centre is in the body
    // Convex: the points it wraps. Mesh: triangles (three indices each), for static bodies.
    std::vector<glm::vec3> points;
    std::vector<std::uint32_t> triangles;
    // Heightfield: samples x samples heights (row by row, along x then z), `spacing` metres
    // apart, centred on the body; for static bodies.
    std::uint32_t samples = 0;
    std::vector<float> heights;
    float spacing = 1.0f;
};

struct Surface {
    float friction = 0.5f;    // 0 slides like ice, 1 grips
    float bounciness = 0.0f;  // 0 stops dead, 1 bounces back as fast as it came
};

// One shape of a compound body: a collider placed in the body's own space.
struct Part {
    Collider collider;
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
};

struct BodyDesc {
    Motion motion = Motion::Static;
    Collider collider;
    // A compound body (a group's parts moving as one): these shapes together, in place of
    // `collider`. Its centre of mass is the parts' together.
    std::vector<Part> parts;
    glm::dvec3 position{0.0};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
    float mass = 0.0f;  // kilograms; 0: from its volume at the density of water
    Surface surface;
    int layer = 0;           // 0 to kLayers - 1
    bool sensor = false;     // reports overlaps but doesn't collide (triggers)
    std::uint64_t user = 0;  // the game's own value (the entity), returned by queries
};

using BodyId = std::uint32_t;
inline constexpr BodyId kNoBody = ~0u;

// Which bodies a query sees.
enum class Touching {
    All,
    Sensors,  // only sensors (triggers)
    Solid,    // everything but sensors
};

// Where a ray hit something.
struct RayHit {
    std::uint64_t user = 0;  // the body's user value (the entity)
    BodyId body = kNoBody;
    glm::dvec3 point{0.0};
    glm::vec3 normal{0.0f};  // the surface's, facing the ray
    double distance = 0.0;   // metres from the ray's origin
};

struct BodyState {
    glm::dvec3 position{0.0};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 velocity{0.0f};  // metres per second
    bool awake = false;        // false: resting, skipped until something wakes it
};

class World {
public:
    // A world on this body's shape; without one, gravity is kFlatGravity everywhere.
    explicit World(std::optional<Body> body = std::nullopt);
    ~World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    BodyId add(const BodyDesc& desc);  // throws PhysicsError for a malformed collider
    void remove(BodyId id);
    std::size_t body_count() const;

    // Which layers collide (all of them, by default; the matrix is symmetric).
    void set_collides(int layer_a, int layer_b, bool collides);
    bool collides(int layer_a, int layer_b) const;

    // Advances the world by dt seconds, in whole fixed steps (the remainder carries over to the
    // next call), at most 15 of them (a quarter of a second): after a long stall the world slows
    // down rather than spending ever longer catching up. before_each_step runs before each one
    // (characters move there). Returns the number of steps taken.
    int step(double dt, const std::function<void()>& before_each_step = {});

    BodyState state(BodyId id) const;
    // Kinematic bodies: where to be by the end of the next step (moving there at the speed that
    // takes). Dynamic and static bodies: placed there at once.
    void move_to(BodyId id, const glm::dvec3& position, const glm::quat& rotation);
    void set_velocity(BodyId id, const glm::vec3& velocity);
    void add_impulse(BodyId id, const glm::vec3& impulse);  // kg m/s, at the centre of mass
    void add_force(BodyId id, const glm::vec3& force);      // newtons, for the next step only

    // The first thing a ray from `origin` along `direction` (normalised here) meets within
    // `distance` metres, skipping `ignore` (the caster's own body, say).
    std::optional<RayHit> raycast(const glm::dvec3& origin, const glm::vec3& direction,
                                  double distance, Touching which = Touching::Solid,
                                  BodyId ignore = kNoBody) const;

    // The bodies a shape overlaps where it is placed (their user values, smallest first).
    std::vector<std::uint64_t> overlaps(const Collider& shape, const glm::dvec3& position,
                                        const glm::quat& rotation,
                                        const glm::vec3& scale = glm::vec3(1.0f),
                                        Touching which = Touching::All) const;

    // The gravity a body feels at a point (the shape's, or kFlatGravity).
    glm::dvec3 gravity(const glm::dvec3& position) const;
    const std::optional<Body>& body() const;

private:
    friend class Character;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct CharacterDesc {
    float radius = 0.3f;       // metres
    float height = 1.0f;       // end to end, along "up"; at least twice the radius
    float max_slope = 50.0f;   // degrees: steeper ground is a wall
    float step_height = 0.3f;  // steps up to this high are walked up
    float mass = 70.0f;        // kilograms, for pushing things
    int layer = 1;             // the "player" layer of a new project
    glm::dvec3 position{0.0};  // the middle of the capsule
    std::uint64_t user = 0;
};

// A character the game walks about (Jolt's CharacterVirtual): an upright capsule that stands on
// the ground, slides along walls, walks up steps and gentle slopes, follows moving platforms
// and pushes dynamic bodies. "Up" and gravity come from the world's shape wherever it is, so it
// walks all the way round a planet or along the inside of a cylinder. Other bodies and queries
// see it as a kinematic capsule on its layer.
class Character {
public:
    Character(World& world, const CharacterDesc& desc);  // throws PhysicsError
    ~Character();
    Character(const Character&) = delete;
    Character& operator=(const Character&) = delete;

    // One physics step (call it from World::step's before_each_step): walks at `walk` metres per
    // second (only the part along the ground counts) and, standing, jumps at jump_speed m/s.
    void step(const glm::vec3& walk, float jump_speed = 0.0f);

    glm::dvec3 position() const;
    void set_position(const glm::dvec3& position);  // a jump to somewhere else; stops falling
    glm::vec3 velocity() const;
    glm::vec3 up() const;
    bool on_ground() const;  // standing on something it can stand on
    BodyId body() const;     // the capsule other bodies and queries see
    const CharacterDesc& desc() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace midgard::physics
