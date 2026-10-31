// SPDX-License-Identifier: Apache-2.0
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <sstream>
#include <string>

#include <miniz.h>
#include <sqlite3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include "midgard/body.h"
#include "midgard/collider_outlines.h"
#include "midgard/game.h"
#include "midgard/gltf.h"
#include "midgard/pak.h"
#include "midgard/scene.h"
#include "midgard/validate.h"
#include "midgard/world_surface.h"

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
                         const std::string& settings = R"({"sky": [0.1, 0.2, 0.3]})",
                         const std::vector<std::pair<std::string, std::string>>& objects = {}) {
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
        for (const auto& [id, parts] : objects)
            exec(db, "INSERT INTO object (id, name, parts) VALUES ('" + id + "', '" + id + "', '" +
                         parts + "')");
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
    // Camera-relative: the camera's world position is its origin, and its view only turns.
    CHECK(near(glm::vec3(camera->origin), {0, 0, 5}));
    CHECK(near(glm::vec3(camera->view * glm::vec4(0, 0, 0, 1)), {0, 0, 0}));
    CHECK(near(glm::vec3(camera->view * glm::vec4(0, 0, -1, 1)), {0, 0, -1}));  // looks along -Z

    const auto light = scene.light();
    CHECK(near(light.direction, {0, -1, 0}));  // pitched down 90 degrees: shining straight down
    CHECK(near(light.color, {1, 1, 1}));
    CHECK(light.intensity == doctest::Approx(2.0f));  // kept apart, so bright lights don't clip
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
    const float distance = game.camera_rig().distance();

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
    CHECK(game.camera_rig().distance() == doctest::Approx(distance));  // the orbit is kept

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
    CHECK_FALSE(game.camera_rig().active());  // no player any more
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

TEST_CASE("a sky entity sets the scene's sky and ambient colours") {
    TestPackage pkg({{"e1_world", R"(NULL, '{"folder": {}}')"},
                     {"e2_sky", R"('e1_world', '{"sky": {"color": [0.4, 0.5, 0.6],
                                                         "ambient": [0.3, 0.3, 0.3]}}')"}});
    midgard::Game game(pkg.dir);
    midgard::Scene& scene = game.scene();
    CHECK(near(scene.settings.sky, {0.4f, 0.5f, 0.6f}));  // not the settings' 0.1, 0.2, 0.3
    CHECK(near(scene.light().ambient, {0.3f, 0.3f, 0.3f}));
    CHECK(scene.registry.valid(scene.find("e1_world")));  // a folder: only a transform

    // Live edits to the sky reach the scene; reloading the settings keeps the sky's colours.
    scene.set_component(scene.find("e2_sky"), "sky",
                        nlohmann::json::parse(R"({"color": [1, 0, 0]})"));
    CHECK(near(scene.settings.sky, {1, 0, 0}));
    game.reload_scene_settings();
    CHECK(near(game.scene().settings.sky, {1, 0, 0}));
}

TEST_CASE("materials carry glTF's factors, clamped to their ranges") {
    TestPackage pkg({
        {"e1_shiny", R"(NULL, '{"mesh": {"asset": "builtin:cube"}, "material": {
            "color": [0.9, 0.5, 0.1], "metallic": 1, "roughness": 0.2,
            "emissive": [1, 0.5, 0], "emissive_strength": 4}}')"},
        {"e2_wild", R"(NULL, '{"mesh": {"asset": "builtin:cube"}, "material": {
            "metallic": 3, "roughness": -1, "emissive_strength": -2}}')"},
        {"e3_plain", R"(NULL, '{"mesh": {"asset": "builtin:cube"}}')"},
    });
    midgard::Game game(pkg.dir);
    const auto& scene = game.scene();
    const auto shiny = scene.draw_item(scene.find("e1_shiny"), 7, glm::dvec3(0.0));
    CHECK(shiny.mesh == 7);
    CHECK(shiny.metallic == 1.0f);
    CHECK(shiny.roughness == doctest::Approx(0.2f));
    CHECK(near(shiny.emissive, {1, 0.5f, 0}));
    CHECK(shiny.emissive_strength == 4.0f);
    const auto wild = scene.draw_item(scene.find("e2_wild"), 0, glm::dvec3(0.0));
    CHECK(wild.metallic == 1.0f);
    CHECK(wild.roughness == 0.0f);
    CHECK(wild.emissive_strength == 0.0f);
    const auto plain = scene.draw_item(scene.find("e3_plain"), 0, glm::dvec3(0.0));
    CHECK(plain.metallic == 0.0f);  // without a material: matte light grey
    CHECK(plain.roughness == doctest::Approx(0.8f));
    CHECK(plain.color.r == doctest::Approx(0.8f));
}

TEST_CASE("point and spot lights: the nearest eight, relative to the camera") {
    std::vector<std::pair<std::string, std::string>> entities;
    for (int i = 0; i < 12; ++i)  // 12 point lights at x = 0, 10, 20, ... 110
        entities.push_back({"p" + std::string(i < 10 ? "0" : "") + std::to_string(i),
                            "NULL, '{\"transform\": {\"position\": [" + std::to_string(i * 10) +
                                ", 0, 0]}, \"light\": {\"type\": \"point\", \"range\": 25, "
                                "\"intensity\": 3}}'"});
    entities.push_back({"s_spot", R"(NULL, '{"transform": {"position": [200, 5, 0],
        "rotation": [-90, 0, 0]}, "light": {"type": "spot", "inner_angle": 10,
        "outer_angle": 5, "color": [0, 0, 1]}}')"});
    entities.push_back({"sun", R"(NULL, '{"light": {"type": "directional", "intensity": 2}}')"});
    TestPackage pkg(entities);
    midgard::Game game(pkg.dir);
    // Seen from x = 205: the spot, then the farthest point lights.
    const midgard::render::Light light = game.scene().light(glm::dvec3(205, 0, 0));
    REQUIRE(light.local.size() == size_t(midgard::render::kMaxLocalLights));
    CHECK(light.local[0].spot);
    CHECK(near(light.local[0].position, {-5, 5, 0}));   // relative to the camera
    CHECK(near(light.local[0].direction, {0, -1, 0}));  // pitched down: shining straight down
    CHECK(light.local[0].outer_angle == doctest::Approx(10.0f));  // never inside the inner cone
    CHECK(near(light.local[1].position, {-95, 0, 0}));            // the point light at x = 110
    CHECK(light.local[1].range == doctest::Approx(25.0f));
    CHECK(light.local[1].intensity == doctest::Approx(3.0f));
    // The spot and seven point lights (x = 110 down to 50); those at 0 to 40 are left out.
    CHECK(near(light.local[7].position, {-155, 0, 0}));
    CHECK(light.intensity == doctest::Approx(2.0f));  // the sun's
    // Turning a light into the sun moves it out of the local lights.
    game.scene().set_component(game.scene().find("s_spot"), "light",
                               nlohmann::json::parse(R"({"type": "directional"})"));
    CHECK_FALSE(game.scene().light(glm::dvec3(205, 0, 0)).local[0].spot);
}

TEST_CASE("positions far from the origin stay exact (double precision, camera-relative)") {
    // 50 km and 10,000 km out: a float would round these to 4 mm and 1 m steps.
    for (const double far : {50'000.0, 10'000'000.0}) {
        const std::string at = std::to_string(static_cast<long long>(far));  // whole metres
        TestPackage pkg({
            {"e1_cube", "NULL, '{\"transform\": {\"position\": [" + at + ", 0, " + at +
                            "]}, \"mesh\": {\"asset\": \"mesh_cube\"}}'"},
            {"e2_near", "NULL, '{\"transform\": {\"position\": [" + at + ".001, 0.25, " + at +
                            "]}, \"mesh\": {\"asset\": \"mesh_cube\"}}'"},
            {"e3_camera", "NULL, '{\"transform\": {\"position\": [" + at + ", 0, " + at +
                              ".5]}, \"camera\": {}}'"},
        });
        midgard::Game game(pkg.dir);
        const auto& scene = game.scene();
        const auto camera = scene.camera(1.0f);
        REQUIRE(camera.has_value());
        CHECK(camera->origin.x == far);  // exact
        const glm::mat4 near_cube = scene.draw_matrix(scene.find("e2_near"), camera->origin);
        // 1 mm across and 0.5 m in front of the camera, to well under a micrometre.
        CHECK(near_cube[3].x == doctest::Approx(0.001).epsilon(1e-6));
        CHECK(near_cube[3].y == doctest::Approx(0.25));
        CHECK(near_cube[3].z == doctest::Approx(-0.5));
        CHECK(scene.world_position(scene.find("e2_near")).x == far + 0.001);
    }
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

namespace {

// Plays the game for a while, a frame at a time.
void play(midgard::Game& game, double seconds) {
    midgard::Actions actions(midgard::InputMap::defaults());
    for (double t = 0.0; t < seconds - 1e-9; t += 1.0 / 60.0) game.update(actions, 1.0f / 60.0f);
}

}  // namespace

TEST_CASE("physics components: scenery, falling crates, kinematic movers, materials") {
    TestPackage pkg({
        {"e1_ground", R"(NULL, '{"transform": {"position": [0, -0.5, 0]},
                        "collider": {"size": [20, 1, 20]}}')"},
        {"e2_crate", R"(NULL, '{"transform": {"position": [0, 3, 0], "rotation": [0, 30, 0]},
                       "mesh": {"asset": "mesh_cube"}, "collider": {}, "rigidbody": {}}')"},
        {"e3_ball", R"(NULL, '{"transform": {"position": [4, 2, 0]},
                      "collider": {"shape": "sphere", "radius": 0.5},
                      "rigidbody": {"mass": 3}, "physics_material": {"bounciness": 0.9}}')"},
        {"e4_lift", R"(NULL, '{"transform": {"position": [-4, 0.1, 0]},
                      "collider": {"size": [2, 0.2, 2]}, "rigidbody": {"motion": "kinematic"}}')"},
        {"e5_rider", R"(NULL, '{"transform": {"position": [-4, 0.7, 0]},
                       "collider": {"shape": "capsule", "radius": 0.3, "height": 1},
                       "rigidbody": {}}')"},
        {"e6_mesh_rock", R"(NULL, '{"transform": {"position": [8, 0, 0]},
                           "mesh": {"asset": "mesh_cube"}, "collider": {"shape": "mesh"}}')"},
        // Solid and a trigger at once: it lands on the ground, and its trigger goes with it.
        {"e7_alarm", R"(NULL, '{"transform": {"position": [-8, 2, 0]}, "collider": {},
                       "rigidbody": {}, "trigger": {"size": [3, 3, 3]}}')"},
        {"e8_player", R"(NULL, '{"transform": {"position": [-6.5, 0.5, 0]}, "player": {}}')"},
    });
    midgard::Game game(pkg.dir);
    midgard::Scene& scene = game.scene();
    const entt::entity crate = scene.find("e2_crate");
    const entt::entity ball = scene.find("e3_ball");
    const entt::entity lift = scene.find("e4_lift");
    const entt::entity rider = scene.find("e5_rider");
    CHECK(game.physics().world().body_count() == 9);  // 7 solid, a trigger, the player
    CHECK(scene.registry.get<midgard::RigidBody>(ball).mass == 3.0f);

    // The lift rises 2 m over a second, carrying the capsule standing on it.
    for (int i = 0; i < 60; ++i) {
        scene.registry.get<midgard::Transform>(lift).position.y += 2.0 / 60.0;
        play(game, 1.0 / 60.0);
    }
    play(game, 2.0);
    // The crate fell onto the ground and stayed upright, turned as it was.
    const auto& c = scene.registry.get<midgard::Transform>(crate);
    CHECK(c.position.y == doctest::Approx(0.5).epsilon(0.02));
    CHECK(c.rotation.y == doctest::Approx(30.0).epsilon(0.02));
    CHECK(std::abs(c.rotation.x) < 0.5f);
    // The bouncy ball is still bouncing after 3 s; the rider rode the lift up.
    CHECK(glm::length(game.physics().world().state(game.physics().body(ball)).velocity) > 0.5f);
    CHECK(scene.registry.get<midgard::Transform>(rider).position.y > 2.0);
    const entt::entity alarm = scene.find("e7_alarm");
    CHECK(scene.registry.get<midgard::Transform>(alarm).position.y ==
          doctest::Approx(0.5).epsilon(0.05));
    // The player, 1.5 m away, is inside the alarm's 3 m trigger now it has landed.
    CHECK(game.physics().triggers_touching(scene, scene.find("e8_player")) ==
          std::vector<entt::entity>{alarm});

    // Live edits rebuild the body: the crate becomes a sphere, a static rock is removed.
    game.set_component("e2_crate", "collider", json::parse(R"({"shape": "sphere"})"));
    CHECK(game.physics().body(crate) != midgard::physics::kNoBody);
    game.remove_entity("e6_mesh_rock");
    CHECK(game.physics().world().body_count() == 8);
    game.set_component("e3_ball", "collider", nullptr);
    CHECK(game.physics().body(ball) == midgard::physics::kNoBody);
}

TEST_CASE("physics: the scene's body gives gravity, and bad colliders name their entity") {
    // A 1 km planet: a crate dropped on its side falls towards the centre and lands there.
    TestPackage planet(
        {
            {"e1_planet", R"(NULL, '{"collider": {"shape": "sphere", "radius": 1000}}')"},
            {"e2_crate", R"(NULL, '{"transform": {"position": [1003, 0, 0]},
                           "collider": {}, "rigidbody": {}}')"},
        },
        R"({"body": {"shape": "sphere_outside", "radius": 1000}})");
    midgard::Game game(planet.dir);
    REQUIRE(game.scene().settings.body.has_value());
    CHECK(game.scene().light(glm::dvec3(1003, 0, 0)).up.x == doctest::Approx(1.0f));
    play(game, 3.0);
    const glm::dvec3 p =
        game.scene().registry.get<midgard::Transform>(game.scene().find("e2_crate")).position;
    CHECK(std::abs(p.x - 1000.5) < 0.03);  // resting on the surface (Jolt allows 2 cm overlap)
    CHECK(std::abs(p.y) < 0.05);

    // Mistakes are refused on load, naming the entity.
    const auto load_error = [](const char* components) {
        TestPackage pkg({{"e9_bad", components}});
        try {
            midgard::Game g(pkg.dir);
        } catch (const std::exception& e) {
            return std::string(e.what());
        }
        return std::string();
    };
    CHECK(load_error(R"(NULL, '{"collider": {}, "rigidbody": {"motion": "flying"}}')")
              .find("flying") != std::string::npos);
    CHECK(
        load_error(R"(NULL, '{"collider": {"shape": "mesh"}, "rigidbody": {}}')").find("e9_bad") !=
        std::string::npos);  // no mesh, and meshes can't be dynamic
    CHECK(load_error(R"(NULL, '{"collider": {"size": [0, 1, 1]}}')").find("e9_bad") !=
          std::string::npos);
    CHECK(load_error(R"(NULL, '{"collider": {}, "rigidbody": {"layer": 16}}')").find("layer") !=
          std::string::npos);
}

TEST_CASE("physics layers come from the project, and rotations round-trip as Euler angles") {
    TestPackage pkg({});
    const midgard::PhysicsLayers layers = midgard::PhysicsLayers::load(pkg.dir / "Game.db");
    CHECK(layers.names[0] == "default");
    CHECK(layers.names[1] == "player");
    REQUIRE(layers.ignored.size() == 1);
    CHECK(layers.ignored[0] == std::pair{1, 2});
    midgard::Game game(pkg.dir);
    CHECK_FALSE(game.physics().world().collides(1, 2));
    CHECK(game.physics().world().collides(0, 2));

    for (const glm::vec3 euler : {glm::vec3(10, 20, 30), glm::vec3(-45, 170, 5),
                                  glm::vec3(80, -90, 0), glm::vec3(0, 0, -120)}) {
        const glm::quat q = glm::quat_cast(glm::mat3(midgard::rotation_matrix(euler)));
        const glm::vec3 back = midgard::euler_degrees(q);
        CHECK(near(glm::vec3(midgard::rotation_matrix(back)[0]),
                   glm::vec3(midgard::rotation_matrix(euler)[0])));
        CHECK(glm::length(back - euler) < 1e-3f);
    }
}

TEST_CASE("the player walks as a character: on the ground, stopped by walls, jumping") {
    TestPackage pkg({
        {"e1_ground", R"(NULL, '{"transform": {"position": [0, -0.5, 0]},
                        "collider": {"size": [40, 1, 40]}}')"},
        {"e2_wall",
         R"(NULL, '{"transform": {"position": [0, 1, -6]}, "collider": {"size": [8, 2, 1]}}')"},
        {"e3_player",
         R"(NULL, '{"transform": {"position": [0, 0.5, 0]}, "player": {"speed": 4}}')"},
        {"e4_camera", R"(NULL, '{"transform": {"position": [0, 2, 6]}, "camera": {}}')"},
    });
    midgard::Game game(pkg.dir);
    midgard::Scene& scene = game.scene();
    const entt::entity player = scene.find("e3_player");
    REQUIRE(game.physics().character() != nullptr);
    midgard::Actions actions(midgard::InputMap::defaults());
    const auto frames = [&](int n) {
        for (int i = 0; i < n; ++i) {
            game.update(actions, 1.0f / 60.0f);
            actions.begin_frame();
        }
    };
    frames(30);
    CHECK(game.physics().character()->on_ground());

    // Forward (away from the camera, along -Z) for a second: 4 m, still on the ground.
    actions.set("keyboard", "W", 1.0f);
    frames(60);
    const glm::dvec3 walked = scene.registry.get<midgard::Transform>(player).position;
    CHECK(walked.z == doctest::Approx(-4.0).epsilon(0.03));
    CHECK(std::abs(walked.y - 0.5) < 0.03);
    // On into the wall (its face at z = -5.5): stopped a radius short of it.
    frames(120);
    CHECK(scene.registry.get<midgard::Transform>(player).position.z ==
          doctest::Approx(-5.2).epsilon(0.01));
    // The camera kept following, level with the ground.
    const glm::dvec3 cam = scene.registry.get<midgard::Transform>(scene.find("e4_camera")).position;
    CHECK(cam.z > scene.registry.get<midgard::Transform>(player).position.z + 3.0);
    actions.set("keyboard", "W", 0.0f);

    // Jump: up, then down again.
    actions.set("keyboard", "Space", 1.0f);
    frames(1);
    actions.set("keyboard", "Space", 0.0f);
    frames(20);
    CHECK(scene.registry.get<midgard::Transform>(player).position.y > 1.0);
    frames(90);
    CHECK(std::abs(scene.registry.get<midgard::Transform>(player).position.y - 0.5) < 0.03);

    // A script (or the editor) moving the player puts the character there.
    scene.registry.get<midgard::Transform>(player).position = {10, 0.5, 10};
    frames(2);
    CHECK(glm::length(game.physics().character()->position() - glm::dvec3(10, 0.5, 10)) < 0.05);
}

TEST_CASE("on a planet the player stands on its side and the camera stays level with it") {
    TestPackage pkg(
        {
            {"e1_planet", R"(NULL, '{"collider": {"shape": "sphere", "radius": 1000}}')"},
            {"e2_player", R"(NULL, '{"transform": {"position": [1000.5, 0, 0]}, "player": {}}')"},
            {"e3_camera", R"(NULL, '{"transform": {"position": [1004, 2, 0]}, "camera": {}}')"},
        },
        R"({"body": {"shape": "sphere_outside", "radius": 1000}})");
    midgard::Game game(pkg.dir);
    midgard::Scene& scene = game.scene();
    midgard::Actions actions(midgard::InputMap::defaults());
    actions.set("keyboard", "W", 1.0f);  // walk "forward" for 5 seconds: 20 m along the ground
    for (int i = 0; i < 300; ++i) game.update(actions, 1.0f / 60.0f);
    const glm::dvec3 p = scene.registry.get<midgard::Transform>(scene.find("e2_player")).position;
    CHECK(glm::length(p) == doctest::Approx(1000.5).epsilon(1e-4));  // standing on the surface
    CHECK(glm::length(p - glm::dvec3(1000.5, 0, 0)) > 15.0);         // and it went somewhere
    // The camera is above the ground behind the player, along the player's "up".
    const glm::dvec3 cam = scene.registry.get<midgard::Transform>(scene.find("e3_camera")).position;
    const glm::dvec3 up = glm::normalize(p);
    CHECK(glm::dot(cam - p, up) > 0.5);
    CHECK(glm::length(cam) > 1001.0);
}

TEST_CASE("collider outlines: each shape's lines, coloured by how it moves") {
    TestPackage pkg({
        {"e1_box", R"(NULL, '{"transform": {"position": [10, 0, 0]}, "collider": {}}')"},
        {"e2_ball", R"(NULL, '{"collider": {"shape": "sphere"}, "rigidbody": {}}')"},
        {"e3_pill", R"(NULL, '{"collider": {"shape": "capsule", "radius": 0.5, "height": 2},
                      "rigidbody": {"motion": "kinematic"}}')"},
        {"e4_zone", R"(NULL, '{"trigger": {"size": [2, 2, 2]}}')"},
        {"e5_rock", R"(NULL, '{"mesh": {"asset": "mesh_cube"}, "collider": {"shape": "convex"}}')"},
        {"e6_player", R"(NULL, '{"transform": {"position": [0, 0.5, 5]}, "player": {}}')"},
    });
    midgard::Game game(pkg.dir);
    const auto& scene = game.scene();
    const midgard::render::MeshData cube = midgard::render::make_cube();
    const auto lines_of = [&](const char* id) {
        midgard::Scene only;  // the one entity, as the scene has it
        const entt::entity e = scene.find(id);
        const entt::entity copy = only.registry.create();
        only.registry.emplace<midgard::EntityInfo>(copy, id, id, entt::null);
        only.registry.emplace<midgard::Transform>(copy, scene.registry.get<midgard::Transform>(e));
        if (auto* c = scene.registry.try_get<midgard::ColliderShape>(e))
            only.registry.emplace<midgard::ColliderShape>(copy, *c);
        if (auto* r = scene.registry.try_get<midgard::RigidBody>(e))
            only.registry.emplace<midgard::RigidBody>(copy, *r);
        if (auto* t = scene.registry.try_get<midgard::Trigger>(e))
            only.registry.emplace<midgard::Trigger>(copy, *t);
        if (auto* p = scene.registry.try_get<midgard::Player>(e))
            only.registry.emplace<midgard::Player>(copy, *p);
        if (auto* m = scene.registry.try_get<midgard::MeshRef>(e))
            only.registry.emplace<midgard::MeshRef>(copy, *m);
        return midgard::collider_outlines(only, glm::dvec3(10, 0, 0),
                                          [&](const std::string&) { return &cube; });
    };
    const auto box = lines_of("e1_box");
    REQUIRE(box.size() == 12);   // a box's edges
    for (const auto& l : box) {  // relative to the origin at x = 10: a 1 m box round 0
        CHECK(std::abs(l.a.x) <= 0.5f + 1e-5f);
        CHECK(std::abs(l.b.y) <= 0.5f + 1e-5f);
    }
    const auto ball = lines_of("e2_ball");
    CHECK(ball.size() == 72);  // three circles of 24
    const auto pill = lines_of("e3_pill");
    CHECK(pill.size() == 2 * 24 + 4 + 4 * 12);  // two rings, four sides, four half-circles
    CHECK(lines_of("e4_zone").size() == 12);
    CHECK(lines_of("e5_rock").size() == 30);  // the cube's edges and face diagonals
    CHECK(lines_of("e6_player").size() == 2 * 24 + 4 + 4 * 12);  // its capsule
    // Colours: static, dynamic, kinematic, trigger and player all differ.
    const std::set<std::array<float, 3>> colours = {
        {box[0].color.r, box[0].color.g, box[0].color.b},
        {ball[0].color.r, ball[0].color.g, ball[0].color.b},
        {pill[0].color.r, pill[0].color.g, pill[0].color.b},
        {lines_of("e4_zone")[0].color.r, lines_of("e4_zone")[0].color.g,
         lines_of("e4_zone")[0].color.b},
        {lines_of("e6_player")[0].color.r, lines_of("e6_player")[0].color.g,
         lines_of("e6_player")[0].color.b}};
    CHECK(colours.size() == 5);

    // The whole scene, through the game, capped when asked.
    CHECK(game.debug_lines(glm::dvec3(0.0)).size() == 12 + 72 + 100 + 12 + 30 + 100);
    CHECK(midgard::collider_outlines(scene, glm::dvec3(0.0), [](const std::string&) {
              return static_cast<const midgard::render::MeshData*>(nullptr);
          }).size() == 12 + 72 + 100 + 12 + 100);  // no mesh, no rock outline
    CHECK(midgard::collider_outlines(
              scene, glm::dvec3(0.0), [&](const std::string&) { return &cube; }, 50)
              .size() == 50);
}

namespace {

midgard::Body test_body(const char* json) {
    return midgard::body_from_json(json::parse(json));
}

}  // namespace

TEST_CASE("the world's surface lies on the shape, faces up, and passes through y = 0") {
    for (const char* settings : {
             R"({"shape": "sphere_outside", "radius": 1000, "origin": [0, -1000, 0]})",
             R"({"shape": "sphere_inside", "radius": 1000, "origin": [0, 1000, 0]})",
             R"({"shape": "curved_cylinder", "radius": 1000, "length": 5000,
                 "origin": [0, 1000, 0]})",
             R"({"shape": "bounded_area", "width": 2000, "length": 2000})",
         }) {
        CAPTURE(std::string(settings));
        const midgard::Body body = test_body(settings);
        const midgard::render::MeshData mesh = midgard::surface_mesh(body);
        REQUIRE(mesh.indices.size() % 3 == 0);
        CHECK(mesh.indices.size() / 3 < 250'000);  // a mesh a phone can draw
        double worst_vertex = 0.0, worst_middle = 0.0;
        int facing_down = 0;
        for (const auto& v : mesh.vertices)
            worst_vertex = std::max(
                worst_vertex,
                std::abs(body.surface({v.position[0], v.position[1], v.position[2]}).height));
        for (size_t i = 0; i < mesh.indices.size(); i += 3) {
            glm::dvec3 p[3];
            for (int k = 0; k < 3; ++k) {
                const float* v = mesh.vertices[mesh.indices[i + k]].position;
                p[k] = {v[0], v[1], v[2]};
            }
            const glm::dvec3 middle = (p[0] + p[1] + p[2]) / 3.0;
            if (glm::dot(glm::cross(p[1] - p[0], p[2] - p[0]), body.up(middle)) <= 0.0)
                ++facing_down;
            worst_middle = std::max(worst_middle, std::abs(body.surface(middle).height));
        }
        CHECK(worst_vertex < 0.001);  // on the surface (floats, 1 km out)
        CHECK(worst_middle < 0.05);   // the flat triangles stay within 5 cm of the curve
        CHECK(facing_down == 0);
        // The scene's ground (the origin) is on the surface, with up along +Y there.
        CHECK(std::abs(body.surface({0, 0, 0}).height) < 1e-9);
        CHECK(body.up({0, 0, 0}).y == doctest::Approx(1.0));
    }
}

TEST_CASE("the exit: the player walks all the way round a planet and inside a cylinder") {
    struct Walk {
        const char* name;
        const char* body;
        const char* key;    // W along -Z (round the planet), D along +X (round the tube)
        glm::dvec3 centre;  // what it walks round
        bool round_z_axis;  // the tube's axis is Z: measure the turn in XY only
    };
    for (const Walk& w : {
             Walk{"planet",
                  R"({"shape": "sphere_outside", "radius": 1000, "origin": [0, -1000, 0]})",
                  "W",
                  {0, -1000, 0},
                  false},
             Walk{"cylinder",
                  R"({"shape": "curved_cylinder", "radius": 1000, "length": 5000,
                               "origin": [0, 1000, 0]})",
                  "D",
                  {0, 1000, 0},
                  true},
         }) {
        CAPTURE(std::string(w.name));
        TestPackage pkg(
            {
                {"e1_ground", R"(NULL, '{"mesh": {"asset": "builtin:surface"},
                                "collider": {"shape": "mesh"}}')"},
                {"e2_player", R"(NULL, '{"transform": {"position": [0, 0.5, 0]},
                                "player": {"speed": 20}}')"},
                {"e3_camera", R"(NULL, '{"transform": {"position": [0, 2, 6]}, "camera": {}}')"},
            },
            std::string(R"({"body": )") + w.body + "}");
        midgard::Game game(pkg.dir);
        midgard::Scene& scene = game.scene();
        const midgard::Body& body = *scene.settings.body;
        const entt::entity player = scene.find("e2_player");
        midgard::Actions actions(midgard::InputMap::defaults());
        actions.set("keyboard", w.key, 1.0f);
        const auto relative = [&](const glm::dvec3& p) {
            glm::dvec3 r = p - w.centre;
            if (w.round_z_axis) r.z = 0.0;
            return glm::normalize(r);
        };
        glm::dvec3 last = relative(scene.registry.get<midgard::Transform>(player).position);
        double turned = 0.0, lowest = 1e9, highest = -1e9;
        int frames = 0, airborne = 0;
        while (turned < 2.0 * 3.14159265358979 && frames < 25000) {
            game.update(actions, 1.0f / 60.0f);
            ++frames;
            const glm::dvec3 p = scene.registry.get<midgard::Transform>(player).position;
            const glm::dvec3 now = relative(p);
            turned += std::acos(std::clamp(glm::dot(last, now), -1.0, 1.0));
            last = now;
            if (!game.physics().character()->on_ground()) ++airborne;
            if (frames > 30) {
                lowest = std::min(lowest, body.surface(p).height);
                highest = std::max(highest, body.surface(p).height);
            }
        }
        // Round in 2 pi R / 20 m/s = 314 s: 18850 frames at 60 a second, give or take.
        CHECK(frames == doctest::Approx(18850).epsilon(0.05));
        CHECK(glm::length(scene.registry.get<midgard::Transform>(player).position -
                          glm::dvec3(0, 0.5, 0)) < 50.0);  // back where it started
        CHECK(lowest > 0.4);                               // never fell into the ground
        CHECK(highest < 0.65);                             // nor left it
        CHECK(airborne < 30);                              // on its feet all the way
        // The camera followed, above the ground behind the player.
        const glm::dvec3 cam =
            scene.registry.get<midgard::Transform>(scene.find("e3_camera")).position;
        CHECK(body.surface(cam).height > 1.0);
    }
}

TEST_CASE("groups: a dynamic group's parts move as one body") {
    TestPackage pkg({
        {"e1_ground", R"(NULL, '{"transform": {"position": [0, -0.5, 0]},
                        "collider": {"size": [40, 1, 40]}}')"},
        // A bench of two boxes side by side, dropped from 3 m, tilted 25 degrees.
        {"e2_bench", R"(NULL, '{"transform": {"position": [0, 3, 0], "rotation": [0, 0, 25]},
                       "group": {}, "rigidbody": {}}')"},
        {"e3_left", R"('e2_bench', '{"transform": {"position": [-0.5, 0, 0]}, "collider": {}}')"},
        {"e4_right", R"('e2_bench', '{"transform": {"position": [0.5, 0, 0]}, "collider": {},
                       "rigidbody": {}}')"},  // its own rigid body is ignored: the group's rules
        // A kinematic group and a static one.
        {"e5_lift", R"(NULL, '{"transform": {"position": [10, 0.5, 0]}, "group": {},
                      "rigidbody": {"motion": "kinematic"}}')"},
        {"e6_step", R"('e5_lift', '{"transform": {"position": [0, 0, 2]}, "collider": {}}')"},
        {"e7_wall", R"(NULL, '{"transform": {"position": [-10, 0, 0]}, "group": {}}')"},
        {"e8_brick", R"('e7_wall', '{"transform": {"position": [0, 0.5, 0]}, "collider": {}}')"},
    });
    midgard::Game game(pkg.dir);
    midgard::Scene& scene = game.scene();
    // Bodies: the ground, the bench (one), the lift (one), the brick (a static group's parts
    // keep their own).
    CHECK(game.physics().world().body_count() == 4);
    CHECK(game.physics().body(scene.find("e3_left")) == midgard::physics::kNoBody);

    play(game, 4.0);
    // The bench tipped flat and landed as one: both boxes rest on the ground, side by side.
    const glm::dvec3 left = scene.world_position(scene.find("e3_left"));
    const glm::dvec3 right = scene.world_position(scene.find("e4_right"));
    CHECK(std::abs(left.y - 0.5) < 0.05);
    CHECK(std::abs(right.y - 0.5) < 0.05);
    CHECK(glm::length(right - left) == doctest::Approx(1.0).epsilon(0.01));  // still 1 m apart
    // The parts kept their places in the group; the group itself moved.
    CHECK(scene.registry.get<midgard::Transform>(scene.find("e3_left")).position.x == -0.5);
    CHECK(std::abs(scene.registry.get<midgard::Transform>(scene.find("e2_bench")).rotation.z) <
          1.0f);

    // Moving the kinematic group carries its part's collider with it.
    auto& lift = scene.registry.get<midgard::Transform>(scene.find("e5_lift"));
    lift.position = {10, 0.5, 6};
    play(game, 0.1);
    const auto hit = game.physics().world().raycast({10, 5, 8}, {0, -1, 0}, 10.0);
    REQUIRE(hit.has_value());  // the step, now at z = 8
    CHECK(static_cast<entt::entity>(hit->user) == scene.find("e5_lift"));

    // Editing a part builds the group's body again: the right box becomes a ball.
    game.set_component("e4_right", "collider", json::parse(R"({"shape": "sphere"})"));
    CHECK(game.physics().world().body_count() == 4);
    CHECK(game.physics().body(scene.find("e2_bench")) != midgard::physics::kNoBody);
}

TEST_CASE("groups: a moving group can't hold a mesh part") {
    TestPackage pkg({
        {"e1_group", R"(NULL, '{"group": {}, "rigidbody": {}}')"},
        {"e2_part",
         R"('e1_group', '{"mesh": {"asset": "mesh_cube"}, "collider": {"shape": "mesh"}}')"},
    });
    try {
        midgard::Game game(pkg.dir);
        FAIL("a dynamic group with a mesh part was accepted");
    } catch (const std::exception& e) {
        CHECK(std::string(e.what()).find("e1_group") != std::string::npos);
        CHECK(std::string(e.what()).find("convex") != std::string::npos);
    }
}

namespace {

// A crate of two parts: a box body with a mesh, and a lid with a collider only.
const char* kCrate = R"([
    {"name": "body", "components": {"mesh": {"asset": "mesh_cube"}, "collider": {},
                                    "material": {"color": [0.6, 0.4, 0.2, 1]}}},
    {"name": "lid", "components": {"transform": {"position": [0, 0.6, 0]},
                                   "collider": {"size": [1, 0.2, 1]}}}
])";

std::vector<entt::entity> parts_of_instance(const midgard::Scene& scene, entt::entity e) {
    std::vector<entt::entity> out;
    for (entt::entity p : scene.registry.view<const midgard::ObjectPart>())
        if (scene.registry.get<midgard::ObjectPart>(p).instance == e) out.push_back(p);
    return out;
}

}  // namespace

TEST_CASE("objects: instances are filled in from their template, and follow it") {
    TestPackage pkg(
        {
            {"e1_crate", R"(NULL, '{"transform": {"position": [3, 0.5, 0]},
                            "object": {"id": "obj_crate"}}')"},
            {"e2_crate", R"(NULL, '{"transform": {"position": [-3, 0.5, 0]},
                            "object": {"id": "obj_crate"}}')"},
            {"e3_ghost", R"(NULL, '{"object": {"id": "obj_missing"}}')"},
        },
        R"({"sky": [0.1, 0.2, 0.3]})", {{"obj_crate", kCrate}});
    midgard::Game game(pkg.dir);
    midgard::Scene& scene = game.scene();
    const entt::entity crate = scene.find("e1_crate");

    // Each instance has the template's parts as its children, placed relative to it.
    const auto parts = parts_of_instance(scene, crate);
    REQUIRE(parts.size() == 2);
    const entt::entity lid = scene.find("e1_crate/1");
    REQUIRE((lid != entt::null));
    CHECK(scene.registry.get<midgard::EntityInfo>(lid).name == "lid");
    CHECK(scene.registry.get<midgard::EntityInfo>(lid).parent == crate);
    CHECK(near(glm::vec3(scene.world_position(lid)), {3, 1.1f, 0}));
    CHECK(parts_of_instance(scene, scene.find("e3_ghost")).empty());  // unknown: nothing shown
    // Static instances: each part is a body of its own (and the ground-less scene has no others).
    CHECK(game.physics().world().body_count() == 4);

    // The template changes: both instances follow, and their old parts' bodies go.
    game.set_object("obj_crate", R"([{"name": "only", "components": {"collider": {}}}])");
    CHECK(parts_of_instance(scene, crate).size() == 1);
    CHECK((scene.find("e1_crate/1") == entt::null));
    CHECK(game.physics().world().body_count() == 2);

    // Removing an instance removes its parts.
    game.remove_entity("e1_crate");
    CHECK((scene.find("e1_crate/0") == entt::null));
    CHECK(game.physics().world().body_count() == 1);

    // An entity becomes an instance when it gains the component, and stops when it loses it.
    game.set_component("e3_ghost", "object", json::parse(R"({"id": "obj_crate"})"));
    CHECK(parts_of_instance(scene, scene.find("e3_ghost")).size() == 1);
    game.set_component("e3_ghost", "object", nullptr);
    CHECK(parts_of_instance(scene, scene.find("e3_ghost")).empty());
    CHECK(game.physics().world().body_count() == 1);
}

TEST_CASE("objects: a moving instance is one body of its parts' colliders") {
    TestPackage pkg(
        {
            {"e1_ground", R"(NULL, '{"transform": {"position": [0, -0.5, 0]},
                            "collider": {"size": [20, 1, 20]}}')"},
            {"e2_crate", R"(NULL, '{"transform": {"position": [0, 4, 0]},
                            "object": {"id": "obj_crate"}, "rigidbody": {}}')"},
        },
        R"({"sky": [0.1, 0.2, 0.3]})", {{"obj_crate", kCrate}});
    midgard::Game game(pkg.dir);
    midgard::Scene& scene = game.scene();
    CHECK(game.physics().world().body_count() == 2);  // the ground, and the crate as one
    play(game, 3.0);
    // It fell and rests on its body (the lid stays on top, 0.6 m up).
    const glm::dvec3 body = scene.world_position(scene.find("e2_crate/0"));
    const glm::dvec3 lid = scene.world_position(scene.find("e2_crate/1"));
    CHECK(std::abs(body.y - 0.5) < 0.05);
    CHECK(lid.y - body.y == doctest::Approx(0.6).epsilon(0.01));
    // The game draws the part with a mesh, with the template's material.
    CHECK(scene.registry.get<midgard::Material>(scene.find("e2_crate/0")).color.r ==
          doctest::Approx(0.6f));
}

TEST_CASE("objects: validation warns about unknown objects and missing textures") {
    TestPackage pkg({
        {"e1_ghost", R"(NULL, '{"object": {"id": "obj_missing"}}')"},
        {"e2_box", R"(NULL, '{"mesh": {"asset": "mesh_cube"},
                          "material": {"textures": {"base_color": "tex_missing"}}}')"},
    });
    const midgard::ValidationReport report = midgard::validate_package(pkg.dir);
    std::string all;
    for (const std::string& w : report.warnings) all += w + "\n";
    CHECK(all.find("obj_missing") != std::string::npos);
    CHECK(all.find("tex_missing") != std::string::npos);
}

TEST_CASE("save_glb writes what load_glb reads back exactly") {
    const midgard::render::MeshData cube = midgard::render::make_cube();
    const midgard::render::MeshData back = midgard::load_glb(midgard::save_glb(cube));
    REQUIRE(back.vertices.size() == cube.vertices.size());
    CHECK(back.indices == cube.indices);
    CHECK(std::memcmp(back.vertices.data(), cube.vertices.data(),
                      cube.vertices.size() * sizeof(midgard::render::Vertex)) == 0);
}

TEST_CASE("meshes: levels of detail are chosen by distance") {
    TestPackage pkg({{"e1_tree", R"(NULL, '{"mesh": {"asset": "mesh_cube", "lods": [
                         {"asset": "mesh_far", "distance": 80},
                         {"asset": "mesh_mid", "distance": 20}]}}')"}});
    midgard::Game game(pkg.dir);
    const auto& ref = game.scene().registry.get<midgard::MeshRef>(game.scene().find("e1_tree"));
    CHECK(ref.at(5.0) == "mesh_cube");
    CHECK(ref.at(20.0) == "mesh_mid");  // sorted nearest first
    CHECK(ref.at(500.0) == "mesh_far");
    CHECK(ref.assets() == std::vector<std::string>{"mesh_cube", "mesh_mid", "mesh_far"});
    // Validation names the levels of detail that are missing.
    std::string all;
    for (const std::string& w : midgard::validate_package(pkg.dir).warnings) all += w + "\n";
    CHECK(all.find("mesh_mid") != std::string::npos);
}

namespace {

// A ground, a player in the middle and a camera behind it.
std::vector<std::pair<std::string, std::string>> camera_scene(const std::string& camera = "{}") {
    return {
        {"e1_ground", R"(NULL, '{"transform": {"position": [0, -0.5, 0]},
                        "collider": {"size": [60, 1, 60]}}')"},
        {"e2_player", R"(NULL, '{"transform": {"position": [0, 0.5, 0]},
                        "mesh": {"asset": "mesh_cube"}, "player": {}}')"},
        {"e3_camera",
         "NULL, '{\"transform\": {\"position\": [0, 3, 6]}, \"camera\": " + camera + "}'"},
    };
}

// Presses an action's key for one frame.
void press(midgard::Game& game, midgard::Actions& actions, const char* key) {
    actions.begin_frame();
    actions.set("keyboard", key, 1.0f);
    game.update(actions, 1.0f / 60.0f);
    actions.begin_frame();
    actions.set("keyboard", key, 0.0f);
}

glm::vec3 camera_axis(const midgard::Scene& scene, const glm::vec3& local) {
    const auto& t = scene.registry.get<midgard::Transform>(scene.find("e3_camera"));
    return glm::vec3(midgard::rotation_matrix(t.rotation) * glm::vec4(local, 0.0f));
}

glm::dvec3 camera_at(const midgard::Scene& scene) {
    return scene.registry.get<midgard::Transform>(scene.find("e3_camera")).position;
}

void play_with(midgard::Game& game, midgard::Actions& actions, double seconds) {
    for (double t = 0.0; t < seconds - 1e-9; t += 1.0 / 60.0) game.update(actions, 1.0f / 60.0f);
}

}  // namespace

TEST_CASE("cameras: the view action steps through the views, keeping the player's facing") {
    TestPackage pkg(camera_scene());
    midgard::Game game(pkg.dir);
    midgard::Scene& scene = game.scene();
    midgard::Actions actions(midgard::InputMap::defaults());
    CHECK(game.camera_rig().view() == midgard::CameraView::Third);
    // Walk right (+X) for a moment: the player turns to face that way.
    actions.set("keyboard", "D", 1.0f);
    play_with(game, actions, 0.5);
    actions.set("keyboard", "D", 0.0f);
    const auto& player = scene.registry.get<midgard::Transform>(scene.find("e2_player"));
    const glm::vec3 facing =
        glm::vec3(midgard::rotation_matrix(player.rotation) * glm::vec4(0, 0, -1, 0));
    CHECK(facing.x > 0.99f);

    press(game, actions, "V");  // first person: looking the way the player faces
    CHECK(game.camera_rig().view() == midgard::CameraView::First);
    play_with(game, actions, 1.0);
    CHECK(glm::dot(camera_axis(scene, {0, 0, -1}), facing) > 0.99f);
    CHECK(glm::length(camera_at(scene) - (player.position + glm::dvec3(0, 0.35, 0))) < 0.01);

    press(game, actions, "V");  // top-down: high above, behind, at its fixed angle
    play_with(game, actions, 1.0);
    CHECK(game.camera_rig().view() == midgard::CameraView::Top);
    const glm::dvec3 above = camera_at(scene) - player.position;
    CHECK(above.y == doctest::Approx(18.0 * std::sin(glm::radians(60.0))).epsilon(0.02));
    CHECK(scene.registry.get<midgard::CameraLens>(scene.find("e3_camera")).fov ==
          doctest::Approx(45.0f));

    press(game, actions, "V");  // third person again: behind the player
    play_with(game, actions, 1.0);
    CHECK(game.camera_rig().view() == midgard::CameraView::Third);
    const glm::dvec3 behind = camera_at(scene) - player.position;
    CHECK(glm::dot(glm::normalize(glm::vec3(float(behind.x), 0.0f, float(behind.z))), facing) <
          -0.99f);
    CHECK(glm::dot(camera_axis(scene, {0, 0, -1}), facing) > 0.5f);  // still looking that way
    // The player faced the same way all along.
    const glm::vec3 still =
        glm::vec3(midgard::rotation_matrix(player.rotation) * glm::vec4(0, 0, -1, 0));
    CHECK(glm::dot(still, facing) > 0.999f);
}

TEST_CASE("cameras: blends between views are continuous, with no jumps") {
    TestPackage pkg(camera_scene());
    midgard::Game game(pkg.dir);
    midgard::Actions actions(midgard::InputMap::defaults());
    play_with(game, actions, 0.5);
    for (const char* step : {"to first person", "to top-down", "to third person"}) {
        CAPTURE(step);
        glm::dvec3 last = camera_at(game.scene());
        glm::vec3 last_forward = camera_axis(game.scene(), {0, 0, -1});
        press(game, actions, "V");
        double worst_step = 0.0;
        float worst_turn = 0.0f;
        for (int frame = 0; frame < 60; ++frame) {
            if (frame) game.update(actions, 1.0f / 60.0f);
            const glm::dvec3 at = camera_at(game.scene());
            const glm::vec3 forward = camera_axis(game.scene(), {0, 0, -1});
            worst_step = std::max(worst_step, glm::length(at - last));
            worst_turn = std::max(
                worst_turn,
                glm::degrees(std::acos(std::clamp(glm::dot(forward, last_forward), -1.0f, 1.0f))));
            last = at;
            last_forward = forward;
        }
        CHECK_FALSE(game.camera_rig().blending());  // done within a second
        CHECK(worst_step < 1.6);                    // metres a frame (up to 16 m in 0.35 s, eased)
        CHECK(worst_turn < 12.0f);                  // degrees a frame
    }
}

TEST_CASE("cameras: the camera never passes through walls") {
    // A small room of four walls round the player, lower than the camera flies.
    auto entities = camera_scene();
    const std::tuple<const char*, const char*, const char*> walls[] = {
        {"e4_north", "[0, 1.5, -3]", "[6.4, 3, 0.4]"},
        {"e5_south", "[0, 1.5, 3]", "[6.4, 3, 0.4]"},
        {"e6_east", "[3, 1.5, 0]", "[0.4, 3, 6.4]"},
        {"e7_west", "[-3, 1.5, 0]", "[0.4, 3, 6.4]"}};
    for (const auto& [id, at, size] : walls)
        entities.push_back({id, std::string("NULL, '{\"transform\": {\"position\": ") + at +
                                    "}, \"collider\": {\"size\": " + size + "}}'"});
    TestPackage pkg(entities);
    midgard::Game game(pkg.dir);
    midgard::Scene& scene = game.scene();
    midgard::Actions actions(midgard::InputMap::defaults());
    play_with(game, actions, 0.5);
    const auto& world = game.physics().world();
    const entt::entity player = scene.find("e2_player");
    // All the way round, low and high, near and far.
    for (int step = 0; step < 72; ++step) {
        CAPTURE(step);
        actions.begin_frame();
        actions.add_delta("mouse", "delta_x", 33.3f);                           // 5 degrees
        actions.add_delta("mouse", "delta_y", step % 12 < 6 ? 20.0f : -20.0f);  // down, up
        actions.add_delta("mouse", "wheel", step % 24 < 12 ? -1.0f : 1.0f);     // out, in
        game.update(actions, 1.0f / 60.0f);
        const glm::dvec3 cam = camera_at(scene);
        // Nothing solid round the camera (a sphere a little smaller than the one it sweeps).
        midgard::physics::Collider probe;
        probe.kind = midgard::physics::Collider::Kind::Sphere;
        probe.radius = 0.2f;
        CHECK(world
                  .overlaps(probe, cam, glm::quat(1, 0, 0, 0), glm::vec3(1.0f),
                            midgard::physics::Touching::Solid)
                  .empty());
        // And nothing between it and the player.
        const glm::dvec3 target =
            scene.registry.get<midgard::Transform>(player).position + glm::dvec3(0, 0.5, 0);
        const glm::dvec3 d = cam - target;
        if (glm::length(d) > 0.05) {
            const auto hit =
                world.raycast(target, glm::vec3(d), glm::length(d),
                              midgard::physics::Touching::Solid, game.physics().body(player));
            CHECK_FALSE(hit.has_value());
        }
    }
    // The walls pulled it in: it's inside the room, not out where it would be in the open.
    CHECK(glm::length(glm::dvec2(camera_at(scene).x, camera_at(scene).z)) < 3.0);
}

TEST_CASE("cameras: on a planet every view stays level with the ground under the player") {
    TestPackage pkg(
        {
            {"e1_planet", R"(NULL, '{"collider": {"shape": "sphere", "radius": 1000}}')"},
            {"e2_player", R"(NULL, '{"transform": {"position": [0, 1000.5, 0]}, "player": {}}')"},
            {"e3_camera", R"(NULL, '{"transform": {"position": [0, 1003, 6]}, "camera": {}}')"},
        },
        R"({"body": {"shape": "sphere_outside", "radius": 1000}})");
    midgard::Game game(pkg.dir);
    midgard::Scene& scene = game.scene();
    midgard::Actions actions(midgard::InputMap::defaults());
    actions.set("keyboard", "W", 1.0f);
    for (const midgard::CameraView view :
         {midgard::CameraView::Third, midgard::CameraView::First, midgard::CameraView::Top}) {
        CAPTURE(midgard::view_name(view));
        game.camera_rig().set_view(scene, view, false);
        for (int i = 0; i < 240; ++i) {  // 16 m round the planet in each view
            game.update(actions, 1.0f / 60.0f);
            if (i % 40) continue;
            const glm::dvec3 cam = camera_at(scene);
            const glm::vec3 up(glm::normalize(cam));
            // Upright: no roll against the planet's up there, and never upside down.
            CHECK(std::abs(glm::dot(camera_axis(scene, {1, 0, 0}), up)) < 0.01f);
            CHECK(glm::dot(camera_axis(scene, {0, 1, 0}), up) > 0.0f);
            CHECK(glm::length(cam) > 1000.2);  // above the ground
        }
    }
}

TEST_CASE("cameras: first person hides the player, and the zoom passes between views") {
    TestPackage pkg(camera_scene());
    midgard::Game game(pkg.dir);
    midgard::Actions actions(midgard::InputMap::defaults());
    play_with(game, actions, 0.5);
    for (int i = 0; i < 40 && game.camera_rig().view() == midgard::CameraView::Third; ++i) {
        actions.begin_frame();
        actions.add_delta("mouse", "wheel", 2.0f);  // zooming in
        game.update(actions, 1.0f / 60.0f);
    }
    CHECK(game.camera_rig().view() == midgard::CameraView::First);
    actions.begin_frame();
    play_with(game, actions, 1.0);               // the blend done
    CHECK(game.camera_rig().hides_player());     // the player's model isn't drawn
    actions.add_delta("mouse", "wheel", -1.0f);  // zooming out again
    game.update(actions, 1.0f / 60.0f);
    CHECK(game.camera_rig().view() == midgard::CameraView::Third);
    CHECK(game.camera_rig().distance() == doctest::Approx(1.5f));
    CHECK_FALSE(game.camera_rig().hides_player());
}

TEST_CASE("cameras: scripted looks give the view back, and shakes fade out") {
    TestPackage pkg(camera_scene());
    midgard::Game game(pkg.dir);
    midgard::Scene& scene = game.scene();
    midgard::Actions actions(midgard::InputMap::defaults());
    play_with(game, actions, 0.5);
    const glm::dvec3 home = camera_at(scene);
    game.camera_rig().look({5, 2, 0}, {0, 1, 0}, 1.0f);
    play_with(game, actions, 0.6);  // blended there, holding
    CHECK(glm::length(camera_at(scene) - glm::dvec3(5, 2, 0)) < 1e-3);
    CHECK(glm::dot(camera_axis(scene, {0, 0, -1}), glm::normalize(glm::vec3(-5, -1, 0))) > 0.999f);
    play_with(game, actions, 1.5);  // the hold over, and the blend back
    CHECK_FALSE(game.camera_rig().scripted());
    CHECK(glm::length(camera_at(scene) - home) < 1e-3);

    // Released early, part-way through blending there: back without waiting out the hold.
    game.camera_rig().look({5, 2, 0}, {0, 1, 0}, 30.0f);
    play_with(game, actions, 0.2);
    game.camera_rig().release();
    CHECK(game.camera_rig().scripted());  // still blending back
    play_with(game, actions, 0.5);
    CHECK_FALSE(game.camera_rig().scripted());
    CHECK(glm::length(camera_at(scene) - home) < 1e-3);

    game.camera_rig().shake(0.3f, 0.5f);
    play_with(game, actions, 0.1);
    CHECK(glm::length(camera_at(scene) - home) > 1e-3);
    CHECK(glm::length(camera_at(scene) - home) < 0.43);  // (up to 0.3 m on two axes)
    play_with(game, actions, 0.5);
    CHECK(glm::length(camera_at(scene) - home) < 1e-6);
}

TEST_CASE("cameras: settings come from the camera component, and edits keep the view") {
    TestPackage pkg(camera_scene(R"({"fov": 50, "view": "top", "blend": 0.2,
        "views": {"third": {"distance": 9, "shoulder": 0.7, "max_distance": 12},
                  "top": {"min_pitch": 70, "max_pitch": 70, "distance": 20}}})"));
    midgard::Game game(pkg.dir);
    midgard::Scene& scene = game.scene();
    midgard::Actions actions(midgard::InputMap::defaults());
    const auto& lens = scene.registry.get<midgard::CameraLens>(scene.find("e3_camera"));
    CHECK(lens.rig.third.fov == 50.0f);
    CHECK(lens.rig.blend == doctest::Approx(0.2f));
    CHECK(lens.rig.third.shoulder == doctest::Approx(0.7f));
    CHECK(game.camera_rig().view() == midgard::CameraView::Top);  // starts top-down
    CHECK(game.camera_rig().distance() == doctest::Approx(9.0f));
    play_with(game, actions, 0.1);
    const glm::dvec3 above = camera_at(scene) - glm::dvec3(0, 0.5, 0);
    CHECK(above.y == doctest::Approx(20.0 * std::sin(glm::radians(70.0))).epsilon(0.01));

    // The Camera editor changes a setting while playing: the view stays top-down.
    game.set_component("e3_camera", "camera",
                       json::parse(R"({"views": {"top": {"min_pitch": 50, "max_pitch": 50}}})"));
    CHECK(game.camera_rig().view() == midgard::CameraView::Top);
    play_with(game, actions, 0.1);
    const glm::dvec3 lower = camera_at(scene) - glm::dvec3(0, 0.5, 0);
    CHECK(lower.y == doctest::Approx(18.0 * std::sin(glm::radians(50.0))).epsilon(0.01));
    // Over the shoulder: the third-person camera sits to the player's right.
    game.set_component("e3_camera", "camera",
                       json::parse(R"({"view": "third", "views": {"third": {"shoulder": 0.8}}})"));
    game.camera_rig().set_view(scene, midgard::CameraView::Third, false);
    play_with(game, actions, 0.1);
    CHECK(camera_at(scene).x == doctest::Approx(0.8).epsilon(0.01));
}

TEST_CASE("input: actions added later get their default controls in older projects") {
    TestPackage pkg(camera_scene());
    // A project made before "view" (migration 7) and "run" (migration 8) existed.
    sqlite3* db = nullptr;
    sqlite3_open(utf8(pkg.dir / "Game.db").c_str(), &db);
    exec(db, "DELETE FROM input_action WHERE id IN ('view', 'run')");
    exec(db, "DELETE FROM input_binding WHERE action IN ('view', 'run')");
    sqlite3_close(db);
    midgard::Actions actions(midgard::InputMap::load(pkg.dir / "Game.db"));
    actions.set("keyboard", "V", 1.0f);
    actions.set("keyboard", "Left Shift", 1.0f);
    CHECK(actions.pressed("view"));
    CHECK(actions.down("run"));
}

TEST_CASE("cameras: in top-down the mouse turns the view, and the keys walk relative to it") {
    TestPackage pkg(camera_scene(R"({"view": "top"})"));
    midgard::Game game(pkg.dir);
    midgard::Scene& scene = game.scene();
    midgard::Actions actions(midgard::InputMap::defaults());
    play_with(game, actions, 0.2);
    const float pitch_before = camera_axis(scene, {0, 0, -1}).y;
    actions.begin_frame();
    actions.add_delta("mouse", "delta_x", -600.0f);  // 90 degrees: the view turns left round
    actions.add_delta("mouse", "delta_y", 500.0f);   // ignored: the angle stays fixed
    game.update(actions, 1.0f / 60.0f);
    actions.begin_frame();
    CHECK(game.camera_rig().yaw() == doctest::Approx(90.0f));
    CHECK(camera_axis(scene, {0, 0, -1}).y == doctest::Approx(pitch_before).epsilon(1e-3));
    // W walks "up the screen": now along -X, and the player turns to face that way.
    const glm::dvec3 start =
        scene.registry.get<midgard::Transform>(scene.find("e2_player")).position;
    actions.set("keyboard", "W", 1.0f);
    play_with(game, actions, 0.5);
    const glm::dvec3 moved =
        scene.registry.get<midgard::Transform>(scene.find("e2_player")).position - start;
    CHECK(moved.x < -1.5);
    CHECK(std::abs(moved.z) < 0.1);
}

TEST_CASE("cameras: the player's profile keeps the view and zoom, and other keys") {
    TestPackage pkg(camera_scene());
    const fs::path file = pkg.dir / "profiles" / "default" / "profile.json";
    CHECK_FALSE(midgard::PlayerProfile::load(file).view);  // none yet: an empty profile

    midgard::PlayerProfile p;
    p.view = midgard::CameraView::Top;
    p.third_distance = 4.0f;
    p.top_distance = 25.0f;
    p.other = json{{"language", "en"}};
    p.save(file);  // (its folder is made)
    const midgard::PlayerProfile back = midgard::PlayerProfile::load(file);
    CHECK(back.view == midgard::CameraView::Top);
    CHECK(back.third_distance == doctest::Approx(4.0f));
    CHECK(back.top_distance == doctest::Approx(25.0f));
    CHECK(back.other == json{{"language", "en"}});
    CHECK_FALSE(fs::exists(file.string() + ".tmp"));

    // Values that make no sense are left out; a file that isn't a profile is an error.
    std::ofstream(file) << R"({"camera": {"view": "sideways", "third_distance": -3}})";
    const midgard::PlayerProfile odd = midgard::PlayerProfile::load(file);
    CHECK_FALSE(odd.view);
    CHECK_FALSE(odd.third_distance);
    std::ofstream(file) << "{not json";
    CHECK_THROWS_AS(midgard::PlayerProfile::load(file), midgard::ProfileError);
}

TEST_CASE("cameras: the game starts in the player's last view, and saves it") {
    TestPackage pkg(camera_scene());
    const fs::path file = pkg.dir / "profile.json";
    midgard::Actions actions(midgard::InputMap::defaults());
    {
        midgard::Game game(pkg.dir);
        game.use_profile(file);  // none yet: the camera's own settings
        CHECK(game.camera_rig().view() == midgard::CameraView::Third);
        CHECK_FALSE(fs::exists(file));
        game.camera_rig().set_orbit(game.scene(), 0.0f, 25.0f, 3.0f);
        press(game, actions, "V");  // third -> first: saved at once
        CHECK(midgard::PlayerProfile::load(file).view == midgard::CameraView::First);
        press(game, actions, "V");  // first -> top-down
        game.save_profile();        // (the game ending)
    }
    const midgard::PlayerProfile saved = midgard::PlayerProfile::load(file);
    CHECK(saved.view == midgard::CameraView::Top);
    CHECK(saved.third_distance == doctest::Approx(3.0f));

    // The next session starts there, at once (no blend from the designer's view).
    midgard::Game game(pkg.dir);
    game.use_profile(file);
    CHECK(game.camera_rig().view() == midgard::CameraView::Top);
    CHECK_FALSE(game.camera_rig().blending());
    CHECK(game.camera_rig().distance() == doctest::Approx(3.0f));
    play_with(game, actions, 0.1);
    CHECK(camera_at(game.scene()).y > 10.0);  // high above the player

    // A damaged profile is reported, the game plays on, and the next save replaces it.
    std::ofstream(file) << "{not json";
    midgard::Game fresh(pkg.dir);
    CHECK_THROWS_AS(fresh.use_profile(file), midgard::ProfileError);
    CHECK(fresh.camera_rig().view() == midgard::CameraView::Third);
    press(fresh, actions, "V");
    CHECK(midgard::PlayerProfile::load(file).view == midgard::CameraView::First);
}

namespace {

// A wall beside the exit check's route: a box turned to stand upright on the planet.
struct Wall {
    glm::dvec3 centre;
    glm::mat3 axes;  // columns: across, up, along (the box's local X, Y, Z)
    glm::vec3 half;
};

glm::vec3 local_to(const Wall& w, const glm::dvec3& p) {
    return glm::transpose(w.axes) * glm::vec3(p - w.centre);
}

bool inside(const Wall& w, const glm::dvec3& p, float margin) {
    const glm::vec3 l = glm::abs(local_to(w, p));
    return l.x < w.half.x + margin && l.y < w.half.y + margin && l.z < w.half.z + margin;
}

// Whether the segment from a to b passes through the wall (slabs in the wall's frame).
bool crosses(const Wall& w, const glm::dvec3& a, const glm::dvec3& b) {
    const glm::vec3 p = local_to(w, a), d = local_to(w, b) - p;
    float enter = 0.0f, leave = 1.0f;
    for (int i = 0; i < 3; ++i) {
        if (std::abs(d[i]) < 1e-9f) {
            if (std::abs(p[i]) > w.half[i]) return false;
            continue;
        }
        float t0 = (-w.half[i] - p[i]) / d[i], t1 = (w.half[i] - p[i]) / d[i];
        if (t0 > t1) std::swap(t0, t1);
        enter = std::max(enter, t0);
        leave = std::min(leave, t1);
        if (enter > leave) return false;
    }
    return true;
}

}  // namespace

TEST_CASE("cameras, the exit: round the planet in every view, never through the walls") {
    // A planet of the smallest size, its top at the origin; the player walks round it along -Z
    // (W), past eight walls 2 m to its right (+X), each 8 m long and 4 m tall.
    constexpr double kRadius = 1000.0, kPi = 3.14159265358979;
    const glm::dvec3 centre(0.0, -kRadius, 0.0);
    std::vector<std::pair<std::string, std::string>> entities = {
        {"e1_ground", R"(NULL, '{"mesh": {"asset": "builtin:surface"},
                        "collider": {"shape": "mesh"}}')"},
        {"e2_player", R"(NULL, '{"transform": {"position": [0, 0.5, 0]},
                        "player": {"speed": 20}}')"},
        {"e3_camera", R"(NULL, '{"transform": {"position": [0, 2, 6]}, "camera": {}}')"},
    };
    std::vector<Wall> walls;
    std::vector<double> stops;  // how far round (radians) each wall is
    for (int k = 0; k < 8; ++k) {
        const double angle = (k + 0.5) * 2.0 * kPi / 8.0;
        const glm::vec3 up(0.0f, float(std::cos(angle)), float(-std::sin(angle)));
        const glm::vec3 along(0.0f, float(-std::sin(angle)), float(-std::cos(angle)));
        const glm::vec3 across = glm::cross(along, up);  // +X
        Wall w{centre + glm::dvec3(up) * kRadius + glm::dvec3(across) * 2.0 + glm::dvec3(up) * 1.5,
               glm::mat3(across, up, -along), glm::vec3(0.2f, 2.0f, 4.0f)};
        const glm::vec3 turn = midgard::upright_rotation(along, up);
        const json components = {{"transform",
                                  {{"position", {w.centre.x, w.centre.y, w.centre.z}},
                                   {"rotation", {turn.x, turn.y, turn.z}}}},
                                 {"collider", {{"size", {0.4, 4.0, 8.0}}}}};
        entities.push_back({"w" + std::to_string(k), "NULL, '" + components.dump() + "'"});
        walls.push_back(w);
        stops.push_back(angle);
    }
    TestPackage pkg(entities,
                    R"({"body": {"shape": "sphere_outside", "radius": 1000,
                        "origin": [0, -1000, 0]}})");
    midgard::Game game(pkg.dir);
    midgard::Scene& scene = game.scene();
    const midgard::Body& body = *scene.settings.body;
    const entt::entity player = scene.find("e2_player");
    midgard::CameraRig& rig = game.camera_rig();
    midgard::Actions actions(midgard::InputMap::defaults());

    std::set<midgard::CameraView> seen;
    int frames = 0, blocked = 0;
    double closest = 1e9;  // the third-person camera's nearest to what it looks at
    // One frame, then the checks: the camera is never in a wall, nothing solid comes between
    // it and what it looks at, it stays above the ground, and level with it.
    const auto step = [&] {
        game.update(actions, 1.0f / 60.0f);
        actions.begin_frame();
        ++frames;
        seen.insert(rig.view());
        const glm::dvec3 cam = camera_at(scene);
        const glm::dvec3 at = scene.world_position(player);
        const glm::vec3 up(glm::normalize(at - centre));
        CHECK(body.surface(cam).height > 0.1);
        for (const Wall& w : walls) {
            CHECK_FALSE(inside(w, cam, 0.1f));  // (the camera keeps 0.25 m from walls)
            if (rig.blending() || rig.view() == midgard::CameraView::First) continue;
            const float height = rig.view() == midgard::CameraView::Third ? 0.5f : 0.0f;
            const glm::dvec3 target = at + glm::dvec3(up * height);
            if (crosses(w, target, cam)) ++blocked;
            if (rig.view() == midgard::CameraView::Third)
                closest = std::min(closest, glm::length(cam - target));
        }
        if (!rig.blending()) {
            const glm::vec3 cam_up(glm::normalize(cam - centre));
            CHECK(std::abs(glm::dot(camera_axis(scene, {1, 0, 0}), cam_up)) < 0.02f);
            CHECK(glm::dot(camera_axis(scene, {0, 1, 0}), cam_up) > 0.0f);
        }
    };
    const auto relative = [&] { return glm::normalize(scene.world_position(player) - centre); };

    glm::dvec3 last = relative();
    double turned = 0.0;
    size_t next = 0;
    int swept = 0;
    while (turned < 2.0 * kPi && frames < 40000) {
        if (next < stops.size() && turned >= stops[next]) {
            // Beside a wall: stop, and in each view (V steps through them) swing the camera
            // all the way round the player, through where the wall is.
            ++next;
            actions.set("keyboard", "W", 0.0f);
            for (int v = 0; v < 3; ++v) {
                for (int i = 0; i < 30; ++i) step();  // the blend into the view
                for (int i = 0; i < 120; ++i) {       // 360 degrees in 2 s
                    actions.add_delta("mouse", "delta_x", 20.0f);
                    step();
                }
                actions.set("keyboard", "V", 1.0f);
                step();
                actions.set("keyboard", "V", 0.0f);
                ++swept;
            }
        }
        actions.set("keyboard", "W", 1.0f);
        step();
        const glm::dvec3 now = relative();
        turned += std::acos(std::clamp(glm::dot(last, now), -1.0, 1.0));
        last = now;
    }
    CHECK(swept == 24);  // every wall, in every view
    CHECK(seen.size() == 3);
    CHECK(blocked == 0);
    CHECK(closest < 2.5);  // the walls did pull the camera in (from 6 m)
    CHECK(glm::length(scene.world_position(player) - glm::dvec3(0, 0.5, 0)) < 50.0);
    MESSAGE("round the planet in ", frames, " frames");
}

TEST_CASE("the player runs at its speed times run_multiplier while the run action is held") {
    struct Case {
        const char* player;
        double walk, run;  // metres in one second, holding W, then W and Left Shift
    };
    for (const Case& c : {Case{"{}", 4.0, 8.0},  // twice as fast running, by default
                          Case{R"({"speed": 3, "run_multiplier": 3.5})", 3.0, 10.5},
                          Case{R"({"run_multiplier": 9})", 4.0, 20.0},      // at most 5 times
                          Case{R"({"run_multiplier": 0.5})", 4.0, 4.0}}) {  // at least 1
        CAPTURE(std::string(c.player));
        TestPackage pkg({
            {"e1_ground", R"(NULL, '{"transform": {"position": [0, -0.5, 0]},
                            "collider": {"size": [200, 1, 200]}}')"},
            {"e2_player", std::string(R"(NULL, '{"transform": {"position": [0, 0.5, 0]},
                            "player": )") +
                              c.player + "}'"},
            {"e3_camera", R"(NULL, '{"transform": {"position": [0, 2, 6]}, "camera": {}}')"},
        });
        midgard::Game game(pkg.dir);
        midgard::Scene& scene = game.scene();
        const entt::entity player = scene.find("e2_player");
        midgard::Actions actions(midgard::InputMap::defaults());
        play_with(game, actions, 0.5);  // settled on the ground
        actions.set("keyboard", "W", 1.0f);
        play_with(game, actions, 0.5);  // up to walking speed
        glm::dvec3 start = scene.world_position(player);
        play_with(game, actions, 1.0);
        const double walked = glm::length(scene.world_position(player) - start);
        CHECK(walked == doctest::Approx(c.walk).epsilon(0.03));
        actions.set("keyboard", "Left Shift", 1.0f);
        play_with(game, actions, 0.5);
        start = scene.world_position(player);
        play_with(game, actions, 1.0);
        CHECK(glm::length(scene.world_position(player) - start) ==
              doctest::Approx(c.run).epsilon(0.03));
    }
}

TEST_CASE("time: the game's clock starts at the standard calendar's start, at its pace") {
    TestPackage pkg(camera_scene());
    midgard::Game game(pkg.dir);
    const midgard::Calendar& c = game.standard_calendar();
    CHECK(c.id == "cal_standard");
    const midgard::Date start = midgard::date_at(c, game.time().now());
    CHECK(start.year == 1);
    CHECK(start.month == 4);
    CHECK(start.day == 1);
    CHECK(start.hour == 6);
    CHECK(game.time().scale() == doctest::Approx(30.0));  // a day in 48 real minutes
    CHECK(game.text("cal_standard.month.3") == "April");
    CHECK(game.text("no.such.key") == "no.such.key");

    midgard::Actions actions(midgard::InputMap::defaults());
    int hours = 0;
    for (int i = 0; i < 60 * 121; ++i) {  // 121 real seconds: an hour and a minute of game time
        game.update(actions, 1.0f / 60.0f);
        for (const midgard::TimeEvent& e : game.time_events())
            hours += e.signal == midgard::TimeSignal::Hour ? 1 : 0;
    }
    CHECK(hours == 1);  // 07:00
    CHECK(midgard::date_at(c, game.time().now()).minute == 0);
    CHECK(midgard::date_at(c, game.time().now()).hour == 7);

    // A skip's signals come with the next frame.
    game.advance_time(c.seconds_per_day(), midgard::SkipPolicy::All);
    CHECK(midgard::date_at(c, game.time().now()).day == 2);
    game.update(actions, 1.0f / 60.0f);
    int days = 0;
    for (const midgard::TimeEvent& e : game.time_events())
        days += e.signal == midgard::TimeSignal::Day ? 1 : 0;
    CHECK(days == 1);
    game.update(actions, 1.0f / 60.0f);
    CHECK(game.time_events().empty());  // delivered once
}

TEST_CASE("time: a world keeps its clock, and loading restores it to the second") {
    TestPackage pkg(camera_scene());
    const fs::path world = pkg.dir / "worlds" / "w1";
    std::int64_t saved = 0;
    std::string id;
    {
        midgard::Game game(pkg.dir);
        game.use_world(world);  // new: the calendar's start
        REQUIRE(game.world() != nullptr);
        id = game.world()->info().id;
        CHECK(id.size() == 32);
        CHECK(game.world()->info().game == "Game");
        CHECK_FALSE(game.world()->clock());
        CHECK(midgard::date_at(game.standard_calendar(), game.time().now()).hour == 6);
        game.advance_time(1'234'567, midgard::SkipPolicy::None);
        game.time().set_scale(0.0);  // a script froze time
        saved = game.time().now();
        game.save_world();
    }
    CHECK(fs::exists(world / "world.json"));
    CHECK(fs::exists(world / "world.db"));
    midgard::Game again(pkg.dir);
    again.use_world(world);
    CHECK(again.world()->info().id == id);  // the same world
    CHECK(again.time().now() == saved);
    CHECK(again.time().scale() == 0.0);

    std::ofstream(world / "world.json") << "{not json";
    midgard::Game damaged(pkg.dir);
    CHECK_THROWS_AS(damaged.use_world(world), midgard::WorldError);
    CHECK(damaged.world() == nullptr);
}

TEST_CASE("time: a package from before calendars gets a plain one") {
    TestPackage pkg(camera_scene());
    sqlite3* db = nullptr;
    sqlite3_open(utf8(pkg.dir / "Game.db").c_str(), &db);
    exec(db, "DELETE FROM calendar");
    sqlite3_close(db);
    midgard::Game game(pkg.dir);
    CHECK(game.standard_calendar().id == "builtin");
    CHECK(game.standard_calendar().problems().empty());
    midgard::Actions actions(midgard::InputMap::defaults());
    game.update(actions, 1.0f);
    CHECK(game.time().now() > 0);
}
