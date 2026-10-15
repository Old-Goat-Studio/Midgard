// SPDX-License-Identifier: Apache-2.0
#include <doctest/doctest.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

#include <miniz.h>
#include <sqlite3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include "midgard/game.h"
#include "midgard/gltf.h"
#include "midgard/pak.h"
#include "midgard/scene.h"

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

std::string utf8(const fs::path& p) {
    const std::u8string u8 = p.u8string();
    return {u8.begin(), u8.end()};
}

// A minimal glTF binary writer for tests (the editor has its own, in Python).
std::vector<uint8_t> write_glb(const midgard::render::MeshData& m) {
    std::vector<uint8_t> bin;
    auto append = [&](const void* data, size_t bytes) {
        const auto* b = static_cast<const uint8_t*>(data);
        bin.insert(bin.end(), b, b + bytes);
    };
    std::vector<float> pos, nrm;
    for (const auto& v : m.vertices) {
        pos.insert(pos.end(), v.position, v.position + 3);
        nrm.insert(nrm.end(), v.normal, v.normal + 3);
    }
    const size_t pbytes = pos.size() * 4, nbytes = nrm.size() * 4, ibytes = m.indices.size() * 4;
    append(pos.data(), pbytes);
    append(nrm.data(), nbytes);
    append(m.indices.data(), ibytes);
    const json doc = {
        {"asset", {{"version", "2.0"}}},
        {"buffers", {{{"byteLength", bin.size()}}}},
        {"bufferViews",
         {{{"buffer", 0}, {"byteOffset", 0}, {"byteLength", pbytes}},
          {{"buffer", 0}, {"byteOffset", pbytes}, {"byteLength", nbytes}},
          {{"buffer", 0}, {"byteOffset", pbytes + nbytes}, {"byteLength", ibytes}}}},
        {"accessors",
         {{{"bufferView", 0},
           {"componentType", 5126},
           {"count", m.vertices.size()},
           {"type", "VEC3"},
           {"min", {-0.5, -0.5, -0.5}},
           {"max", {0.5, 0.5, 0.5}}},
          {{"bufferView", 1},
           {"componentType", 5126},
           {"count", m.vertices.size()},
           {"type", "VEC3"}},
          {{"bufferView", 2},
           {"componentType", 5125},
           {"count", m.indices.size()},
           {"type", "SCALAR"}}}},
        {"meshes",
         {{{"primitives", {{{"attributes", {{"POSITION", 0}, {"NORMAL", 1}}}, {"indices", 2}}}}}}},
    };
    std::string text = doc.dump();
    while (text.size() % 4) text.push_back(' ');
    while (bin.size() % 4) bin.push_back(0);
    std::vector<uint8_t> out;
    auto put = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(uint8_t(v >> (8 * i)));
    };
    put(0x46546C67);  // "glTF"
    put(2);
    put(uint32_t(12 + 8 + text.size() + 8 + bin.size()));
    put(uint32_t(text.size()));
    put(0x4E4F534A);  // "JSON"
    out.insert(out.end(), text.begin(), text.end());
    put(uint32_t(bin.size()));
    put(0x004E4942);  // "BIN\0"
    out.insert(out.end(), bin.begin(), bin.end());
    return out;
}

void exec(sqlite3* db, const std::string& sql) {
    char* err = nullptr;
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        std::string message = err ? err : "?";
        sqlite3_free(err);
        FAIL("SQL failed: " << message << "\n" << sql);
    }
}

// A package folder with the real migrations applied, a scene, and game.pak with a cube mesh.
struct TestPackage {
    fs::path dir;

    explicit TestPackage(const std::vector<std::pair<std::string, std::string>>& entities,
                         const std::string& settings = R"({"sky": [0.1, 0.2, 0.3]})") {
        std::random_device rd;
        dir = fs::temp_directory_path() / ("midgard_scene_" + std::to_string(rd()));
        fs::create_directories(dir);

        sqlite3* db = nullptr;
        sqlite3_open(utf8(dir / "Game.db").c_str(), &db);
        std::vector<fs::path> migrations;
        for (const auto& e : fs::directory_iterator(fs::path(MIDGARD_SCHEMA_DIR) / "migrations"))
            migrations.push_back(e.path());
        std::sort(migrations.begin(), migrations.end());
        for (const auto& m : migrations) {
            std::ifstream in(m);
            std::stringstream sql;
            sql << in.rdbuf();
            exec(db, sql.str());
        }
        exec(db, "PRAGMA user_version = " + std::to_string(migrations.size()));
        exec(db, "INSERT INTO string VALUES ('scn_main.name', 'Main', '')");
        exec(db,
             "INSERT INTO scene (id, name_key, settings) VALUES ('scn_main', 'scn_main.name', '" +
                 settings + "')");
        for (const auto& [id, rest] : entities)
            exec(db, "INSERT INTO entity (id, scene, name, parent, components) VALUES ('" + id +
                         "', 'scn_main', '" + id + "', " + rest + ")");
        sqlite3_close(db);

        const auto glb = write_glb(midgard::render::make_cube());
        mz_zip_archive zip{};
        REQUIRE(mz_zip_writer_init_file(&zip, utf8(dir / "game.pak").c_str(), 0));
        REQUIRE(mz_zip_writer_add_mem(&zip, "meshes/cube.glb", glb.data(), glb.size(),
                                      MZ_DEFAULT_LEVEL));
        mz_zip_writer_finalize_archive(&zip);
        mz_zip_writer_end(&zip);

        std::ofstream(dir / "manifest.json") << json{
            {"format", "midgard-package"},
            {"format_version", "1.0"},
            {"game", {{"name", "Game"}, {"version", "0.1.0"}}},
            {"built_with", {{"editor", "0.1.0+1"}, {"engine_abi", 0}}},
            {"target", {{"os", "windows"}, {"arch", "x86_64"}, {"role", "game"}}},
            {"entry", {{"scene", "scn_main"}}},
            {"database", "Game.db"},
            {"schema_version", migrations.size()},
            {"content", {{{"id", "mesh_cube"}, {"path", "meshes/cube.glb"}, {"sha256", std::string(64, '0')}}}},
        }.dump();
    }
    ~TestPackage() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

bool near(const glm::vec3& a, const glm::vec3& b) {
    return glm::length(a - b) < 1e-4f;
}

}  // namespace

TEST_CASE("a cube survives a glb round trip") {
    const auto cube = midgard::render::make_cube();
    const auto loaded = midgard::load_glb(write_glb(cube));
    CHECK(loaded.vertices.size() == cube.vertices.size());
    CHECK(loaded.indices == cube.indices);
    CHECK(loaded.vertices[5].normal[1] == cube.vertices[5].normal[1]);
}

TEST_CASE("garbage is not a glb") {
    const std::vector<uint8_t> junk{1, 2, 3, 4, 5};
    CHECK_THROWS_AS(midgard::load_glb(junk), midgard::AssetError);
}

TEST_CASE("game.pak reads its files and reports missing ones") {
    TestPackage pkg({});
    midgard::Pak pak(pkg.dir / "game.pak");
    CHECK(pak.contains("meshes/cube.glb"));
    CHECK_FALSE(pak.contains("nope.glb"));
    CHECK(pak.files() == std::vector<std::string>{"meshes/cube.glb"});
    CHECK_FALSE(pak.read("meshes/cube.glb").empty());
    CHECK_THROWS_AS(pak.read("nope.glb"), midgard::AssetError);
}

TEST_CASE("the scene is built from the entity table") {
    TestPackage pkg({
        {"e1_cube", R"(NULL, '{"transform": {"position": [1, 0, 0]}, "mesh": {"asset": "mesh_cube"},
                       "material": {"color": [0.8, 0.6, 0.1]}}')"},
        {"e2_child",
         R"('e1_cube', '{"transform": {"position": [0, 2, 0]}, "mesh": {"asset": "mesh_cube"}}')"},
        {"e3_camera", R"(NULL, '{"transform": {"position": [0, 0, 5]}, "camera": {"fov": 60}}')"},
        {"e4_sun",
         R"(NULL, '{"transform": {"rotation": [-90, 0, 0]}, "light": {"type": "directional",
                      "color": [1, 1, 1], "intensity": 2}}')"},
    });
    midgard::Game game(pkg.dir);
    const midgard::Scene& scene = game.scene();
    CHECK(scene.registry.view<midgard::MeshRef>().size() == 2);
    CHECK(near(scene.settings.sky, {0.1f, 0.2f, 0.3f}));

    entt::entity child = entt::null;
    for (auto e : scene.registry.view<midgard::EntityInfo>())
        if (scene.registry.get<midgard::EntityInfo>(e).id == "e2_child") child = e;
    REQUIRE(scene.registry.valid(child));
    CHECK(near(glm::vec3(scene.world_matrix(child)[3]), {1, 2, 0}));  // parent + child offsets

    const auto camera = scene.camera(16.0f / 9.0f);
    REQUIRE(camera.has_value());
    CHECK(near(glm::vec3(camera->view * glm::vec4(0, 0, 5, 1)), {0, 0, 0}));  // camera at origin

    const auto light = scene.light();
    CHECK(near(light.direction, {0, -1, 0}));  // pitched down 90 degrees: shining straight down
    CHECK(near(light.color, {2, 2, 2}));
}

TEST_CASE("a scene without a camera gets a default view") {
    TestPackage pkg({{"e1", R"(NULL, '{"mesh": {"asset": "builtin:cube"}}')"}});
    midgard::Game game(pkg.dir);
    CHECK_FALSE(game.scene().camera(1.0f).has_value());
    // glm's matrix != is ambiguous under C++20 rewritten comparisons, so check a component:
    // the default camera sits away from the origin, so its view has a translation.
    const bool has_default_view = game.camera(1.0f).view[3][2] != 0.0f;
    CHECK(has_default_view);
}

TEST_CASE("bad component JSON is reported with the entity id") {
    TestPackage pkg({{"e_bad", R"(NULL, '{"transform": {"position": [1, 2]}}')"}});
    CHECK_THROWS_WITH_AS(midgard::Game(pkg.dir), doctest::Contains("e_bad"), midgard::SceneError);
}

TEST_CASE("a missing entry scene is reported") {
    TestPackage pkg({});
    {
        sqlite3* db = nullptr;
        sqlite3_open(utf8(pkg.dir / "Game.db").c_str(), &db);
        exec(db, "DELETE FROM scene");
        sqlite3_close(db);
    }
    CHECK_THROWS_WITH_AS(midgard::Game(pkg.dir), doctest::Contains("not in the database"),
                         midgard::SceneError);
}

TEST_CASE("live edits change the running scene") {
    TestPackage pkg({
        {"e1_cube",
         R"(NULL, '{"transform": {"position": [0, 0.5, 0]}, "mesh": {"asset": "mesh_cube"},
                       "player": {"speed": 4}}')"},
        {"e2_camera", R"(NULL, '{"transform": {"position": [0, 3, 6]}, "camera": {}}')"},
    });
    midgard::Game game(pkg.dir);
    midgard::Scene& scene = game.scene();
    const entt::entity cube = scene.find("e1_cube");
    REQUIRE(scene.registry.valid(cube));
    const float distance = game.third_person().distance();

    // Replace a whole entity: same handle, new components.
    game.put_entity({{"id", "e1_cube"},
                     {"name", "Box"},
                     {"components",
                      {{"transform", {{"position", {2, 0.5, 0}}}},
                       {"mesh", {{"asset", "mesh_cube"}}},
                       {"material", {{"color", {1, 0, 0, 1}}}},
                       {"player", {{"speed", 6}}}}}});
    CHECK(scene.find("e1_cube") == cube);
    CHECK(scene.registry.get<midgard::EntityInfo>(cube).name == "Box");
    CHECK(near(scene.registry.get<midgard::Transform>(cube).position, {2, 0.5f, 0}));
    CHECK(scene.registry.get<midgard::Material>(cube).color.r == 1.0f);
    CHECK(scene.registry.get<midgard::Player>(cube).speed == 6.0f);
    CHECK(game.third_person().distance() == doctest::Approx(distance));  // the orbit is kept

    // One component, then removing it.
    game.set_component("e1_cube", "material", {{"color", {0, 1, 0}}});
    CHECK(scene.registry.get<midgard::Material>(cube).color.g == 1.0f);
    game.set_component("e1_cube", "material", nullptr);
    CHECK_FALSE(scene.registry.all_of<midgard::Material>(cube));
    CHECK_THROWS_AS(game.set_component("nobody", "material", nullptr), midgard::SceneError);
    CHECK_THROWS_AS(game.put_entity({{"id", "e1_cube"}, {"components", {{"mesh", 3}}}}),
                    midgard::SceneError);

    // New entities appear and removed ones go.
    game.put_entity({{"id", "e3_new"},
                     {"name", "New"},
                     {"parent", "e1_cube"},
                     {"components", {{"mesh", {{"asset", "builtin:cube"}}}}}});
    const entt::entity added = scene.find("e3_new");
    REQUIRE(scene.registry.valid(added));
    CHECK(scene.registry.get<midgard::EntityInfo>(added).parent == cube);
    game.remove_entity("e1_cube");
    CHECK_FALSE(scene.registry.valid(scene.find("e1_cube")));
    CHECK_FALSE(scene.registry.valid(scene.registry.get<midgard::EntityInfo>(added).parent));
    CHECK_FALSE(game.third_person().active());  // no player any more
}

TEST_CASE("scene settings and entity rows reload from the database") {
    TestPackage pkg({{"e1_cube", R"(NULL, '{"mesh": {"asset": "builtin:cube"}}')"}});
    midgard::Game game(pkg.dir);
    sqlite3* db = nullptr;
    sqlite3_open(utf8(pkg.dir / "Game.db").c_str(), &db);
    exec(db, R"(UPDATE scene SET settings = '{"sky": [0.5, 0.6, 0.7]}')");
    exec(db, R"(UPDATE entity SET name = 'Renamed',
                components = '{"transform": {"position": [0, 4, 0]}}' WHERE id = 'e1_cube')");
    exec(db, R"(INSERT INTO entity (id, scene, name, parent, components)
                VALUES ('e2_new', 'scn_main', 'New', 'e1_cube', '{}'))");
    sqlite3_close(db);

    game.reload_scene_settings();
    CHECK(near(game.scene().settings.sky, {0.5f, 0.6f, 0.7f}));
    game.reload_entity("e1_cube");
    const entt::entity cube = game.scene().find("e1_cube");
    CHECK(game.scene().registry.get<midgard::EntityInfo>(cube).name == "Renamed");
    CHECK_FALSE(game.scene().registry.all_of<midgard::MeshRef>(cube));
    CHECK(near(game.scene().registry.get<midgard::Transform>(cube).position, {0, 4, 0}));
    game.reload_entity("e2_new");
    CHECK(game.scene().registry.valid(game.scene().find("e2_new")));
    game.reload_entity("e9_missing");  // not in the database: nothing to add, nothing to remove
    CHECK_FALSE(game.scene().registry.valid(game.scene().find("e9_missing")));
}

TEST_CASE("triggers report the player entering and leaving") {
    TestPackage pkg({
        {"e1_player", R"(NULL, '{"transform": {"position": [0, 0, 0]}, "player": {}}')"},
        {"e2_trigger", R"(NULL, '{"transform": {"position": [3, 0, 0], "scale": [2, 1, 1]},
                         "trigger": {"size": [1, 2, 2]}, "script": {"module": "door"}}')"},
    });
    midgard::Game game(pkg.dir);
    midgard::Actions actions(midgard::InputMap::defaults());
    midgard::Scene& scene = game.scene();
    const entt::entity player = scene.find("e1_player");
    const entt::entity trigger = scene.find("e2_trigger");
    CHECK(scene.registry.get<midgard::ScriptRef>(trigger).module == "door");

    game.update(actions, 0.016f);
    CHECK(game.trigger_events().empty());
    scene.registry.get<midgard::Transform>(player).position = {2.1f, 0.0f, 0.0f};  // half width 1
    game.update(actions, 0.016f);
    REQUIRE(game.trigger_events().size() == 1);
    CHECK(game.trigger_events()[0].entered);
    CHECK(game.trigger_events()[0].trigger == trigger);
    game.update(actions, 0.016f);
    CHECK(game.trigger_events().empty());  // still inside: nothing new
    scene.registry.get<midgard::Transform>(player).position = {0.0f, 0.0f, 0.0f};
    game.update(actions, 0.016f);
    REQUIRE(game.trigger_events().size() == 1);
    CHECK_FALSE(game.trigger_events()[0].entered);
}
