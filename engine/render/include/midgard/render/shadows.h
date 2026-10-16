// SPDX-License-Identifier: Apache-2.0
// Cascaded shadow maps for the sun: the camera's view out to the shadow distance is cut into
// slices, nearest first, and each slice gets its own shadow map, so nearby shadows are sharp and
// distant ones still exist.
//
// The light's axes are built from the local "up" (Light::up), not a fixed world axis: on a
// planet or inside a hollow world "up" is different everywhere, and the sun may stand anywhere
// relative to it (roadmap §6). Each cascade is a sphere around its slice, so its size doesn't
// change as the camera turns, and is moved in whole shadow-map texels, so shadow edges don't
// shimmer as the camera moves.
#pragma once

#include <array>
#include <cstdint>

#include <glm/glm.hpp>

namespace midgard::render {

inline constexpr int kMaxCascades = 4;

struct ShadowSettings {
    int cascades = 4;            // 0 turns shadows off; at most kMaxCascades
    uint32_t resolution = 2048;  // each cascade's shadow map (square)
    // Metres from the camera where shadows end; they fade out over the last tenth.
    float distance = 60.0f;
    // Where the slices are cut: 0 equal lengths, 1 logarithmic (the same ratio each time).
    float split_blend = 0.7f;
    // Metres towards the sun beyond a slice where objects still cast shadows into it.
    float caster_margin = 50.0f;
};

// Each cascade, in the camera-relative space everything is drawn in (render::Camera).
struct ShadowCascades {
    int count = 0;
    // Camera-relative position to the cascade's shadow clip space: x and y -1..1, z 0..1 (0
    // nearest the sun).
    std::array<glm::mat4, kMaxCascades> view_proj{};
    std::array<float, kMaxCascades> far_depth{};    // the view depth where the slice ends
    std::array<float, kMaxCascades> texel_size{};   // metres per shadow-map texel
    std::array<float, kMaxCascades> half_size{};    // metres from the centre to each side
    std::array<float, kMaxCascades> depth_range{};  // metres from z 0 to z 1
};

// Fits the cascades to a perspective camera: its view and projection (as in render::Camera),
// its origin (whose position in the world anchors the texel snapping), the direction sunlight
// travels and the local "up".
ShadowCascades fit_cascades(const glm::mat4& view, const glm::mat4& projection,
                            const glm::dvec3& origin, const glm::vec3& sun_direction,
                            const glm::vec3& up, const ShadowSettings& settings);

// Whether a bounding sphere (camera-relative) can cast a shadow into cascade `index`.
bool casts_into(const ShadowCascades& cascades, int index, const glm::vec3& center, float radius);

}  // namespace midgard::render
