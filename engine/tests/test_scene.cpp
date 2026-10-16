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
