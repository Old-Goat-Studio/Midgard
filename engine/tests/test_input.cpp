// SPDX-License-Identifier: Apache-2.0
#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

#include <sqlite3.h>

#include "midgard/input.h"
#include "midgard/third_person.h"

namespace fs = std::filesystem;
using midgard::Actions;
using midgard::InputMap;

TEST_CASE("keys drive actions, opposite keys cancel") {
    Actions a(InputMap::defaults());
    a.set("keyboard", "W", 1.0f);
    CHECK(a.axis("move_y") == doctest::Approx(1.0f));
    a.set("keyboard", "S", 1.0f);
    CHECK(a.axis("move_y") == doctest::Approx(0.0f));
    a.set("keyboard", "W", 0.0f);
    CHECK(a.axis("move_y") == doctest::Approx(-1.0f));
}

TEST_CASE("keyboard and stick together never exceed full speed") {
    Actions a(InputMap::defaults());
    a.set("keyboard", "D", 1.0f);
    a.set("gamepad", "leftx", 1.0f);
    CHECK(a.axis("move_x") == doctest::Approx(1.0f));
}

TEST_CASE("mouse deltas last one frame, stick look is per second") {
    Actions a(InputMap::defaults());
    a.add_delta("mouse", "delta_x", 10.0f);
    CHECK(a.axis("look_x", 0.016f) == doctest::Approx(1.5f));  // 10 px * 0.15 degrees
    a.begin_frame();
    CHECK(a.axis("look_x", 0.016f) == doctest::Approx(0.0f));
    a.set("gamepad", "rightx", 0.5f);
    CHECK(a.axis("look_x", 0.1f) == doctest::Approx(6.0f));  // 0.5 * 120 deg/s * 0.1 s
}

TEST_CASE("pressed fires only on the frame a button goes down") {
    Actions a(InputMap::defaults());
    a.begin_frame();
    a.set("keyboard", "Space", 1.0f);
    CHECK(a.pressed("jump"));
    CHECK(a.down("jump"));
    a.begin_frame();
    CHECK_FALSE(a.pressed("jump"));
    CHECK(a.down("jump"));
}

TEST_CASE("bindings load from a package database, with defaults for old packages") {
    std::random_device rd;
    const fs::path dir = fs::temp_directory_path() / ("midgard_input_" + std::to_string(rd()));
    fs::create_directories(dir);
    const fs::path db_file = dir / "Game.db";
    sqlite3* db = nullptr;
    const std::u8string u8 = db_file.u8string();
    sqlite3_open(std::string(u8.begin(), u8.end()).c_str(), &db);
    std::vector<fs::path> migrations;
    for (const auto& e : fs::directory_iterator(fs::path(MIDGARD_SCHEMA_DIR) / "migrations"))
        migrations.push_back(e.path());
    std::sort(migrations.begin(), migrations.end());
    for (const auto& m : migrations) {
        std::ifstream in(m);
        std::stringstream sql;
        sql << in.rdbuf();
        REQUIRE(sqlite3_exec(db, sql.str().c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
    }
    sqlite3_exec(db, "UPDATE input_binding SET control = 'Up' WHERE control = 'W'", nullptr,
                 nullptr, nullptr);
    sqlite3_close(db);

    Actions a(InputMap::load(db_file));
    a.set("keyboard", "Up", 1.0f);
    CHECK(a.axis("move_y") == doctest::Approx(1.0f));  // the package's own binding is used
    a.set("keyboard", "Up", 0.0f);
    a.set("keyboard", "W", 1.0f);
    CHECK(a.axis("move_y") == doctest::Approx(0.0f));

    std::error_code ec;
    fs::remove_all(dir, ec);
}

namespace {

midgard::Scene player_scene() {
    midgard::Scene scene;
    const auto player = scene.registry.create();
    scene.registry.emplace<midgard::EntityInfo>(player, "e1", "Player", entt::null);
    scene.registry.emplace<midgard::Transform>(player);
    scene.registry.emplace<midgard::Player>(player, 4.0f);
    const auto camera = scene.registry.create();
    scene.registry.emplace<midgard::EntityInfo>(camera, "e2", "Camera", entt::null);
    scene.registry.emplace<midgard::Transform>(camera, glm::vec3(0.0f, 0.5f, 6.0f));
    scene.registry.emplace<midgard::CameraLens>(camera);
    return scene;
}

glm::vec3 position_of(const midgard::Scene& scene, const char* id) {
    for (auto e : scene.registry.view<const midgard::EntityInfo>())
        if (scene.registry.get<midgard::EntityInfo>(e).id == id)
            return scene.registry.get<midgard::Transform>(e).position;
    return glm::vec3(NAN);
}

}  // namespace

TEST_CASE("the third-person camera starts where the designer placed it") {
    midgard::Scene scene = player_scene();
    midgard::ThirdPersonCamera tp(scene);
    REQUIRE(tp.active());
    CHECK(tp.distance() == doctest::Approx(6.0f));
    CHECK(tp.yaw() == doctest::Approx(0.0f));
    CHECK(tp.elevation() == doctest::Approx(0.0f));
}

TEST_CASE("forward moves the player away from the camera, and the camera follows") {
    midgard::Scene scene = player_scene();
    midgard::ThirdPersonCamera tp(scene);
    Actions a(InputMap::defaults());
    a.set("keyboard", "W", 1.0f);
    tp.update(scene, a, 0.5f);
    const glm::vec3 player = position_of(scene, "e1");
    CHECK(player.z == doctest::Approx(-2.0f));  // 4 m/s for 0.5 s, along -Z (away from camera)
    const glm::vec3 camera = position_of(scene, "e2");
    CHECK(glm::length(camera - (player + glm::vec3(0, 0.5f, 0))) == doctest::Approx(6.0f));
}

TEST_CASE("look orbits, zoom changes distance, and both are limited") {
    midgard::Scene scene = player_scene();
    midgard::ThirdPersonCamera tp(scene);
    Actions a(InputMap::defaults());
    a.add_delta("mouse", "delta_x", 100.0f);  // 15 degrees
    tp.update(scene, a, 0.016f);
    CHECK(tp.yaw() == doctest::Approx(-15.0f));
    // Mouse up = look up, which lowers the camera behind the player; far past the limits:
    a.begin_frame();
    a.add_delta("mouse", "delta_y", -10000.0f);
    tp.update(scene, a, 0.016f);
    CHECK(tp.elevation() == doctest::Approx(midgard::ThirdPersonCamera::kMinElevation));
    a.begin_frame();
    a.add_delta("mouse", "delta_y", 10000.0f);  // mouse down = look down, camera rises
    tp.update(scene, a, 0.016f);
    CHECK(tp.elevation() == doctest::Approx(midgard::ThirdPersonCamera::kMaxElevation));
    a.begin_frame();
    a.add_delta("mouse", "wheel", 100.0f);  // zoom right in
    tp.update(scene, a, 0.016f);
    CHECK(tp.distance() == doctest::Approx(midgard::ThirdPersonCamera::kMinDistance));
}

TEST_CASE("scenes without a player have no third-person camera") {
    midgard::Scene scene;
    CHECK_FALSE(midgard::ThirdPersonCamera(scene).active());
}
