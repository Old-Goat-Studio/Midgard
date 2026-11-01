// SPDX-License-Identifier: Apache-2.0
// The light inside a world (roadmap §6, "Day and night on each world shape"): a curved cylinder
// is lit by a light rod down its axis, 5 m across and as long as the cylinder, and a hollow
// world by a light sphere 10 m across at its centre. Their light comes straight down everywhere
// (midgard/sunlight.h); this draws the rod or sphere itself, glowing as brightly as the light
// that reaches the camera's place (dim when the shades there make it night). It casts no shadow.
#pragma once

#include <optional>

#include <glm/glm.hpp>

#include "midgard/render/renderer.h"
#include "midgard/scene.h"

namespace midgard {

inline constexpr double kLightRodDiameter = 5.0;      // metres
inline constexpr double kLightSphereDiameter = 10.0;  // metres

// A cylinder along Z, 1 m across and 1 m long, centred on the origin (sides only: the rod's ends
// meet the cylinder's end walls).
render::MeshData make_light_rod();

// How far a camera must see inside a curved cylinder or a hollow world (its light on the axis or
// at the centre, and its far side overhead): 2.2 radii; 0 for other shapes.
double inside_view_distance(const std::optional<Body>& body);

class WorldLight {
public:
    void upload(render::Renderer& renderer);
    void release(render::Renderer& renderer);
    // The rod or sphere to draw, relative to `origin`, in the scene's light there; none for a
    // world that has neither (or before upload).
    std::optional<render::DrawItem> draw(const Scene& scene, const glm::dvec3& origin) const;

private:
    std::optional<render::MeshId> rod_, sphere_;
};

}  // namespace midgard
