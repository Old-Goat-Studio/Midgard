// SPDX-License-Identifier: Apache-2.0
#include <doctest/doctest.h>

#include <cmath>

#include <glm/glm.hpp>

#include "midgard/render/mesh_data.h"

using midgard::render::make_cube;

TEST_CASE("the cube has 6 faces of 4 vertices and 2 triangles") {
    const auto cube = make_cube();
    CHECK(cube.vertices.size() == 24);
    CHECK(cube.indices.size() == 36);
    for (const auto& v : cube.vertices)
        for (float p : v.position) CHECK(std::abs(p) == doctest::Approx(0.5f));
}

TEST_CASE("every cube triangle winds counter-clockwise when seen from outside") {
    const auto cube = make_cube();
    for (size_t i = 0; i < cube.indices.size(); i += 3) {
        auto pos = [&](size_t k) {
            const auto& v = cube.vertices[cube.indices[i + k]];
            return glm::vec3(v.position[0], v.position[1], v.position[2]);
        };
        const auto& n0 = cube.vertices[cube.indices[i]].normal;
        const glm::vec3 normal(n0[0], n0[1], n0[2]);
        const glm::vec3 face = glm::cross(pos(1) - pos(0), pos(2) - pos(0));
        CHECK(glm::dot(face, normal) > 0.0f);
    }
}
