// SPDX-License-Identifier: Apache-2.0
#include "midgard/light_markers.h"

#include <algorithm>
#include <cmath>

namespace midgard {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr float kHiddenAlpha = 0.3f;  // a bulb behind something: this faint
constexpr int kSegments = 20;

class Marker {
public:
    Marker(std::vector<render::DebugLine>& out, const glm::dvec3& origin, size_t max)
        : out_(out), origin_(origin), max_(max) {}

    bool full() const { return out_.size() + 2 > max_; }

    // A line seen where nothing hides it, and faintly where something does.
    void add(const glm::dvec3& a, const glm::dvec3& b, const glm::vec4& c, bool ghost = true) {
        if (full()) return;
        const glm::vec3 pa(a - origin_), pb(b - origin_);
        out_.push_back({pa, pb, c, false});
        if (ghost) out_.push_back({pa, pb, glm::vec4(glm::vec3(c), c.a * kHiddenAlpha), true});
    }

    void arc(const glm::dvec3& centre, const glm::dvec3& u, const glm::dvec3& v, double radius,
             double from, double to, const glm::vec4& c, bool ghost = true) {
        const int steps = std::max(2, int(std::ceil(kSegments * (to - from) / (2 * kPi))));
        glm::dvec3 last = centre + radius * (u * std::cos(from) + v * std::sin(from));
        for (int i = 1; i <= steps; ++i) {
            const double a = from + (to - from) * i / steps;
            const glm::dvec3 next = centre + radius * (u * std::cos(a) + v * std::sin(a));
            add(last, next, c, ghost);
            last = next;
        }
    }

private:
    std::vector<render::DebugLine>& out_;
    glm::dvec3 origin_;
    size_t max_;
};

// Any unit vector at right angles to d.
glm::dvec3 across(const glm::dvec3& d) {
    const glm::dvec3 other = std::abs(d.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
    return glm::normalize(glm::cross(d, other));
}

// The bulb: a glass globe, its neck, a screw base and a filament, with rays round the top.
// at: the light's position (the middle of the globe); r, u: the camera's right and up.
void bulb(Marker& m, const glm::dvec3& at, const glm::dvec3& r, const glm::dvec3& u, double size,
          const glm::vec4& c) {
    const auto p = [&](double x, double y) { return at + size * (x * r + y * u); };
    const double globe = 0.3;
    const double open = glm::radians(50.0);  // the globe is open below, for its neck
    m.arc(at, r, u, globe * size, -kPi / 2 + open, 3 * kPi / 2 - open, c);
    const double nx = globe * std::sin(open), ny = -globe * std::cos(open);
    m.add(p(-nx, ny), p(-0.12, -0.42), c);  // the neck
    m.add(p(nx, ny), p(0.12, -0.42), c);
    for (const double y : {-0.42, -0.5, -0.58}) m.add(p(-0.12, y), p(0.12, y), c);  // the screw
    m.add(p(-0.12, -0.42), p(-0.12, -0.58), c);
    m.add(p(0.12, -0.42), p(0.12, -0.58), c);
    m.add(p(-0.12, -0.58), p(-0.05, -0.66), c);  // its tip
    m.add(p(0.12, -0.58), p(0.05, -0.66), c);
    m.add(p(-0.05, -0.66), p(0.05, -0.66), c);
    m.add(p(-0.08, -0.3), p(-0.08, -0.02), c);  // the filament on its two wires
    m.add(p(0.08, -0.3), p(0.08, -0.02), c);
    m.add(p(-0.08, -0.02), p(-0.04, 0.06), c);
    m.add(p(-0.04, 0.06), p(0.0, -0.02), c);
    m.add(p(0.0, -0.02), p(0.04, 0.06), c);
    m.add(p(0.04, 0.06), p(0.08, -0.02), c);
    for (int i = 0; i < 7; ++i) {  // rays, over the top
        const double a = kPi * (i + 0.5) / 7.0;
        const glm::dvec3 d = std::cos(a) * r + std::sin(a) * u;
        m.add(at + size * 0.42 * d, at + size * 0.58 * d, c, false);
    }
}

}  // namespace

double light_marker_size(double distance) {
    return std::clamp(distance * 0.06, 0.12, 50.0);
}

std::vector<render::DebugLine> light_markers(const Scene& scene, const glm::dvec3& origin,
                                             const LightMarkerView& view, size_t max_lines) {
    std::vector<render::DebugLine> out;
    Marker m(out, origin, max_lines);
    const entt::registry& reg = scene.registry;
    for (entt::entity e : reg.view<const LocalLightSource, const EntityInfo>()) {
        if (m.full()) break;
        const LocalLightSource& light = reg.get<LocalLightSource>(e);
        const bool selected = !view.selected.empty() && reg.get<EntityInfo>(e).id == view.selected;
        const glm::dmat4 world = scene.world_matrix(e);
        const glm::dvec3 at(world[3]);
        const double size = light_marker_size(glm::distance(at, view.eye));
        // Its own colour, lifted towards white so a dim or dark one still shows.
        const glm::vec4 c(glm::mix(light.color, glm::vec3(1.0f), 0.35f), 1.0f);
        bulb(m, at, view.right, view.up, size, c);
        const glm::vec4 faint(glm::vec3(c), 0.45f);
        if (light.spot) {  // the cone: as far as it reaches when selected, else a short one
            const glm::dvec3 d = glm::normalize(glm::dvec3(world * glm::dvec4(0, 0, -1, 0)));
            const glm::dvec3 a = across(d), b = glm::cross(d, a);
            const double length = selected ? light.range : std::min<double>(light.range, size * 4);
            const double radius = length * std::tan(glm::radians(double(light.outer_angle)));
            const glm::dvec3 end = at + d * length;
            m.arc(end, a, b, radius, 0.0, 2 * kPi, faint, false);
            for (int i = 0; i < 4; ++i) {
                const double t = kPi * 0.5 * i;
                m.add(at + d * size * 0.3, end + radius * (std::cos(t) * a + std::sin(t) * b),
                      faint, false);
            }
        } else if (selected) {  // a point light's reach: a sphere of three circles
            const glm::dvec3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
            for (int i = 0; i < 3; ++i)
                m.arc(at, axes[(i + 1) % 3], axes[(i + 2) % 3], light.range, 0.0, 2 * kPi, faint,
                      false);
        }
    }
    return out;
}

}  // namespace midgard
