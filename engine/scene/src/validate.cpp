// SPDX-License-Identifier: Apache-2.0
#include "midgard/validate.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <set>

#include "midgard/calendar.h"
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

    // The calendars (the cook checks of roadmap §6).
    try {
        for (std::string& problem : calendar_problems(load_calendars(database)))
            report.errors.push_back(std::move(problem));
    } catch (const std::exception& e) {
        report.errors.emplace_back(e.what());
    }

    // The world types (milestone 5.11): every version's recipe, as the game would read it.
    try {
        for (std::string& problem : world_type_problems(load_world_type_versions(database)))
            report.errors.push_back(std::move(problem));
    } catch (const std::exception& e) {
        report.errors.emplace_back(e.what());
    }

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
            for (const std::string& asset : scene.registry.get<MeshRef>(e).assets())
                if (asset != kBuiltinCube && asset != kBuiltinSurface &&
                    !content_ids.contains(asset))
                    report.warnings.push_back(
                        "entity '" + scene.registry.get<EntityInfo>(e).id + "' uses mesh '" +
                        asset + "', which is not in the package; it will not be drawn");
        }
        if (scene.settings.terrain) {
            for (std::string& problem : terrain_problems(*scene.settings.terrain))
                report.errors.push_back(std::move(problem));
            // A ground slower to make than a chunk's budget allows.
            if (!scene.settings.terrain->graph.empty()) {
                try {
                    const double cost =
                        WorldGraph::from_json(nlohmann::json::parse(scene.settings.terrain->graph))
                            .cost();
                    if (cost > kGraphCostBudget) {
                        char text[160];
                        std::snprintf(text, sizeof(text),
                                      "the world type's ground costs %.1f times the 5.9 ground, "
                                      "over the %.0f a chunk's budget allows: it streams slowly",
                                      cost, kGraphCostBudget);
                        report.warnings.emplace_back(text);
                    }
                } catch (const std::exception&) {  // (its problems are reported above)
                }
            }
            if (!scene.settings.body)
                report.warnings.push_back(
                    "the scene has terrain settings but no world shape; there is no terrain");
            // A world type made for other shapes or sizes.
            if (scene.settings.recipe && scene.settings.body)
                for (std::string& misfit :
                     recipe_misfits(*scene.settings.recipe, *scene.settings.body))
                    report.errors.push_back("world type '" + scene.settings.world->world_type +
                                            "': " + std::move(misfit));
            // Terrain is the ground: a World Surface lies at height 0 under it (at sea level,
            // hiding the water there and solid where the sea should be).
            for (entt::entity e : scene.registry.view<const MeshRef>()) {
                const std::vector<std::string> assets = scene.registry.get<MeshRef>(e).assets();
                if (scene.settings.body &&
                    std::find(assets.begin(), assets.end(), kBuiltinSurface) != assets.end())
                    report.warnings.push_back(
                        "entity '" + scene.registry.get<EntityInfo>(e).id +
                        "' is a World Surface, but the scene has terrain: it lies at height 0 "
                        "under the terrain, hiding its water; delete it");
            }
        }
        for (const TerrainStamp& stamp : scene.terrain_stamps)
            for (std::string& problem : stamp_problems(stamp))
                report.errors.push_back(std::move(problem));
        for (const auto& [id, env] : scene.environments)
            for (std::string& problem : environment_problems(env))
                report.errors.push_back(std::move(problem));
        for (entt::entity e : scene.registry.view<const Sky>()) {
            const std::string& env = scene.registry.get<Sky>(e).environment;
            if (!env.empty() && !scene.environments.contains(env))
                report.warnings.push_back("entity '" + scene.registry.get<EntityInfo>(e).id +
                                          "' links to environment '" + env +
                                          "', which is not in the package; its sky is flat");
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
