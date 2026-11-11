// SPDX-License-Identifier: Apache-2.0
#include "midgard/physics.h"

#include <Jolt/Jolt.h>
// Jolt.h first: it sets up the configuration every other Jolt header relies on.
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/PhysicsStepListener.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace midgard::physics {

namespace {

// Jolt's types are global: registered once for the whole program.
void init_jolt() {
    static std::once_flag once;
    static bool matches = false;
    std::call_once(once, [] {
        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
        matches = JPH::VerifyJoltVersionID();
    });
    if (!matches)
        throw PhysicsError(
            "Jolt Physics was built with different settings from the engine (double precision "
            "and the rest must match: see vcpkg-overlays/joltphysics)");
}

// Object layers: our collision layer times two, plus one if the body moves. Jolt's broad phase
// keeps moving and non-moving bodies apart (two broad-phase layers), so resting scenery costs
// nothing to test against itself.
JPH::ObjectLayer object_layer(int layer, bool moving) {
    return static_cast<JPH::ObjectLayer>(layer * 2 + (moving ? 1 : 0));
}
bool is_moving(JPH::ObjectLayer l) {
    return (l & 1) != 0;
}
int layer_of(JPH::ObjectLayer l) {
    return l / 2;
}

class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return 2; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer l) const override {
        return JPH::BroadPhaseLayer(is_moving(l) ? 1 : 0);
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer l) const override {
        return l.GetValue() ? "moving" : "still";
    }
#endif
};

class ObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer l, JPH::BroadPhaseLayer bp) const override {
        return is_moving(l) || bp.GetValue() == 1;
    }
};

// Which layers collide: a bit per layer pair.
class LayerPairs final : public JPH::ObjectLayerPairFilter {
public:
    LayerPairs() { masks.fill(0xFFFF); }
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        if (!is_moving(a) && !is_moving(b)) return false;
        return (masks[size_t(layer_of(a))] >> layer_of(b)) & 1;
    }
    std::array<std::uint16_t, kLayers> masks;
};

JPH::Vec3 to_jolt(const glm::vec3& v) {
    return {v.x, v.y, v.z};
}
JPH::RVec3 to_jolt(const glm::dvec3& v) {
    return {v.x, v.y, v.z};
}
JPH::Quat to_jolt(const glm::quat& q) {
    return {q.x, q.y, q.z, q.w};
}
glm::vec3 from_jolt(const JPH::Vec3& v) {
    return {v.GetX(), v.GetY(), v.GetZ()};
}
glm::dvec3 from_jolt(const JPH::DVec3& v) {
    return {v.GetX(), v.GetY(), v.GetZ()};
}
glm::quat from_jolt(const JPH::Quat& q) {
    return {q.GetW(), q.GetX(), q.GetY(), q.GetZ()};
}

JPH::ShapeRefC make(const JPH::ShapeSettings& settings) {
    const JPH::ShapeSettings::ShapeResult result = settings.Create();
    if (result.HasError()) throw PhysicsError("collider: " + std::string(result.GetError()));
    return result.Get();
}

// The collider as a Jolt shape, with the body's scale applied: baked into boxes, spheres and
// capsules (so they stay simple shapes), wrapped around the others.
JPH::ShapeRefC make_shape(const Collider& c, const glm::vec3& scale) {
    const glm::vec3 s = glm::abs(scale);
    const float largest = std::max({s.x, s.y, s.z});
    JPH::ShapeRefC shape;
    bool scaled = false;
    switch (c.kind) {
        case Collider::Kind::Box: {
            const glm::vec3 half = c.size * s * 0.5f;
            if (std::min({half.x, half.y, half.z}) <= 0.0f)
                throw PhysicsError("collider: a box needs a size above zero");
            // Rounded edges (Jolt's convex radius) no thicker than a fifth of the thinnest side.
            const float rounding =
                std::min(JPH::cDefaultConvexRadius, 0.2f * std::min({half.x, half.y, half.z}));
            shape = make(JPH::BoxShapeSettings(to_jolt(half), rounding));
            scaled = true;
            break;
        }
        case Collider::Kind::Sphere:
            if (c.radius <= 0.0f)
                throw PhysicsError("collider: a sphere needs a radius above zero");
            shape = make(JPH::SphereShapeSettings(c.radius * largest));
            scaled = true;
            break;
        case Collider::Kind::Capsule: {
            const float radius = c.radius * std::max(s.x, s.z);
            const float height = c.height * s.y;
            if (radius <= 0.0f) throw PhysicsError("collider: a capsule needs a radius above zero");
            if (height < 2.0f * radius)
                throw PhysicsError("collider: a capsule must be at least as tall as it is wide");
            shape = make(JPH::CapsuleShapeSettings(0.5f * height - radius, radius));
            scaled = true;
            break;
        }
        case Collider::Kind::Convex: {
            if (c.points.size() < 4) throw PhysicsError("collider: a convex shape needs 4 points");
            JPH::Array<JPH::Vec3> points;
            for (const glm::vec3& p : c.points) points.push_back(to_jolt(p));
            shape = make(JPH::ConvexHullShapeSettings(points));
            break;
        }
        case Collider::Kind::Mesh: {
            if (c.triangles.empty() || c.triangles.size() % 3 != 0)
                throw PhysicsError("collider: a mesh needs triangles (three indices each)");
            JPH::VertexList vertices;
            for (const glm::vec3& p : c.points) vertices.push_back(JPH::Float3(p.x, p.y, p.z));
            JPH::IndexedTriangleList triangles;
            for (size_t i = 0; i < c.triangles.size(); i += 3) {
                if (std::max({c.triangles[i], c.triangles[i + 1], c.triangles[i + 2]}) >=
                    c.points.size())
                    throw PhysicsError("collider: a mesh triangle uses a point it doesn't have");
                triangles.push_back(
                    JPH::IndexedTriangle(c.triangles[i], c.triangles[i + 1], c.triangles[i + 2]));
            }
            shape = make(JPH::MeshShapeSettings(vertices, triangles));
            break;
        }
        case Collider::Kind::Heightfield: {
            const std::uint32_t n = c.samples;
            if (n < 2 || c.heights.size() != size_t(n) * n)
                throw PhysicsError("collider: a heightfield needs samples x samples heights");
            const float dz = c.spacing_z > 0.0f ? c.spacing_z : c.spacing;
            const float span_x = float(n - 1) * c.spacing, span_z = float(n - 1) * dz;
            shape = make(JPH::HeightFieldShapeSettings(
                c.heights.data(), JPH::Vec3(-0.5f * span_x, 0.0f, -0.5f * span_z),
                JPH::Vec3(c.spacing, 1.0f, dz), n));
            break;
        }
    }
    if (!scaled && s != glm::vec3(1.0f))
        shape = make(JPH::ScaledShapeSettings(shape, to_jolt(scale)));
    if (c.offset != glm::vec3(0.0f))
        shape = make(JPH::RotatedTranslatedShapeSettings(to_jolt(c.offset * scale),
                                                         JPH::Quat::sIdentity(), shape));
    return shape;
}

}  // namespace

struct World::Impl : JPH::PhysicsStepListener {
    std::optional<Body> body;
    BroadPhaseLayers broad_phase_layers;
    ObjectVsBroadPhase object_vs_broad_phase;
    LayerPairs layer_pairs;
    std::unique_ptr<JPH::TempAllocatorImpl> temp;
    std::unique_ptr<JPH::JobSystemThreadPool> jobs;
    JPH::PhysicsSystem system;
    double carried = 0.0;  // time not yet stepped
    // Kinematic bodies on their way somewhere: moved there by the next step, then stopped.
    struct Target {
        JPH::BodyID id;
        glm::dvec3 position;
        glm::quat rotation;
    };
    std::vector<Target> targets;
    // Water: the sea's level, and each dynamic body's buoyancy (by its id) when it isn't 1.
    // The water's slowing, a second (all under): moving along it and turning, and bobbing up
    // and down (more, so a floating thing soon settles).
    static constexpr float kWaterDrag = 1.5f, kWaterBobDrag = 4.0f;
    std::optional<double> sea_level;
    std::unordered_map<std::uint32_t, float> buoyancy;

    glm::dvec3 gravity(const glm::dvec3& p) const { return body ? body->gravity(p) : kFlatGravity; }
    glm::dvec3 up(const glm::dvec3& p) const {
        const glm::dvec3 g = gravity(p);
        const double length = glm::length(g);
        return length > 1e-9 ? -g / length : glm::dvec3(0.0, 1.0, 0.0);
    }
    double water_depth(const glm::dvec3& p) const {
        if (!sea_level) return -1.0e30;
        return *sea_level - (body ? body->surface(p).height : p.y);
    }

    // Before every step: each moving body gets the gravity where it is, as a force.
    void OnStep(const JPH::PhysicsStepListenerContext& context) override {
        const JPH::BodyLockInterfaceNoLock& locks = system.GetBodyLockInterfaceNoLock();
        const JPH::uint count = system.GetNumActiveBodies(JPH::EBodyType::RigidBody);
        const JPH::BodyID* ids = system.GetActiveBodiesUnsafe(JPH::EBodyType::RigidBody);
        for (JPH::uint i = 0; i < count; ++i) {
            JPH::BodyLockWrite lock(locks, ids[i]);
            if (!lock.Succeeded()) continue;
            JPH::Body& b = lock.GetBody();
            if (!b.IsDynamic()) continue;
            const float inverse_mass = b.GetMotionProperties()->GetInverseMass();
            if (inverse_mass <= 0.0f) continue;
            const glm::dvec3 at = from_jolt(b.GetCenterOfMassPosition());
            const glm::dvec3 g = gravity(at);
            const JPH::Vec3 down(float(g.x), float(g.y), float(g.z));
            b.AddForce(down / inverse_mass);
            // In the water: lifted by what of it is under the surface (a plane through the
            // surface above or below its middle, level there), and slowed.
            if (!sea_level || b.IsSensor()) continue;
            const double depth = water_depth(at);
            const JPH::AABox box = b.GetWorldSpaceBounds();
            const float reach = box.GetExtent().Length();
            if (depth < -double(reach)) continue;  // (nowhere near it)
            const glm::dvec3 up_here = up(at);
            const glm::dvec3 surface = at + up_here * depth;
            const auto found = buoyancy.find(ids[i].GetIndexAndSequenceNumber());
            const float lift = found != buoyancy.end() ? found->second : 1.0f;
            const JPH::Vec3 normal(float(up_here.x), float(up_here.y), float(up_here.z));
            float total = 0.0f, under = 0.0f;
            JPH::Vec3 centre;  // of the part under, from the centre of mass
            b.GetSubmergedVolume(to_jolt(surface), normal, total, under, centre);
            if (total <= 0.0f || under <= 0.0f) continue;
            const float share = std::clamp(under / total, 0.0f, 1.0f);
            // Lifted by its buoyancy times its weight, for the share of it under, at the middle
            // of that part (so it rights itself), and slowed.
            const float weight = float(glm::length(g)) / inverse_mass;
            b.AddForce(normal * (lift * weight * share), b.GetCenterOfMassPosition() + centre);
            const float slowed = std::exp(-kWaterDrag * share * context.mDeltaTime);
            const float bob = std::exp(-kWaterBobDrag * share * context.mDeltaTime);
            JPH::MotionProperties& motion = *b.GetMotionProperties();
            const JPH::Vec3 v = motion.GetLinearVelocity();
            const JPH::Vec3 rising = normal * v.Dot(normal);
            motion.SetLinearVelocity((v - rising) * slowed + rising * bob);
            motion.SetAngularVelocity(motion.GetAngularVelocity() * slowed);
        }
    }

    JPH::BodyID id(BodyId body_id) const { return JPH::BodyID(body_id); }
};

World::World(std::optional<Body> body) : impl_(std::make_unique<Impl>()) {
    init_jolt();
    Impl& w = *impl_;
    w.body = std::move(body);
    w.temp = std::make_unique<JPH::TempAllocatorImpl>(16 * 1024 * 1024);
    // A few threads of its own (Jolt is deterministic whatever their number).
    const unsigned threads = std::clamp(std::thread::hardware_concurrency(), 2u, 4u) - 1;
    w.jobs = std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs,
                                                        JPH::cMaxPhysicsBarriers, int(threads));
    w.system.Init(65536, 0, 65536, 16384, w.broad_phase_layers, w.object_vs_broad_phase,
                  w.layer_pairs);
    w.system.SetGravity(JPH::Vec3::sZero());  // gravity comes from the shape, in OnStep
    w.system.AddStepListener(&w);
}

World::~World() {
    if (!impl_) return;
    JPH::BodyInterface& bodies = impl_->system.GetBodyInterface();
    JPH::BodyIDVector ids;
    impl_->system.GetBodies(ids);
    for (const JPH::BodyID& id : ids) {
        bodies.RemoveBody(id);
        bodies.DestroyBody(id);
    }
    impl_->system.RemoveStepListener(impl_.get());
}

BodyId World::add(const BodyDesc& d) {
    if (d.layer < 0 || d.layer >= kLayers)
        throw PhysicsError("layer " + std::to_string(d.layer) + " is not 0 to " +
                           std::to_string(kLayers - 1));
    const bool compound = !d.parts.empty();
    bool fixed_only =
        d.collider.kind == Collider::Kind::Mesh || d.collider.kind == Collider::Kind::Heightfield;
    if (fixed_only && d.motion == Motion::Dynamic && !d.sensor)
        throw PhysicsError(
            "collider: meshes and heightfields can't be dynamic (use a convex "
            "shape, or make the body static or kinematic)");
    for (const Part& part : d.parts)
        fixed_only = fixed_only || part.collider.kind == Collider::Kind::Mesh ||
                     part.collider.kind == Collider::Kind::Heightfield;
    if (compound && fixed_only && d.motion == Motion::Dynamic && !d.sensor)
        throw PhysicsError(
            "collider: a group that moves can't have mesh or heightfield parts "
            "(use convex shapes)");
    JPH::ShapeRefC shape;
    if (compound) {
        JPH::StaticCompoundShapeSettings settings;
        for (const Part& part : d.parts)
            settings.AddShape(to_jolt(part.position * d.scale),
                              to_jolt(glm::normalize(part.rotation)),
                              make_shape(part.collider, part.scale * d.scale));
        shape = make(settings);
    } else {
        shape = make_shape(d.collider, d.scale);
    }
    const JPH::EMotionType motion = d.motion == Motion::Static      ? JPH::EMotionType::Static
                                    : d.motion == Motion::Kinematic ? JPH::EMotionType::Kinematic
                                                                    : JPH::EMotionType::Dynamic;
    JPH::BodyCreationSettings s(shape, to_jolt(d.position), to_jolt(glm::normalize(d.rotation)),
                                motion, object_layer(d.layer, d.motion != Motion::Static));
    s.mFriction = std::max(d.surface.friction, 0.0f);
    s.mRestitution = std::clamp(d.surface.bounciness, 0.0f, 1.0f);
    s.mIsSensor = d.sensor;
    s.mUserData = d.user;
    s.mGravityFactor = 0.0f;
    if (d.mass > 0.0f && d.motion == Motion::Dynamic) {
        s.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        s.mMassPropertiesOverride.mMass = d.mass;
    }
    JPH::BodyInterface& bodies = impl_->system.GetBodyInterface();
    const JPH::BodyID id =
        bodies.CreateAndAddBody(s, d.motion == Motion::Static ? JPH::EActivation::DontActivate
                                                              : JPH::EActivation::Activate);
    if (id.IsInvalid()) throw PhysicsError("the physics world is full");
    if (d.motion == Motion::Dynamic && !d.sensor && d.buoyancy != 1.0f)
        impl_->buoyancy[id.GetIndexAndSequenceNumber()] = std::max(d.buoyancy, 0.0f);
    return id.GetIndexAndSequenceNumber();
}

void World::remove(BodyId id) {
    JPH::BodyInterface& bodies = impl_->system.GetBodyInterface();
    const JPH::BodyID b = impl_->id(id);
    if (!bodies.IsAdded(b)) return;
    bodies.RemoveBody(b);
    bodies.DestroyBody(b);
    impl_->buoyancy.erase(id);
}

std::size_t World::body_count() const {
    return impl_->system.GetNumBodies();
}

void World::set_collides(int a, int b, bool collides) {
    if (a < 0 || b < 0 || a >= kLayers || b >= kLayers) throw PhysicsError("no such layer");
    auto& m = impl_->layer_pairs.masks;
    const auto bit = [](int layer) { return std::uint16_t(1u << layer); };
    if (collides) {
        m[size_t(a)] |= bit(b);
        m[size_t(b)] |= bit(a);
    } else {
        m[size_t(a)] &= std::uint16_t(~bit(b));
        m[size_t(b)] &= std::uint16_t(~bit(a));
    }
}

bool World::collides(int a, int b) const {
    if (a < 0 || b < 0 || a >= kLayers || b >= kLayers) return false;
    return (impl_->layer_pairs.masks[size_t(a)] >> b) & 1;
}

int World::step(double dt, const std::function<void()>& before_each_step) {
    Impl& w = *impl_;
    w.carried += std::max(dt, 0.0);
    // At most a quarter of a second at once: after a long stall the world slows down rather
    // than spending ever longer catching up.
    constexpr int kMaxSteps = 15;
    int steps = 0;
    JPH::BodyInterface& bodies = w.system.GetBodyInterface();
    while (w.carried >= kStep && steps < kMaxSteps) {
        if (before_each_step) before_each_step();
        for (const Impl::Target& t : w.targets)
            if (bodies.IsAdded(t.id))
                bodies.MoveKinematic(t.id, to_jolt(t.position), to_jolt(t.rotation), float(kStep));
        w.system.Update(float(kStep), 1, w.temp.get(), w.jobs.get());
        for (const Impl::Target& t : w.targets)
            if (bodies.IsAdded(t.id))
                bodies.SetLinearAndAngularVelocity(t.id, JPH::Vec3::sZero(), JPH::Vec3::sZero());
        w.targets.clear();
        w.carried -= kStep;
        ++steps;
    }
    if (steps == kMaxSteps) w.carried = std::min(w.carried, kStep);
    return steps;
}

BodyState World::state(BodyId id) const {
    const JPH::BodyInterface& bodies = impl_->system.GetBodyInterface();
    const JPH::BodyID b = impl_->id(id);
    BodyState s;
    if (!bodies.IsAdded(b)) return s;
    JPH::RVec3 p;
    JPH::Quat q;
    bodies.GetPositionAndRotation(b, p, q);
    s.position = from_jolt(p);
    s.rotation = from_jolt(q);
    s.velocity = from_jolt(bodies.GetLinearVelocity(b));
    s.awake = bodies.IsActive(b);
    return s;
}

void World::move_to(BodyId id, const glm::dvec3& position, const glm::quat& rotation) {
    JPH::BodyInterface& bodies = impl_->system.GetBodyInterface();
    const JPH::BodyID b = impl_->id(id);
    if (!bodies.IsAdded(b)) return;
    if (bodies.GetMotionType(b) == JPH::EMotionType::Kinematic) {
        // Moved during the next step (MoveKinematic gives it the speed to get there), so it
        // pushes what's in its way; the latest target of a frame wins.
        auto& targets = impl_->targets;
        std::erase_if(targets, [&](const Impl::Target& t) { return t.id == b; });
        targets.push_back({b, position, glm::normalize(rotation)});
    } else
        bodies.SetPositionAndRotation(b, to_jolt(position), to_jolt(glm::normalize(rotation)),
                                      JPH::EActivation::Activate);
}

void World::set_velocity(BodyId id, const glm::vec3& velocity) {
    JPH::BodyInterface& bodies = impl_->system.GetBodyInterface();
    const JPH::BodyID b = impl_->id(id);
    if (!bodies.IsAdded(b)) return;
    bodies.SetLinearVelocity(b, to_jolt(velocity));
    bodies.ActivateBody(b);
}

void World::add_impulse(BodyId id, const glm::vec3& impulse) {
    JPH::BodyInterface& bodies = impl_->system.GetBodyInterface();
    const JPH::BodyID b = impl_->id(id);
    if (bodies.IsAdded(b)) bodies.AddImpulse(b, to_jolt(impulse));
}

void World::add_force(BodyId id, const glm::vec3& force) {
    JPH::BodyInterface& bodies = impl_->system.GetBodyInterface();
    const JPH::BodyID b = impl_->id(id);
    if (bodies.IsAdded(b)) bodies.AddForce(b, to_jolt(force), JPH::EActivation::Activate);
}

namespace {

class TouchingFilter final : public JPH::BodyFilter {
public:
    explicit TouchingFilter(Touching which, JPH::BodyID ignore = {})
        : which_(which), ignore_(ignore) {}
    bool ShouldCollide(const JPH::BodyID& id) const override { return id != ignore_; }
    bool ShouldCollideLocked(const JPH::Body& body) const override {
        return which_ == Touching::All || (which_ == Touching::Sensors) == body.IsSensor();
    }

private:
    Touching which_;
    JPH::BodyID ignore_;
};

}  // namespace

std::optional<RayHit> World::raycast(const glm::dvec3& origin, const glm::vec3& direction,
                                     double distance, Touching which, BodyId ignore) const {
    if (!(distance > 0.0) || glm::dot(direction, direction) < 1e-12f) return std::nullopt;
    const glm::vec3 d = glm::normalize(direction);
    const JPH::RRayCast ray{to_jolt(origin), to_jolt(d * float(distance))};
    JPH::RayCastResult hit;
    const TouchingFilter filter(which, ignore == kNoBody ? JPH::BodyID() : JPH::BodyID(ignore));
    if (!impl_->system.GetNarrowPhaseQuery().CastRay(ray, hit, {}, {}, filter)) return std::nullopt;
    RayHit out;
    out.body = hit.mBodyID.GetIndexAndSequenceNumber();
    const JPH::RVec3 point = ray.GetPointOnRay(hit.mFraction);
    out.point = from_jolt(point);
    out.distance = double(hit.mFraction) * distance;
    JPH::BodyLockRead lock(impl_->system.GetBodyLockInterface(), hit.mBodyID);
    if (lock.Succeeded()) {
        const JPH::Body& body = lock.GetBody();
        out.user = body.GetUserData();
        glm::vec3 n = from_jolt(body.GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, point));
        if (glm::dot(n, d) > 0.0f) n = -n;  // the side the ray came from
        out.normal = n;
    }
    return out;
}

double World::sweep_sphere(const glm::dvec3& origin, const glm::vec3& direction, double distance,
                           float radius, Touching which, BodyId ignore) const {
    if (!(distance > 0.0) || glm::dot(direction, direction) < 1e-12f) return 0.0;
    if (radius <= 0.0f) {
        const auto hit = raycast(origin, direction, distance, which, ignore);
        return hit ? hit->distance : distance;
    }
    const glm::vec3 d = glm::normalize(direction);
    const JPH::ShapeRefC sphere = new JPH::SphereShape(radius);
    const JPH::RShapeCast cast(sphere, JPH::Vec3::sReplicate(1.0f),
                               JPH::RMat44::sTranslation(to_jolt(origin)),
                               to_jolt(d * float(distance)));
    JPH::ShapeCastSettings settings;
    settings.mBackFaceModeTriangles = JPH::EBackFaceMode::CollideWithBackFaces;
    settings.mBackFaceModeConvex = JPH::EBackFaceMode::CollideWithBackFaces;
    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> hit;
    const TouchingFilter filter(which, ignore == kNoBody ? JPH::BodyID() : JPH::BodyID(ignore));
    impl_->system.GetNarrowPhaseQuery().CastShape(cast, settings, to_jolt(origin), hit, {}, {},
                                                  filter);
    if (!hit.HadHit()) return distance;
    // A little short of the contact, so the sphere ends clear of it.
    return std::max(double(hit.mHit.mFraction) * distance - 0.01, 0.0);
}

std::vector<std::uint64_t> World::overlaps(const Collider& c, const glm::dvec3& position,
                                           const glm::quat& rotation, const glm::vec3& scale,
                                           Touching which) const {
    const JPH::ShapeRefC shape = make_shape(c, scale);
    const JPH::Quat q = to_jolt(glm::normalize(rotation));
    const JPH::RVec3 centre = to_jolt(position) + JPH::RVec3(q * shape->GetCenterOfMass());
    JPH::CollideShapeSettings settings;
    settings.mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> hits;
    const TouchingFilter filter(which);
    impl_->system.GetNarrowPhaseQuery().CollideShape(shape, JPH::Vec3::sReplicate(1.0f),
                                                     JPH::RMat44::sRotationTranslation(q, centre),
                                                     settings, centre, hits, {}, {}, filter);
    const JPH::BodyInterface& bodies = impl_->system.GetBodyInterface();
    std::vector<std::uint64_t> out;
    for (const JPH::CollideShapeResult& hit : hits.mHits)
        out.push_back(bodies.GetUserData(hit.mBodyID2));
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

glm::dvec3 World::gravity(const glm::dvec3& position) const {
    return impl_->gravity(position);
}

const std::optional<Body>& World::body() const {
    return impl_->body;
}

void World::set_water(std::optional<double> sea_level) {
    impl_->sea_level = sea_level;
}

std::optional<double> World::water() const {
    return impl_->sea_level;
}

double World::water_depth(const glm::dvec3& position) const {
    return impl_->water_depth(position);
}

// ---- characters -----------------------------------------------------------------------------

namespace {

// Queries for a character: everything solid on layers its layer collides with.
class SolidOnly final : public JPH::BodyFilter {
public:
    bool ShouldCollideLocked(const JPH::Body& body) const override { return !body.IsSensor(); }
};

}  // namespace

struct Character::Impl {
    World::Impl* world = nullptr;
    CharacterDesc desc;
    JPH::Ref<JPH::CharacterVirtual> character;
    float half_cylinder = 0.0f;
    bool swimming = false;
    // How deep its middle floats: the water at its shoulders.
    // Swimming starts with its middle this deep (chest-deep water, standing)...
    float float_depth() const { return 0.3f * desc.height; }
    // ...and it floats with its middle this deep: lying along the surface as a swimmer does,
    // its back at the surface, not standing in the water up to its shoulders.
    float swim_depth() const { return 0.08f * desc.height; }

    JPH::Vec3 up_at(const JPH::RVec3& p) const {
        const glm::dvec3 g = world->gravity(from_jolt(p));
        const double length = glm::length(g);
        if (length < 1e-9) return JPH::Vec3::sAxisY();
        const glm::dvec3 up = -g / length;
        return JPH::Vec3(float(up.x), float(up.y), float(up.z));
    }
};

Character::Character(World& world, const CharacterDesc& d) : impl_(std::make_unique<Impl>()) {
    Impl& c = *impl_;
    c.world = world.impl_.get();
    c.desc = d;
    if (d.radius <= 0.0f || d.height < 2.0f * d.radius)
        throw PhysicsError("character: the capsule must be at least as tall as it is wide");
    if (d.layer < 0 || d.layer >= kLayers) throw PhysicsError("character: no such layer");
    c.half_cylinder = 0.5f * d.height - d.radius;
    const JPH::ShapeRefC capsule = make(JPH::CapsuleShapeSettings(c.half_cylinder, d.radius));

    JPH::Ref<JPH::CharacterVirtualSettings> s = new JPH::CharacterVirtualSettings();
    s->mShape = capsule;
    s->mMaxSlopeAngle = glm::radians(std::clamp(d.max_slope, 0.0f, 89.0f));
    s->mMass = std::max(d.mass, 1.0f);
    // Contacts below the lower half-sphere's centre hold it up; the rest are walls.
    s->mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), c.half_cylinder);
    s->mInnerBodyShape = capsule;  // so bodies and queries see it
    s->mInnerBodyLayer = object_layer(d.layer, true);
    s->mEnhancedInternalEdgeRemoval = true;  // smooth over the edges between mesh triangles
    const JPH::RVec3 at = to_jolt(d.position);
    const JPH::Vec3 up = c.up_at(at);
    s->mUp = up;
    c.character = new JPH::CharacterVirtual(s, at, JPH::Quat::sFromTo(JPH::Vec3::sAxisY(), up),
                                            d.user, &c.world->system);
    c.world->system.GetBodyInterface().SetUserData(c.character->GetInnerBodyID(), d.user);
}

Character::~Character() = default;

void Character::step(const glm::vec3& walk, float jump_speed) {
    Impl& c = *impl_;
    JPH::CharacterVirtual& ch = *c.character;
    const JPH::Vec3 up = c.up_at(ch.GetPosition());
    ch.SetUp(up);
    ch.SetRotation(JPH::Quat::sFromTo(JPH::Vec3::sAxisY(), up));
    ch.UpdateGroundVelocity();

    // Walking: only along the ground (up and down a slope as it goes). Up and down: while
    // standing, the ground's own movement (a lift carries it), a jump, and only the part of
    // gravity that presses into the ground: none along it, so it doesn't slide down a slope it
    // can stand on, yet it stays pressed to ground that curves away (a planet). In the air, its
    // own speed plus gravity.
    JPH::Vec3 along = to_jolt(walk);
    along -= up * along.Dot(up);
    const JPH::Vec3 velocity = ch.GetLinearVelocity();
    const JPH::Vec3 ground = ch.GetGroundVelocity();
    const bool standing = ch.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround &&
                          (velocity - ground).Dot(up) < 0.1f;
    const glm::dvec3 g = c.world->gravity(from_jolt(ch.GetPosition()));
    const JPH::Vec3 gravity(float(g.x), float(g.y), float(g.z));
    // Swimming: its middle deeper than it floats. The water holds it at the surface (a damped
    // spring in place of gravity) and it swims along it, slower than it walks. Where the
    // ground comes up under it, it stands and walks out; at a bank it can jump.
    const float depth = float(c.world->water_depth(from_jolt(ch.GetPosition())));
    // (It starts swimming chest-deep, floats up to the surface, and keeps swimming there until
    // its feet find the ground again.)
    c.swimming = depth > c.float_depth() || (c.swimming && depth > 0.0f && !standing);
    JPH::Vec3 next;
    if (c.swimming) {
        constexpr float kSpring = 20.0f, kDamping = 9.0f, kSwim = 0.6f;
        const float rising = velocity.Dot(up);
        float speed =
            rising + (kSpring * (depth - c.swim_depth()) - kDamping * rising) * float(kStep);
        speed = std::clamp(speed, -20.0f, 3.0f);
        if (jump_speed > 0.0f && depth < c.swim_depth() + 0.3f) speed = jump_speed;
        next = up * speed + along * kSwim;
    } else if (standing) {
        const JPH::Vec3 normal = ch.GetGroundNormal();
        const float speed = along.Length();
        JPH::Vec3 on_slope = along - normal * along.Dot(normal);
        if (speed > 0.0f && on_slope.LengthSq() > 1e-12f) on_slope = on_slope.Normalized() * speed;
        next = up * ground.Dot(up) + on_slope + normal * (gravity.Dot(normal) * float(kStep));
        if (jump_speed > 0.0f) next += up * jump_speed;
    } else {
        next = up * velocity.Dot(up) + gravity * float(kStep) + along;
    }
    ch.SetLinearVelocity(next);

    JPH::CharacterVirtual::ExtendedUpdateSettings settings;
    settings.mStickToFloorStepDown = c.swimming ? JPH::Vec3::sZero() : -up * 0.5f;
    settings.mWalkStairsStepUp = up * c.desc.step_height;
    const JPH::ObjectLayer layer = object_layer(c.desc.layer, true);
    const SolidOnly solid;
    const JPH::RVec3 before = ch.GetPosition();
    const auto update = [&] {
        ch.ExtendedUpdate(float(kStep), gravity, settings,
                          c.world->system.GetDefaultBroadPhaseLayerFilter(layer),
                          c.world->system.GetDefaultLayerFilter(layer), solid, {}, *c.world->temp);
    };
    update();
    // Walking on ground with nothing in the way, the ground's contacts can, at rare exact
    // positions and speeds (where two triangles meet), cancel the step entirely, and with the
    // same input the next step too: the character would stand still for good (seen walking on
    // curved terrain). A step that should have gone somewhere but went almost nowhere is taken
    // again from a centimetre higher, clear of those contacts (sticking to the floor brings it
    // back down). A wall really in the way stops the step either way.
    if (standing && !c.swimming && jump_speed <= 0.0f) {
        const float wanted = (next - up * next.Dot(up)).Length() * float(kStep);
        const float moved = JPH::Vec3(ch.GetPosition() - before).Length();
        if (wanted > 0.01f && moved < 0.001f * wanted) {
            ch.SetPosition(before + JPH::RVec3(up * 0.01f));
            ch.SetLinearVelocity(next);
            update();
        }
    }
}

glm::dvec3 Character::position() const {
    return from_jolt(impl_->character->GetPosition());
}

void Character::set_position(const glm::dvec3& position) {
    impl_->character->SetPosition(to_jolt(position));
    impl_->character->SetLinearVelocity(JPH::Vec3::sZero());
}

glm::vec3 Character::velocity() const {
    return from_jolt(impl_->character->GetLinearVelocity());
}

glm::vec3 Character::up() const {
    return from_jolt(impl_->character->GetUp());
}

bool Character::on_ground() const {
    return impl_->character->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
}

bool Character::swimming() const {
    return impl_->swimming;
}

BodyId Character::body() const {
    return impl_->character->GetInnerBodyID().GetIndexAndSequenceNumber();
}

const CharacterDesc& Character::desc() const {
    return impl_->desc;
}

}  // namespace midgard::physics
