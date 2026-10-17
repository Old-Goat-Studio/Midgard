// SPDX-License-Identifier: Apache-2.0
#include "viewport.h"

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
}

void Viewport::remove_entity(const std::string& entity_id) {
    scene_.remove_entity(entity_id);
}

void Viewport::clear() {
    scene_.registry.clear();
}

void Viewport::set_settings(const std::string& settings_json) {
    const nlohmann::json s = nlohmann::json::parse(settings_json);
    const auto vec = [&](const char* key, glm::vec3 fallback) {
        const auto it = s.find(key);
        if (it == s.end() || !it->is_array() || it->size() < 3) return fallback;
        return glm::vec3((*it)[0].get<float>(), (*it)[1].get<float>(), (*it)[2].get<float>());
    };
    scene_.settings.sky = vec("sky", scene_.settings.sky);
    scene_.settings.ambient = vec("ambient", scene_.settings.ambient);
    // The world's shape: "up" for lights and the player's outline, and the world's surface.
    const std::string shape_json = s.contains("body") ? s.at("body").dump() : std::string();
    scene_.settings.body.reset();
    if (const auto it = s.find("body"); it != s.end()) {
        try {
            scene_.settings.body = body_from_json(*it);
        } catch (const std::exception&) {  // the World Shape dialog refuses these anyway
        }
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
    render::MeshData mesh = load_glb(glb);  // throws AssetError on a bad file
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

void Viewport::set_object(const std::string& object_id, const std::string& parts_json) {
    scene_.set_object(object_id, parts_json);
    upload_meshes();
}

void Viewport::set_camera(const glm::vec3& eye, const glm::vec3& target, float fov_degrees) {
    eye_ = eye;
    target_ = target;
    fov_ = fov_degrees;
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

void Viewport::detach() {
    renderer_.reset();  // the renderer waits for the GPU before it lets go of the surface
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
    renderer_->set_debug_lines(
        show_colliders_
            ? collider_outlines(scene_, camera.origin,
                                [this](const std::string& asset) -> const render::MeshData* {
                                    const auto it = mesh_data_.find(asset);
                                    return it == mesh_data_.end() ? nullptr : &it->second;
                                })
            : std::vector<render::DebugLine>{});
    const float aspect = float(renderer_->width()) / float(std::max(renderer_->height(), 1u));
    camera.projection = render::perspective(glm::radians(fov_), aspect, 0.05f, 2000.0f);
    return renderer_->render_frame(camera, scene_.light(camera.origin), draws,
                                   glm::vec4(scene_.settings.sky, 1.0f));
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
