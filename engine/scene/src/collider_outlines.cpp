// SPDX-License-Identifier: Apache-2.0
#include "midgard/collider_outlines.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

namespace midgard {
namespace {

const glm::vec4 kStatic{0.55f, 0.75f, 0.5f, 1.0f};
const glm::vec4 kDynamic{1.0f, 0.6f, 0.2f, 1.0f};
const glm::vec4 kKinematic{0.4f, 0.65f, 1.0f, 1.0f};
const glm::vec4 kTrigger{1.0f, 0.85f, 0.2f, 1.0f};
const glm::vec4 kPlayer{0.3f, 0.9f, 0.95f, 1.0f};
constexpr int kCircleSegments = 24;
constexpr double kPi = 3.14159265358979323846;

class Lines {
public:
    Lines(std::vector<render::DebugLine>& out, const glm::dvec3& origin, size_t max)
        : out_(out), origin_(origin), max_(max) {}

    bool full() const { return out_.size() >= max_; }
    void add(const glm::dvec3& a, const glm::dvec3& b, const glm::vec4& c) {
        if (!full()) out_.push_back({glm::vec3(a - origin_), glm::vec3(b - origin_), c});
    }
    // An arc from angle `from` to `to` (radians) round `centre`, in the plane of u and v.
    void arc(const glm::dvec3& centre, const glm::dvec3& u, const glm::dvec3& v, double radius,
             double from, double to, const glm::vec4& c) {
        const int steps = std::max(2, int(std::ceil(kCircleSegments * (to - from) / (2 * kPi))));
        glm::dvec3 last = centre + radius * (u * std::cos(from) + v * std::sin(from));
        for (int i = 1; i <= steps; ++i) {
            const double a = from + (to - from) * i / steps;
            const glm::dvec3 next = centre + radius * (u * std::cos(a) + v * std::sin(a));
            add(last, next, c);
            last = next;
        }
    }
    void circle(const glm::dvec3& centre, const glm::dvec3& u, const glm::dvec3& v, double radius,
                const glm::vec4& c) {
        arc(centre, u, v, radius, 0.0, 2 * kPi, c);
    }

private:
    std::vector<render::DebugLine>& out_;
    glm::dvec3 origin_;
    size_t max_;
};

// An entity's placement: its world matrix, the unit directions of its axes, and their scale.
struct Placement {
    glm::dmat4 m;
    glm::dvec3 axis[3];
    glm::dvec3 scale;

    explicit Placement(const glm::dmat4& world) : m(world) {
        for (int i = 0; i < 3; ++i) {
            scale[i] = glm::length(glm::dvec3(world[i]));
            axis[i] = scale[i] > 0.0 ? glm::dvec3(world[i]) / scale[i] : glm::dvec3(0.0);
        }
    }
    glm::dvec3 at(const glm::vec3& local) const {
        return glm::dvec3(m * glm::dvec4(glm::dvec3(local), 1.0));
    }
};

void box(Lines& lines, const Placement& p, const glm::vec3& centre, const glm::vec3& size,
         const glm::vec4& c) {
    glm::dvec3 corner[8];
    for (int i = 0; i < 8; ++i)
        corner[i] =
            p.at(centre + 0.5f * size * glm::vec3(i & 1 ? 1 : -1, i & 2 ? 1 : -1, i & 4 ? 1 : -1));
    for (int i = 0; i < 8; ++i)
        for (int bit : {1, 2, 4})
            if (!(i & bit)) lines.add(corner[i], corner[i | bit], c);
}

// A capsule along `up` round `centre`: rings at the ends of its middle, lines down its sides,
// and half-circles over each end.
void capsule(Lines& lines, const glm::dvec3& centre, const glm::dvec3& up, const glm::dvec3& x,
             double radius, double height, const glm::vec4& c) {
    const glm::dvec3 z = glm::cross(x, up);
    const double half = std::max(0.5 * height - radius, 0.0);
    const glm::dvec3 top = centre + up * half, bottom = centre - up * half;
    lines.circle(top, x, z, radius, c);
    lines.circle(bottom, x, z, radius, c);
    for (const glm::dvec3& side : {x, -x, z, -z})
        lines.add(top + side * radius, bottom + side * radius, c);
    for (const glm::dvec3& across : {x, z}) {
        lines.arc(top, across, up, radius, 0.0, kPi, c);
        lines.arc(bottom, across, -up, radius, 0.0, kPi, c);
    }
}

// Any direction perpendicular to `up`.
glm::dvec3 across(const glm::dvec3& up) {
    const glm::dvec3 any = std::abs(up.x) < 0.9 ? glm::dvec3(1, 0, 0) : glm::dvec3(0, 0, 1);
    return glm::normalize(glm::cross(up, any));
}

void collider(Lines& lines, const Placement& p, const ColliderShape& s, const MeshLookup& mesh,
              const std::string& own_mesh, const glm::vec4& c) {
    using Kind = physics::Collider::Kind;
    const glm::dvec3 centre = p.at(s.offset);
    switch (s.kind) {
        case Kind::Box:
            box(lines, p, s.offset, s.size, c);
            break;
        case Kind::Sphere: {  // as physics sizes it: by the largest scale
            const double r = s.radius * std::max({p.scale.x, p.scale.y, p.scale.z});
            lines.circle(centre, p.axis[0], p.axis[1], r, c);
            lines.circle(centre, p.axis[1], p.axis[2], r, c);
            lines.circle(centre, p.axis[2], p.axis[0], r, c);
            break;
        }
        case Kind::Capsule:
            capsule(lines, centre, p.axis[1], p.axis[0], s.radius * std::max(p.scale.x, p.scale.z),
                    s.height * p.scale.y, c);
            break;
        case Kind::Convex:
        case Kind::Mesh: {  // the mesh's edges (a convex collider wraps them)
            const render::MeshData* data = mesh(s.mesh.empty() ? own_mesh : s.mesh);
            if (!data) break;
            std::set<std::pair<std::uint32_t, std::uint32_t>> edges;
            for (size_t i = 0; i + 2 < data->indices.size(); i += 3)
                for (int k = 0; k < 3; ++k) {
                    const std::uint32_t a = data->indices[i + k],
                                        b = data->indices[i + (k + 1) % 3];
                    edges.insert({std::min(a, b), std::max(a, b)});
                }
            const auto point = [&](std::uint32_t i) {
                const float* v = data->vertices[i].position;
                return p.at(s.offset + glm::vec3(v[0], v[1], v[2]));
            };
            for (const auto& [a, b] : edges) {
                if (lines.full()) break;
                if (data->vertices[a].position[0] == data->vertices[b].position[0] &&
                    data->vertices[a].position[1] == data->vertices[b].position[1] &&
                    data->vertices[a].position[2] == data->vertices[b].position[2])
                    continue;  // the same corner twice (split for its normals)
                lines.add(point(a), point(b), c);
            }
            break;
        }
        case Kind::Heightfield: {  // the grid of samples
            const std::uint32_t n = s.samples;
            if (n < 2 || s.heights.size() != size_t(n) * n) break;
            const float half = 0.5f * float(n - 1) * s.spacing;
            const auto sample = [&](std::uint32_t i, std::uint32_t j) {
                return p.at(s.offset + glm::vec3(float(i) * s.spacing - half,
                                                 s.heights[size_t(j) * n + i],
                                                 float(j) * s.spacing - half));
            };
            for (std::uint32_t j = 0; j < n; ++j)
                for (std::uint32_t i = 0; i < n; ++i) {
                    if (i + 1 < n) lines.add(sample(i, j), sample(i + 1, j), c);
                    if (j + 1 < n) lines.add(sample(i, j), sample(i, j + 1), c);
                }
            break;
        }
    }
}

}  // namespace

std::vector<render::DebugLine> collider_outlines(const Scene& scene, const glm::dvec3& origin,
                                                 const MeshLookup& mesh, size_t max_lines) {
    std::vector<render::DebugLine> out;
    Lines lines(out, origin, max_lines);
    const entt::registry& reg = scene.registry;
    for (entt::entity e : reg.view<const EntityInfo, const Transform>()) {
        if (lines.full()) break;
        const auto* shape = reg.try_get<ColliderShape>(e);
        const auto* trigger = reg.try_get<Trigger>(e);
        const auto* player = reg.try_get<Player>(e);
        if (!shape && !trigger && !player) continue;
        const Placement p(scene.world_matrix(e));
        if (player) {  // the character: an upright capsule (its own collider is left out)
            const glm::dvec3 at = glm::dvec3(p.m[3]);
            const glm::dvec3 up =
                scene.settings.body ? scene.settings.body->up(at) : glm::dvec3(0, 1, 0);
            capsule(lines, at, up, across(up), player->radius, player->height, kPlayer);
        } else if (shape) {
            glm::vec4 c = kStatic;
            if (const auto* rb = reg.try_get<RigidBody>(e))
                c = rb->motion == physics::Motion::Dynamic     ? kDynamic
                    : rb->motion == physics::Motion::Kinematic ? kKinematic
                                                               : kStatic;
            const auto* own = reg.try_get<MeshRef>(e);
            collider(lines, p, *shape, mesh, own ? own->asset : std::string(), c);
        }
        if (trigger) box(lines, p, glm::vec3(0.0f), trigger->size, kTrigger);
    }
    return out;
}

}  // namespace midgard
