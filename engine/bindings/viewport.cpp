// SPDX-License-Identifier: Apache-2.0
#include "viewport.h"

#include "midgard/scene_nav.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <volk.h>

#include <algorithm>

#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include "midgard/body.h"
#include "midgard/collider_outlines.h"
#include "midgard/game.h"
#include "midgard/gltf.h"
#include "midgard/light_markers.h"
#include "midgard/world_surface.h"

namespace midgard::editor {
namespace {

// The window-system surface structures, declared here so no window-system headers are needed.
struct Win32SurfaceInfo {
    VkStructureType sType = VkStructureType(1000009000);  // WIN32_SURFACE_CREATE_INFO_KHR
    const void* pNext = nullptr;
    VkFlags flags = 0;
    void* hinstance = nullptr;
    void* hwnd = nullptr;
};
using CreateWin32Surface = VkResult(VKAPI_PTR*)(VkInstance, const Win32SurfaceInfo*,
                                                const VkAllocationCallbacks*, VkSurfaceKHR*);

struct XcbSurfaceInfo {
    VkStructureType sType = VkStructureType(1000005000);  // XCB_SURFACE_CREATE_INFO_KHR
    const void* pNext = nullptr;
    VkFlags flags = 0;
    void* connection = nullptr;
    uint32_t window = 0;
};
using CreateXcbSurface = VkResult(VKAPI_PTR*)(VkInstance, const XcbSurfaceInfo*,
                                              const VkAllocationCallbacks*, VkSurfaceKHR*);

std::string text(const nlohmann::json& j, const char* key) {
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

}  // namespace

// An X11 connection of the viewport's own (window ids are global to the X server), opened
// through libxcb at run time so the module has no build-time X11 dependency.
struct Viewport::Platform {
#ifndef _WIN32
    void* xcb_library = nullptr;
    void* connection = nullptr;
    void (*disconnect)(void*) = nullptr;

    void* xcb_connection() {
        if (connection) return connection;
        xcb_library = dlopen("libxcb.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!xcb_library) throw render::RenderError("libxcb.so.1 is not available");
        using Connect = void* (*)(const char*, int*);
        auto connect = reinterpret_cast<Connect>(dlsym(xcb_library, "xcb_connect"));
        disconnect = reinterpret_cast<void (*)(void*)>(dlsym(xcb_library, "xcb_disconnect"));
        if (!connect || !disconnect) throw render::RenderError("libxcb.so.1 is incomplete");
        connection = connect(nullptr, nullptr);
        return connection;
    }
    ~Platform() {
        if (connection && disconnect) disconnect(connection);
        if (xcb_library) dlclose(xcb_library);
    }
#endif
};

Viewport::Viewport() : platform_(std::make_unique<Platform>()) {
    scene_.id = "editor";
    characters_.set_automatic(false);  // (nothing moves here: the preview sets "speed")
}

Viewport::~Viewport() {
    detach();
}

void Viewport::put_entity(const std::string& entity_json) {
    const nlohmann::json e = nlohmann::json::parse(entity_json);
    const auto components = e.find("components");
    scene_.put_entity(text(e, "id"), text(e, "name"), text(e, "parent"),
                      components != e.end() ? *components : nlohmann::json::object());
    upload_meshes();
    nav_stale_ = true;
}

void Viewport::remove_entity(const std::string& entity_id) {
    scene_.remove_entity(entity_id);
    nav_stale_ = true;
}

void Viewport::clear() {
    scene_.registry.clear();
    nav_stale_ = true;
}

void Viewport::set_settings(const std::string& settings_json) {
    const nlohmann::json s = nlohmann::json::parse(settings_json);
    const auto vec = [&](const char* key, glm::vec3 fallback) {
        const auto it = s.find(key);
        if (it == s.end() || !it->is_array() || it->size() < 3) return fallback;
        return glm::vec3((*it)[0].get<float>(), (*it)[1].get<float>(), (*it)[2].get<float>());
    };
    nav_stale_ = true;
    scene_.settings.sky = vec("sky", scene_.settings.sky);
    scene_.settings.ambient = vec("ambient", scene_.settings.ambient);
    const auto it_mode = s.find("sun_mode");
    const auto mode = it_mode != s.end() && it_mode->is_string()
                          ? sun_mode_from_name(it_mode->get<std::string>())
                          : std::nullopt;
    scene_.settings.sun_mode = mode.value_or(SunMode::Uniform);
    // The world's shape: "up" for lights and the player's outline, and the world's surface.
    const std::string shape_json = s.contains("body") ? s.at("body").dump() : std::string();
    scene_.settings.body.reset();
    if (const auto it = s.find("body"); it != s.end()) {
        try {
            scene_.settings.body = body_from_json(*it);
        } catch (const std::exception&) {  // the World Shape dialog refuses these anyway
        }
    }
    // Terrain: settings that can't be read are left out (the cook names them).
    scene_.settings.terrain.reset();
    if (const auto it = s.find("terrain"); it != s.end()) {
        try {
            // (A world type's name is looked up by the editor, which sends the settings.)
            if (!is_world_ref(*it)) scene_.settings.terrain = terrain_from_json(*it);
        } catch (const std::exception&) {
        }
    }
    const std::string terrain_json =
        shape_json + "|" + (scene_.settings.terrain ? s.at("terrain").dump() : std::string());
    if (terrain_json != terrain_json_) {
        terrain_json_ = terrain_json;
        drop_terrain();
    }
    if (shape_json != body_json_) {  // a new shape: its surface is made again when it's next drawn
        body_json_ = shape_json;
        mesh_data_.erase(kBuiltinSurface);
        if (renderer_) {
            if (const auto it = meshes_.find(kBuiltinSurface); it != meshes_.end())
                renderer_->replace_mesh(it->second, surface_mesh(scene_.settings.body));
        }
        if (meshes_.contains(kBuiltinSurface) || needs_surface())
            mesh_data_[kBuiltinSurface] = surface_mesh(scene_.settings.body);
    }
}

void Viewport::set_mesh(const std::string& asset, std::span<const uint8_t> glb) {
    render::MeshData mesh = load_glb(glb);         // throws AssetError on a bad file
    files_[asset].assign(glb.begin(), glb.end());  // (a character's model or clips, maybe)
    characters_.forget(asset);
    if (renderer_) {
        if (auto it = meshes_.find(asset); it != meshes_.end())
            renderer_->replace_mesh(it->second, mesh);
        else
            meshes_.emplace(asset, renderer_->upload_mesh(mesh));
    }
    mesh_data_[asset] = std::move(mesh);
}

void Viewport::set_texture(const std::string& texture, std::span<const uint8_t> ktx2) {
    texture_data_[texture].assign(ktx2.begin(), ktx2.end());
    materials_.forget(texture);
    upload_meshes();
}

void Viewport::set_environment(const std::string& id, const std::string& name,
                               const std::string& curves_json, const std::string& settings_json) {
    scene_.environments[id] = environment_from_json(id, name, curves_json, settings_json);
}

void Viewport::set_object(const std::string& object_id, const std::string& parts_json) {
    scene_.set_object(object_id, parts_json);
    upload_meshes();
}

void Viewport::set_camera(const glm::vec3& eye, const glm::vec3& target, float fov_degrees) {
    eye_ = eye;
    target_ = target;
    fov_ = fov_degrees;
}

std::optional<CameraRig::Pose> Viewport::preview_camera(CameraView view) {
    // The rig moves the camera it follows; every camera goes back where the designer put it.
    std::vector<std::pair<entt::entity, std::pair<Transform, CameraLens>>> cameras;
    for (entt::entity e : scene_.registry.view<const CameraLens, const Transform>())
        cameras.push_back(
            {e, {scene_.registry.get<Transform>(e), scene_.registry.get<CameraLens>(e)}});
    std::optional<CameraRig::Pose> pose;
    CameraRig rig(scene_);
    if (rig.active()) {
        rig.set_view(scene_, view, false);
        rig.follow(scene_, 0.0f);
        pose = rig.pose();
    }
    for (const auto& [e, saved] : cameras) {
        scene_.registry.replace<Transform>(e, saved.first);
        scene_.registry.replace<CameraLens>(e, saved.second);
    }
    return pose;
}

void Viewport::attach_window(const std::string& platform, uint64_t handle, uint32_t width,
                             uint32_t height) {
    detach();
    render::RendererDesc desc;
    desc.width = std::max(width, 1u);
    desc.height = std::max(height, 1u);
    if (platform == "windows") {
        desc.instance_extensions = {"VK_KHR_surface", "VK_KHR_win32_surface"};
        desc.create_surface = [handle](VkInstance_T* instance) -> VkSurfaceKHR_T* {
            auto create = reinterpret_cast<CreateWin32Surface>(
                vkGetInstanceProcAddr(instance, "vkCreateWin32SurfaceKHR"));
            if (!create) return nullptr;
            Win32SurfaceInfo info;
#ifdef _WIN32
            info.hinstance = GetModuleHandleW(nullptr);
#endif
            info.hwnd = reinterpret_cast<void*>(static_cast<uintptr_t>(handle));
            VkSurfaceKHR surface = VK_NULL_HANDLE;
            return create(instance, &info, nullptr, &surface) == VK_SUCCESS ? surface : nullptr;
        };
    } else if (platform == "xcb") {
#ifdef _WIN32
        throw render::RenderError("X11 windows are not available on Windows");
#else
        void* connection = platform_->xcb_connection();
        desc.instance_extensions = {"VK_KHR_surface", "VK_KHR_xcb_surface"};
        desc.create_surface = [handle, connection](VkInstance_T* instance) -> VkSurfaceKHR_T* {
            auto create = reinterpret_cast<CreateXcbSurface>(
                vkGetInstanceProcAddr(instance, "vkCreateXcbSurfaceKHR"));
            if (!create) return nullptr;
            XcbSurfaceInfo info;
            info.connection = connection;
            info.window = static_cast<uint32_t>(handle);
            VkSurfaceKHR surface = VK_NULL_HANDLE;
            return create(instance, &info, nullptr, &surface) == VK_SUCCESS ? surface : nullptr;
        };
#endif
    } else {
        throw render::RenderError("the viewport can't draw into '" + platform +
                                  "' windows (on Linux, start the editor with X11)");
    }
    renderer_ = std::make_unique<render::Renderer>(desc);
    upload_meshes();
}

void Viewport::attach_offscreen(uint32_t width, uint32_t height) {
    detach();
    render::RendererDesc desc;
    desc.offscreen = true;
    desc.width = std::max(width, 1u);
    desc.height = std::max(height, 1u);
    renderer_ = std::make_unique<render::Renderer>(desc);
    upload_meshes();
}

void Viewport::set_terrain_stamps(const std::string& stamps_json) {
    std::vector<TerrainStamp> stamps;
    for (const nlohmann::json& s : nlohmann::json::parse(stamps_json))
        stamps.push_back(stamp_from_json(s));  // throws on a malformed stamp
    stamps_ = stamps;
    scene_.terrain_stamps = stamps;
    if (nav_world_) nav_world_->wait();  // (its jobs read the terrain being changed)
    if (terrain_) terrain_->set_stamps(std::move(stamps));
    if (nav_world_) nav_world_->ground_changed();
}

std::optional<Viewport::TerrainGround> Viewport::terrain_ground(const glm::dvec3& point) const {
    if (!scene_.settings.body || !scene_.settings.terrain) return std::nullopt;
    const Terrain own(*scene_.settings.body, *scene_.settings.terrain, stamps_);
    const Terrain& t = terrain_ ? terrain_->terrain() : own;
    return TerrainGround{t.above_ground(point), t.ground(point), t.body().up(point)};
}

std::optional<Viewport::TerrainHit> Viewport::terrain_hit(const glm::dvec3& origin,
                                                          const glm::dvec3& direction,
                                                          double distance,
                                                          std::optional<int> face) const {
    if (!scene_.settings.body || !scene_.settings.terrain) return std::nullopt;
    const Terrain own(*scene_.settings.body, *scene_.settings.terrain, stamps_);
    const Terrain& t = terrain_ ? terrain_->terrain() : own;
    const std::optional<glm::dvec3> p = t.raycast(origin, direction, distance);
    if (!p) return std::nullopt;
    SurfacePoint s = t.body().surface(*p);
    const Shape::Kind kind = t.body().shape.kind();
    if (face && *face != s.face && *face >= 0 && *face < 6 &&
        (kind == Shape::Kind::SphereOutside || kind == Shape::Kind::SphereInside)) {
        if (const auto on = cube_face_point(*face, *p - t.body().origin)) {
            s.face = *face;
            s.u = on->x;
            s.v = on->y;
        }
    }
    return TerrainHit{s.face, s.u, s.v, *p, s.height};
}

entt::entity Viewport::character(const std::string& entity_id) const {
    const entt::entity e = scene_.find(entity_id);
    if (e == entt::null || !scene_.registry.all_of<CharacterRef>(e))
        throw AnimationError("'" + entity_id + "' isn't a character");
    return e;
}

void Viewport::set_animation(const std::string& entity_id, const std::string& name, float value) {
    characters_.set_parameter(character(entity_id), name, value);
}

void Viewport::trigger_animation(const std::string& entity_id, const std::string& name) {
    characters_.trigger(character(entity_id), name);
}

float Viewport::animation(const std::string& entity_id, const std::string& name) const {
    return characters_.parameter(character(entity_id), name);
}

std::string Viewport::animation_state(const std::string& entity_id, int layer) const {
    return characters_.state_of(character(entity_id), size_t(std::max(0, layer)));
}

void Viewport::restart_animation(const std::string& entity_id) {
    characters_.restart(character(entity_id));
}

void Viewport::drop_characters() {
    characters_.renderer_gone();
}

void Viewport::drop_terrain() {
    nav_world_.reset();  // (its jobs read the terrain)
    if (placed_) placed_->clear(scene_);
    placed_.reset();
    if (terrain_ && renderer_) terrain_->release(*renderer_);
    terrain_.reset();
}

void Viewport::detach() {
    drop_terrain();
    drop_characters();
    world_light_ = WorldLight{};  // (its meshes go with the renderer)
    renderer_.reset();            // the renderer waits for the GPU before it lets go of the surface
    meshes_.clear();
    materials_.clear();
}

void Viewport::resize(uint32_t width, uint32_t height) {
    if (renderer_) renderer_->resize(width, height);
}

bool Viewport::needs_surface() const {
    for (entt::entity e : scene_.registry.view<const MeshRef>())
        if (scene_.registry.get<MeshRef>(e).asset == kBuiltinSurface) return true;
    return false;
}

void Viewport::upload_meshes() {
    // The world's surface is made here (from the scene's body), not sent by the editor.
    if (needs_surface() && !mesh_data_.contains(kBuiltinSurface))
        mesh_data_[kBuiltinSurface] = surface_mesh(scene_.settings.body);
    if (!renderer_) return;
    world_light_.upload(*renderer_);
    for (entt::entity e : scene_.registry.view<const MeshRef>()) {
        for (const std::string& asset : scene_.registry.get<MeshRef>(e).assets()) {
            if (meshes_.contains(asset)) continue;
            if (asset == kBuiltinCube) {
                meshes_.emplace(asset, renderer_->upload_mesh(render::make_cube()));
            } else if (asset == kBuiltinSurface) {
                if (!mesh_data_.contains(asset))
                    mesh_data_[asset] = surface_mesh(scene_.settings.body);
                meshes_.emplace(asset, renderer_->upload_mesh(mesh_data_[asset]));
            } else if (auto it = mesh_data_.find(asset); it != mesh_data_.end()) {
                meshes_.emplace(asset, renderer_->upload_mesh(it->second));
            }
        }
    }
    for (const auto& [asset, mesh] : mesh_data_)
        if (!meshes_.contains(asset)) meshes_.emplace(asset, renderer_->upload_mesh(mesh));
    // Textures the editor hasn't sent yet are left out until it does.
    materials_.upload(scene_, *renderer_, [this](const std::string& texture) {
        const auto it = texture_data_.find(texture);
        if (it == texture_data_.end()) throw AssetError("not sent to the viewport");
        return it->second;
    });
}

bool Viewport::render() {
    if (!renderer_) return false;
    std::vector<render::DrawItem> draws;
    // Camera-relative, as in the game (see render::Camera).
    render::Camera camera;
    camera.origin = glm::dvec3(eye_);
    camera.view = glm::lookAt(glm::vec3(0.0f), target_ - eye_, glm::vec3(0, 1, 0));
    upload_meshes();  // (cheap when nothing is new)
    for (entt::entity e : scene_.registry.view<const MeshRef>()) {
        const MeshRef& ref = scene_.registry.get<MeshRef>(e);
        const double distance =
            ref.lods.empty() ? 0.0 : glm::distance(scene_.world_position(e), camera.origin);
        const auto it = meshes_.find(ref.at(distance));
        if (it == meshes_.end()) continue;
        render::DrawItem d = scene_.draw_item(e, it->second, camera.origin);
        if (const Material* m = scene_.registry.try_get<Material>(e)) materials_.apply(*m, d);
        draws.push_back(d);
    }
    std::vector<render::DebugLine> lines =
        show_colliders_
            ? collider_outlines(scene_, camera.origin,
                                [this](const std::string& asset) -> const render::MeshData* {
                                    const auto it = mesh_data_.find(asset);
                                    return it == mesh_data_.end() ? nullptr : &it->second;
                                })
            : std::vector<render::DebugLine>{};
    // Terrain: the chunks for this view, all of them now (the viewport draws on request, so
    // it doesn't stream over several frames as the game does).
    float far_plane = 2000.0f;
    if (scene_.settings.body && scene_.settings.terrain) {
        if (!terrain_) {
            if (!jobs_) jobs_ = std::make_unique<JobSystem>();
            terrain_ = std::make_unique<TerrainStreamer>(*scene_.settings.body,
                                                         *scene_.settings.terrain, jobs_.get());
            terrain_->set_stamps(stamps_);
        }
        terrain_->update(camera.origin, renderer_.get(), true);
        const std::optional<LocalSun> sun = scene_.local_sun(camera.origin);
        terrain_->set_season(sun ? sun->season : scene_.sun_clock.season,
                             sun ? sun->autumn : scene_.sun_clock.autumn);
        const std::vector<render::DrawItem> ground = terrain_->draws(camera.origin);
        draws.insert(draws.end(), ground.begin(), ground.end());
        // The world type's objects round the camera (drawn next time, once their meshes are
        // made: they join the scene like any instance).
        if (!terrain_->terrain().settings().placements.empty()) {
            if (!placed_) placed_ = std::make_unique<PlacedObjects>(terrain_->terrain());
            placed_->update(scene_, camera.origin, 8);
        }
        far_plane = kTerrainViewDistance;
    }
    // Characters, at the start of their clips.
    characters_.load(scene_, [this](const std::string& asset) {
        const auto it = files_.find(asset);
        if (it == files_.end()) throw AssetError("the editor hasn't sent it");
        return it->second;
    });
    characters_.upload(*renderer_);
    characters_.update(scene_, pending_dt_);
    pending_dt_ = 0.0f;
    characters_.draws(scene_, camera.origin, materials_, entt::null, draws);
    if (show_navigation_)  // (after the terrain: its navigation is made from it)
        for (const render::DebugLine& l : navigation_lines(camera.origin)) lines.push_back(l);
    if (show_lights_) {  // a bulb facing the camera at each light
        LightMarkerView marker_view;
        marker_view.eye = glm::dvec3(eye_);
        const glm::dvec3 forward = glm::normalize(glm::dvec3(target_ - eye_));
        glm::dvec3 right = glm::cross(forward, glm::dvec3(0, 1, 0));
        right = glm::length(right) > 1e-6 ? glm::normalize(right) : glm::dvec3(1, 0, 0);
        marker_view.right = right;
        marker_view.up = glm::normalize(glm::cross(right, forward));
        marker_view.selected = selected_;
        for (const render::DebugLine& l : light_markers(scene_, camera.origin, marker_view))
            lines.push_back(l);
    }
    renderer_->set_debug_lines(lines);
    // A curved cylinder's light rod or a hollow world's light sphere, and seeing that far.
    if (const auto light = world_light_.draw(scene_, camera.origin)) draws.push_back(*light);
    far_plane = std::max(far_plane, float(inside_view_distance(scene_.settings.body)));
    const float aspect = float(renderer_->width()) / float(std::max(renderer_->height(), 1u));
    camera.projection = render::perspective(glm::radians(fov_), aspect, 0.05f, far_plane);
    renderer_->set_environment(scene_.environment(camera.origin));
    renderer_->set_terrain_look(terrain_ ? terrain_->look() : render::TerrainLook{});
    return renderer_->render_frame(camera, scene_.light(camera.origin), draws,
                                   glm::vec4(scene_.settings.sky, 1.0f));
}

std::vector<render::DebugLine> Viewport::navigation_lines(const glm::dvec3& origin) {
    const glm::vec4 colour(0.2f, 0.95f, 0.75f, 1.0f);
    const auto lookup = [this](const std::string& asset) -> const render::MeshData* {
        const auto it = mesh_data_.find(asset);
        return it == mesh_data_.end() ? nullptr : &it->second;
    };
    std::vector<render::DebugLine> out;
    if (terrain_) {  // round where the view looks, all of it now (the view draws on request)
        if (!nav_world_) {
            nav_world_ = std::make_unique<NavWorld>(terrain_->terrain(), jobs_.get());
            nav_stale_ = true;
        }
        if (nav_stale_) {
            nav_world_->set_scenery(scene_, lookup);
            nav_stale_ = false;
        }
        const glm::dvec3 at = terrain_->terrain().ground(glm::dvec3(target_));
        nav_world_->update(at, true);
        for (const auto& [a, b] : nav_world_->edges()) {
            const glm::dvec3 up_a = terrain_->terrain().body().up(a) * 0.05;
            const glm::dvec3 up_b = terrain_->terrain().body().up(b) * 0.05;
            out.push_back({glm::vec3(a + up_a - origin), glm::vec3(b + up_b - origin), colour});
        }
        return out;
    }
    if (!flat_world(scene_)) return out;
    if (nav_stale_ || !nav_) {
        if (!jobs_) jobs_ = std::make_unique<JobSystem>();
        nav_ = build_navmesh(NavSettings{}, nav_geometry(scene_, glm::dvec3(0.0), lookup),
                             jobs_.get());
        nav_stale_ = false;
    }
    const glm::vec3 lift(0.0f, 0.05f, 0.0f);
    for (const auto& [a, b] : nav_->edges())
        out.push_back({glm::vec3(glm::dvec3(a) - origin) + lift,
                       glm::vec3(glm::dvec3(b) - origin) + lift, colour});
    return out;
}

std::vector<uint8_t> Viewport::read_pixels() {
    if (!renderer_) throw render::RenderError("the viewport is not attached");
    return renderer_->read_pixels();
}

std::string Viewport::device_name() const {
    return renderer_ ? renderer_->device_name() : std::string();
}

size_t Viewport::entity_count() const {
    return scene_.registry.view<const EntityInfo>().size();
}

}  // namespace midgard::editor
