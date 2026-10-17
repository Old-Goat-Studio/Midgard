// SPDX-License-Identifier: Apache-2.0
// Importing glTF 2.0 models (.gltf with its .bin and image files, or a single .glb), read with
// cgltf: every mesh in the scene with its node's transform applied, grouped into one part per
// material, the materials' metal/roughness factors and textures (decoded, with mipmaps), and
// warnings for what was left out. The editor imports objects with it and the cook step turns
// the result into package data.
#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "midgard/render/mesh_data.h"

namespace midgard {

struct ImportSettings {
    float scale = 1.0f;  // e.g. 0.01 for a model made in centimetres
    bool z_up = false;   // made with +Z up (Blender's default export is already Y up)
};

// glTF's metal/roughness material. Texture fields index Model::textures (-1: none).
struct ModelMaterial {
    std::string name;
    glm::vec4 base_color{1.0f};
    float metallic = 1.0f;
    float roughness = 1.0f;
    glm::vec3 emissive{0.0f};
    float emissive_strength = 1.0f;
    float normal_scale = 1.0f;
    float occlusion_strength = 1.0f;
    bool transparent = false;  // alpha mode BLEND (drawn opaque until transparency arrives)
    bool double_sided = false;
    int base_color_texture = -1, metallic_roughness_texture = -1, normal_texture = -1,
        occlusion_texture = -1, emissive_texture = -1;
};

struct ModelTexture {
    std::string name;          // the image's name or file
    render::TextureData data;  // RGBA8 with mipmaps; sRGB for colour, linear for the rest
};

// The triangles of one material, in the model's space (node transforms, scale and up axis
// applied).
struct ModelPart {
    std::string name;
    render::MeshData mesh;
    int material = -1;  // into Model::materials (-1: glTF's default material)
};

struct Model {
    std::vector<ModelPart> parts;
    std::vector<ModelMaterial> materials;
    std::vector<ModelTexture> textures;
    glm::vec3 bounds_min{0.0f}, bounds_max{0.0f};
    std::vector<std::string> warnings;  // what was left out, and why
};

// A model from a file. External buffers and images are read from beside it (never from outside
// its folder). Throws AssetError for a file that isn't valid glTF or has no triangles.
Model load_model(const std::filesystem::path& file, const ImportSettings& settings = {});
// A .glb (or a .gltf with everything embedded) from memory.
Model load_model(std::span<const uint8_t> bytes, const ImportSettings& settings = {});

}  // namespace midgard
