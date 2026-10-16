// SPDX-License-Identifier: Apache-2.0
// Built-in test scenes for the renderer (midgard_runtime --scene <name>): everything in them is
// generated in code, so image tests need no asset files.
//
//   materials  spheres from smooth to rough (left to right) and plain to metal (top to bottom)
//   textures   one object per material texture: base colour, normal, metal/roughness,
//              occlusion, emissive
//   compressed the same, with every texture through KTX2 (Basis UASTC) and the GPU's format
//   lights     a dim scene lit by a red point light and a blue spot light
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
};

std::unique_ptr<TestScene> make(const std::string& name);  // nullptr for an unknown name
std::vector<std::string> names();

}  // namespace midgard::test_scenes
