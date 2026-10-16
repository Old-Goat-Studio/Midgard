// SPDX-License-Identifier: Apache-2.0
// The Vulkan renderer. It knows nothing about windows: the caller supplies the instance
// extensions and a function that creates the surface (SDL in the runtime), or asks for an
// offscreen target (screenshots, tests, CI with a software Vulkan driver).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "midgard/render/ktx.h"
#include "midgard/render/mesh_data.h"
#include "midgard/render/overlay.h"

struct VkInstance_T;
struct VkSurfaceKHR_T;

namespace midgard::render {

class RenderError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

using MeshId = uint32_t;
using TextureId = uint32_t;
// A material is a set of textures (glTF's five); its factors come with each DrawItem. 0 is the
// material with no textures: white base colour, flat normals, no occlusion.
using MaterialId = uint32_t;
inline constexpr TextureId kNoTexture = ~0u;
inline constexpr int kMaxLocalLights = 8;

// Camera-relative rendering: worlds are tens of kilometres across, more than 32-bit floats can
// place precisely. So positions are doubles everywhere except here: the view and every draw's
// model matrix are relative to `origin` (the camera's world position), which keeps the floats
// small and exact near the camera. The renderer itself never sees world positions.
struct Camera {
    glm::mat4 view{1.0f};        // from the camera's origin (rotation only, for a scene camera)
    glm::mat4 projection{1.0f};  // Vulkan clip space: depth 0..1, y down
    glm::dvec3 origin{0.0};      // the world position that view and draws are relative to
};

// A world transform (double precision) as a model matrix relative to a camera's origin.
glm::mat4 relative_to(const glm::dmat4& world, const glm::dvec3& origin);

// A point or spot light. Colours are sRGB as picked; intensity scales them (glTF
// KHR_lights_punctual: inverse-square falloff, faded to zero at the range).
struct LocalLight {
    glm::vec3 position{0.0f};  // relative to the camera's origin, like everything drawn
    glm::vec3 color{1.0f};
    float intensity = 1.0f;
    float range = 10.0f;  // metres
    bool spot = false;
    glm::vec3 direction{0.0f, 0.0f, -1.0f};  // where a spot shines
    float inner_angle = 20.0f;               // degrees from the axis: full brightness inside
    float outer_angle = 30.0f;               // and none outside
};

// The lighting for a frame: the sun, ambient light from above and below along "up", and the
// local lights nearest the camera (the first kMaxLocalLights are used).
struct Light {
    glm::vec3 direction{-0.4f, -1.0f, -0.6f};  // the direction sunlight travels
    glm::vec3 color{1.0f, 0.97f, 0.9f};        // sRGB
    float intensity = 1.0f;
    glm::vec3 ambient{0.18f, 0.2f, 0.26f};          // sRGB, from the sky
    glm::vec3 ambient_ground{0.11f, 0.12f, 0.16f};  // sRGB, from below
    glm::vec3 up{0.0f, 1.0f, 0.0f};                 // "up" at the camera (the world's shape)
    std::vector<LocalLight> local;
};

// What to draw: a mesh, its model matrix relative to the camera's origin, and glTF's material
// factors (multiplied by the material's textures where it has them).
struct DrawItem {
    MeshId mesh = 0;
    glm::mat4 model{1.0f};
    glm::vec4 color{1.0f};  // base colour, sRGB, and alpha
    float metallic = 0.0f;
    float roughness = 0.8f;
    glm::vec3 emissive{0.0f};  // sRGB
    float emissive_strength = 1.0f;
    float normal_scale = 1.0f;
    float occlusion_strength = 1.0f;
    MaterialId material = 0;
};

// The textures of a material (kNoTexture: none, a neutral default).
struct MaterialTextures {
    TextureId base_color = kNoTexture;          // sRGB
    TextureId metallic_roughness = kNoTexture;  // linear: G roughness, B metallic
    TextureId normal = kNoTexture;              // linear, tangent space
    TextureId occlusion = kNoTexture;           // linear: R
    TextureId emissive = kNoTexture;            // sRGB
};

struct RendererDesc {
    // Window target: instance extensions the window system needs, and a surface factory.
    std::vector<const char*> instance_extensions;
    std::function<VkSurfaceKHR_T*(VkInstance_T*)> create_surface;
    // Size in pixels (the window's drawable size, or the offscreen image size).
    uint32_t width = 1280;
    uint32_t height = 720;
    bool offscreen = false;
    // Use the Khronos validation layer (and fail to start if it isn't installed); its messages
    // are kept in validation_messages().
    bool validation = false;
};

class Renderer {
public:
    explicit Renderer(const RendererDesc& desc);  // throws RenderError
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    MeshId upload_mesh(const MeshData& mesh);
    // A texture with its mipmaps (render::make_texture builds them), on the GPU.
    TextureId upload_texture(const TextureData& texture);
    // A KTX2 texture (render::encode_ktx2), transcoded to texture_format(). Throws RenderError.
    TextureId upload_ktx2(std::span<const uint8_t> ktx2);
    // The compressed format this GPU reads (BC7, ASTC 4x4 or ETC2, else RGBA8).
    GpuFormat texture_format() const;
    MaterialId create_material(const MaterialTextures& textures);
    // Replaces an uploaded mesh (hot reload). Waits for the GPU to finish with the old one.
    void replace_mesh(MeshId id, const MeshData& mesh);

    // Draws one frame. Returns false if the window surface changed and the frame was skipped.
    bool render_frame(const Camera& camera, const Light& light, std::span<const DrawItem> draws,
                      const glm::vec4& clear_color);

    // Brightness before tone mapping (1 = as lit); the environment will drive it later.
    void set_exposure(float exposure);

    // The debug text overlay drawn over every frame from now on (empty: none).
    void set_overlay(std::vector<OverlayLine> lines);

    // The window's drawable size changed.
    void resize(uint32_t width, uint32_t height);

    // The window has a new native surface (Android, on returning from the background).
    void recreate_surface();

    // Offscreen only: the last frame as tightly packed RGBA8, top row first.
    std::vector<uint8_t> read_pixels();

    uint32_t width() const;
    uint32_t height() const;
    std::string device_name() const;
    // With RendererDesc::validation: every message the Khronos validation layer reported (errors
    // and warnings). Tests fail on any.
    const std::vector<std::string>& validation_messages() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// A right-handed perspective projection for Vulkan (depth 0..1, y flipped).
glm::mat4 perspective(float fov_y_radians, float aspect, float near_plane, float far_plane);

// Writes RGBA8 pixels as a PNG. Returns false on failure.
bool write_png(const std::string& path, uint32_t width, uint32_t height,
               const std::vector<uint8_t>& rgba);

}  // namespace midgard::render
