// SPDX-License-Identifier: Apache-2.0
// The editor's 3D viewport: the engine's renderer drawing the scene being edited into a native
// window inside an editor dock (roadmap §2, channel 3 and §3 "Viewport docks").
//
// The scene and the mesh data live here for the viewport's whole life. The renderer is attached
// to a window and detached again whenever the editor's window system replaces that window (a
// dock moved to another monitor or floated), so it never draws into a window that has gone.
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "midgard/render/renderer.h"
#include "midgard/scene.h"

namespace midgard::editor {

class Viewport {
public:
    Viewport();
    ~Viewport();
    Viewport(const Viewport&) = delete;
    Viewport& operator=(const Viewport&) = delete;

    // ---- the scene (kept while detached) ----
    void put_entity(const std::string& entity_json);  // {"id", "name", "parent", "components"}
    void remove_entity(const std::string& entity_id);
    void clear();
    // {"sky": [...], "ambient": [...], "body": {...}}; a malformed body is left out.
    void set_settings(const std::string& settings_json);
    void set_mesh(const std::string& asset, std::span<const uint8_t> glb);
    void set_camera(const glm::vec3& eye, const glm::vec3& target, float fov_degrees);
    // Outlines of colliders, triggers and the player over the scene (midgard/collider_outlines.h).
    void set_show_colliders(bool show) { show_colliders_ = show; }
    bool show_colliders() const { return show_colliders_; }

    // ---- the target ----
    // platform: "windows" (handle = HWND) or "xcb" (handle = X11 window id).
    void attach_window(const std::string& platform, uint64_t handle, uint32_t width,
                       uint32_t height);
    void attach_offscreen(uint32_t width, uint32_t height);
    void detach();  // waits for the GPU; safe to call when not attached
    bool attached() const { return renderer_ != nullptr; }
    void resize(uint32_t width, uint32_t height);
    // Draws one frame. Returns false if there is nothing to draw into (or the frame was skipped
    // because the window changed size).
    bool render();
    std::vector<uint8_t> read_pixels();  // offscreen only: RGBA8, top row first
    std::string device_name() const;
    size_t entity_count() const;

private:
    void upload_meshes();

    Scene scene_;
    std::unordered_map<std::string, render::MeshData> mesh_data_;
    std::unordered_map<std::string, render::MeshId> meshes_;  // on the current renderer
    std::unique_ptr<render::Renderer> renderer_;
    glm::vec3 eye_{4.0f, 3.0f, 6.0f}, target_{0.0f, 0.5f, 0.0f};
    float fov_ = 55.0f;
    bool show_colliders_ = false;
    struct Platform;
    std::unique_ptr<Platform> platform_;  // per-window-system state (the X11 connection)
};

}  // namespace midgard::editor
