// SPDX-License-Identifier: Apache-2.0
// The Vulkan renderer. It knows nothing about windows: the caller supplies the instance
// extensions and a function that creates the surface (SDL in the runtime), or asks for an
// offscreen target (screenshots, tests, CI with a software Vulkan driver).
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "midgard/render/atmosphere.h"
#include "midgard/render/ktx.h"
#include "midgard/render/mesh_data.h"
#include "midgard/render/overlay.h"
#include "midgard/render/quality.h"
#include "midgard/render/shadows.h"

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
// A surface shader (milestone 5.17): someone's own code for a mesh's surface (shaders/
// surface.glsl), lit as Midgard's materials are. 0 is Midgard's own (mesh.frag); kErrorShader is
// bright magenta, drawn for one that couldn't be used.
using ShaderId = uint32_t;
inline constexpr ShaderId kErrorShader = 1;
// What a surface shader's settings (5.17) can hold: numbers in vec4 slots, and textures.
inline constexpr uint32_t kShaderSettings = 16;
inline constexpr uint32_t kShaderMaps = 4;
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

// The lighting for a frame: the sun (which casts shadows), ambient light from above and below
// along "up", and the local lights nearest the camera (the first kMaxLocalLights are used).
struct Light {
    glm::vec3 direction{-0.4f, -1.0f, -0.6f};  // the direction sunlight travels
    glm::vec3 color{1.0f, 0.97f, 0.9f};        // sRGB
    float intensity = 1.0f;
    glm::vec3 ambient{0.18f, 0.2f, 0.26f};          // sRGB, from the sky
    glm::vec3 ambient_ground{0.11f, 0.12f, 0.16f};  // sRGB, from below
    glm::vec3 up{0.0f, 1.0f, 0.0f};  // "up" at the camera (the world's shape); shadows use it
    std::vector<LocalLight> local;
};

// The terrain's look (midgard/terrain.h's Climate and Biome, as the renderer takes them).
inline constexpr int kMaxTerrainBiomes = 8;
struct TerrainBiome {
    float temperature_min = -100.0f, temperature_max = 100.0f;  // degrees Celsius
    float moisture_min = 0.0f, moisture_max = 1.0f;
    glm::vec3 color{0.27f, 0.42f, 0.16f};      // sRGB, lush
    glm::vec3 dry_color{0.55f, 0.50f, 0.28f};  // sRGB, dry
};
struct TerrainLook {
    // The world's shape, for "up" at each point: 0 flat (+Y), 1 a curved cylinder (towards its
    // axis, along Z through `center`), 2 a sphere's outside (away from `center`), 3 its inside.
    int shape = 0;
    glm::dvec3 center{0.0};
    bool climate = false;              // snow where the temperature is below freezing
    float season_offset = 0.0f;        // degrees Celsius added by the season (summer warmer)
    std::vector<TerrainBiome> biomes;  // none: the 5.9 grass
};

// A line drawn into the scene for debugging (collider outlines and the like): its ends relative
// to the camera's origin, like everything drawn, and its colour (sRGB, unlit; alpha blends it).
// Things in front of it hide it, unless it is on_top: then it shows through them (drawn after
// the others, so a faint on-top copy of a line shows where something hides it).
struct DebugLine {
    glm::vec3 a{0.0f}, b{0.0f};
    glm::vec4 color{1.0f};
    bool on_top = false;
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
    ShaderId shader = 0;  // its surface shader (0: Midgard's own material)
    // The surface shader's settings (5.17), in the slots its compiler gave them (at most
    // kShaderSettings; its textures are the material's shader_maps). The caller keeps them
    // until render_frame returns.
    std::span<const glm::vec4> settings;
    bool casts_shadow = true;  // false: lit and shadowed, but leaves no shadow of its own
    // A water surface: drawn after everything solid, blended, in the environment's Water look
    // (its colour and material are ignored). Its vertices' tangent w is the depth below them.
    // Waves follow its uv (metres across a face, over 16); with `emissive.x` 1 they follow
    // instead its vertices' tangent xyz, a position fixed to the world (a sphere's faces' uvs
    // don't meet at their edges, so the waves there come from 3D). Model: a translation only.
    bool water = false;
    // Terrain ground (shaders/terrain.frag): coloured by its own material from each vertex's
    // data (uv: moisture and detail; tangent: "up" there, and the height). Here `color` is
    // (snow cover, autumn, dry grass, frame) and `emissive` (snow line, sea level, 0), as they
    // are. frame 1: up is encoded as it is; 2 to 7: in the frame of axis (frame - 2) / 2,
    // negative if odd (midgard/terrain_streamer.h's up_frame, for spheres).
    bool terrain = false;
    // A skinned mesh's joint matrices (each joint's pose times its inverse bind matrix, in the
    // mesh's own space; milestone 5.13), indexed by the mesh's vertex joints. Empty: not
    // skinned. The caller keeps them until render_frame returns.
    std::span<const glm::mat4> skin;
};

// The textures of a material (kNoTexture: none, a neutral default).
struct MaterialTextures {
    TextureId base_color = kNoTexture;          // sRGB
    TextureId metallic_roughness = kNoTexture;  // linear: G roughness, B metallic
    TextureId normal = kNoTexture;              // linear, tangent space
    TextureId occlusion = kNoTexture;           // linear: R
    TextureId emissive = kNoTexture;            // sRGB
    // A surface shader's textures (5.17's settings; kNoTexture: white).
    std::array<TextureId, kShaderMaps> shader_maps{kNoTexture, kNoTexture, kNoTexture, kNoTexture};
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
    // What the renderer spends on a frame: the tier's shadows, texture size and so on (the
    // phone tier on phones). Its parts can be changed one by one (shadows.cascades = 0: none).
    QualitySettings quality = quality_settings(default_quality());
};

class Renderer {
public:
    explicit Renderer(const RendererDesc& desc);  // throws RenderError
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    MeshId upload_mesh(const MeshData& mesh);
    // A skinned mesh: four joints (indices into a draw's `skin`) and their weights a vertex.
    MeshId upload_skinned_mesh(const MeshData& mesh, std::span<const glm::u16vec4> joints,
                               std::span<const glm::vec4> weights);
    // A texture with its mipmaps (render::make_texture builds them), on the GPU.
    TextureId upload_texture(const TextureData& texture);
    // A KTX2 texture (render::encode_ktx2), transcoded to texture_format(). Throws RenderError.
    TextureId upload_ktx2(std::span<const uint8_t> ktx2);
    // The compressed format this GPU reads (BC7, ASTC 4x4 or ETC2, else RGBA8).
    GpuFormat texture_format() const;
    MaterialId create_material(const MaterialTextures& textures);
    // A surface shader from its SPIR-V (a fragment shader made from shaders/surface.glsl, as
    // midgard/shader_compiler.h's compile_surface makes them), for draws to use. kErrorShader
    // (never throws) if it can't be used: not SPIR-V, or the GPU refuses it.
    ShaderId create_surface_shader(std::span<const uint32_t> spirv);
    // A surface shader made again from new SPIR-V (edited: 5.17's live changes), keeping its
    // id. False, and the old one kept, if the GPU can't use it (or the id isn't a shader).
    bool replace_surface_shader(ShaderId shader, std::span<const uint32_t> spirv);
    // How many surface shaders there are (the error shader included).
    size_t surface_shader_count() const;
    // Replaces an uploaded mesh (hot reload). Waits for the GPU to finish with the old one.
    void replace_mesh(MeshId id, const MeshData& mesh);
    // Frees a mesh (streamed terrain chunks): it isn't drawn from now on, its id may be given
    // to a later upload_mesh, and its memory goes once no frame in flight can still be using
    // it (no waiting).
    void free_mesh(MeshId id);
    size_t mesh_count() const;  // meshes on the GPU, not counting those freed

    // Draws one frame. Returns false if the window surface changed and the frame was skipped.
    // clear_color is the sky when the environment has none.
    bool render_frame(const Camera& camera, const Light& light, std::span<const DrawItem> draws,
                      const glm::vec4& clear_color);

    // The sky and fog for every frame from now on (render/atmosphere.h). The default is a flat
    // sky in each frame's clear colour and no fog.
    void set_environment(const Environment& environment);

    // The terrain's look, for every frame from now on (shaders/terrain.frag): the world's shape
    // (the terrain works out "up" at each point from it) and its climate and biomes.
    void set_terrain_look(const TerrainLook& look);

    // Brightness before tone mapping (1 = as lit); the environment will drive it later.
    void set_exposure(float exposure);

    // The debug text overlay drawn over every frame from now on (empty: none).
    void set_overlay(std::vector<OverlayLine> lines);

    // Debug lines drawn into every frame from now on (empty: none). Their ends are relative to
    // the camera's origin, so a moving camera needs them again each frame.
    void set_debug_lines(std::vector<DebugLine> lines);

    // The window's drawable size changed.
    void resize(uint32_t width, uint32_t height);

    // The window has a new native surface (Android, on returning from the background).
    void recreate_surface();

    // Offscreen only: the last frame as tightly packed RGBA8, top row first.
    std::vector<uint8_t> read_pixels();

    uint32_t width() const;
    uint32_t height() const;
    std::string device_name() const;
    // A software renderer (like lavapipe): right, but far slower than any GPU.
    bool software() const;
    const QualitySettings& quality() const;
    // With RendererDesc::validation: every message the Khronos validation layer reported (errors
    // and warnings). Tests fail on any.
    const std::vector<std::string>& validation_messages() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// A right-handed perspective projection for Vulkan (depth 0..1, y flipped).
// (Reversed depth: the near plane maps to 1 and the far plane to 0.)
glm::mat4 perspective(float fov_y_radians, float aspect, float near_plane, float far_plane);

// Writes RGBA8 pixels as a PNG. Returns false on failure.
bool write_png(const std::string& path, uint32_t width, uint32_t height,
               const std::vector<uint8_t>& rgba);

}  // namespace midgard::render
