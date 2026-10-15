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

#include "midgard/render/mesh_data.h"

struct VkInstance_T;
struct VkSurfaceKHR_T;

namespace midgard::render {

class RenderError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

using MeshId = uint32_t;

struct Camera {
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};  // Vulkan clip space: depth 0..1, y down
};

struct Light {
    glm::vec3 direction{-0.4f, -1.0f, -0.6f};  // the direction the light travels
    glm::vec3 color{1.0f, 0.97f, 0.9f};
    glm::vec3 ambient{0.18f, 0.2f, 0.26f};
};

struct DrawItem {
    MeshId mesh = 0;
    glm::mat4 model{1.0f};
    glm::vec4 color{1.0f};
};

struct RendererDesc {
    // Window target: instance extensions the window system needs, and a surface factory.
    std::vector<const char*> instance_extensions;
    std::function<VkSurfaceKHR_T*(VkInstance_T*)> create_surface;
    // Size in pixels (the window's drawable size, or the offscreen image size).
    uint32_t width = 1280;
    uint32_t height = 720;
    bool offscreen = false;
    bool validation = false;  // use the Khronos validation layer if it is installed
};

class Renderer {
public:
    explicit Renderer(const RendererDesc& desc);  // throws RenderError
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    MeshId upload_mesh(const MeshData& mesh);

    // Draws one frame. Returns false if the window surface changed and the frame was skipped.
    bool render_frame(const Camera& camera, const Light& light, std::span<const DrawItem> draws,
                      const glm::vec4& clear_color);

    // The window's drawable size changed.
    void resize(uint32_t width, uint32_t height);

    // The window has a new native surface (Android, on returning from the background).
    void recreate_surface();

    // Offscreen only: the last frame as tightly packed RGBA8, top row first.
    std::vector<uint8_t> read_pixels();

    uint32_t width() const;
    uint32_t height() const;
    std::string device_name() const;

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
