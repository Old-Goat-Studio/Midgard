// SPDX-License-Identifier: Apache-2.0
#include "midgard/render/shadows.h"

#include <algorithm>
#include <cmath>

namespace midgard::render {

namespace {

// Texel snapping is anchored to a 100 m grid near the camera rather than the world's origin:
// the numbers stay small (worlds are thousands of kilometres across), and crossing a grid line
// moves shadows by less than a texel, once.
constexpr double kSnapGrid = 100.0;

glm::vec3 unproject(const glm::mat4& inverse_projection, float x, float y, float z) {
    const glm::vec4 p = inverse_projection * glm::vec4(x, y, z, 1.0f);
    return glm::vec3(p) / p.w;
}

}  // namespace

ShadowCascades fit_cascades(const glm::mat4& view, const glm::mat4& projection,
                            const glm::dvec3& origin, const glm::vec3& sun_direction,
                            const glm::vec3& up, const ShadowSettings& settings) {
    ShadowCascades out;
    const int count = std::clamp(settings.cascades, 0, kMaxCascades);
    if (count == 0 || settings.resolution < 4 || settings.distance <= 0.0f) return out;

    // The camera's frustum: its near and far depth, and the rays through its four corners
    // (scaled so each reaches depth 1; the camera looks down -z).
    const glm::mat4 inverse_projection = glm::inverse(projection);
    const float near_depth = -unproject(inverse_projection, 0, 0, 0).z;
    const float far_depth = -unproject(inverse_projection, 0, 0, 1).z;
    std::array<glm::vec3, 4> rays;
    for (int i = 0; i < 4; ++i) {
        const glm::vec3 p =
            unproject(inverse_projection, i & 1 ? 1.0f : -1.0f, i & 2 ? 1.0f : -1.0f, 1.0f);
        rays[size_t(i)] = p / -p.z;
    }
    const float end = std::min(settings.distance, far_depth);
    if (!(end > near_depth)) return out;

    // Where each slice ends: between equal lengths and a logarithmic series (the same ratio
    // from each slice to the next, so each covers about as many texels per pixel).
    std::array<float, kMaxCascades + 1> split{};
    split[0] = near_depth;
    for (int i = 1; i <= count; ++i) {
        const float f = float(i) / float(count);
        const float equal = near_depth + (end - near_depth) * f;
        const float logarithmic = near_depth * std::pow(end / near_depth, f);
        split[size_t(i)] = equal + (logarithmic - equal) * settings.split_blend;
    }
    split[size_t(count)] = end;

    // The light's axes, in the local "up" frame: `across` is level (perpendicular to up), so the
    // shadow map is laid out the same way relative to the ground wherever the player stands.
    // When the sun is straight overhead (or underfoot) any level axis will do.
    const glm::vec3 forward = glm::normalize(sun_direction);
    const glm::vec3 local_up = glm::normalize(up);
    glm::vec3 across = glm::cross(forward, local_up);
    if (glm::dot(across, across) < 1e-6f) {
        const glm::vec3 any = std::abs(local_up.x) < 0.9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 0, 1);
        across = glm::cross(any, local_up);
    }
    across = glm::normalize(across);
    const glm::vec3 lift = glm::cross(across, forward);

    // Where the camera sits within its snapping cell (rounded to a float first, so the same
    // view far from the world's origin snaps exactly as it does near it).
    const glm::dvec3 anchor =
        glm::dvec3(glm::vec3(origin - glm::floor(origin / kSnapGrid) * kSnapGrid));

    const glm::mat4 inverse_view = glm::inverse(view);
    out.count = count;
    for (int c = 0; c < count; ++c) {
        std::array<glm::vec3, 8> corners;
        glm::vec3 center(0.0f);
        for (int i = 0; i < 8; ++i) {
            const float depth = split[size_t(c + (i >> 2))];
            const glm::vec3 p(inverse_view * glm::vec4(rays[size_t(i & 3)] * depth, 1.0f));
            corners[size_t(i)] = p;
            center += p / 8.0f;
        }
        float radius = 0.0f;
        for (const glm::vec3& p : corners) radius = std::max(radius, glm::length(p - center));
        radius = std::ceil(radius * 16.0f) / 16.0f;  // the same size every frame

        // Two texels of slack, so the sphere still fits after the centre moves to a texel.
        const float texel = 2.0f * radius / float(settings.resolution - 2);
        const float half = texel * float(settings.resolution) * 0.5f;

        // Snap the centre to whole texels across the light, measured from the anchor.
        const glm::dvec3 from_anchor = anchor + glm::dvec3(center);
        for (const glm::vec3& axis : {across, lift}) {
            const double s = glm::dot(from_anchor, glm::dvec3(axis));
            const double snapped = std::round(s / texel) * texel;
            center += axis * float(snapped - s);
        }

        // Light space: x across, y lift, z from the near plane (towards the sun, the margin
        // beyond the sphere) to the far side of the sphere.
        const float range = 2.0f * half + settings.caster_margin;
        glm::mat4 m(0.0f);
        for (int k = 0; k < 3; ++k) {
            m[k][0] = across[k] / half;
            m[k][1] = lift[k] / half;
            m[k][2] = forward[k] / range;
        }
        m[3][0] = -glm::dot(across, center) / half;
        m[3][1] = -glm::dot(lift, center) / half;
        m[3][2] = (-glm::dot(forward, center) + half + settings.caster_margin) / range;
        m[3][3] = 1.0f;

        out.view_proj[size_t(c)] = m;
        out.far_depth[size_t(c)] = split[size_t(c + 1)];
        out.texel_size[size_t(c)] = texel;
        out.half_size[size_t(c)] = half;
        out.depth_range[size_t(c)] = range;
    }
    return out;
}

bool casts_into(const ShadowCascades& cascades, int index, const glm::vec3& center, float radius) {
    if (index < 0 || index >= cascades.count) return false;
    const size_t i = size_t(index);
    const glm::vec4 p = cascades.view_proj[i] * glm::vec4(center, 1.0f);
    const float r = radius / cascades.half_size[i];
    // Anything between the sun and the far side may cast (nearer than the near plane too: the
    // shadow pass clamps depth, where the GPU can).
    return std::abs(p.x) <= 1.0f + r && std::abs(p.y) <= 1.0f + r &&
           p.z <= 1.0f + radius / cascades.depth_range[i];
}

}  // namespace midgard::render
