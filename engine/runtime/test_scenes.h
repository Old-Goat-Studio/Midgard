// SPDX-License-Identifier: Apache-2.0
// Built-in test scenes for the renderer (midgard_runtime --scene <name>): everything in them is
// generated in code, so image tests need no asset files.
//
//   materials  spheres from smooth to rough (left to right) and plain to metal (top to bottom)
//   textures   one object per material texture: base colour, normal, metal/roughness,
//              occlusion, emissive
//   compressed the same, with every texture through KTX2 (Basis UASTC) and the GPU's format
//   lights     a dim scene lit by a red point light and a blue spot light
//   shadows    sun shadows on flat ground, from 2 to 45 metres away (every cascade)
//   shadows_curved  sun shadows on a small planet, where "up" leans 50 degrees from y
//   sky_day    the analytic sky by day, with distance haze over a wide plain
//   sky_sunset the sun on the horizon: its disc and glow, and low fog lying on the ground
#pragma once

#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "midgard/render/renderer.h"

namespace midgard::test_scenes {

class TestScene {
public:
    virtual ~TestScene() = default;
    virtual void upload(render::Renderer& renderer) = 0;
    virtual std::vector<render::DrawItem> draws(const glm::dvec3& origin) const = 0;
    virtual render::Camera camera(float aspect) const = 0;
    virtual render::Light light(const glm::dvec3& origin) const = 0;
    virtual glm::vec4 sky() const = 0;
    // The sky and fog; by default a flat sky (sky()) and no fog.
    virtual render::Environment environment() const { return {}; }
};

std::unique_ptr<TestScene> make(const std::string& name);  // nullptr for an unknown name
std::vector<std::string> names();

}  // namespace midgard::test_scenes
