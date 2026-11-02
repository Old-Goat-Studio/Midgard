// SPDX-License-Identifier: Apache-2.0
// Where a scene's point and spot lights are, for the editor's viewport: a light bulb in each
// light's colour, facing the camera, at a size that stays readable at any distance. A bulb
// hidden by something in front (a lamp's shade, a candle's wax) still shows faintly through
// it. A spot light's bulb has its cone; the selected light also shows how far it reaches.
// The game draws none of these: there a light is only its light.
#pragma once

#include <string>
#include <vector>

#include "midgard/render/renderer.h"
#include "midgard/scene.h"

namespace midgard {

struct LightMarkerView {
    glm::dvec3 eye{0.0};              // the camera, in world space
    glm::dvec3 right{1.0, 0.0, 0.0};  // the camera's right and up (unit length)
    glm::dvec3 up{0.0, 1.0, 0.0};
    std::string selected;  // an entity id: its reach is shown too ("" for none)
};

// The lines, relative to `origin` (the camera's), at most about max_lines of them.
std::vector<render::DebugLine> light_markers(const Scene& scene, const glm::dvec3& origin,
                                             const LightMarkerView& view,
                                             size_t max_lines = 20'000);

// A bulb's height in metres at `distance` from the camera (about the same size on screen).
double light_marker_size(double distance);

}  // namespace midgard
