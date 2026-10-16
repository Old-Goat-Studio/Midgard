// SPDX-License-Identifier: Apache-2.0
// CPU-side mesh and texture data, independent of Vulkan.
#pragma once

#include <cstdint>
#include <vector>

namespace midgard::render {

// 48 bytes. The tangent's w is the bitangent's sign: bitangent = w * cross(normal, tangent), as
// in glTF and MikkTSpace.
struct Vertex {
    float position[3];
    float normal[3];
    float uv[2];
    float tangent[4];
};

struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;  // triangles, counter-clockwise when seen from outside
};

// A unit cube centred on the origin (side 1), with flat normals and each face's uvs 0..1.
MeshData make_cube();
// A sphere of radius 0.5 centred on the origin: rings from pole to pole, segments around.
MeshData make_sphere(int rings = 24, int segments = 48);

// Fills in tangents with MikkTSpace (the standard normal maps are baked against). The mesh needs
// normals and uvs.
void generate_tangents(MeshData& mesh);

// An RGBA8 texture and its mipmaps (level 0 first, each half the size of the one before, down
// to 1 x 1). sRGB textures are averaged in linear light, so their mipmaps don't darken.
struct TextureData {
    uint32_t width = 0, height = 0;
    bool srgb = true;  // colour data (base colour, emissive); false for normals and masks
    std::vector<std::vector<uint8_t>> levels;
};

TextureData make_texture(uint32_t width, uint32_t height, bool srgb, std::vector<uint8_t> rgba);

// The exact sRGB transfer functions (IEC 61966-2-1), for 0..1 values.
float srgb_to_linear(float c);
float linear_to_srgb(float c);

}  // namespace midgard::render
