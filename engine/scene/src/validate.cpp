// SPDX-License-Identifier: Apache-2.0
#include "midgard/validate.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <sqlite3.h>
#include <nlohmann/json.hpp>

#include "midgard/audio.h"
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

namespace {

// Whether an entity in a scene is an arrival.
bool is_arrival(const std::filesystem::path& database, const std::string& scene,
                const std::string& entity) {
    bool found = false;
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(database.string().c_str(), &db, SQLITE_OPEN_READONLY, nullptr) ==
        SQLITE_OK) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT components FROM entity WHERE id = ? AND scene = ?", -1,
                               &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, entity.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 2, scene.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(st) == SQLITE_ROW) {
                const nlohmann::json c = nlohmann::json::parse(
                    reinterpret_cast<const char*>(sqlite3_column_text(st, 0)), nullptr, false);
                found = c.is_object() && c.contains("arrival");
            }
            sqlite3_finalize(st);
        }
    }
    sqlite3_close(db);
    return found;
}

// The scenes in a package's database, by id.
std::vector<std::string> scene_ids(const std::filesystem::path& database) {
    std::vector<std::string> out;
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(database.string().c_str(), &db, SQLITE_OPEN_READONLY, nullptr) ==
        SQLITE_OK) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT id FROM scene ORDER BY id", -1, &st, nullptr) ==
            SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW)
                out.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(st, 0)));
            sqlite3_finalize(st);
        }
    }
    sqlite3_close(db);
    return out;
}

// One scene's checks: its entities, terrain, environments, objects, textures and colliders.
void validate_scene(const Manifest& m, const std::filesystem::path& dir,
                    const std::filesystem::path& database, const Pak* pak,
                    const std::set<std::string>& content_ids, const std::string& scene_id,
                    const std::vector<std::string>& all_scenes, ValidationReport& report) {
    try {
        const Scene scene = Scene::load(database, scene_id);
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
                if (asset != kBuiltinCube && asset != kBuiltinSurface && asset != kBuiltinSphere &&
                    asset != kBuiltinCylinder && !content_ids.contains(asset))
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
        // Scene exits lead to a scene in the package, and to an arrival there.
        for (entt::entity e : scene.registry.view<const SceneExit>()) {
            const SceneExit& x = scene.registry.get<SceneExit>(e);
            const std::string who = "entity '" + scene.registry.get<EntityInfo>(e).id + "'";
            if (x.scene.empty()) {
                report.errors.push_back(who +
                                        " is a scene exit that leads nowhere: choose its "
                                        "scene");
            } else if (x.scene == "@space") {  // (Game::kSystemSpace: a dock, an exit site)
                const auto& body = scene.settings.body;
                const bool outside = body && (body->shape.kind() == Shape::Kind::SphereOutside ||
                                              body->shape.kind() == Shape::Kind::SphereInside ||
                                              body->shape.kind() == Shape::Kind::CurvedCylinder);
                if (!outside)
                    report.errors.push_back(who +
                                            " is a way out into space, but only a planet, "
                                            "a habitat or a hollow world has one");
                else if (!x.arrival.empty() && !is_arrival(database, scene.id, x.arrival))
                    report.errors.push_back(who + " comes back in at arrival '" + x.arrival +
                                            "', which is not an arrival in its own scene");
            } else if (std::find(all_scenes.begin(), all_scenes.end(), x.scene) == all_scenes.end())
                report.errors.push_back(who + " is a scene exit to scene '" + x.scene +
                                        "', which is not in the package");
            else if (!x.arrival.empty() && !is_arrival(database, x.scene, x.arrival))
                report.errors.push_back(who + " is a scene exit to arrival '" + x.arrival +
                                        "', which is not an arrival in scene '" + x.scene + "'");
        }
        for (entt::entity e : scene.registry.view<const Material>()) {
            const Material& material = scene.registry.get<Material>(e);
            std::vector<std::string> used(material.textures.begin(), material.textures.end());
            used.insert(used.end(), material.shader_textures.begin(),
                        material.shader_textures.end());
            for (const std::string& texture : used)
                if (!texture.empty() && !content_ids.contains(texture))
                    report.warnings.push_back("entity '" + scene.registry.get<EntityInfo>(e).id +
                                              "' uses texture '" + texture +
                                              "', which is not in the package; drawn without it");
            if (!material.shader.empty() && !content_ids.contains(material.shader))
                report.warnings.push_back("entity '" + scene.registry.get<EntityInfo>(e).id +
                                          "' uses surface shader '" + material.shader +
                                          "', which is not in the package; drawn magenta");
        }
        (void)scene.light();
        (void)scene.camera(16.0f / 9.0f);
        // Every collider builds (a missing mesh, a malformed shape or a dynamic mesh is an
        // error naming its entity).
        const Pak* paks = pak;
        const PhysicsLayers layers = PhysicsLayers::load(database);
        const ScenePhysics physics(scene, layers, [&](const std::string& asset) {
            if (asset == kBuiltinCube) return render::make_cube();
            if (asset == kBuiltinSphere) return render::make_sphere();
            if (asset == kBuiltinCylinder) return render::make_cylinder();
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
}

}  // namespace

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
                } else if (c.path.size() > 4 && c.path.ends_with(".spv")) {
                    // A surface shader (milestone 5.17), compiled by the editor.
                    static const uint8_t magic[4] = {0x03, 0x02, 0x23, 0x07};
                    if (bytes.size() < 20 || bytes.size() % 4 != 0 ||
                        std::memcmp(bytes.data(), magic, 4) != 0)
                        throw AssetError("not a compiled shader (SPIR-V)");
                } else if (c.path.starts_with("audio/")) {
                    // A sound (milestone 5.14): read through by the game's own decoders.
                    try {
                        if (audio::sound_info(bytes).seconds <= 0.0)
                            throw AssetError("an empty sound");
                    } catch (const audio::AudioError& e) {
                        throw AssetError(std::string("not a sound the game can play: ") + e.what());
                    }
                } else {
                    load_glb(bytes);
                }
                ++report.assets;
            } catch (const std::exception& e) {
                report.errors.emplace_back("content '" + c.id + "' (" + c.path + "): " + e.what());
            }
        }
    }

    // Every scene (from 5.11a a game has many), the starting one first; with more than one,
    // each problem names its scene.
    std::vector<std::string> scenes = scene_ids(database);
    const auto entry = std::find(scenes.begin(), scenes.end(), m.entry_scene);
    if (entry == scenes.end()) {
        if (!scenes.empty())
            report.errors.push_back("the starting scene '" + m.entry_scene +
                                    "' is not in the package");
        scenes.insert(scenes.begin(), m.entry_scene);  // (its load names the problem)
    } else {
        std::rotate(scenes.begin(), entry, entry + 1);
    }
    for (const std::string& id : scenes) {
        ValidationReport one;
        validate_scene(m, dir, database, pak.get(), content_ids, id, scenes, one);
        const std::string prefix = scenes.size() > 1 ? "scene '" + id + "': " : "";
        for (std::string& e : one.errors) report.errors.push_back(prefix + e);
        for (std::string& w : one.warnings) report.warnings.push_back(prefix + w);
        report.entities += one.entities;
    }

    try {
        (void)InputMap::load(database);
    } catch (const std::exception& e) {
        report.errors.emplace_back("input bindings: " + std::string(e.what()));
    }
    return report;
}

}  // namespace midgard
