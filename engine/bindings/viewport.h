// SPDX-License-Identifier: Apache-2.0
// The editor's 3D viewport: the engine's renderer drawing the scene being edited into a native
// window inside an editor dock (roadmap §2, channel 3 and §3 "Viewport docks").
//
// The scene and the mesh data live here for the viewport's whole life. The renderer is attached
// to a window and detached again whenever the editor's window system replaces that window (a
// dock moved to another monitor or floated), so it never draws into a window that has gone.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "midgard/camera_rig.h"
#include "midgard/characters.h"
#include "midgard/materials.h"
#include "midgard/nav_world.h"
#include "midgard/navmesh.h"
#include "midgard/placed_objects.h"
#include "midgard/render/renderer.h"
#include "midgard/scene.h"
#include "midgard/terrain_streamer.h"
#include "midgard/world_light.h"

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
    // A KTX2 texture for a texture content id (materials' "textures").
    void set_texture(const std::string& texture, std::span<const uint8_t> ktx2);
    // An object's template (a JSON list of parts): its instances are filled in again.
    void set_object(const std::string& object_id, const std::string& parts_json);
    // An environment (a row of the environment table) for the Sky to link to.
    void set_environment(const std::string& id, const std::string& name,
                         const std::string& curves_json, const std::string& settings_json);
    void set_camera(const glm::vec3& eye, const glm::vec3& target, float fov_degrees);
    // The terrain's sculpting and painting (a JSON list of stamps, in order).
    void set_terrain_stamps(const std::string& stamps_json);
    // Where a ray first meets the terrain: (face, u, v) on it and the point, or none.
    struct TerrainHit {
        int face;
        double u, v;
        glm::dvec3 point;
        double height;  // along up, as the terrain's heights are
    };
    // The terrain at a point: how high the point is above it, the ground under it, and "up"
    // there; none without terrain.
    struct TerrainGround {
        double above;
        glm::dvec3 ground, up;
    };
    std::optional<TerrainGround> terrain_ground(const glm::dvec3& point) const;
    // Where a ray first meets the terrain (the editor's sculpt and paint brushes), or none. On a
    // sphere, `face` gives u and v in that face's terms, carried on past its edges (a stroke
    // that began on it and crosses an edge), where it can.
    std::optional<TerrainHit> terrain_hit(const glm::dvec3& origin, const glm::dvec3& direction,
                                          double distance,
                                          std::optional<int> face = std::nullopt) const;
    // The time of day for suns that follow the clock (the Calendar dock's scrubber), or none.
    void set_solar_phase(std::optional<double> phase) { scene_.solar_phase = phase; }
    std::optional<double> solar_phase() const { return scene_.solar_phase; }
    // The seconds surface shaders (and water) see as their time (5.17's previews animate it);
    // negative: the scene's own.
    void set_clock(double seconds) { clock_ = seconds; }
    double clock() const { return clock_; }
    // How far into summer (1) or winter (-1), for the environment's season tints; on a tilted
    // planet in Latitude mode it also sets the year, so the sun's declination follows.
    void set_season(double season) {
        season = std::clamp(season, -1.0, 1.0);
        scene_.sun_clock.season = season;
        const double year = std::asin(season) / (2.0 * 3.14159265358979323846);
        scene_.sun_clock.year = year - std::floor(year);
    }
    double season() const { return scene_.sun_clock.season; }
    // Where the game's camera would be in one of its views, from the scene as edited (the
    // Camera editor's preview): the game's own camera rig works it out, so the two agree. Walls
    // don't stop it here (the viewport has no physics). None without a player and a camera.
    std::optional<CameraRig::Pose> preview_camera(CameraView view);
    // Outlines of colliders, triggers and the player over the scene (midgard/collider_outlines.h).
    void set_show_colliders(bool show) { show_colliders_ = show; }
    bool show_colliders() const { return show_colliders_; }
    // A light bulb where each point and spot light is (midgard/light_markers.h): off unless
    // asked for (the editor's small previews light their subjects with lamps of their own).
    void set_show_lights(bool show) { show_lights_ = show; }
    bool show_lights() const { return show_lights_; }
    // The entity selected in the editor (a selected light shows how far it reaches).
    void set_selected(const std::string& entity_id) { selected_ = entity_id; }
    const std::string& selected() const { return selected_; }
    // Where agents can walk (milestone 5.12): a flat scene's navigation from its colliders, or
    // the terrain's round where the view looks, as the game makes them.
    void set_show_navigation(bool show) { show_navigation_ = show; }
    bool show_navigation() const { return show_navigation_; }

    // ---- characters' animation (milestone 5.13: the Animation editor's preview) ----
    // Moves their clips and graphs on by dt at the next render (the viewport shows them still
    // otherwise). "speed" is a parameter like the rest here: nothing moves them.
    void animate(float dt) { pending_dt_ += dt; }
    // An entity's graph (ValueError: none, or no such parameter).
    void set_animation(const std::string& entity_id, const std::string& name, float value);
    void trigger_animation(const std::string& entity_id, const std::string& name);
    float animation(const std::string& entity_id, const std::string& name) const;
    std::string animation_state(const std::string& entity_id, int layer) const;
    // Restarts an entity's character (its clip or graph from the start).
    void restart_animation(const std::string& entity_id);

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
    bool needs_surface() const;  // an entity draws the world's surface

    Scene scene_;
    double clock_ = -1.0;  // (set_clock)
    std::unordered_map<std::string, render::MeshData> mesh_data_;
    std::unordered_map<std::string, render::MeshId> meshes_;  // on the current renderer
    std::unordered_map<std::string, std::vector<uint8_t>> texture_data_;
    MaterialCache materials_;  // on the current renderer
    std::unique_ptr<render::Renderer> renderer_;
    glm::vec3 eye_{4.0f, 3.0f, 6.0f}, target_{0.0f, 0.5f, 0.0f};
    float fov_ = 55.0f;
    bool show_colliders_ = false;
    bool show_navigation_ = false;
    bool show_lights_ = false;
    std::string selected_;
    std::unique_ptr<NavMesh> nav_;  // a flat scene's, made when shown after it went stale
    bool nav_stale_ = true;
    std::vector<render::DebugLine> navigation_lines(const glm::dvec3& origin);
    std::string body_json_;  // the world shape the surface mesh was made for
    // Terrain (midgard/terrain_streamer.h), on the current renderer: made when first drawn,
    // and again when the world shape or terrain settings change.
    std::string terrain_json_;
    std::vector<TerrainStamp> stamps_;
    std::unique_ptr<JobSystem> jobs_;
    std::unique_ptr<TerrainStreamer> terrain_;
    WorldLight world_light_;  // a curved cylinder's light rod or a hollow world's light sphere
    std::unique_ptr<PlacedObjects> placed_;  // the world type's objects round the camera
    std::unique_ptr<NavWorld> nav_world_;    // the terrain's navigation (made when shown)
    // Characters (milestone 5.13), shown at the start of their clips: read from the files the
    // editor sent (set_mesh keeps their bytes).
    Characters characters_;
    float pending_dt_ = 0.0f;
    entt::entity character(const std::string& entity_id) const;
    std::unordered_map<std::string, std::vector<uint8_t>> files_;
    void drop_terrain();
    void drop_characters();
    struct Platform;
    std::unique_ptr<Platform> platform_;  // per-window-system state (the X11 connection)
};

}  // namespace midgard::editor
