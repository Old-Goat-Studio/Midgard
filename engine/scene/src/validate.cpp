// SPDX-License-Identifier: Apache-2.0
#include "midgard/validate.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <memory>
#include <set>

#include "midgard/game.h"
#include "midgard/gltf.h"
#include "midgard/input.h"
#include "midgard/package.h"
#include "midgard/pak.h"
#include "midgard/scene.h"
#include "midgard/scene_physics.h"
#include "midgard/world_surface.h"

namespace midgard {

ValidationReport validate_package(const std::filesystem::path& dir) {
    ValidationReport report;
    PackageInfo package;
    try {
        package = open_package(dir);
    } catch (const std::exception& e) {
        report.errors.emplace_back(e.what());
        return report;
    }
    const Manifest& m = package.manifest;
    const std::filesystem::path database = dir / m.database;

    std::set<std::string> content_ids;
    for (const ContentEntry& c : m.content) content_ids.insert(c.id);
    std::unique_ptr<Pak> pak;
    if (!m.content.empty()) {
        try {
            pak = std::make_unique<Pak>(dir / "game.pak");
        } catch (const std::exception& e) {
            report.errors.emplace_back("game.pak: " + std::string(e.what()));
        }
    }
    if (pak) {
        for (const ContentEntry& c : m.content) {
            try {
                const std::vector<uint8_t> bytes = pak->read(c.path);
                if (c.path.size() > 5 && c.path.ends_with(".ktx2")) {
                    static const uint8_t magic[12] = {0xAB, 'K',  'T',  'X',  ' ',  '2',
                                                      '0',  0xBB, '\r', '\n', 0x1A, '\n'};
                    if (bytes.size() < 12 || std::memcmp(bytes.data(), magic, 12) != 0)
                        throw AssetError("not a KTX2 texture");
                } else {
                    load_glb(bytes);
                }
                ++report.assets;
            } catch (const std::exception& e) {
                report.errors.emplace_back("content '" + c.id + "' (" + c.path + "): " + e.what());
            }
        }
    }

    try {
        const Scene scene = Scene::load(database, m.entry_scene);
        report.entities = scene.registry.view<const EntityInfo>().size();
        for (entt::entity e : scene.registry.view<const ScriptRef>()) {
            std::string path = scene.registry.get<ScriptRef>(e).module;
            std::replace(path.begin(), path.end(), '.', '/');
            const std::filesystem::path scripts = dir / "scripts";
            if (!std::filesystem::is_regular_file(scripts / (path + ".py")) &&
                !std::filesystem::is_regular_file(scripts / path / "__init__.py"))
                report.warnings.push_back("entity '" + scene.registry.get<EntityInfo>(e).id +
                                          "' uses script '" +
                                          scene.registry.get<ScriptRef>(e).module +
                                          "', which is not in the package; it will not run");
        }
        for (entt::entity e : scene.registry.view<const MeshRef>()) {
            const std::string& asset = scene.registry.get<MeshRef>(e).asset;
            if (asset != kBuiltinCube && asset != kBuiltinSurface && !content_ids.contains(asset))
                report.warnings.push_back("entity '" + scene.registry.get<EntityInfo>(e).id +
                                          "' uses mesh '" + asset +
                                          "', which is not in the package; it will not be drawn");
        }
        for (entt::entity e : scene.registry.view<const ObjectRef>()) {
            const std::string& object = scene.registry.get<ObjectRef>(e).id;
            if (!scene.objects.contains(object))
                report.warnings.push_back("entity '" + scene.registry.get<EntityInfo>(e).id +
                                          "' places object '" + object +
                                          "', which is not in the package; nothing is shown");
        }
        for (entt::entity e : scene.registry.view<const Material>())
            for (const std::string& texture : scene.registry.get<Material>(e).textures)
                if (!texture.empty() && !content_ids.contains(texture))
                    report.warnings.push_back("entity '" + scene.registry.get<EntityInfo>(e).id +
                                              "' uses texture '" + texture +
                                              "', which is not in the package; drawn without it");
        (void)scene.light();
        (void)scene.camera(16.0f / 9.0f);
        // Every collider builds (a missing mesh, a malformed shape or a dynamic mesh is an
        // error naming its entity).
        const Pak* paks = pak.get();
        const PhysicsLayers layers = PhysicsLayers::load(database);
        const ScenePhysics physics(scene, layers, [&](const std::string& asset) {
            if (asset == kBuiltinCube) return render::make_cube();
            if (asset == kBuiltinSurface) return surface_mesh(scene.settings.body);
            for (const ContentEntry& c : m.content)
                if (c.id == asset) {
                    if (!paks) throw AssetError("game.pak is missing");
                    return load_glb(paks->read(c.path));
                }
            throw AssetError("mesh '" + asset + "' is not in the package");
        });
    } catch (const std::exception& e) {
        report.errors.emplace_back(e.what());
    }

    try {
        (void)InputMap::load(database);
    } catch (const std::exception& e) {
        report.errors.emplace_back("input bindings: " + std::string(e.what()));
    }
    return report;
}

}  // namespace midgard
