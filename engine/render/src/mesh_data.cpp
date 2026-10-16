// SPDX-License-Identifier: Apache-2.0
#include "midgard/render/mesh_data.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

#include <mikktspace.h>

namespace midgard::render {

MeshData make_cube() {
    // Each face: outward normal, then two axes (u, v) with u × v = normal, so the corners
    // (-u-v, +u-v, +u+v, -u+v) run counter-clockwise when seen from outside.
    struct Face {
        float n[3], u[3], v[3];
    };
    static const Face faces[] = {
        {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}}, {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
        {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}}, {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},  {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}},
    };
    static const float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};

    MeshData mesh;
    for (const Face& f : faces) {
        const auto base = static_cast<uint32_t>(mesh.vertices.size());
        for (const auto& c : corners) {
            Vertex vtx{};
            for (int i = 0; i < 3; ++i) {
                vtx.position[i] = 0.5f * (f.n[i] + c[0] * f.u[i] + c[1] * f.v[i]);
                vtx.normal[i] = f.n[i];
            }
            // Texture u along the face's u axis, v downwards (glTF's uvs start top-left).
            vtx.uv[0] = 0.5f * (c[0] + 1.0f);
            vtx.uv[1] = 0.5f * (1.0f - c[1]);
            mesh.vertices.push_back(vtx);
        }
        for (uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) mesh.indices.push_back(base + i);
    }
    generate_tangents(mesh);
    return mesh;
}

MeshData make_sphere(int rings, int segments) {
    if (rings < 2 || segments < 3) throw std::invalid_argument("make_sphere: too few rings");
    constexpr float kPi = 3.14159265358979f;
    MeshData mesh;
    for (int r = 0; r <= rings; ++r) {
        const float v = float(r) / float(rings);
        const float polar = v * kPi;  // 0 at the top
        for (int s = 0; s <= segments; ++s) {
            const float u = float(s) / float(segments);
            const float azimuth = u * 2.0f * kPi;
            const float n[3] = {std::sin(polar) * std::sin(azimuth), std::cos(polar),
                                std::sin(polar) * std::cos(azimuth)};
            Vertex vtx{};
            for (int i = 0; i < 3; ++i) {
                vtx.normal[i] = n[i];
                vtx.position[i] = 0.5f * n[i];
            }
            vtx.uv[0] = u;
            vtx.uv[1] = v;
            mesh.vertices.push_back(vtx);
        }
    }
    const auto row = static_cast<uint32_t>(segments + 1);
    for (uint32_t r = 0; r < uint32_t(rings); ++r)
        for (uint32_t s = 0; s < uint32_t(segments); ++s) {
            const uint32_t a = r * row + s, b = a + row;
            // Counter-clockwise from outside (u grows eastward, v downward).
            if (r != 0) mesh.indices.insert(mesh.indices.end(), {a, b, a + 1});
            if (r + 1 != uint32_t(rings))
                mesh.indices.insert(mesh.indices.end(), {a + 1, b, b + 1});
        }
    generate_tangents(mesh);
    return mesh;
}

namespace {

// MikkTSpace reads the mesh through these callbacks.
MeshData* mesh_of(const SMikkTSpaceContext* ctx) {
    return static_cast<MeshData*>(ctx->m_pUserData);
}
const Vertex& corner(const SMikkTSpaceContext* ctx, int face, int vert) {
    MeshData* m = mesh_of(ctx);
    return m->vertices[m->indices[size_t(face) * 3 + size_t(vert)]];
}

}  // namespace

void generate_tangents(MeshData& mesh) {
    SMikkTSpaceInterface iface{};
    iface.m_getNumFaces = [](const SMikkTSpaceContext* ctx) {
        return static_cast<int>(mesh_of(ctx)->indices.size() / 3);
    };
    iface.m_getNumVerticesOfFace = [](const SMikkTSpaceContext*, int) { return 3; };
    iface.m_getPosition = [](const SMikkTSpaceContext* ctx, float out[], int face, int vert) {
        std::copy_n(corner(ctx, face, vert).position, 3, out);
    };
    iface.m_getNormal = [](const SMikkTSpaceContext* ctx, float out[], int face, int vert) {
        std::copy_n(corner(ctx, face, vert).normal, 3, out);
    };
    iface.m_getTexCoord = [](const SMikkTSpaceContext* ctx, float out[], int face, int vert) {
        std::copy_n(corner(ctx, face, vert).uv, 2, out);
    };
    // Shared vertices get one tangent each: the last face to write it wins, which is what
    // smooth meshes want (their faces agree); hard edges have their own vertices.
    iface.m_setTSpaceBasic = [](const SMikkTSpaceContext* ctx, const float t[], float sign,
                                int face, int vert) {
        MeshData* m = mesh_of(ctx);
        Vertex& v = m->vertices[m->indices[size_t(face) * 3 + size_t(vert)]];
        std::copy_n(t, 3, v.tangent);
        v.tangent[3] = sign;
    };
    SMikkTSpaceContext ctx{&iface, &mesh};
    genTangSpaceDefault(&ctx);
}

float srgb_to_linear(float c) {
    c = std::clamp(c, 0.0f, 1.0f);
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

float linear_to_srgb(float c) {
    c = std::clamp(c, 0.0f, 1.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

TextureData make_texture(uint32_t width, uint32_t height, bool srgb, std::vector<uint8_t> rgba) {
    if (width == 0 || height == 0 || rgba.size() != size_t(width) * height * 4)
        throw std::invalid_argument("make_texture: the pixels don't match the size");
    TextureData t;
    t.width = width;
    t.height = height;
    t.srgb = srgb;
    std::array<float, 256> to_linear{};
    for (int i = 0; i < 256; ++i) to_linear[i] = srgb ? srgb_to_linear(i / 255.0f) : i / 255.0f;
    t.levels.push_back(std::move(rgba));
    uint32_t w = width, h = height;
    while (w > 1 || h > 1) {
        const uint32_t nw = std::max(1u, w / 2), nh = std::max(1u, h / 2);
        const std::vector<uint8_t>& src = t.levels.back();
        std::vector<uint8_t> dst(size_t(nw) * nh * 4);
        for (uint32_t y = 0; y < nh; ++y)
            for (uint32_t x = 0; x < nw; ++x)
                for (int c = 0; c < 4; ++c) {
                    // A 2 x 2 box (clamped at an odd edge); colour in linear light, alpha as is.
                    float sum = 0.0f;
                    for (uint32_t dy = 0; dy < 2; ++dy)
                        for (uint32_t dx = 0; dx < 2; ++dx) {
                            const uint32_t sx = std::min(w - 1, x * 2 + dx);
                            const uint32_t sy = std::min(h - 1, y * 2 + dy);
                            const uint8_t v = src[(size_t(sy) * w + sx) * 4 + size_t(c)];
                            sum += c == 3 ? v / 255.0f : to_linear[v];
                        }
                    const float avg = sum / 4.0f;
                    const float out = (c == 3 || !srgb) ? avg : linear_to_srgb(avg);
                    dst[(size_t(y) * nw + x) * 4 + size_t(c)] =
                        static_cast<uint8_t>(std::lround(out * 255.0f));
                }
        t.levels.push_back(std::move(dst));
        w = nw;
        h = nh;
    }
    return t;
}

}  // namespace midgard::render
