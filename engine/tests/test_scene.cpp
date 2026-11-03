// SPDX-License-Identifier: Apache-2.0
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <numeric>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <tuple>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
// (after windows.h)
#include <psapi.h>
#undef near  // (windows.h's old macros)
#undef far
#elif defined(__linux__)
#include <unistd.h>
#endif

#include <miniz.h>
#include <sqlite3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include "midgard/audio.h"
#include "midgard/body.h"
#include "midgard/collider_outlines.h"
#include "midgard/footsteps.h"
#include "midgard/game.h"
#include "midgard/gltf.h"
#include "midgard/light_markers.h"
#include "midgard/pak.h"
#include "midgard/scene.h"
#include "midgard/validate.h"
#include "midgard/world_light.h"
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
// Content a package carries besides the cube: its id, its path in game.pak, its bytes.
struct PackageFile {
    std::string id, path;
    std::vector<std::uint8_t> bytes;
};

struct TestPackage {
    fs::path dir;

    explicit TestPackage(const std::vector<std::pair<std::string, std::string>>& entities,
                         const std::string& settings = R"({"sky": [0.1, 0.2, 0.3]})",
                         const std::vector<std::pair<std::string, std::string>>& objects = {},
                         const std::vector<PackageFile>& files = {}) {
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
        json content = json::array(
            {{{"id", "mesh_cube"}, {"path", "meshes/cube.glb"}, {"sha256", std::string(64, '0')}}});
        for (const PackageFile& f : files) {
            REQUIRE(mz_zip_writer_add_mem(&zip, f.path.c_str(), f.bytes.data(), f.bytes.size(),
                                          MZ_DEFAULT_LEVEL));
            content.push_back({{"id", f.id}, {"path", f.path}, {"sha256", std::string(64, '0')}});
        }
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
            {"content", content},
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
        {"e1_cube",
         R"(NULL, '{"transform": {"position": [1, 0, 0]}, "mesh": {"asset": "mesh_cube"},
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

TEST_CASE("a project starts with ten weather types, which the scene loads") {
    TestPackage pkg({});
    {
        sqlite3* db = nullptr;
        sqlite3_open(utf8(pkg.dir / "Game.db").c_str(), &db);
        exec(db, R"(INSERT INTO weather_type VALUES ('acid', 'Acid rain',
                    '{"falls": "rain", "amount": 0.3, "tint": [0.8, 1, 0.7]}'))");
        sqlite3_close(db);
    }
    midgard::Game game(pkg.dir);
    const auto& types = game.scene().weather_types;
    CHECK(types.size() == 11);
    for (const char* id : {"clear", "cloudy", "rain", "thunderstorm", "snow", "blizzard", "fog",
                           "wind", "sandstorm", "ash_fall", "acid"})
        CHECK_MESSAGE(types.count(id) == 1, id);
    const auto& blizzard = types.at("blizzard");
    CHECK(blizzard.name == "Blizzard");
    CHECK(blizzard.falls == midgard::weather::Falls::snow);
    CHECK(blizzard.visibility == 120.0f);
    CHECK(blizzard.temperature == -10.0f);
    CHECK(types.at("thunderstorm").lightning > 0.0f);
    CHECK(types.at("sandstorm").falls == midgard::weather::Falls::sand);
    CHECK(types.at("ash_fall").falls == midgard::weather::Falls::ash);
    CHECK(types.at("clear").darkness == 0.0f);
    CHECK(types.at("acid").amount == doctest::Approx(0.3));
}

TEST_CASE("a weather type that isn't one is reported as the scene loads") {
    TestPackage pkg({});
    {
        sqlite3* db = nullptr;
        sqlite3_open(utf8(pkg.dir / "Game.db").c_str(), &db);
        exec(db, R"(INSERT INTO weather_type VALUES ('frogs', 'Frogs', '{"falls": "frogs"}'))");
        sqlite3_close(db);
    }
    CHECK_THROWS_WITH_AS(midgard::Game(pkg.dir), doctest::Contains("Frogs"), midgard::SceneError);
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

TEST_CASE("time: a sun that follows the clock rises, peaks where it was placed, and sets") {
    // Placed shining down at 60 degrees towards -Z: its noon.
    auto entities = camera_scene();
    entities.push_back({"e4_sun", R"(NULL, '{"transform": {"rotation": [-60, 0, 0]},
                        "light": {"type": "directional", "intensity": 2, "follows_clock": true}}')"});
    entities.push_back({"e5_lamp", R"(NULL, '{"transform": {"rotation": [-60, 0, 0]},
                        "light": {"type": "directional"}}')"});  // (the second: not the sun)
    TestPackage pkg(entities);
    midgard::Game game(pkg.dir);
    midgard::Scene& scene = game.scene();
    const glm::vec3 up(0, 1, 0);

    scene.solar_phase.reset();  // as placed
    const midgard::render::Light placed = scene.light({0, 0, 0});
    CHECK(placed.intensity == doctest::Approx(2.0f));

    scene.solar_phase = 0.25;  // noon: where it was placed, at full strength
    midgard::render::Light l = scene.light({0, 0, 0});
    CHECK(glm::length(l.direction - placed.direction) < 1e-5f);
    CHECK(l.intensity == doctest::Approx(2.0f));
    CHECK(l.ambient.x == doctest::Approx(placed.ambient.x));

    scene.solar_phase = 0.0;  // sunrise: level with the horizon, half way through twilight
    l = scene.light({0, 0, 0});
    CHECK(std::abs(glm::dot(l.direction, up)) < 1e-5f);
    CHECK(l.intensity == doctest::Approx(1.0f));
    const glm::vec3 rising = -l.direction;

    scene.solar_phase = 0.5;  // sunset: on the other side
    l = scene.light({0, 0, 0});
    CHECK(glm::dot(-l.direction, rising) == doctest::Approx(-1.0f));

    scene.solar_phase = 0.75;  // midnight: no sun, dim ambient light
    l = scene.light({0, 0, 0});
    CHECK(l.intensity == 0.0f);
    CHECK(l.ambient.x == doctest::Approx(placed.ambient.x * 0.25f));

    scene.solar_phase = 0.1;  // morning: up, and climbing from the side it rose on
    l = scene.light({0, 0, 0});
    CHECK(glm::dot(-l.direction, up) > 0.2f);
    CHECK(glm::dot(-l.direction, rising) > 0.5f);

    // The game's clock moves it: the standard calendar starts at 06:00, before sunrise.
    game.set_time(game.time().now());
    REQUIRE(scene.solar_phase.has_value());
    CHECK(*scene.solar_phase > 0.9);
    CHECK(scene.light({0, 0, 0}).intensity < 0.5f);
    const midgard::Calendar& c = game.standard_calendar();
    game.set_time(midgard::time_of(c, 1, 4, 1, 12 * 3600));  // noon
    CHECK(*scene.solar_phase == doctest::Approx(0.25));
    CHECK(scene.light({0, 0, 0}).intensity == doctest::Approx(2.0f));
}

TEST_CASE("a sun that follows the clock is one distant sun: dark on a planet's far side") {
    TestPackage pkg(
        {
            {"e1_sun", R"(NULL, '{"transform": {"rotation": [-50, 35, 0]},
                          "light": {"type": "directional", "follows_clock": true}}')"},
        },
        R"({"body": {"shape": "sphere_outside", "radius": 1000, "origin": [0, -1000, 0]}})");
    midgard::Game game(pkg.dir);
    midgard::Scene& scene = game.scene();
    scene.solar_phase = 0.25;  // noon at the scene's start, the top of the planet
    const glm::dvec3 centre(0, -1000, 0);
    float last = -1.0f, worst_jump = 0.0f;
    // Walking over to the far side, about 9 m (half a degree) at a time.
    for (int step = 0; step <= 360; ++step) {
        const double degrees = step * 0.5;
        const double a = glm::radians(degrees);
        const glm::dvec3 at = centre + glm::dvec3(std::sin(a), std::cos(a), 0.0) * 1001.0;
        const midgard::render::Light light = scene.light(at);
        CAPTURE(degrees);
        if (last >= 0.0f) worst_jump = std::max(worst_jump, std::abs(light.intensity - last));
        last = light.intensity;
        if (step == 0) CHECK(light.intensity == doctest::Approx(1.0f));  // noon here
        if (degrees >= 140) {  // night: no sun, and the ambient light down to a quarter
            CHECK(light.intensity == 0.0f);
            CHECK(light.ambient.r == doctest::Approx(scene.settings.ambient.r * 0.25f));
        }
    }
    CHECK(worst_jump < 0.1f);  // it gets darker smoothly: never a sudden change
}

TEST_CASE("a world keeps where the player was, and loading puts them back there") {
    TestPackage pkg(camera_scene());
    const fs::path world = pkg.dir / "worlds" / "w1";
    glm::dvec3 left_at;
    glm::vec3 facing;
    {
        midgard::Game game(pkg.dir);
        game.use_world(world);
        CHECK_FALSE(game.world()->place());  // a new world: the scene's start
        midgard::Actions actions(midgard::InputMap::defaults());
        actions.set("keyboard", "D", 1.0f);  // walk right for a second
        play_with(game, actions, 1.0);
        const auto& t =
            game.scene().registry.get<midgard::Transform>(game.scene().find("e2_player"));
        left_at = t.position;
        facing = t.rotation;
        CHECK(left_at.x > 2.0);
        game.save_world();
    }
    midgard::Game again(pkg.dir);
    again.use_world(world);
    midgard::Scene& scene = again.scene();
    const entt::entity player = scene.find("e2_player");
    CHECK(glm::length(scene.registry.get<midgard::Transform>(player).position - left_at) < 1e-9);
    CHECK(glm::length(scene.registry.get<midgard::Transform>(player).rotation - facing) < 1e-4f);
    // The character is there too once the game runs, standing (not falling from the start).
    midgard::Actions idle(midgard::InputMap::defaults());
    play_with(again, idle, 0.5);
    CHECK(glm::length(again.physics().character()->position() - left_at) < 0.05);
    // And the camera looks at the player there, not at the scene's start.
    CHECK(glm::length(camera_at(scene) - left_at) < 10.0);
    CHECK(camera_at(scene).x > 2.0);
}

TEST_CASE("environments: the Sky links to one, and the sky, sunlight and fog follow the clock") {
    TestPackage pkg({
        {"e1_sun", R"(NULL, '{"transform": {"rotation": [-60, 30, 0]},
                      "light": {"type": "directional"}}')"},
        {"e2_sky", R"(NULL, '{"sky": {"environment": "env_earth"}}')"},
    });
    midgard::Game game(pkg.dir);
    midgard::Scene& scene = game.scene();
    // Every project has the Earth-like environment (migration 10), and an empty one is a full day.
    REQUIRE(scene.environments.contains("env_earth"));
    REQUIRE(scene.active_environment() != nullptr);
    const glm::dvec3 at(0.0);
    const auto glow = [](const midgard::render::Environment& e) {
        return (e.sky_colors.zenith.r + e.sky_colors.zenith.g + e.sky_colors.zenith.b) *
               e.sky_colors.brightness;
    };
    scene.solar_phase = 0.25;  // noon: the sun (following the clock with an environment) is up
    const midgard::render::Environment noon = scene.environment(at);
    CHECK(noon.sky);
    CHECK(scene.light(at).intensity > 0.5f);
    scene.solar_phase = 0.75;  // midnight: no sunlight, a dark sky
    const midgard::render::Environment midnight = scene.environment(at);
    CHECK(midnight.sky);
    CHECK(scene.light(at).intensity < 0.01f);
    CHECK(glow(midnight) < glow(noon) * 0.2f);

    // Without a clock, or without a link, the sky is flat as before, with the sun a small white
    // disc in it.
    const auto flat = [](const midgard::render::Environment& e) {
        const midgard::render::SkyColors& s = e.sky_colors;
        return e.sky && s.zenith == s.horizon && s.horizon == s.horizon_away &&
               s.horizon_away == s.ground && s.glow_brightness == 0.0f &&
               s.sun == glm::vec3(1.0f) && s.sun_size == 1.5f;
    };
    scene.solar_phase.reset();
    CHECK(flat(scene.environment(at)));
    scene.solar_phase = 0.25;
    game.set_component("e2_sky", "sky", json::parse(R"({"color": [0.1, 0.2, 0.3]})"));
    CHECK(flat(scene.environment(at)));
    CHECK(scene.environment(at).sky_colors.zenith == glm::vec3(0.1f, 0.2f, 0.3f));
    // No sun in the scene: no disc, and no sky pass at all.
    game.remove_entity("e1_sun");
    CHECK_FALSE(scene.environment(at).sky);
}

TEST_CASE("environments: validation names a missing link and refuses a broken curve") {
    TestPackage missing({{"e1_sky", R"(NULL, '{"sky": {"environment": "env_moon"}}')"}});
    std::string all;
    for (const std::string& w : midgard::validate_package(missing.dir).warnings) all += w + "\n";
    CHECK(all.find("env_moon") != std::string::npos);

    TestPackage broken({{"e1_sky", R"(NULL, '{"sky": {"environment": "env_earth"}}')"}});
    sqlite3* db = nullptr;
    sqlite3_open(utf8(broken.dir / "Game.db").c_str(), &db);
    exec(db, R"(UPDATE environment SET curves = '{"fog_density": [[0.7, 0.01], [0.2, 0.02]]}')");
    sqlite3_close(db);
    const midgard::ValidationReport report = midgard::validate_package(broken.dir);
    CHECK_FALSE(report.errors.empty());
    CHECK(report.errors.front().find("out of order") != std::string::npos);
}

TEST_CASE("sun_mode in the scene settings picks a planet's light model") {
    TestPackage latitude({{"e1_sun", R"(NULL, '{"transform": {"rotation": [-50, 35, 0]},
                                          "light": {"type": "directional",
                                                    "follows_clock": true}}')"}},
                         R"({"body": {"shape": "sphere_outside", "radius": 1000,
                                      "origin": [0, -1000, 0]}, "sun_mode": "latitude"})");
    midgard::Game game(latitude.dir);
    CHECK(game.scene().settings.sun_mode == midgard::SunMode::Latitude);
    game.scene().solar_phase = 0.25;
    REQUIRE(game.scene().local_sun(glm::dvec3(0.0)).has_value());
    TestPackage wrong({}, R"({"sun_mode": "sideways"})");
    CHECK_THROWS_WITH_AS(midgard::Game(wrong.dir), doctest::Contains("sun_mode"),
                         midgard::SceneError);
}

TEST_CASE("terrain in the scene settings, and the cook's checks of it") {
    TestPackage pkg({}, R"({"body": {"shape": "bounded_area", "width": 3000, "length": 2000,
                                     "seed": 7},
                            "terrain": {"height_scale": 90, "wall": "fortification"}})");
    midgard::Game game(pkg.dir);
    REQUIRE(game.scene().settings.terrain.has_value());
    CHECK(game.scene().settings.terrain->height_scale == 90.0);
    CHECK(game.scene().settings.terrain->wall == midgard::WallLook::Fortification);
    CHECK(midgard::validate_package(pkg.dir).errors.empty());

    TestPackage bad({}, R"({"body": {"shape": "bounded_area", "width": 3000, "length": 2000},
                            "terrain": {"octaves": 40}})");
    const midgard::ValidationReport report = midgard::validate_package(bad.dir);
    REQUIRE_FALSE(report.errors.empty());
    CHECK(report.errors.front().find("octaves") != std::string::npos);

    TestPackage no_body({}, R"({"terrain": {}})");
    std::string all;
    for (const std::string& w : midgard::validate_package(no_body.dir).warnings) all += w + "\n";
    CHECK(all.find("no world shape") != std::string::npos);

    // The game streams it, and its camera sees as far as it is drawn.
    REQUIRE(game.terrain() != nullptr);
    CHECK(game.terrain()->terrain().roots_u() == 3);
    const glm::mat4 p = game.camera(1.0f).projection;
    const float far_plane = p[3][2] / p[2][2];  // (reversed depth: the far plane at 0)
    CHECK(far_plane >= midgard::kTerrainViewDistance * 0.99f);
    TestPackage none({});
    CHECK(midgard::Game(none.dir).terrain() == nullptr);

    TestPackage unreadable({}, R"({"terrain": {"wall": "hedge"}})");
    CHECK_THROWS_WITH_AS(midgard::Game(unreadable.dir), doctest::Contains("wall"),
                         midgard::SceneError);
}

TEST_CASE("terrain is solid: the player starting under it is lifted, stands and walks on it") {
    // The player in a folder (as the editor's World folder keeps it).
    TestPackage pkg({{"e0_world", R"(NULL, '{"folder": {}}')"},
                     {"e1_player", R"('e0_world', '{"transform": {"position": [40, -50, -30]},
                                         "player": {"speed": 6}}')"},
                     {"e2_camera", R"(NULL, '{"transform": {"position": [40, 2, -24]},
                                         "camera": {}}')"}},
                    R"({"body": {"shape": "bounded_area", "width": 3000, "length": 3000,
                                 "seed": 7},
                        "terrain": {"height_scale": 40}})");
    midgard::Game game(pkg.dir);
    midgard::Scene& scene = game.scene();
    const entt::entity player = scene.find("e1_player");
    REQUIRE(game.ground() != nullptr);
    const midgard::Terrain& t = game.terrain()->terrain();
    // Lifted onto the ground (it was 50 m under it): standing, its middle half its height up.
    CHECK(t.above_ground(scene.world_position(player)) == doctest::Approx(0.55).epsilon(0.01));
    midgard::Actions actions(midgard::InputMap::defaults());
    for (int i = 0; i < 120; ++i) game.update(actions, 1.0f / 60.0f);  // 2 s: it settles
    const double settled = t.above_ground(scene.world_position(player));
    CHECK(settled > -0.05);  // not through the ground
    CHECK(settled < 1.5);    // standing on it (the capsule's middle above the surface)
    // Walking 10 s over the hills: always on the ground, never through it or floating off.
    actions.set("keyboard", "W", 1.0f);
    const glm::dvec3 start = scene.world_position(player);
    for (int i = 0; i < 600; ++i) {
        game.update(actions, 1.0f / 60.0f);
        actions.begin_frame();
        const double above = t.above_ground(scene.world_position(player));
        CHECK(above > -0.1);
        CHECK(above < 3.0);
    }
    CHECK(glm::length(scene.world_position(player) - start) > 30.0);  // it went somewhere
    CHECK(game.ground()->bodies() > 0);
    // Put under the ground mid-game (a script, say): it is put back on top.
    actions.set("keyboard", "W", 0.0f);
    scene.registry.get<midgard::Transform>(player).position.y -= 20.0;
    for (int i = 0; i < 60; ++i) game.update(actions, 1.0f / 60.0f);
    CHECK(t.above_ground(scene.world_position(player)) > -0.05);
    CHECK(t.above_ground(scene.world_position(player)) < 1.5);
}

TEST_CASE("terrain stamps: from the database, followed live, solid, and checked by the cook") {
    TestPackage pkg({}, R"({"body": {"shape": "bounded_area", "width": 3000, "length": 3000,
                                     "seed": 7},
                            "terrain": {}})");
    sqlite3* db = nullptr;
    sqlite3_open(utf8(pkg.dir / "Game.db").c_str(), &db);
    // A 30 m mound in the middle of the map.
    exec(db, R"(INSERT INTO terrain_stamp (scene, stamp) VALUES ('scn_main',
        '{"tool": "raise", "radius": 40, "strength": 30, "points": [[0.5, 0.5]]}'))");
    sqlite3_close(db);
    CHECK(midgard::validate_package(pkg.dir).errors.empty());
    midgard::Game game(pkg.dir);
    REQUIRE(game.scene().terrain_stamps.size() == 1);
    const midgard::Terrain& t = game.terrain()->terrain();
    const double mound = t.height(0, 0.5, 0.5);
    CHECK(mound == doctest::Approx(t.base_height(0, 0.5, 0.5) + 30.0));
    // The solid ground has the mound (the ground is built round the player, or the origin).
    const auto top = game.physics().world().raycast({0.0, 200.0, 0.0}, {0, -1, 0}, 400.0);
    REQUIRE(top.has_value());
    CHECK(top->point.y == doctest::Approx(mound).epsilon(0.01));
    // The editor takes it away (the live link): drawn and solid ground follow.
    game.set_terrain_stamps({});
    const double flat = game.terrain()->terrain().height(0, 0.5, 0.5);
    CHECK(flat == doctest::Approx(mound - 30.0));
    const auto ground = game.physics().world().raycast({0.0, 200.0, 0.0}, {0, -1, 0}, 400.0);
    REQUIRE(ground.has_value());
    CHECK(ground->point.y == doctest::Approx(flat).epsilon(0.01));

    // A stamp the cook refuses.
    sqlite3_open(utf8(pkg.dir / "Game.db").c_str(), &db);
    exec(db, R"(INSERT INTO terrain_stamp (scene, stamp) VALUES ('scn_main',
        '{"tool": "smooth", "radius": 10, "strength": 3, "points": [[0.5, 0.5]]}'))");
    sqlite3_close(db);
    const midgard::ValidationReport report = midgard::validate_package(pkg.dir);
    REQUIRE_FALSE(report.errors.empty());
    CHECK(report.errors.front().find("strength") != std::string::npos);
}

TEST_CASE("terrain, the exit: across a bounded area, never through the ground") {
    // 1.4 km over hills from one side of the land to the other (the player walks -Z, W held).
    TestPackage pkg({{"e1_player", R"(NULL, '{"transform": {"position": [-200, 0.5, 700]},
                                         "player": {"speed": 25}}')"},
                     {"e2_camera", R"(NULL, '{"transform": {"position": [-200, 40, 706]},
                                         "camera": {}}')"}},
                    R"({"body": {"shape": "bounded_area", "width": 3000, "length": 3000,
                                 "seed": 7},
                        "terrain": {"height_scale": 25}})");
    midgard::Game game(pkg.dir);
    midgard::Scene& scene = game.scene();
    const entt::entity player = scene.find("e1_player");
    const midgard::Terrain& t = game.terrain()->terrain();
    midgard::Actions actions(midgard::InputMap::defaults());
    for (int i = 0; i < 60; ++i) game.update(actions, 1.0f / 60.0f);  // settle
    actions.set("keyboard", "W", 1.0f);
    const glm::dvec3 start = scene.world_position(player);
    int frames = 0, off_ground = 0;
    double lowest = 1e9;
    while (start.z - scene.world_position(player).z < 1400.0 && frames < 8000) {
        game.update(actions, 1.0f / 60.0f);
        actions.begin_frame();
        ++frames;
        const double above = t.above_ground(scene.world_position(player));
        lowest = std::min(lowest, above);
        if (above > 3.0) ++off_ground;  // (a moment in the air over a crest is fine)
    }
    MESSAGE("across the bounded area in ", frames, " frames");
    CHECK(start.z - scene.world_position(player).z >= 1400.0);
    CHECK(lowest > -0.1);  // never through the ground
    CHECK(off_ground < frames / 50);
}

TEST_CASE("terrain, the exit: all the way round a habitat, with spin gravity, no seams") {
    // Inside a curved cylinder (radius 1 km): the player walks round the axis (+X at the
    // bottom, W held), across the join where u = 0 meets u = 1, back to where it started. From
    // three starting places: the paths from x = 3 and x = -5 used to come to a stop where the
    // ground's contacts cancelled a step (physics::Character::step now takes it again).
    constexpr double kRadius = 1000.0, kPi = 3.14159265358979;
    for (const double start : {0.0, 3.0, -5.0}) {
        CAPTURE(start);
        const std::string player_row = "NULL, '{\"transform\": {\"position\": [" +
                                       std::to_string(start) +
                                       ", 0.5, 0]}, \"player\": {\"speed\": 25}}'";
        const std::string camera_row = "NULL, '{\"transform\": {\"position\": [" +
                                       std::to_string(start - 6.0) + ", 40, 0]}, \"camera\": {}}'";
        TestPackage pkg({{"e1_player", player_row}, {"e2_camera", camera_row}},
                        R"({"body": {"shape": "curved_cylinder", "radius": 1000, "length": 5000,
                                 "seed": 9, "origin": [0, 1000, 0]},
                        "terrain": {"height_scale": 20}})");
        midgard::Game game(pkg.dir);
        midgard::Scene& scene = game.scene();
        const entt::entity player = scene.find("e1_player");
        const midgard::Terrain& t = game.terrain()->terrain();
        const glm::dvec3 axis_point(0.0, kRadius, 0.0);
        midgard::Actions actions(midgard::InputMap::defaults());
        for (int i = 0; i < 60; ++i) game.update(actions, 1.0f / 60.0f);  // settle
        actions.set("keyboard", "W", 1.0f);
        const auto angle = [&] {
            const glm::dvec3 p = scene.world_position(player) - axis_point;
            return std::atan2(p.x, -p.y);  // 0 at the bottom, increasing towards +X
        };
        double turned = 0.0, last = angle(), lowest = 1e9, worst_gravity = 0.0;
        int frames = 0, crossings = 0;
        while (std::abs(turned) < 2.0 * kPi && frames < 25000) {
            game.update(actions, 1.0f / 60.0f);
            actions.begin_frame();
            ++frames;
            const double now = angle();
            double d = now - last;
            if (d > kPi) d -= 2.0 * kPi;
            if (d < -kPi) d += 2.0 * kPi;
            if (std::abs(now - last) > kPi) ++crossings;  // across the join (u = 0 and 1)
            turned += d;
            last = now;
            const glm::dvec3 p = scene.world_position(player);
            lowest = std::min(lowest, t.above_ground(p));
            // Spin gravity: g r / R, away from the axis.
            const glm::dvec3 out(p.x - axis_point.x, p.y - axis_point.y, 0.0);
            const double r = glm::length(out);
            const glm::dvec3 want = out / r * (9.81 * r / kRadius);
            worst_gravity =
                std::max(worst_gravity, glm::length(game.physics().world().gravity(p) - want));
        }
        MESSAGE("round the habitat in ", frames, " frames");
        CHECK(std::abs(turned) >= 2.0 * kPi);  // all the way round
        CHECK(crossings >= 1);                 // over the join
        CHECK(lowest > -0.1);                  // never through the ground, seams included
        CHECK(worst_gravity < 1e-6);
    }
}

TEST_CASE("above a planet the sky's horizon dips to where the ground's edge is") {
    TestPackage pkg({{"e1_sun", R"(NULL, '{"transform": {"rotation": [-50, 35, 0]},
                                      "light": {"type": "directional"}}')"}},
                    R"({"body": {"shape": "sphere_outside", "radius": 1000,
                                 "origin": [0, -1000, 0]}})");
    midgard::Game game(pkg.dir);
    const midgard::Scene& scene = game.scene();
    CHECK(scene.horizon_dip(glm::dvec3(0.0)) == doctest::Approx(0.0f));  // on the ground
    // 2 m up: the ground's edge is acos(1000 / 1002) below level, about 3.6 degrees.
    const float dip = scene.horizon_dip(glm::dvec3(0.0, 2.0, 0.0));
    CHECK(glm::degrees(dip) == doctest::Approx(3.62f).epsilon(0.01));
    CHECK(scene.environment(glm::dvec3(0.0, 2.0, 0.0)).horizon_dip == doctest::Approx(dip));
    // Flat ground: level.
    TestPackage flat({{"e1_sun", R"(NULL, '{"light": {"type": "directional"}}')"}});
    midgard::Game level(flat.dir);
    CHECK(level.scene().horizon_dip(glm::dvec3(0.0, 50.0, 0.0)) == 0.0f);
}

namespace {

// The player walks (W held) from `from` (a direction from the sphere's centre) heading along
// `heading` (along the ground there), all the way round the sphere and back. It stays on the
// ground, crosses face edges, passes cube corners when its path goes through them, and gravity
// points down all the way.
struct SphereWalk {
    double turned = 0.0, lowest = 1e9, worst_gravity_angle = 0.0, nearest_corner = 1e9;
    double worst_gravity_strength = 0.0;  // how far |g| is from the surface's 9.81
    int frames = 0, faces_crossed = 0;
};

SphereWalk walk_round_sphere(const char* shape, const glm::dvec3& from, const glm::dvec3& heading) {
    constexpr double kRadius = 1000.0, kPi = 3.14159265358979;
    const bool outside = std::string(shape) == "sphere_outside";
    const glm::dvec3 centre(0.0, outside ? -kRadius : kRadius, 0.0);
    const std::string body_json = std::string(R"({"shape": ")") + shape +
                                  R"(", "radius": 1000, "seed": 13, "origin": [0, )" +
                                  (outside ? "-1000" : "1000") + ", 0]}";
    const std::string settings =
        R"({"body": )" + body_json + R"(, "terrain": {"height_scale": 20}})";
    // Where to start: just above the ground there, and the camera behind and above.
    json body = json::parse(body_json);
    body["id"] = "b";
    const midgard::Terrain terrain(midgard::body_from_json(body),
                                   midgard::terrain_from_json(json{{"height_scale", 20}}));
    const glm::dvec3 d = glm::normalize(from);
    const glm::dvec3 ground = terrain.ground(centre + d * kRadius);
    const glm::dvec3 up = terrain.body().up(ground);
    const glm::dvec3 ahead = glm::normalize(heading - up * glm::dot(heading, up));
    const glm::dvec3 at = ground + up * 1.0;
    const glm::dvec3 camera = at - ahead * 6.0 + up * 40.0;
    const auto vec = [](const glm::dvec3& p) {
        return "[" + std::to_string(p.x) + ", " + std::to_string(p.y) + ", " + std::to_string(p.z) +
               "]";
    };
    TestPackage pkg({{"e1_player", "NULL, '{\"transform\": {\"position\": " + vec(at) +
                                       "}, \"player\": {\"speed\": 40}}'"},
                     {"e2_camera", "NULL, '{\"transform\": {\"position\": " + vec(camera) +
                                       "}, \"camera\": {}}'"}},
                    settings);
    midgard::Game game(pkg.dir);
    midgard::Scene& scene = game.scene();
    const entt::entity player = scene.find("e1_player");
    const midgard::Terrain& t = game.terrain()->terrain();
    midgard::Actions actions(midgard::InputMap::defaults());
    for (int i = 0; i < 60; ++i) game.update(actions, 1.0f / 60.0f);  // settle
    actions.set("keyboard", "W", 1.0f);
    // Angle round the path's great circle (from `from` towards `heading`).
    const glm::dvec3 e0 = d, e1 = glm::normalize(ahead - d * glm::dot(ahead, d));
    const auto angle = [&] {
        const glm::dvec3 q = scene.world_position(player) - centre;
        return std::atan2(glm::dot(q, e1), glm::dot(q, e0));
    };
    SphereWalk w;
    double last = angle();
    int face = t.body().surface(scene.world_position(player)).face;
    while (std::abs(w.turned) < 2.0 * kPi && w.frames < 16000) {
        game.update(actions, 1.0f / 60.0f);
        actions.begin_frame();
        ++w.frames;
        const double now = angle();
        double step = now - last;
        if (step > kPi) step -= 2.0 * kPi;
        if (step < -kPi) step += 2.0 * kPi;
        w.turned += step;
        last = now;
        const glm::dvec3 p = scene.world_position(player);
        w.lowest = std::min(w.lowest, t.above_ground(p));
        const int f = t.body().surface(p).face;
        if (f != face) ++w.faces_crossed;
        face = f;
        const glm::dvec3 g = game.physics().world().gravity(p);
        w.worst_gravity_strength =
            std::max(w.worst_gravity_strength, std::abs(glm::length(g) - 9.81));
        const double cosine = glm::dot(glm::normalize(g), -t.body().up(p));
        w.worst_gravity_angle = std::max(w.worst_gravity_angle, std::acos(std::min(cosine, 1.0)));
        const glm::dvec3 q = glm::normalize(p - centre);
        for (const double x : {-1.0, 1.0})
            for (const double y : {-1.0, 1.0})
                for (const double z : {-1.0, 1.0})
                    w.nearest_corner = std::min(
                        w.nearest_corner,
                        kRadius * std::acos(std::min(
                                      glm::dot(q, glm::normalize(glm::dvec3(x, y, z))), 1.0)));
    }
    return w;
}

}  // namespace

TEST_CASE("terrain, the exit: all the way round a planet, by three paths, no seams") {
    struct Path {
        const char* name;
        glm::dvec3 from, heading;
        bool corners;
    };
    for (const Path& path : {Path{"the equator", {1, 0, 0}, {0, 0, 1}, false},
                             Path{"over both poles", {0, 1, 0}, {0, 0, -1}, false},
                             Path{"diagonally through corners", {0, 1, 0}, {1, 0, 1}, true}}) {
        CAPTURE(path.name);
        const SphereWalk w = walk_round_sphere("sphere_outside", path.from, path.heading);
        MESSAGE("round the planet (", path.name, ") in ", w.frames, " frames");
        CHECK(std::abs(w.turned) >= 2.0 * 3.14159265358979);  // all the way round
        CHECK(w.faces_crossed >= 4);                          // over face edges
        CHECK(w.lowest > -0.1);                               // never through the ground
        CHECK(w.worst_gravity_angle < 1e-6);                  // down, to the centre
        CHECK(w.worst_gravity_strength < 1e-9);               // 9.81 m/s^2 all the way
        if (path.corners) CHECK(w.nearest_corner < 25.0);     // through cube corners
    }
}

TEST_CASE("terrain, the exit: all the way round inside a hollow world, no seams") {
    for (const auto& [from, heading, corners] :
         {std::tuple{glm::dvec3(1, 0, 0), glm::dvec3(0, 0, 1), false},
          std::tuple{glm::dvec3(0, -1, 0), glm::dvec3(1, 0, 1), true}}) {
        const SphereWalk w = walk_round_sphere("sphere_inside", from, heading);
        MESSAGE("round inside the hollow world in ", w.frames, " frames");
        CHECK(std::abs(w.turned) >= 2.0 * 3.14159265358979);
        CHECK(w.faces_crossed >= 4);
        CHECK(w.lowest > -0.1);
        CHECK(w.worst_gravity_angle < 1e-6);  // outwards, away from the centre
        CHECK(w.worst_gravity_strength < 1e-9);
        if (corners) CHECK(w.nearest_corner < 25.0);
    }
}

// ---- characters, the exit (milestone 5.13) ------------------------------------------------------

namespace {

std::vector<std::uint8_t> sample_file(const char* name) {
    std::ifstream in(std::string(MIDGARD_ASSETS_DIR "/third_party/characters/quaternius/") + name,
                     std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), {});
}

}  // namespace

TEST_CASE("characters, the exit: the Superhero walks and runs round the planet, feet planted") {
    // The hilly planet the terrain exits walk round; the player the Superhero, idle, walking or
    // sprinting as fast as it goes (blended by its speed), its feet on the ground, its arms in
    // first person. Half way round it walks, then runs; eight times it stops, and there its feet
    // stand on the slope as an idle character's do, in each of the three views.
    constexpr double kRadius = 1000.0, kPi = 3.14159265358979;
    const glm::dvec3 centre(0.0, -kRadius, 0.0);
    const std::string body_json =
        R"({"shape": "sphere_outside", "radius": 1000, "seed": 13, "origin": [0, -1000, 0]})";
    json body = json::parse(body_json);
    body["id"] = "b";
    const midgard::Terrain terrain(midgard::body_from_json(body),
                                   midgard::terrain_from_json(json{{"height_scale", 20}}));
    const glm::dvec3 ground = terrain.ground(centre + glm::dvec3(1, 0, 0) * kRadius);
    const glm::dvec3 up = terrain.body().up(ground);
    const glm::dvec3 at = ground + up * 1.0;
    const glm::dvec3 camera = at - glm::dvec3(0, 0, 1) * 6.0 + up * 3.0;
    const auto vec = [](const glm::dvec3& p) { return json{p.x, p.y, p.z}; };
    const json graph = json::parse(R"({"layers": [{"name": "base", "start": "idle",
        "states": {"idle": {"clip": "Idle_Loop"},
                   "move": {"blend": "speed", "clips": [["Idle_Loop", 0], ["Walk_Loop", 10],
                                                        ["Sprint_Loop", 40]]}},
        "transitions": [{"from": "idle", "to": "move", "when": [["speed", ">", 0.5]]},
                        {"from": "move", "to": "idle", "when": [["speed", "<=", 0.5]]}]}]})");
    const json player = {{"transform", {{"position", vec(at)}}},
                         {"player", {{"speed", 20}, {"run_multiplier", 2}, {"height", 1.8}}},
                         {"character",
                          {{"model", "asset:hero.glb"},
                           {"clips", "asset:moves.glb"},
                           {"graph", graph},
                           {"ik", {{"feet", true}}},
                           {"first_person", json::object()}}}};
    const json cam = {{"transform", {{"position", vec(camera)}}}, {"camera", json::object()}};
    TestPackage pkg(
        {{"e1_player", "NULL, '" + player.dump() + "'"},
         {"e2_camera", "NULL, '" + cam.dump() + "'"}},
        R"({"body": )" + body_json + R"(, "terrain": {"height_scale": 20}})", {},
        {{"asset:hero.glb", "characters/hero.glb", sample_file("superhero_male.glb")},
         {"asset:moves.glb", "characters/moves.glb", sample_file("animation_library.glb")}});
    midgard::Game game(pkg.dir);
    midgard::Scene& scene = game.scene();
    const entt::entity e = scene.find("e1_player");
    const midgard::Characters& characters = game.characters();
    midgard::CameraRig& rig = game.camera_rig();
    midgard::Actions actions(midgard::InputMap::defaults());
    int frames = 0;
    const auto step = [&] {
        game.update(actions, 1.0f / 60.0f);
        actions.begin_frame();
        ++frames;
    };
    for (int i = 0; i < 60; ++i) step();  // settle
    REQUIRE(characters.pose(e));
    CHECK(game.character_problems().empty());

    // Each foot's ankle above the ground under it, along the local up.
    const auto ankle = [&](midgard::HumanBone foot) {
        const glm::dvec3 p = *characters.bone_position(scene, e, foot);
        const glm::vec3 local_up(glm::normalize(p - centre));
        const auto hit = game.physics().world().raycast(  // (not its own capsule)
            p + glm::dvec3(local_up) * 0.6, -local_up, 2.0, midgard::physics::Touching::Solid,
            game.physics().body(e));
        REQUIRE(hit);
        return std::pair{glm::dot(p - hit->point, glm::dvec3(local_up)), hit->point};
    };
    const glm::dvec3 start = glm::normalize(scene.world_position(e) - centre);
    glm::dvec3 last = start;
    double turned = 0.0, steepest = 0.0;
    int stops = 0, moving_checked = 0;
    std::vector<float> walk_speeds, run_speeds;
    std::set<midgard::CameraView> seen;
    while (turned < 2.0 * kPi && frames < 30000) {
        const bool running = turned > kPi;
        if (turned >= (stops + 0.5) * 2.0 * kPi / 8.0) {
            // A stop: idle, both feet on the ground (on a slope too), in each view.
            ++stops;
            actions.set("keyboard", "W", 0.0f);
            actions.set("keyboard", "Left Shift", 0.0f);
            for (int i = 0; i < 90; ++i) step();
            CHECK(characters.state_of(e) == "idle");
            const auto [left, left_ground] = ankle(midgard::HumanBone::LeftFoot);
            const auto [right, right_ground] = ankle(midgard::HumanBone::RightFoot);
            CAPTURE(stops);
            CHECK(left > 0.03);  // (above the ground, at an idle ankle's height: 8 cm or so)
            CHECK(left < 0.15);
            CHECK(right > 0.03);
            CHECK(right < 0.15);
            CHECK(std::abs(left - right) < 0.03);  // (as on flat ground: idle feet alike)
            const glm::dvec3 here = glm::normalize(scene.world_position(e) - centre);
            steepest = std::max(steepest, std::abs(glm::dot(left_ground - right_ground, here)));
            for (int v = 0; v < 3; ++v) {
                for (int i = 0; i < 30; ++i) step();  // the blend into the view
                seen.insert(rig.view());
                if (rig.view() == midgard::CameraView::First) {
                    CHECK(rig.hides_player());
                    CHECK(characters.first_person(scene, e, glm::mat4(1.0f)));  // its arms
                } else {
                    CHECK_FALSE(rig.hides_player());  // its whole body
                }
                actions.set("keyboard", "V", 1.0f);
                step();
                actions.set("keyboard", "V", 0.0f);
            }
        }
        actions.set("keyboard", "W", 1.0f);
        actions.set("keyboard", "Left Shift", running ? 1.0f : 0.0f);
        step();
        // Moving: in the blend, its speed along the ground (less up hills: on average, what
        // the player's is).
        if (frames % 600 == 0 && characters.state_of(e) == "move") {
            (running ? run_speeds : walk_speeds).push_back(characters.parameter(e, "speed"));
            ++moving_checked;
        }
        const glm::dvec3 now = glm::normalize(scene.world_position(e) - centre);
        turned += std::acos(std::clamp(glm::dot(last, now), -1.0, 1.0));
        last = now;
    }
    MESSAGE("round the planet in ", frames, " frames; the steepest ground between its feet ",
            steepest * 100.0, " cm");
    CHECK(turned >= 2.0 * kPi);  // all the way round
    CHECK(stops == 8);
    CHECK(seen.size() == 3);  // every view
    CHECK(moving_checked > 10);
    const auto mean = [](const std::vector<float>& v) {
        return v.empty() ? 0.0 : std::accumulate(v.begin(), v.end(), 0.0) / double(v.size());
    };
    CHECK(mean(walk_speeds) == doctest::Approx(20.0).epsilon(0.1));
    CHECK(mean(run_speeds) == doctest::Approx(40.0).epsilon(0.1));
    CHECK(steepest > 0.02);  // (it did stand on slopes: one foot higher than the other)
}

TEST_CASE("inside worlds' lights: the rod's mesh, and how far the camera sees") {
    // A cylinder along Z, 1 m across and long: sides facing outwards.
    const midgard::render::MeshData rod = midgard::make_light_rod();
    REQUIRE_FALSE(rod.indices.empty());
    for (const midgard::render::Vertex& v : rod.vertices) {
        CHECK(std::hypot(v.position[0], v.position[1]) == doctest::Approx(0.5));
        CHECK(std::abs(v.position[2]) == doctest::Approx(0.5));
    }
    for (size_t i = 0; i < rod.indices.size(); i += 3) {
        const auto p = [&](size_t k) {
            const float* q = rod.vertices[rod.indices[i + k]].position;
            return glm::dvec3(q[0], q[1], q[2]);
        };
        const glm::dvec3 n = glm::cross(p(1) - p(0), p(2) - p(0));
        const glm::dvec3 middle = (p(0) + p(1) + p(2)) / 3.0;
        CHECK(glm::dot(n, glm::dvec3(middle.x, middle.y, 0.0)) > 0.0);  // counter-clockwise, out
    }
    // Inside a habitat or a hollow world the camera sees past the far side; elsewhere, no
    // change.
    const auto body = [](const char* text) { return midgard::body_from_json(json::parse(text)); };
    CHECK(midgard::inside_view_distance(body(R"({"id": "h", "shape": "curved_cylinder",
        "radius": 1000, "length": 5000})")) == doctest::Approx(2200.0));
    CHECK(midgard::inside_view_distance(body(R"({"id": "s", "shape": "sphere_inside",
        "radius": 20000})")) == doctest::Approx(44000.0));
    CHECK(midgard::inside_view_distance(body(R"({"id": "p", "shape": "sphere_outside",
        "radius": 3000})")) == 0.0);
    CHECK(midgard::inside_view_distance(std::nullopt) == 0.0);
}

TEST_CASE("the cook warns about a World Surface under terrain") {
    // A World Surface made before terrain was turned on lies at height 0, at sea level: it
    // hides the water and the player walks on it.
    const char* settings = R"({"body": {"shape": "curved_cylinder", "radius": 1000,
                                        "length": 5000, "origin": [0, 1000, 0]},
                               "terrain": {}})";
    TestPackage both({{"e7_ground", R"(NULL, '{"mesh": {"asset": "builtin:surface"},
                                         "collider": {"shape": "mesh"}}')"}},
                     settings);
    std::string all;
    for (const std::string& w : midgard::validate_package(both.dir).warnings) all += w + "\n";
    CHECK(all.find("e7_ground") != std::string::npos);
    CHECK(all.find("World Surface") != std::string::npos);
    // Without terrain it is the ground, as meant.
    TestPackage surface({{"e7_ground", R"(NULL, '{"mesh": {"asset": "builtin:surface"}}')"}},
                        R"({"body": {"shape": "curved_cylinder", "radius": 1000,
                                     "length": 5000, "origin": [0, 1000, 0]}})");
    for (const std::string& w : midgard::validate_package(surface.dir).warnings)
        CHECK(w.find("World Surface") == std::string::npos);
}

TEST_CASE("a scene's terrain from a world type: the same ground, and what the cook refuses") {
    // A habitat whose terrain names a world type's version (migration 12) makes exactly the
    // ground the same settings made inline.
    const char* body = R"("body": {"shape": "curved_cylinder", "radius": 1000, "length": 5000,
                                   "seed": 9, "origin": [0, 1000, 0]})";
    TestPackage inline_pkg(
        {}, std::string("{") + body + R"(, "terrain": {"height_scale": 35, "seed": 4}})");
    TestPackage typed({}, std::string("{") + body +
                              R"(, "terrain": {"world_type": "wt_hills", "version": 2,
                                                "seed": 4}})");
    const auto with_types = [](const TestPackage& pkg, const char* versions_sql) {
        sqlite3* db = nullptr;
        sqlite3_open(utf8(pkg.dir / "Game.db").c_str(), &db);
        exec(db, "INSERT INTO world_type (id, name, version) VALUES ('wt_hills', 'Hills', 2)");
        exec(db, versions_sql);
        sqlite3_close(db);
    };
    with_types(typed, R"(INSERT INTO world_type_version (world_type, version, recipe) VALUES
        ('wt_hills', 1, '{"terrain": {"height_scale": 90}}'),
        ('wt_hills', 2, '{"shapes": ["curved_cylinder"], "terrain": {"height_scale": 35}}'))");
    CHECK(midgard::validate_package(typed.dir).errors.empty());
    midgard::Game a(inline_pkg.dir), b(typed.dir);
    REQUIRE(b.scene().settings.world.has_value());
    CHECK(b.scene().settings.world->version == 2);
    CHECK(b.scene().settings.terrain->height_scale == 35.0);  // version 2's, not 1's
    const midgard::Terrain& ta = a.terrain()->terrain();
    const midgard::Terrain& tb = b.terrain()->terrain();
    for (const midgard::ChunkKey k : {midgard::ChunkKey{0, 0, 0, 0}, midgard::ChunkKey{0, 2, 5, 3}})
        CHECK(ta.chunk_hash(k) == tb.chunk_hash(k));

    // A version that isn't there: the scene can't load, and the cook says why.
    TestPackage missing(
        {}, std::string("{") + body + R"(, "terrain": {"world_type": "wt_hills", "version": 7}})");
    with_types(missing, R"(INSERT INTO world_type_version (world_type, version, recipe) VALUES
        ('wt_hills', 1, '{}'))");
    std::string all;
    for (const std::string& e : midgard::validate_package(missing.dir).errors) all += e + "\n";
    CHECK(all.find("no version 7") != std::string::npos);
    // A world type for other shapes, and a recipe the cook refuses (in any version).
    TestPackage misfit(
        {}, std::string("{") + body + R"(, "terrain": {"world_type": "wt_hills", "version": 1}})");
    with_types(misfit, R"(INSERT INTO world_type_version (world_type, version, recipe) VALUES
        ('wt_hills', 1, '{"shapes": ["sphere_outside"]}'),
        ('wt_hills', 2, '{"terrain": {"octaves": 0}}'))");
    all.clear();
    for (const std::string& e : midgard::validate_package(misfit.dir).errors) all += e + "\n";
    CHECK(all.find("isn't for a curved_cylinder") != std::string::npos);
    CHECK(all.find("version 2") != std::string::npos);
}

TEST_CASE("a world type's graph makes the game's ground, and the cook weighs its cost") {
    const char* body = R"("body": {"shape": "bounded_area", "width": 3000, "length": 3000,
                                   "seed": 9})";
    TestPackage pkg(
        {}, std::string("{") + body + R"(, "terrain": {"world_type": "wt_flat", "version": 1}})");
    sqlite3* db = nullptr;
    sqlite3_open(utf8(pkg.dir / "Game.db").c_str(), &db);
    exec(db, "INSERT INTO world_type (id, name, version) VALUES ('wt_flat', 'Flat', 1)");
    // Level ground 12 m up, everywhere (before the ocean band round the edge).
    exec(db, R"(INSERT INTO world_type_version (world_type, version, recipe) VALUES ('wt_flat', 1,
        '{"graph": {"output": "h", "nodes": {"h": {"op": "constant", "value": 12}}}}'))");
    sqlite3_close(db);
    CHECK(midgard::validate_package(pkg.dir).errors.empty());
    midgard::Game game(pkg.dir);
    REQUIRE(game.terrain() != nullptr);
    CHECK(game.terrain()->terrain().height(0, 0.5, 0.5) == doctest::Approx(12.0));
    CHECK(game.terrain()->terrain().height(0, 0.31, 0.62) == doctest::Approx(12.0));
    // A costly ground: the cook warns it streams slowly (noise of 12 octaves under a filter).
    sqlite3_open(utf8(pkg.dir / "Game.db").c_str(), &db);
    exec(db, R"(UPDATE world_type_version SET recipe = '{"graph": {"output": "h", "nodes": {
        "n": {"op": "noise", "size": 400, "octaves": 12},
        "h": {"op": "erode", "in": ["n"], "radius": 8}}}}')");
    sqlite3_close(db);
    std::string all;
    for (const std::string& w : midgard::validate_package(pkg.dir).warnings) all += w + "\n";
    CHECK(all.find("streams slowly") != std::string::npos);
}

TEST_CASE("a world type's objects: placed round the player, solid, and following it") {
    // Posts (a box each) on a bounded area, one cell in three, within 60 m of the player.
    TestPackage pkg(
        {{"e1_player", R"(NULL, '{"transform": {"position": [0, 30, 0]}, "player": {}}')"}},
        R"({"body": {"shape": "bounded_area", "width": 3000, "length": 3000, "seed": 7},
            "terrain": {"world_type": "wt_posts", "version": 1}})",
        {{"obj_post", R"([{"name": "post", "components": {
              "transform": {"position": [0, 1, 0]}, "mesh": {"asset": "builtin:cube"},
              "collider": {"shape": "box", "size": [0.5, 2, 0.5]}}}])"}});
    sqlite3* db = nullptr;
    sqlite3_open(utf8(pkg.dir / "Game.db").c_str(), &db);
    exec(db, "INSERT INTO world_type (id, name, version) VALUES ('wt_posts', 'Posts', 1)");
    exec(db, R"(INSERT INTO world_type_version (world_type, version, recipe) VALUES ('wt_posts', 1,
        '{"terrain": {"placements": [{"object": "obj_post", "spacing": 12, "chance": 0.35,
          "slope": [0, 90], "height": [-1000, 1000], "distance": 60}]}}'))");
    sqlite3_close(db);
    CHECK(midgard::validate_package(pkg.dir).errors.empty());
    midgard::Game game(pkg.dir);
    REQUIRE(game.placed() != nullptr);
    const size_t near_start = game.placed()->count();
    CHECK(near_start > 5);
    // Each an instance of the template, standing on the ground.
    const midgard::Terrain& t = game.terrain()->terrain();
    int checked = 0;
    for (entt::entity e : game.scene().registry.view<const midgard::ObjectRef>()) {
        const std::string& id = game.scene().registry.get<midgard::EntityInfo>(e).id;
        if (!midgard::PlacedObjects::is_placed(id)) continue;
        const glm::dvec3 p = game.scene().world_position(e);
        CHECK(std::abs(t.above_ground(p)) < 1e-3);
        CHECK(glm::length(p - glm::dvec3(0, p.y, 0)) < 60.0 + 200.0);  // round the start
        // Solid: a ray down onto its top hits the post, not the ground.
        const auto hit = game.physics().world().raycast(p + glm::dvec3(0, 10, 0), {0, -1, 0}, 20.0);
        REQUIRE(hit.has_value());
        CHECK(hit->point.y == doctest::Approx(p.y + 2.0).epsilon(1e-3));
        if (++checked == 3) break;
    }
    CHECK(checked == 3);
    // The player moves 600 m away: the old ones go, new ones come.
    game.put_entity(json::parse(R"({"id": "e1_player", "components": {
        "transform": {"position": [600, 30, 0]}, "player": {}}})"));
    midgard::Actions actions(midgard::InputMap::defaults());
    for (int i = 0; i < 40; ++i) game.update(actions, 1.0f / 60.0f);
    int near_new = 0, near_old = 0;
    for (entt::entity e : game.scene().registry.view<const midgard::ObjectRef>()) {
        if (!midgard::PlacedObjects::is_placed(
                game.scene().registry.get<midgard::EntityInfo>(e).id))
            continue;
        const glm::dvec3 p = game.scene().world_position(e);
        if (glm::length(glm::dvec2(p.x - 600.0, p.z)) < 100.0) ++near_new;
        if (glm::length(glm::dvec2(p.x, p.z)) < 100.0) ++near_old;
    }
    CHECK(near_new > 5);
    CHECK(near_old == 0);
}

TEST_CASE("scene exits and arrivals: read, outlined, and checked by the cook") {
    TestPackage pkg({
        {"e1_player", R"(NULL, '{"transform": {}, "player": {}}')"},
        {"e2_door", R"(NULL, '{"transform": {"position": [4, 1, 0]},
                      "scene_exit": {"scene": "scn_cave", "arrival": "e9_in",
                                     "activation": "use", "size": [1, 2, 0.5], "fade": 1}}')"},
        {"e3_back", R"(NULL, '{"transform": {}, "arrival": {"name": "From the cave"}}')"},
    });
    {
        midgard::Game game(pkg.dir);
        const midgard::Scene& scene = game.scene();
        const auto& door = scene.registry.get<midgard::SceneExit>(scene.find("e2_door"));
        CHECK(door.scene == "scn_cave");
        CHECK(door.arrival == "e9_in");
        CHECK(door.activation == midgard::ExitActivation::Use);
        CHECK(std::string(midgard::exit_activation_name(door.activation)) == "use");
        CHECK(door.size == glm::vec3(1.0f, 2.0f, 0.5f));
        CHECK(door.fade == 1.0f);
        CHECK(scene.registry.get<midgard::Arrival>(scene.find("e3_back")).name == "From the cave");
        // Outlined: the exit's box (12 edges) and the arrival's post and arrow (4), beside the
        // player's capsule.
        CHECK(game.debug_lines(glm::dvec3(0.0)).size() == 12 + 4 + 2 * 24 + 4 + 4 * 12);
    }
    const auto problems = [&] {
        const midgard::ValidationReport r = midgard::validate_package(pkg.dir);
        return r.errors;
    };
    sqlite3* db = nullptr;
    sqlite3_open(utf8(pkg.dir / "Game.db").c_str(), &db);
    // No cave yet.
    auto errors = problems();
    REQUIRE(errors.size() == 1);
    CHECK(errors[0] ==
          "entity 'e2_door' is a scene exit to scene 'scn_cave', which is not in "
          "the package");
    exec(db, "INSERT INTO string VALUES ('scn_cave.name', 'Cave', '')");
    exec(db,
         "INSERT INTO scene (id, name_key, settings) VALUES ('scn_cave', 'scn_cave.name', "
         "'{}')");
    exec(db,
         "INSERT INTO entity (id, scene, name, parent, components) VALUES ('e9_in', "
         "'scn_cave', 'In', NULL, '{\"transform\": {}}')");
    errors = problems();  // (two scenes now: each problem names its scene)
    REQUIRE(errors.size() == 1);
    CHECK(errors[0] ==
          "scene 'scn_main': entity 'e2_door' is a scene exit to arrival 'e9_in', "
          "which is not an arrival in scene 'scn_cave'");
    exec(db,
         "UPDATE entity SET components = '{\"transform\": {}, \"arrival\": {}}' WHERE id = "
         "'e9_in'");
    CHECK(problems().empty());
    exec(db,
         "UPDATE entity SET components = '{\"transform\": {}, \"scene_exit\": "
         "{\"activation\": \"wish\"}}' WHERE id = 'e2_door'");
    errors = problems();
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].find("enter, use or script") != std::string::npos);
    exec(db,
         "UPDATE entity SET components = '{\"transform\": {}, \"scene_exit\": {}}' WHERE id "
         "= 'e2_door'");
    errors = problems();
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].find("leads nowhere") != std::string::npos);
    sqlite3_close(db);
}

TEST_CASE("walking into a scene exit fades out, swaps the scene and arrives") {
    TestPackage pkg({
        {"e1_player", R"(NULL, '{"transform": {"position": [0, 1, 0]}, "player": {}}')"},
        {"e2_door", R"(NULL, '{"transform": {"position": [3, 1, 0]},
                      "scene_exit": {"scene": "scn_cave", "arrival": "e9_in", "fade": 0.2,
                                     "size": [1, 2, 2]}}')"},
    });
    {
        sqlite3* db = nullptr;
        sqlite3_open(utf8(pkg.dir / "Game.db").c_str(), &db);
        exec(db, "INSERT INTO string VALUES ('scn_cave.name', 'Cave', '')");
        exec(db,
             "INSERT INTO scene (id, name_key, settings) VALUES ('scn_cave', "
             "'scn_cave.name', '{\"sky\": [0, 0, 0]}')");
        // The cave's own player, its arrival (facing -X: turned 90 degrees) and an exit back
        // round the arrival, which mustn't fire on arriving.
        exec(db,
             "INSERT INTO entity (id, scene, name, parent, components) VALUES "
             "('e8_cave_player', 'scn_cave', 'Player', NULL, "
             "'{\"transform\": {\"position\": [0, 1, 0]}, \"player\": {}}'), "
             "('e9_in', 'scn_cave', 'In', NULL, '{\"transform\": {\"position\": [10, 1, 5], "
             "\"rotation\": [0, 90, 0]}, \"arrival\": {\"name\": \"In\"}}'), "
             "('e10_out', 'scn_cave', 'Out', NULL, '{\"transform\": {\"position\": [10, 1, "
             "5]}, \"scene_exit\": {\"scene\": \"scn_main\", \"fade\": 0}}')");
        sqlite3_close(db);
    }
    midgard::Game game(pkg.dir);
    midgard::Actions actions(midgard::InputMap::defaults());
    const auto player_at = [&] {
        midgard::Scene& s = game.scene();
        for (entt::entity e : s.registry.view<const midgard::Player>()) return s.world_position(e);
        return glm::dvec3(-1.0);
    };
    game.update(actions, 0.016f);
    CHECK_FALSE(game.changing_scene());
    // Into the door's box: the change starts, and the picture darkens over 0.2 s.
    midgard::Scene& main = game.scene();
    main.registry.get<midgard::Transform>(main.find("e1_player")).position = {3.0, 1.0, 0.0};
    game.update(actions, 0.016f);
    REQUIRE(game.changing_scene());
    game.update(actions, 0.1f);
    CHECK(game.fade() == doctest::Approx(0.5f));
    CHECK(game.scene().id == "scn_main");
    game.update(actions, 0.1f);  // dark: the cave now
    CHECK(game.scene_changed());
    CHECK(game.scene().id == "scn_cave");
    CHECK(game.scene_change_error().empty());
    CHECK(glm::length(player_at() - glm::dvec3(10.0, 1.0, 5.0)) < 0.2);
    const midgard::Scene& cave = game.scene();
    CHECK(std::abs(cave.registry.get<midgard::Transform>(cave.find("e8_cave_player")).rotation.y -
                   90.0f) < 0.5f);
    CHECK(game.fade() == doctest::Approx(1.0f));
    game.update(actions, 0.1f);
    CHECK_FALSE(game.scene_changed());
    CHECK(game.fade() == doctest::Approx(0.5f));
    game.update(actions, 0.1f);
    CHECK_FALSE(game.changing_scene());
    CHECK(game.fade() == 0.0f);
    // Arriving inside the cave's way out didn't take it; stepping out and back in does.
    for (int i = 0; i < 5; ++i) game.update(actions, 0.016f);
    CHECK(game.scene().id == "scn_cave");
    midgard::Scene& in_cave = game.scene();
    in_cave.registry.get<midgard::Transform>(in_cave.find("e8_cave_player")).position = {20.0, 1.0,
                                                                                         5.0};
    game.update(actions, 0.016f);
    in_cave.registry.get<midgard::Transform>(in_cave.find("e8_cave_player")).position = {10.0, 1.0,
                                                                                         5.0};
    game.update(actions, 0.016f);  // (no fade: at once)
    game.update(actions, 0.016f);
    CHECK(game.scene().id == "scn_main");
    while (game.changing_scene()) game.update(actions, 0.016f);

    // A scene or arrival that isn't there: the player stays.
    game.change_scene("scn_cave", "e_nowhere", 0.0f);
    game.update(actions, 0.016f);
    CHECK(game.scene().id == "scn_main");
    CHECK(game.scene_change_error().find("e_nowhere") != std::string::npos);
    while (game.changing_scene()) game.update(actions, 0.016f);

    // The world remembers the scene: a new game carries on there.
    game.change_scene("scn_cave", "e9_in", 0.0f);
    for (int i = 0; i < 3; ++i) game.update(actions, 0.016f);
    REQUIRE(game.scene().id == "scn_cave");
    const fs::path world = pkg.dir / "world";
    game.use_world(world);
    game.save_world();
    midgard::Game again(pkg.dir);
    CHECK(again.scene().id == "scn_main");
    again.use_world(world);
    CHECK(again.scene().id == "scn_cave");
    CHECK(glm::length(again.scene().world_position(again.scene().find("e8_cave_player")) -
                      glm::dvec3(10.0, 1.0, 5.0)) < 0.2);
}

TEST_CASE("a scene exit set to use is taken by pressing Use inside it, or by a script") {
    TestPackage pkg({
        {"e1_player", R"(NULL, '{"transform": {"position": [0, 1, 0]}, "player": {}}')"},
        {"e2_pad", R"(NULL, '{"transform": {"position": [0, 1, 0]},
                     "scene_exit": {"scene": "scn_main", "activation": "use", "fade": 0}}')"},
        {"e3_lever", R"(NULL, '{"transform": {"position": [50, 1, 0]},
                       "scene_exit": {"scene": "scn_main", "activation": "script", "fade": 0}}')"},
    });
    midgard::Game game(pkg.dir);
    midgard::Actions actions(midgard::InputMap::defaults());
    // Standing on the pad (from the start): nothing until Use is pressed.
    for (int i = 0; i < 3; ++i) {
        actions.begin_frame();
        game.update(actions, 0.016f);
    }
    CHECK_FALSE(game.changing_scene());
    actions.begin_frame();
    actions.set("keyboard", "E", 1.0f);
    game.update(actions, 0.016f);
    CHECK(game.changing_scene());
    while (game.changing_scene()) {
        actions.begin_frame();
        actions.set("keyboard", "E", 0.0f);
        game.update(actions, 0.016f);
    }
    CHECK(game.previous_scene() == "scn_main");
    // A script-only exit: walking in does nothing; take_exit takes it.
    midgard::Scene& s = game.scene();
    s.registry.get<midgard::Transform>(s.find("e1_player")).position = {50.0, 1.0, 0.0};
    actions.begin_frame();
    game.update(actions, 0.016f);
    CHECK_FALSE(game.changing_scene());
    CHECK(game.take_exit(game.scene().find("e3_lever")));
    CHECK_FALSE(game.take_exit(game.scene().find("e1_player")));  // not an exit
    CHECK(game.changing_scene());
    CHECK(game.arrival_problem("scn_main", "e2_pad").find("no arrival") != std::string::npos);
    CHECK(game.arrival_problem("scn_gone", "").find("no scene") != std::string::npos);
    CHECK(game.arrival_problem("scn_main", "").empty());
}

namespace {

// The process's resident memory in bytes (0 where it can't be read).
// (On Windows, the private memory committed: the working set grows and shrinks as Windows
// pleases, which made this flaky on CI.)
size_t resident_bytes() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(),
                                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                sizeof(counters)))
        return counters.PrivateUsage;
    return 0;
#elif defined(__linux__)
    std::ifstream statm("/proc/self/statm");
    size_t pages = 0, resident = 0;
    if (statm >> pages >> resident) return resident * size_t(sysconf(_SC_PAGESIZE));
    return 0;
#else
    return 0;
#endif
}

// A package with a flat main scene and a land with terrain (a bounded area), with a door each
// way: the main scene's at x = 3, the land's beside its arrival.
struct TwoScenes : TestPackage {
    TwoScenes()
        : TestPackage({
              {"e1_player", R"(NULL, '{"transform": {"position": [0, 1, 0]}, "player": {}}')"},
              {"e2_door", R"(NULL, '{"transform": {"position": [3, 1, 0]},
                            "scene_exit": {"scene": "scn_land", "arrival": "e9_in", "fade": 0.1,
                                           "size": [1, 2, 2]}}')"},
              {"e3_in", R"(NULL, '{"transform": {"position": [-3, 1, 0]}, "arrival": {}}')"},
          }) {
        sqlite3* db = nullptr;
        sqlite3_open(utf8(dir / "Game.db").c_str(), &db);
        exec(db, "INSERT INTO string VALUES ('scn_land.name', 'Land', '')");
        exec(db, R"(INSERT INTO scene (id, name_key, settings) VALUES ('scn_land', 'scn_land.name',
                 '{"body": {"id": "land", "shape": "bounded_area", "width": 3000, "length": 3000,
                            "seed": 5},
                   "terrain": {"seed": 5, "height_scale": 40}}'))");
        exec(db, R"(INSERT INTO entity (id, scene, name, parent, components) VALUES
                 ('e8_player', 'scn_land', 'Player', NULL,
                  '{"transform": {"position": [0, 60, 0]}, "player": {}}'),
                 ('e9_in', 'scn_land', 'In', NULL,
                  '{"transform": {"position": [200, 60, -150]}, "arrival": {}}'),
                 ('e10_back', 'scn_land', 'Back', NULL,
                  '{"transform": {"position": [210, 60, -150]},
                    "scene_exit": {"scene": "scn_main", "arrival": "e3_in", "fade": 0.1,
                                   "size": [1, 400, 1]}}'))");
        sqlite3_close(db);
    }
};

// Through the door and back: walks the player into the scene's exit and runs the change,
// streaming the ground headlessly as a frame would.
void go_through(midgard::Game& game, midgard::Actions& actions) {
    midgard::Scene& s = game.scene();
    entt::entity player = entt::null;
    for (entt::entity e : s.registry.view<const midgard::Player>()) player = e;
    entt::entity exit = entt::null;
    for (entt::entity e : s.registry.view<const midgard::SceneExit>()) exit = e;
    REQUIRE((player != entt::null));
    REQUIRE((exit != entt::null));
    s.registry.get<midgard::Transform>(player).position = s.world_position(exit);
    for (int i = 0; i < 100 && !game.scene_changed(); ++i) {
        game.update(actions, 0.02f);
        game.stream_terrain(game.camera(1.6f).origin);
    }
    REQUIRE(game.scene_changed());
    while (game.changing_scene()) {
        game.update(actions, 0.02f);
        game.stream_terrain(game.camera(1.6f).origin);
    }
}

}  // namespace

TEST_CASE("arriving in a scene shows its ground at once, the player on it") {
    TwoScenes pkg;
    midgard::Game game(pkg.dir);
    midgard::Actions actions(midgard::InputMap::defaults());
    midgard::Scene& s = game.scene();
    s.registry.get<midgard::Transform>(s.find("e1_player")).position = {3.0, 1.0, 0.0};
    for (int i = 0; i < 100 && !game.scene_changed(); ++i) game.update(actions, 0.02f);
    REQUIRE(game.scene().id == "scn_land");
    // The first frame in the land (no waiting asked for): every chunk in view is ready.
    game.stream_terrain(game.camera(1.6f).origin);
    REQUIRE(game.terrain() != nullptr);
    const midgard::TerrainStreamStats stats = game.terrain()->stats();
    CHECK(stats.wanted > 0);
    CHECK(stats.complete);
    // On the ground, not under it, where the arrival is.
    const glm::dvec3 at = game.scene().world_position(game.scene().find("e8_player"));
    CHECK(game.terrain()->terrain().above_ground(at) > -0.01);
    CHECK(std::abs(at.x - 200.0) < 0.5);
    CHECK(std::abs(at.z + 150.0) < 0.5);
}

TEST_CASE("many scene changes leave nothing behind") {
    TwoScenes pkg;
    midgard::Game game(pkg.dir);
    midgard::Actions actions(midgard::InputMap::defaults());
    // Warm up: the heap and the working set settle after a few dozen changes (measured: up to
    // 60 MB by 30 round trips, then flat to 60).
    for (int i = 0; i < 30; ++i) {
        go_through(game, actions);
        go_through(game, actions);
    }
    REQUIRE(game.scene().id == "scn_main");
    const size_t entities = game.scene().registry.view<const midgard::EntityInfo>().size();
    const size_t bodies = game.physics().world().body_count();
    // Two windows of 30 round trips: a leak grows memory in both, while the allocator settling
    // (a one-off jump, at a moment that differs from machine to machine) shows in one at most.
    double grown[2] = {0.0, 0.0};
    for (double& window : grown) {
        const size_t before = resident_bytes();
        for (int i = 0; i < 30; ++i) {
            go_through(game, actions);
            CHECK(game.scene().id == "scn_land");
            CHECK(game.terrain()->stats().complete);
            go_through(game, actions);
        }
        window = (double(resident_bytes()) - double(before)) / (1024.0 * 1024.0);
    }
    REQUIRE(game.scene().id == "scn_main");
    CHECK(game.scene().registry.view<const midgard::EntityInfo>().size() == entities);
    CHECK(game.physics().world().body_count() == bodies);
    CHECK(game.terrain() == nullptr);  // (the main scene has none)
    if (resident_bytes() > 0) {
        MESSAGE("memory grew by " << grown[0] << " MB, then " << grown[1]
                                  << " MB, over two windows of 30 round trips");
        CHECK(std::min(grown[0], grown[1]) < 16.0);
    }
}

TEST_CASE("a flat scene's navigation comes from its static colliders and follows edits") {
    TestPackage pkg({
        {"e1_ground", R"(NULL, '{"transform": {"position": [0, -0.5, 0], "scale": [60, 1, 60]},
                        "collider": {"shape": "box"}}')"},
        {"e2_wall", R"(NULL, '{"transform": {"position": [0, 1.5, 0]},
                      "collider": {"shape": "box", "size": [30, 3, 1]}}')"},
        {"e3_crate", R"(NULL, '{"transform": {"position": [0, 0.5, -20], "scale": [4, 1, 4]},
                       "collider": {"shape": "box"}, "rigidbody": {"motion": "dynamic"}}')"},
    });
    midgard::Game game(pkg.dir);
    const midgard::NavMesh* nav = game.navmesh();
    REQUIRE(nav != nullptr);
    CHECK(nav->tile_count() >= 4);
    const auto around = game.find_path({0, 0, -10}, {0, 0, 10});
    REQUIRE(around);
    CHECK(around->complete);
    CHECK(around->points.size() >= 3);  // round the wall
    // A crate that moves isn't scenery: a path runs straight over where it stands.
    const auto past_crate = game.find_path({-5, 0, -20}, {5, 0, -20});
    REQUIRE(past_crate);
    CHECK(past_crate->points.size() == 2);
    // The wall taken away (a live edit): straight through.
    game.remove_entity("e2_wall");
    const auto straight = game.find_path({0, 0, -10}, {0, 0, 10});
    REQUIRE(straight);
    CHECK(straight->points.size() == 2);
    // Its edges join the debug lines when asked for.
    const size_t without = game.debug_lines(glm::dvec3(0.0)).size();
    game.set_show_navigation(true);
    CHECK(game.debug_lines(glm::dvec3(0.0)).size() > without);
}

TEST_CASE("a planet has no flat navigation") {
    TestPackage pkg({{"e1_player", R"(NULL, '{"transform": {}, "player": {}}')"}},
                    R"({"body": {"id": "p", "shape": "sphere_outside", "radius": 1000}})");
    midgard::Game game(pkg.dir);
    CHECK(game.navmesh() == nullptr);
    CHECK_FALSE(game.find_path({0, 0, 0}, {10, 0, 0}));
}

TEST_CASE("a terrain scene's navigation is made round the player and follows sculpting") {
    TestPackage pkg({{"e1_player", R"(NULL, '{"transform": {"position": [0, 20, 0]},
                                         "player": {}}')"}},
                    R"({"body": {"id": "land", "shape": "bounded_area", "width": 2000,
                                 "length": 2000, "seed": 2},
                        "terrain": {"seed": 2, "height_scale": 1, "base": 5}})");
    midgard::Game game(pkg.dir);
    CHECK(game.navmesh() == nullptr);  // (terrain: per face)
    REQUIRE(game.nav_world() != nullptr);
    game.stream_navigation(true);
    CHECK(game.nav_world()->stats().complete);
    const midgard::Terrain& t = game.terrain()->terrain();
    const auto on_ground = [&](double x, double z) {
        const midgard::SurfacePoint s = t.body().surface({x, 0.0, z});
        return t.body().position({0, s.u, s.v, t.height(0, s.u, s.v)});
    };
    const glm::dvec3 from = on_ground(-30, 0), to = on_ground(30, 0);
    const auto straight = game.find_path(from, to);
    REQUIRE(straight);
    CHECK(straight->complete);
    const auto length = [](const std::vector<glm::vec3>& p) {
        double total = 0.0;
        for (size_t i = 1; i < p.size(); ++i) total += glm::distance(p[i - 1], p[i]);
        return total;
    };
    CHECK(length(straight->points) < 61.0);
    // A steep mound sculpted across the way: the path goes round it.
    const midgard::SurfacePoint middle = t.body().surface({0.0, 0.0, 0.0});
    game.set_terrain_stamps(
        {midgard::stamp_from_json(nlohmann::json{{"tool", "raise"},
                                                 {"radius", 8.0},
                                                 {"strength", 25.0},
                                                 {"face", 0},
                                                 {"points", {{middle.u, middle.v}}}})});
    game.stream_navigation(true);
    const auto round = game.find_path(from, to);
    REQUIRE(round);
    CHECK(round->complete);
    CHECK(length(round->points) > length(straight->points) + 2.0);
    for (const glm::vec3& p : round->points) CHECK(std::abs(t.above_ground(glm::dvec3(p))) < 0.6);
}

namespace {

// 5.12's exit: the player (an agent) follows a path from `from` to `to` over a world's terrain:
// it gets there, never through the ground. Returns where it stopped.
glm::dvec3 walk_path(const char* body, const char* terrain, const glm::dvec3& start,
                     const glm::dvec3& end) {
    const std::string player = R"(NULL, '{"transform": {"position": [)" + std::to_string(start.x) +
                               ", " + std::to_string(start.y) + ", " + std::to_string(start.z) +
                               R"(]}, "player": {}}')";
    TestPackage pkg({{"e1_player", player}},
                    std::string(R"({"body": )") + body + R"(, "terrain": )" + terrain + "}");
    midgard::Game game(pkg.dir);
    midgard::Actions actions(midgard::InputMap::defaults());
    for (int i = 0; i < 60; ++i) {  // (lands first)
        game.update(actions, 1.0f / 60.0f);
        game.stream_navigation(true);
    }
    const midgard::Terrain& t = game.terrain()->terrain();
    const entt::entity e = game.scene().find("e1_player");
    const auto path = game.find_path(game.scene().world_position(e), end);
    REQUIRE(path);
    REQUIRE(path->complete);
    std::vector<glm::dvec3> points(path->points.begin(), path->points.end());
    game.follow_path(points);
    double lowest = 1e9;
    for (int frame = 0; frame < 60 * 60 && game.following_path(); ++frame) {
        game.update(actions, 1.0f / 60.0f);
        game.stream_navigation(true);
        lowest = std::min(lowest, t.above_ground(game.scene().world_position(e)));
    }
    CHECK_FALSE(game.following_path());  // got there within a minute
    CHECK(lowest > -0.3);                // never through the ground
    const glm::dvec3 stopped = game.scene().world_position(e);
    CHECK(glm::distance(stopped, end) < 1.5);
    return stopped;
}

}  // namespace

TEST_CASE("the exit: an agent walks across a planet's face edge") {
    const char* body = R"({"id": "p", "shape": "sphere_outside", "radius": 1000, "seed": 12,
                           "origin": [0, -1000, 0]})";
    const char* terrain = R"({"seed": 12, "height_scale": 4, "base": 6})";
    const midgard::Terrain t(midgard::body_from_json(nlohmann::json::parse(body)),
                             midgard::terrain_from_json(nlohmann::json::parse(terrain)));
    const auto ground = [&](double u, double v, double above) {  // face 2's frame, continued
        midgard::SurfacePoint s = midgard::cube_surface(midgard::cube_direction(2, u, v));
        s.height = t.height(s.face, s.u, s.v) + above;
        return t.body().position(s);
    };
    // From 24 m short of face 2's edge to 31 m past it, onto the next face.
    const glm::dvec3 start = ground(0.985, 0.5, 1.0), end = ground(1.02, 0.5, 0.0);
    REQUIRE(t.body().surface(start).face == 2);
    REQUIRE(t.body().surface(end).face != 2);
    const glm::dvec3 stopped = walk_path(body, terrain, start, end);
    CHECK(t.body().surface(stopped).face == t.body().surface(end).face);
}

TEST_CASE("the exit: an agent walks across a cylinder's join") {
    const char* body = R"({"id": "h", "shape": "curved_cylinder", "radius": 1000,
                           "length": 5000, "seed": 13, "origin": [0, 1000, 0]})";
    const char* terrain = R"({"seed": 13, "height_scale": 4, "base": 6})";
    const midgard::Terrain t(midgard::body_from_json(nlohmann::json::parse(body)),
                             midgard::terrain_from_json(nlohmann::json::parse(terrain)));
    const auto ground = [&](double u, double v, double above) {
        return t.body().position({0, u, v, t.height(0, u, v) + above});
    };
    // From 19 m before the join (u = 0.997) to 25 m past it (u = 0.004).
    const glm::dvec3 start = ground(0.997, 0.5, 1.0), end = ground(0.004, 0.5, 0.0);
    const glm::dvec3 stopped = walk_path(body, terrain, start, end);
    CHECK(t.body().surface(stopped).u < 0.01);  // over the join, not the long way round
}

TEST_CASE("light markers: a bulb at each point and spot light, seen through what hides it") {
    TestPackage pkg({
        {"e1_candle", R"(NULL, '{"transform": {"position": [5, 1, 0]}}')"},
        {"e2_flame", R"('e1_candle', '{"transform": {"position": [0, 0.5, 0]},
                       "light": {"type": "point", "color": [1, 0.6, 0.2], "range": 4}}')"},
        {"e3_torch", R"(NULL, '{"transform": {"position": [-5, 2, 0], "rotation": [-90, 0, 0]},
                       "light": {"type": "spot", "range": 6, "outer_angle": 30}}')"},
        {"e4_sun", R"(NULL, '{"light": {"type": "directional"}}')"},
    });
    midgard::Game game(pkg.dir);
    const auto& scene = game.scene();
    midgard::LightMarkerView view;
    view.eye = glm::dvec3(0, 1.5, 10);
    const glm::dvec3 origin = view.eye;
    const auto lines = midgard::light_markers(scene, origin, view);
    REQUIRE(!lines.empty());
    // Each line is drawn twice where it can be hidden: solid, and faintly on top.
    size_t on_top = 0;
    for (const auto& l : lines)
        if (l.on_top) {
            ++on_top;
            CHECK(l.color.a < 0.5f);
        }
    CHECK(on_top > 10);
    CHECK(on_top < lines.size());
    // Two bulbs, none for the sun: one round the flame (which its candle carries), one round
    // the torch; each the size the camera's distance gives it.
    const glm::dvec3 flame(5, 1.5, 0), torch(-5, 2, 0);
    size_t near_flame = 0, near_torch = 0;
    for (const auto& l : lines) {
        const glm::dvec3 a = glm::dvec3(l.a) + origin;
        const double size = midgard::light_marker_size(glm::distance(flame, view.eye));
        if (glm::distance(a, flame) < size)
            ++near_flame;
        else if (glm::distance(a, torch) < 6.5)
            ++near_torch;  // (its cone reaches out)
        else
            FAIL("a line far from both lights");
    }
    CHECK(near_flame > 20);
    CHECK(near_torch > 20);
    // The bulb takes the light's colour, lifted towards white.
    bool orange = false;
    for (const auto& l : lines)
        if (!l.on_top && glm::distance(glm::dvec3(l.a) + origin, flame) < 1.0)
            orange = orange || (l.color.r > 0.9f && l.color.b < 0.6f);
    CHECK(orange);
    // Selected, a point light shows its reach: lines out at its range.
    view.selected = "e2_flame";
    const auto chosen = midgard::light_markers(scene, origin, view);
    CHECK(chosen.size() > lines.size());
    bool reach = false;
    for (const auto& l : chosen)
        reach = reach || std::abs(glm::distance(glm::dvec3(l.a) + origin, flame) - 4.0) < 1e-3;
    CHECK(reach);
    CHECK(midgard::light_markers(scene, origin, view, 10).size() <= 10);
}

TEST_CASE("a flickering light dims and brightens, each its own way, the same everywhere") {
    // The share of its brightness: steady at 0; down to a quarter at most at 1.
    CHECK(midgard::flicker_brightness(0.0f, 1.0f, 12.3, 7) == 1.0f);
    float low = 1.0f, high = 0.0f;
    double changes = 0.0;
    float last = midgard::flicker_brightness(1.0f, 1.0f, 0.0, 7);
    for (int i = 1; i < 6000; ++i) {  // a minute, at 100 a second
        const float b = midgard::flicker_brightness(1.0f, 1.0f, i * 0.01, 7);
        low = std::min(low, b);
        high = std::max(high, b);
        changes += std::abs(b - last);
        last = b;
    }
    CHECK(low >= 0.25f);
    CHECK(low < 0.5f);
    CHECK(high > 0.95f);
    CHECK(high <= 1.0f);
    CHECK(changes > 5.0);  // (it moves)
    // A little flicker dims a little; faster flickers change more in the same time.
    for (int i = 0; i < 100; ++i) CHECK(midgard::flicker_brightness(0.2f, 1.0f, i, 7) >= 0.85f);
    double slow = 0.0, fast = 0.0;
    for (int i = 1; i < 1000; ++i) {
        slow += std::abs(midgard::flicker_brightness(1.0f, 1.0f, i * 0.01, 7) -
                         midgard::flicker_brightness(1.0f, 1.0f, (i - 1) * 0.01, 7));
        fast += std::abs(midgard::flicker_brightness(1.0f, 3.0f, i * 0.01, 7) -
                         midgard::flicker_brightness(1.0f, 3.0f, (i - 1) * 0.01, 7));
    }
    CHECK(fast > slow * 2.0);
    // Two candles side by side don't flicker together.
    CHECK(midgard::flicker_brightness(1.0f, 1.0f, 3.0, 7) !=
          midgard::flicker_brightness(1.0f, 1.0f, 3.0, 8));

    // In a scene: the game's time moves the flame, not the lamp.
    TestPackage pkg({
        {"e1_candle", R"(NULL, '{"light": {"type": "point", "intensity": 2, "flicker": 0.6,
                       "flicker_speed": 1.5}}')"},
        {"e2_lamp", R"(NULL, '{"transform": {"position": [0, 0, 0.1]},
                       "light": {"type": "point", "intensity": 2}}')"},
    });
    midgard::Game game(pkg.dir);
    midgard::Actions actions(midgard::InputMap::defaults());
    const auto intensities = [&] {
        std::map<float, float> by_z;  // (told apart by where they are)
        for (const auto& l : game.scene().light().local) by_z[l.position.z] = l.intensity;
        return std::pair{by_z.at(0.0f), by_z.at(0.1f)};
    };
    const auto first = intensities();
    std::set<float> flame;
    for (int i = 0; i < 30; ++i) {
        game.update(actions, 0.05f);
        const auto [candle, lamp] = intensities();
        CHECK(lamp == 2.0f);
        CHECK(candle <= 2.0f);
        CHECK(candle >= 2.0f * (1.0f - 0.75f * 0.6f) - 1e-4f);
        flame.insert(candle);
    }
    CHECK(flame.size() > 20);
    CHECK(first.first <= 2.0f);
}

TEST_CASE("footsteps: the terrain's ground underfoot, as it's drawn") {
    const midgard::TerrainSettings s = midgard::terrain_from_json(json::parse(R"({
        "base": 10, "height_scale": 50, "feature_size": 300, "snow_line": 35,
        "sea_level": 0, "ocean_band": 300, "ocean_depth": 20})"));
    const midgard::Body body = midgard::body_from_json(json::parse(
        R"({"id": "a", "shape": "bounded_area", "width": 2000, "length": 2000, "seed": 3})"));
    const midgard::Terrain t(body, s);
    std::map<std::string, int> found;
    for (int i = 0; i <= 160; ++i)  // (12.5 m apart: a beach is narrow)
        for (int j = 0; j <= 160; ++j) {
            const double u = i / 160.0, v = j / 160.0;
            const double h = t.height(0, u, v);
            const glm::dvec3 at = t.position(0, u, v, h);
            const std::string g = midgard::terrain_ground(t, body, at);
            ++found[g];
            CAPTURE(g);
            CAPTURE(h);
            if (g == "water") CHECK(h < -0.2);
            if (g == "snow") CHECK(h > 35.0);
            if (g == "sand") CHECK(h < 1.75);
            if (h > 2.0 && h < 30.0 && g != "rock") CHECK(g == "grass");
        }
    for (const std::string g : {"water", "sand", "grass", "rock", "snow"}) {
        CAPTURE(g);
        CHECK(found[g] > 0);
    }
    CHECK(midgard::footstep_surfaces().size() == 8);
}

// ---- audio, the exit (milestone 5.14) -----------------------------------------------------------

namespace {

constexpr double kTwoPi = 6.283185307179586;

// A mono tone's WAV (for a test's sounds).
std::vector<std::uint8_t> tone_wav(double hz, double seconds, float amplitude) {
    std::vector<float> s(size_t(seconds * 48000));
    for (size_t i = 0; i < s.size(); ++i)
        s[i] = amplitude * float(std::sin(kTwoPi * hz * double(i) / 48000.0));
    return midgard::audio::wav_bytes(s, 1, 48000);
}

// How strong one frequency is in a stretch of interleaved stereo (left channel; Goertzel).
double tone_strength(const std::vector<float>& s, size_t from, size_t to, double hz) {
    const double k = 2.0 * std::cos(kTwoPi * hz / 48000.0);
    double s1 = 0.0, s2 = 0.0;
    size_t n = 0;
    for (size_t i = from; i < to && i * 2 < s.size(); ++i, ++n) {
        const double s0 = s[i * 2] + k * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return n ? 2.0 * std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - k * s1 * s2)) / double(n) : 0.0;
}

}  // namespace

TEST_CASE(
    "audio, the exit: walking the planet, footsteps per surface, ambience at dusk, music "
    "crossfading between biomes") {
    // The test planet, with a climate: warm grassland round the equator ("Warm"), cold snowfields
    // towards the pole ("Cold"). The player walks from the equator towards the pole. Its
    // footsteps sound like the ground (a tone for each surface); the music is a playlist for
    // each biome (crossfading when it changes); the ambience has a day layer and a night layer,
    // and the clock is put at dusk on the way. All of it is mixed offline, and heard.
    constexpr double kRadius = 1000.0;
    const glm::dvec3 centre(0.0, -kRadius, 0.0);
    const std::string body_json =
        R"({"shape": "sphere_outside", "radius": 1000, "seed": 13, "origin": [0, -1000, 0]})";
    const json terrain_json = json::parse(R"({"height_scale": 12, "base": 8,
        "climate": {"temperature": 14, "latitude_cooling": 60, "lapse_rate": 6.5,
                    "variation": 1, "season_swing": 0},
        "biomes": [{"name": "Warm", "temperature": [4, 60], "moisture": [0, 1],
                    "color": [0.3, 0.5, 0.2], "dry_color": [0.5, 0.5, 0.3]},
                   {"name": "Cold", "temperature": [-60, 4], "moisture": [0, 1],
                    "color": [0.8, 0.8, 0.8], "dry_color": [0.7, 0.7, 0.7]}]})");
    json body = json::parse(body_json);
    body["id"] = "b";
    const midgard::Terrain terrain(midgard::body_from_json(body),
                                   midgard::terrain_from_json(terrain_json));
    const auto vec = [](const glm::dvec3& p) { return json{p.x, p.y, p.z}; };
    const auto on_ground = [&](double angle) {  // from the equator (+X) towards the pole (+Y)
        const glm::dvec3 dir(std::cos(angle), std::sin(angle), 0.0);
        const glm::dvec3 g = terrain.ground(centre + dir * kRadius);
        return g + terrain.body().up(g) * 1.0;
    };
    const glm::dvec3 start = on_ground(0.0);
    // Sounds: a tone each, told apart by its pitch.
    const std::map<std::string, double> tones = {
        {"music_warm", 220}, {"music_cold", 330},  {"day", 440},
        {"night", 550},      {"step_grass", 1000}, {"step_snow", 1200},
        {"step_sand", 1400}, {"step_rock", 1600},  {"step_other", 1800}};
    std::vector<PackageFile> files;
    for (const auto& [name, hz] : tones) {
        const bool looped = name.rfind("music", 0) == 0 || name == "day" || name == "night";
        files.push_back(
            {"snd:" + name, "audio/" + name + ".wav", tone_wav(hz, looped ? 2.0 : 0.08, 0.3f)});
    }
    const json footsteps = {{"every", 2.0},
                            {"sounds",
                             {{"grass", "sfx_step_grass"},
                              {"snow", "sfx_step_snow"},
                              {"sand", "sfx_step_sand"},
                              {"rock", "sfx_step_rock"},
                              {"default", "sfx_step_other"}}}};
    const json player = {{"transform", {{"position", vec(start)}}},
                         {"player", {{"speed", 25}, {"height", 1.8}}},
                         {"footsteps", footsteps}};
    const json cam = {{"transform", {{"position", vec(start + glm::dvec3(0, 3, 6))}}},
                      {"camera", json::object()}};
    TestPackage pkg({{"e1_player", "NULL, '" + player.dump() + "'"},
                     {"e2_camera", "NULL, '" + cam.dump() + "'"}},
                    R"({"body": )" + body_json + R"(, "terrain": )" + terrain_json.dump() + "}", {},
                    files);
    {
        sqlite3* db = nullptr;
        sqlite3_open(utf8(pkg.dir / "Game.db").c_str(), &db);
        const auto event = [&](const std::string& id, const std::string& sound, const json& more) {
            json e = {{"sounds", {"snd:" + sound}}, {"spatial", false}};
            e.update(more);
            exec(db,
                 "INSERT INTO sound_event VALUES ('" + id + "', '" + id + "', '" + e.dump() + "')");
        };
        event("sfx_music_warm", "music_warm", {{"bus", "music"}, {"loop", true}});
        event("sfx_music_cold", "music_cold", {{"bus", "music"}, {"loop", true}});
        event("sfx_day", "day", {{"bus", "ambience"}, {"loop", true}});
        event("sfx_night", "night", {{"bus", "ambience"}, {"loop", true}});
        for (const char* s : {"grass", "snow", "sand", "rock", "other"})
            event(std::string("sfx_step_") + s, std::string("step_") + s, json::object());
        exec(db, R"(INSERT INTO music_playlist VALUES ('mus_warm', 'Warm',
                    '{"tracks": ["sfx_music_warm"], "biomes": ["Warm"], "crossfade": 2}'))");
        exec(db, R"(INSERT INTO music_playlist VALUES ('mus_cold', 'Cold',
                    '{"tracks": ["sfx_music_cold"], "biomes": ["Cold"], "crossfade": 2}'))");
        exec(db, R"(INSERT INTO ambience VALUES ('amb_world', 'World', '{"crossfade": 1,
                    "layers": [{"event": "sfx_day", "times": ["dawn", "day"]},
                               {"event": "sfx_night", "times": ["dusk", "night"]}]}'))");
        sqlite3_close(db);
    }
    midgard::Game game(pkg.dir);
    midgard::audio::MixerOptions offline;
    offline.device = false;
    midgard::audio::Mixer mixer(offline);
    game.set_audio(&mixer);
    midgard::Actions actions(midgard::InputMap::defaults());

    // A time of day, by trying the clock half an hour at a time.
    const auto put_clock_at = [&](const std::string& wanted) {
        const std::int64_t base = game.time().now();
        for (int h = 0; h < 96; ++h) {
            game.set_time(base + h * 1800);
            game.update(actions, 0.0f);
            if (game.music_context().time == wanted) return true;
        }
        return false;
    };
    REQUIRE(put_clock_at("day"));

    std::vector<float> heard;
    std::vector<float> block(800 * 2);  // (a sixtieth of a second, stereo)
    std::vector<double> mix_ms;
    int frame = 0;
    std::vector<std::string> biomes;  // the biome under the listener, each frame
    std::vector<std::string> playlists;
    std::vector<std::pair<std::string, std::string>> steps;  // (surface, sound)
    std::size_t seen = 0;
    const auto run = [&](int frames) {
        for (int i = 0; i < frames; ++i, ++frame) {
            game.update(actions, 1.0f / 60.0f);
            actions.begin_frame();
            const auto t0 = std::chrono::steady_clock::now();
            mixer.mix(block);
            mix_ms.push_back(
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                    .count());
            heard.insert(heard.end(), block.begin(), block.end());
            biomes.push_back(game.music_context().biome);
            playlists.push_back(game.music()->playing());
            const std::size_t taken = game.footsteps_taken();
            const auto& log = game.footsteps();
            for (std::size_t n = taken - seen; n > 0; --n)
                steps.emplace_back(log[log.size() - n].surface, log[log.size() - n].sound);
            seen = taken;
        }
    };
    run(120);  // settle, at the equator, by day
    CHECK(biomes.back() == "Warm");
    CHECK(playlists.back() == "mus_warm");
    // Walk towards the pole: 40 degrees of latitude, about 700 m.
    std::vector<glm::dvec3> path;
    for (int i = 1; i <= 80; ++i) path.push_back(on_ground(0.6981 * i / 80.0));
    game.follow_path(path);
    int dusk_at = -1;
    for (int i = 0; i < 2400; ++i) {
        run(1);
        if (dusk_at < 0 && i == 900) {  // half way: dusk falls
            REQUIRE(put_clock_at("dusk"));
            dusk_at = frame;
        }
    }
    const glm::dvec3 end = game.scene().world_position(game.scene().find("e1_player"));
    CHECK(glm::distance(end, path.back()) < 3.0);  // (it got there)
    CHECK(biomes.back() == "Cold");
    CHECK(game.take_sound_problems().empty());

    // Footsteps: one each 2 m (about 350), each the sound of the surface under it, grass
    // and snow both among them.
    std::map<std::string, int> surfaces;
    const std::map<std::string, std::string> sound_of = {{"grass", "sfx_step_grass"},
                                                         {"snow", "sfx_step_snow"},
                                                         {"sand", "sfx_step_sand"},
                                                         {"rock", "sfx_step_rock"}};
    for (const auto& [surface, sound] : steps) {
        ++surfaces[surface];
        const auto it = sound_of.find(surface);
        CHECK(sound == (it != sound_of.end() ? it->second : "sfx_step_other"));
    }
    CHECK(steps.size() > 300);
    CHECK(steps.size() < 400);
    CHECK(surfaces["grass"] > 20);
    CHECK(surfaces["snow"] > 20);

    // Music: Warm, then (two seconds after the biome stays Cold) Cold, crossfading over 2 s.
    int changed = -1;
    for (int f = 1; f < int(playlists.size()); ++f)
        if (playlists[f] == "mus_cold" && playlists[f - 1] == "mus_warm") changed = f;
    REQUIRE(changed > 0);
    int cold_from = changed;  // when the listener came into the Cold biome to stay
    while (cold_from > 0 && biomes[cold_from - 1] == "Cold") --cold_from;
    CHECK((changed - cold_from) / 60.0 == doctest::Approx(2.0).epsilon(0.05));
    const auto at = [](int f) { return size_t(f) * 800; };
    CHECK(tone_strength(heard, at(changed - 60), at(changed - 10), 220) > 0.2);
    CHECK(tone_strength(heard, at(changed - 60), at(changed - 10), 330) < 0.01);
    const double mid_old = tone_strength(heard, at(changed + 55), at(changed + 65), 220);
    const double mid_new = tone_strength(heard, at(changed + 55), at(changed + 65), 330);
    CHECK(mid_old == doctest::Approx(0.15).epsilon(0.35));  // (half way through the crossfade)
    CHECK(mid_new == doctest::Approx(0.15).epsilon(0.35));
    CHECK(tone_strength(heard, at(changed + 130), at(changed + 180), 220) < 0.01);
    CHECK(tone_strength(heard, at(changed + 130), at(changed + 180), 330) > 0.2);

    // Ambience: the day layer until dusk, then (over its 1 s crossfade) the night layer.
    REQUIRE(dusk_at > 0);
    CHECK(tone_strength(heard, at(dusk_at - 60), at(dusk_at - 5), 440) > 0.2);
    CHECK(tone_strength(heard, at(dusk_at - 60), at(dusk_at - 5), 550) < 0.01);
    CHECK(tone_strength(heard, at(dusk_at + 90), at(dusk_at + 150), 550) > 0.2);
    CHECK(tone_strength(heard, at(dusk_at + 90), at(dusk_at + 150), 440) < 0.01);

    // Mixing: well within each frame's time.
    std::sort(mix_ms.begin(), mix_ms.end());
    CHECK(mix_ms[mix_ms.size() / 2] < 1.0);
}

// ---- weather in the game (milestone 5.15) ------------------------------------------------------

TEST_CASE("the game's weather is the weather where the player is, as forecast") {
    const json player = {{"transform", {{"position", {0, 1, 0}}}}, {"player", json::object()}};
    TestPackage pkg({{"e1_player", "NULL, '" + player.dump() + "'"}},
                    R"({"weather": {"cell": 2000, "tables": [{"weather": {"clear": 1, "rain": 1}}],
                                    "lasts": {"default": [1, 3]}}})");
    midgard::Game game(pkg.dir);
    midgard::Actions actions(midgard::InputMap::defaults());
    REQUIRE(game.weather() != nullptr);
    game.update(actions, 0.0f);
    CHECK(game.weather_changes().empty());  // (the weather it starts with isn't a change)
    const std::string first = game.weather_now().weather;
    CHECK((first == "clear" || first == "rain"));
    CHECK_FALSE(game.weather_now().forced);
    REQUIRE(game.weather_type() != nullptr);
    CHECK(game.weather_type()->id == first);

    const std::int64_t start = game.time().now();
    const auto forecast = game.weather_forecast(48.0);
    REQUIRE(!forecast.empty());
    CHECK(forecast.front().start == start);
    CHECK(forecast.front().weather == first);
    CHECK(forecast.back().end == start + 48 * 3600);
    CHECK(forecast.size() >= 10);
    // The clock goes on, ten game minutes at a time: the weather is what the forecast said, and
    // each change is told once.
    size_t n = 0, changes = 0;
    std::string was = first;
    for (std::int64_t t = 600; t < 48 * 3600; t += 600) {
        game.advance_time(600);
        game.update(actions, 0.0f);
        while (start + t >= forecast[n].end) ++n;
        CHECK(game.weather_now().weather == forecast[n].weather);
        if (game.weather_now().weather != was) {
            REQUIRE(game.weather_changes().size() == 1);
            CHECK(game.weather_changes()[0].from.weather == was);
            CHECK(game.weather_changes()[0].to.weather == game.weather_now().weather);
            was = game.weather_now().weather;
            ++changes;
        } else {
            CHECK(game.weather_changes().empty());
        }
    }
    CHECK(changes >= 5);

    // Somewhere else, other weather (a cell of its own).
    const auto here = game.weather_cell();
    REQUIRE(here.has_value());
    midgard::Transform& t =
        game.scene().registry.get<midgard::Transform>(game.scene().find("e1_player"));
    t.position.x += 9000.0;
    CHECK(game.weather_cell() != here);
}

TEST_CASE("a script's weather is everywhere, for a while, then the weather comes back") {
    const json player = {{"transform", {{"position", {0, 1, 0}}}}, {"player", json::object()}};
    TestPackage pkg({{"e1_player", "NULL, '" + player.dump() + "'"}},
                    R"({"weather": {"tables": [{"weather": {"clear": 1}}]}})");
    midgard::Game game(pkg.dir);
    midgard::Actions actions(midgard::InputMap::defaults());
    game.update(actions, 0.0f);
    CHECK(game.weather_now() == midgard::weather::Now{"clear", -1, false});

    CHECK_FALSE(game.force_weather("frogs", 1.0));
    REQUIRE(game.force_weather("thunderstorm", 2.0));
    CHECK(game.weather_now() == midgard::weather::Now{"thunderstorm", -1, true});
    CHECK(game.weather_changes().empty());  // (told on the next update)
    game.update(actions, 0.0f);
    REQUIRE(game.weather_changes().size() == 1);
    CHECK(game.weather_changes()[0].from.weather == "clear");
    CHECK(game.weather_changes()[0].to.forced);
    const std::int64_t now = game.time().now();
    const auto forecast = game.weather_forecast(6.0);
    REQUIRE(forecast.size() == 2);
    CHECK(forecast[0] == midgard::weather::Spell{"thunderstorm", now, now + 7200, -1, true});
    CHECK(forecast[1].weather == "clear");
    CHECK(forecast[1].start == now + 7200);
    CHECK_FALSE(forecast[1].forced);

    game.advance_time(3600);
    game.update(actions, 0.0f);
    CHECK(game.weather_now().forced);
    game.advance_time(3601);
    game.update(actions, 0.0f);
    CHECK(game.weather_now() == midgard::weather::Now{"clear", -1, false});
    REQUIRE(game.weather_changes().size() == 1);
    CHECK(game.weather_changes()[0].from.weather == "thunderstorm");

    // Until released.
    REQUIRE(game.force_weather("fog", 0.0));
    game.advance_time(100 * 3600);
    game.update(actions, 0.0f);
    CHECK(game.weather_now().weather == "fog");
    CHECK(game.weather_forecast(24.0).size() == 1);
    game.release_weather();
    CHECK(game.weather_now().weather == "clear");
}

TEST_CASE("a scene without weather has none, until a script forces some; its sounds play") {
    std::vector<PackageFile> files{{"snd:rain", "audio/rain.wav", tone_wav(500, 2.0, 0.3f)}};
    TestPackage pkg({}, R"({"sky": [0.1, 0.2, 0.3]})", {}, files);
    {
        sqlite3* db = nullptr;
        sqlite3_open(utf8(pkg.dir / "Game.db").c_str(), &db);
        exec(db, R"(INSERT INTO sound_event VALUES ('sfx_rain', 'Rain',
                    '{"sounds": ["snd:rain"], "spatial": false, "loop": true,
                      "bus": "ambience"}'))");
        exec(db, R"(UPDATE weather_type SET weather = json_set(weather, '$.sounds',
                    json('[{"event": "sfx_rain", "volume": 0.5}]')) WHERE id = 'rain')");
        sqlite3_close(db);
    }
    midgard::Game game(pkg.dir);
    midgard::audio::MixerOptions offline;
    offline.device = false;
    midgard::audio::Mixer mixer(offline);
    game.set_audio(&mixer);
    midgard::Actions actions(midgard::InputMap::defaults());
    game.update(actions, 0.0f);
    CHECK(game.weather() == nullptr);
    CHECK(game.weather_now().weather.empty());
    CHECK(game.weather_forecast(5.0).size() == 1);

    REQUIRE(game.force_weather("rain", 0.0));
    game.update(actions, 0.1f);
    const auto playing = game.sounds()->playing_events();
    CHECK(std::count(playing.begin(), playing.end(), "sfx_rain") == 1);
    // It fades in, over a few seconds.
    std::vector<float> block(480 * 2);
    const auto loudness = [&] {
        double sum = 0.0;
        for (int i = 0; i < 10; ++i) {
            game.update(actions, 0.01f);
            mixer.mix(block);
            for (float s : block) sum += double(s) * s;
        }
        return std::sqrt(sum / (10.0 * double(block.size())));
    };
    const double early = loudness();
    for (int i = 0; i < 50; ++i) loudness();  // (fades go on as the sound is mixed)
    const double later = loudness();
    CHECK(later > 3.0 * early);
    CHECK(later > 0.01);

    // Clear weather has no sounds: the rain fades away.
    REQUIRE(game.force_weather("clear", 0.0));
    for (int i = 0; i < 60; ++i) loudness();
    CHECK(loudness() < 0.001);
    const auto after = game.sounds()->playing_events();
    CHECK(std::count(after.begin(), after.end(), "sfx_rain") == 0);
}

TEST_CASE("the weather's look darkens the sky and light, brings cloud and fog, and flashes") {
    midgard::render::Environment env;
    env.sky = true;
    env.fog.density = 0.001f;
    env.fog.height_falloff = 0.05f;
    midgard::render::Light light;
    light.intensity = 2.0f;
    // A clear sky changes nothing.
    {
        midgard::render::Environment e = env;
        midgard::render::Light l = light;
        midgard::apply(midgard::look_of(nullptr), e, l);
        CHECK(e.sky_colors.brightness == env.sky_colors.brightness);
        CHECK(e.sky_colors.zenith == env.sky_colors.zenith);
        CHECK(e.fog.density == env.fog.density);
        CHECK(e.clouds.cover == 0.0f);
        CHECK(l.intensity == light.intensity);
        CHECK(l.ambient == light.ambient);
    }
    const auto storm_type = midgard::weather::weather_type_from_json(
        "storm", "Storm",
        R"({"clouds": 1, "darkness": 0.75, "tint": [0.8, 0.8, 0.9], "visibility": 300})");
    const midgard::WeatherLook storm = midgard::look_of(&storm_type);
    CHECK(storm.fog == doctest::Approx(0.01));  // (5% of the light through 300 m)
    midgard::render::Environment e = env;
    midgard::render::Light l = light;
    midgard::apply(storm, e, l);
    CHECK(e.clouds.cover == 1.0f);
    CHECK(e.sky_colors.brightness < 0.5f * env.sky_colors.brightness);
    CHECK(e.sky_colors.sun_brightness == 0.0f);  // (behind the clouds)
    CHECK(e.fog.density == doctest::Approx(0.011));
    CHECK(e.fog.height_falloff < 0.01f);  // (rain fills the air, not only the valleys)
    CHECK(l.intensity < 0.3f * light.intensity);
    CHECK(l.ambient.x < light.ambient.x);
    CHECK(l.ambient.z / l.ambient.x > light.ambient.z / light.ambient.x);  // (bluer)
    // Halfway from clear to the storm.
    const midgard::WeatherLook half = midgard::blend(midgard::look_of(nullptr), storm, 0.5f);
    CHECK(half.clouds == doctest::Approx(0.5));
    CHECK(half.fog == doctest::Approx(0.005));
    // A flash lights everything for a moment.
    midgard::WeatherLook flash = storm;
    flash.flash = 1.0f;
    midgard::render::Environment fe = env;
    midgard::render::Light fl = light;
    midgard::apply(flash, fe, fl);
    CHECK(fl.ambient.x > l.ambient.x + 2.0f);
    CHECK(fe.sky_colors.brightness > 3.0f * e.sky_colors.brightness);
}

TEST_CASE("in a game the storm rolls in over seconds, lightning flashes and thunder follows") {
    std::vector<PackageFile> files{{"snd:thunder", "audio/thunder.wav", tone_wav(80, 0.4, 0.3f)}};
    TestPackage pkg({}, R"({"sky": [0.1, 0.2, 0.3]})", {}, files);
    {
        sqlite3* db = nullptr;
        sqlite3_open(utf8(pkg.dir / "Game.db").c_str(), &db);
        exec(db, R"(INSERT INTO sound_event VALUES ('sfx_thunder', 'Thunder',
                    '{"sounds": ["snd:thunder"], "spatial": false}'))");
        exec(db, R"(UPDATE weather_type SET weather = json_set(weather, '$.thunder',
                    'sfx_thunder') WHERE id = 'thunderstorm')");
        sqlite3_close(db);
    }
    midgard::Game game(pkg.dir);
    midgard::audio::MixerOptions offline;
    offline.device = false;
    midgard::audio::Mixer mixer(offline);
    game.set_audio(&mixer);
    midgard::Actions actions(midgard::InputMap::defaults());
    std::vector<float> block(800 * 2);  // (a 60th of a second)
    const auto frame = [&] {
        game.update(actions, 1.0f / 60.0f);
        mixer.mix(block);
    };
    frame();
    CHECK(game.weather_look().clouds == 0.0f);
    REQUIRE(game.force_weather("thunderstorm", 0.0));
    frame();
    const float early = game.weather_look().clouds;
    CHECK(early > 0.0f);
    CHECK(early < 0.1f);  // (it rolls in)
    for (int i = 0; i < 60 * 3; ++i) frame();
    const float three = game.weather_look().clouds;
    CHECK(three > 0.4f);
    CHECK(three < 0.8f);
    for (int i = 0; i < 60 * 20; ++i) frame();
    CHECK(game.weather_look().clouds > 0.99f);
    CHECK(game.environment(glm::dvec3(0.0)).clouds.cover > 0.99f);
    CHECK(game.light(glm::dvec3(0.0)).ambient.x < game.scene().light(glm::dvec3(0.0)).ambient.x);

    // Four flashes a minute: over three minutes, about twelve, each with its thunder.
    const std::size_t before = game.lightning_flashes();
    int lit = 0, thunders = 0;
    bool thundering = false;
    for (int i = 0; i < 60 * 180; ++i) {
        frame();
        lit += game.weather_look().flash > 0.5f;
        const auto playing = game.sounds()->playing_events();
        const bool now = std::count(playing.begin(), playing.end(), "sfx_thunder") > 0;
        thunders += now && !thundering;
        thundering = now;
    }
    const std::size_t flashes = game.lightning_flashes() - before;
    CHECK(flashes >= 5);
    CHECK(flashes <= 24);
    CHECK(lit >= int(flashes));      // (each flash is bright for a frame or more)
    CHECK(lit < int(flashes) * 12);  // (and only a moment)
    CHECK(thunders >= int(flashes) - 2);
    CHECK(thunders <= int(flashes));

    // Clear again: the clouds go, and the lightning stops.
    REQUIRE(game.force_weather("clear", 0.0));
    for (int i = 0; i < 60 * 30; ++i) frame();
    CHECK(game.weather_look().clouds < 0.1f);
    const std::size_t after = game.lightning_flashes();
    for (int i = 0; i < 60 * 60; ++i) frame();
    CHECK(game.lightning_flashes() == after);
}

TEST_CASE("rain eases in, gives way to snow, blows with the wind, and stops") {
    TestPackage pkg({}, R"({"sky": [0.1, 0.2, 0.3]})");
    midgard::Game game(pkg.dir);
    midgard::Actions actions(midgard::InputMap::defaults());
    using midgard::render::Falls;
    const auto run = [&](double seconds) {
        for (int i = 0; i < int(seconds * 60.0); ++i) game.update(actions, 1.0f / 60.0f);
    };
    run(0.1);
    CHECK(game.falling().kind == Falls::None);
    CHECK(game.environment(glm::dvec3(0.0)).falling.kind == Falls::None);

    REQUIRE(game.force_weather("rain", 0.0));  // (0.6 of a downpour, wind 5 m/s)
    run(1.0);
    CHECK(game.falling().kind == Falls::Rain);
    const float early = game.falling().amount;
    CHECK(early > 0.05f);
    CHECK(early < 0.4f);
    run(20.0);
    CHECK(game.falling().amount == doctest::Approx(0.6).epsilon(0.02));
    const glm::vec3 v = game.falling().velocity;
    CHECK(v.y == doctest::Approx(-9.0));  // (falling, down the world's up)
    CHECK(v.x > 1.0f);                    // (blown east)
    CHECK(v.x < 9.0f);
    const auto env = game.environment(glm::dvec3(0.0));
    CHECK(env.falling.kind == Falls::Rain);
    CHECK(env.falling.amount == game.falling().amount);
    // Moving: the rain carried by its velocity (wrapped far away).
    const glm::dvec3 before = game.falling().moved;
    run(1.0);
    const glm::dvec3 moved = game.falling().moved - before;
    CHECK(moved.y == doctest::Approx(-9.0).epsilon(0.02));

    // Snow: the rain fades away first, then the snow comes.
    REQUIRE(game.force_weather("snow", 0.0));
    run(1.0);
    CHECK(game.falling().kind == Falls::Rain);
    CHECK(game.falling().amount < 0.5f);
    run(20.0);
    CHECK(game.falling().kind == Falls::Snow);
    run(20.0);
    CHECK(game.falling().amount == doctest::Approx(0.5).epsilon(0.02));
    CHECK(game.falling().velocity.y == doctest::Approx(-1.2));

    REQUIRE(game.force_weather("clear", 0.0));
    run(30.0);
    CHECK(game.falling().kind == Falls::None);
    CHECK(game.falling().amount == 0.0f);
}

TEST_CASE("phones draw a quarter as much rain and snow") {
    using namespace midgard::render;
    CHECK(quality_settings(Quality::Desktop).falling == 32768);
    CHECK(quality_settings(Quality::Phone).falling == 8192);
    CHECK(falling_box(Falls::Rain) == 24.0f);
    for (const Falls f : {Falls::Rain, Falls::Snow, Falls::Ash, Falls::Sand})
        CHECK(std::fmod(480.0, double(falling_box(f))) == 0.0);  // (the game wraps at 480 m)
}
