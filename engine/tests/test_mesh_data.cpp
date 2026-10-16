// SPDX-License-Identifier: Apache-2.0
#include <doctest/doctest.h>

#include <cmath>

#include <glm/glm.hpp>

#include "midgard/render/mesh_data.h"

using midgard::render::make_cube;
using midgard::render::make_sphere;
using midgard::render::make_texture;
using midgard::render::MeshData;

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

namespace {

glm::vec3 v3(const float* f) {
    return {f[0], f[1], f[2]};
}

// Tangents are unit length, perpendicular to the normal, and point along increasing u.
void check_tangents(const MeshData& mesh) {
    for (size_t i = 0; i < mesh.indices.size(); i += 3) {
        const auto& a = mesh.vertices[mesh.indices[i]];
        const auto& b = mesh.vertices[mesh.indices[i + 1]];
        const auto& c = mesh.vertices[mesh.indices[i + 2]];
        for (const auto* v : {&a, &b, &c}) {
            REQUIRE(glm::length(v3(v->tangent)) == doctest::Approx(1.0f).epsilon(1e-3));
            REQUIRE(std::abs(glm::dot(v3(v->tangent), v3(v->normal))) < 1e-3f);
            REQUIRE(std::abs(v->tangent[3]) == 1.0f);
        }
        // Along the triangle, position changes along the tangent the way u does.
        const glm::vec3 dp = v3(b.position) - v3(a.position);
        const float du = b.uv[0] - a.uv[0];
        if (std::abs(du) > 1e-4f && glm::length(dp) > 1e-4f)
            REQUIRE(glm::dot(dp, v3(a.tangent)) * du >= -1e-4f);
    }
}

}  // namespace

TEST_CASE("the cube has uvs 0..1 on every face and MikkTSpace tangents") {
    const auto cube = make_cube();
    for (const auto& v : cube.vertices)
        for (float t : v.uv) CHECK((t == 0.0f || t == 1.0f));
    check_tangents(cube);
}

TEST_CASE("the sphere is round, winds outward and has tangents") {
    const MeshData sphere = make_sphere(16, 32);
    CHECK(sphere.vertices.size() == 17 * 33);
    CHECK(sphere.indices.size() == size_t(32 * (2 * 16 - 2) * 3));  // no slivers at the poles
    for (const auto& v : sphere.vertices) {
        REQUIRE(glm::length(v3(v.position)) == doctest::Approx(0.5f));
        REQUIRE(glm::length(v3(v.normal)) == doctest::Approx(1.0f));
    }
    for (size_t i = 0; i < sphere.indices.size(); i += 3) {
        const glm::vec3 a = v3(sphere.vertices[sphere.indices[i]].position);
        const glm::vec3 b = v3(sphere.vertices[sphere.indices[i + 1]].position);
        const glm::vec3 c = v3(sphere.vertices[sphere.indices[i + 2]].position);
        REQUIRE(glm::dot(glm::cross(b - a, c - a), a + b + c) > 0.0f);  // outward
    }
    check_tangents(sphere);
    CHECK_THROWS(make_sphere(1, 2));
}

TEST_CASE("sRGB conversions follow the standard and round-trip") {
    using midgard::render::linear_to_srgb;
    using midgard::render::srgb_to_linear;
    CHECK(srgb_to_linear(0.0f) == 0.0f);
    CHECK(srgb_to_linear(1.0f) == doctest::Approx(1.0f));
    CHECK(srgb_to_linear(0.5f) == doctest::Approx(0.21404f).epsilon(1e-4));
    CHECK(srgb_to_linear(0.04f) == doctest::Approx(0.04f / 12.92f));  // the linear toe
    for (int i = 0; i <= 255; ++i)
        REQUIRE(std::lround(linear_to_srgb(srgb_to_linear(i / 255.0f)) * 255.0f) == i);
}

TEST_CASE("textures get mipmaps down to 1 x 1, averaged in linear light") {
    // A 4 x 2 sRGB texture: black and white columns.
    std::vector<uint8_t> px;
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 4; ++x) {
            const uint8_t v = (x % 2) ? 255 : 0;
            px.insert(px.end(), {v, v, v, 255});
        }
    const auto t = make_texture(4, 2, true, px);
    REQUIRE(t.levels.size() == 3);  // 4x2, 2x1, 1x1
    CHECK(t.levels[1].size() == 2 * 1 * 4);
    CHECK(t.levels[2].size() == 4);
    // Half black, half white is 0.5 in linear light: sRGB 188, not 128 (which would darken).
    CHECK(t.levels[1][0] == 188);
    CHECK(t.levels[2][0] == 188);
    CHECK(t.levels[2][3] == 255);  // alpha averages as it is
    // The same data as a linear texture (a mask) averages to 128.
    CHECK(make_texture(4, 2, false, px).levels[2][0] == 128);
    // Odd sizes clamp at the edge.
    CHECK(make_texture(3, 1, false, std::vector<uint8_t>(12, 9)).levels.size() == 2);
    CHECK_THROWS(make_texture(2, 2, true, std::vector<uint8_t>(3)));
}
