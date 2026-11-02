// SPDX-License-Identifier: Apache-2.0
// First-person arms (midgard/characters.h; milestone 5.13): a skinned mesh cut to some joints;
// the Superhero's arms cut from its body, held at the camera by its eye and in step with its
// body (the same joints); and a rig of their own whose graph follows the body's states.
#include <doctest/doctest.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "midgard/characters.h"
#include "midgard/gltf_skin.h"
#include "skinned_chain.h"

using nlohmann::json;

namespace {

std::vector<std::uint8_t> read(const std::string& name) {
    std::ifstream in(std::string(MIDGARD_ASSETS_DIR "/third_party/characters/quaternius/") + name,
                     std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), {});
}

// A vertex where the skin puts it, through a model matrix.
glm::vec3 skinned(const glm::mat4& model, std::span<const glm::mat4> skin,
                  const midgard::render::Vertex& v, const glm::u16vec4& j, const glm::vec4& w) {
    glm::mat4 m(0.0f);
    for (int k = 0; k < 4; ++k) m += w[k] * skin[j[k]];
    return glm::vec3(model * m * glm::vec4(v.position[0], v.position[1], v.position[2], 1.0f));
}

}  // namespace

TEST_CASE("a skinned mesh cut down to the triangles on some joints") {
    midgard::render::MeshData mesh;
    for (int i = 0; i < 6; ++i) {
        midgard::render::Vertex v{};
        v.position[0] = float(i);
        mesh.vertices.push_back(v);
    }
    mesh.indices = {0, 1, 2, 3, 4, 5, 2, 3, 4};
    // Vertices 0-2 hang from joint 1, 3-5 from joint 0 (mostly).
    std::vector<glm::u16vec4> joints(6, glm::u16vec4(0, 1, 0, 0));
    std::vector<glm::vec4> weights = {{0.1f, 0.9f, 0, 0}, {0, 1, 0, 0}, {0.4f, 0.6f, 0, 0},
                                      {1, 0, 0, 0},       {1, 0, 0, 0}, {0.7f, 0.3f, 0, 0}};
    midgard::render::MeshData out;
    std::vector<glm::u16vec4> out_joints;
    std::vector<glm::vec4> out_weights;
    midgard::Characters::cut_skinned(mesh, joints, weights, {false, true}, out, out_joints,
                                     out_weights);
    CHECK(out.indices == std::vector<std::uint32_t>{0, 1, 2});  // (the mixed one goes)
    REQUIRE(out.vertices.size() == 3);
    CHECK(out.vertices[2].position[0] == 2.0f);
    CHECK(out_weights[0] == glm::vec4(0.1f, 0.9f, 0, 0));
}

TEST_CASE("first-person arms: cut from the body, held at the camera, in step with the body") {
    const auto hero = read("superhero_male.glb"), library = read("animation_library.glb");
    midgard::Scene scene;
    json c = {{"model", "asset:hero.glb"}, {"clips", "asset:library.glb"}, {"clip", "Idle_Loop"}};
    c["first_person"] = json::object();
    json components = json::object();
    components["transform"] = {{"position", {3, 0, 2}}, {"rotation", {0, 40, 0}}};
    components["character"] = c;
    scene.put_entity("player", "player", "", components);
    const entt::entity player = scene.find("player");
    midgard::Characters characters;
    REQUIRE(characters
                .load(scene,
                      [&](const std::string& a) { return a == "asset:hero.glb" ? hero : library; })
                .empty());
    characters.set_player(player);
    characters.update(scene, 0.4f);
    // Held at a camera turned and placed anywhere.
    const glm::mat4 camera = glm::translate(glm::mat4(1.0f), glm::vec3(10, 2, -4)) *
                             glm::rotate(glm::mat4(1.0f), 0.7f, glm::vec3(0, 1, 0));
    const auto view = characters.first_person(scene, player, camera);
    REQUIRE(view);
    // The arms only: fewer vertices than the body, each hanging from a shoulder's joints.
    const midgard::SkinnedModel model = midgard::load_skinned_glb(hero);
    const midgard::Skeleton skeleton = midgard::Skeleton::build(model.skeleton);
    REQUIRE(view->mesh);
    const auto& arms = *view->mesh;
    CHECK(arms.vertices.size() > 500);
    CHECK(arms.vertices.size() < model.mesh.vertices.size() / 2);
    // In step with the body (its own joints): each vertex as far from the camera as the body's
    // is from its head.
    const midgard::Pose& pose = *characters.pose(player);
    const glm::vec3 head =
        glm::vec3(model.armature * pose.model()[size_t(skeleton.find("Head"))][3]);
    const glm::mat4 inverse_camera = glm::inverse(camera);
    // The body's own skinned vertices (where the body draws them, in the model).
    const auto body_vertex = [&](size_t i) {
        // (the arms' vertex i is some body vertex: found by its rest position)
        for (size_t b = 0; b < model.mesh.vertices.size(); ++b)
            if (std::equal(std::begin(model.mesh.vertices[b].position),
                           std::end(model.mesh.vertices[b].position),
                           std::begin(arms.vertices[i].position)))
                return skinned(model.armature, view->skin, model.mesh.vertices[b], model.joints[b],
                               model.weights[b]);
        return glm::vec3(1e9f);
    };
    for (size_t i = 0; i < arms.vertices.size(); i += 97) {
        // (the arms' mesh keeps the body's skin data: same joints, same weights)
        const midgard::Characters::FirstPersonView& v = *view;
        const glm::vec3 body = body_vertex(i);
        REQUIRE(body.x < 1e8f);
        const glm::vec3 drawn = glm::vec3(
            inverse_camera *
            glm::vec4(glm::vec3(v.model * glm::inverse(model.armature) * glm::vec4(body, 1.0f)),
                      1.0f));
        CHECK(glm::length(drawn) == doctest::Approx(glm::length(body - head)).epsilon(1e-3));
    }
    // The character's left hand (+X in the model) on the camera's left (-X), below the eye.
    const glm::vec3 hand =
        glm::vec3(model.armature * pose.model()[size_t(skeleton.find("hand_l"))][3]);
    const glm::vec3 seen = glm::vec3(
        inverse_camera *
        glm::vec4(glm::vec3(view->model * glm::inverse(model.armature) * glm::vec4(hand, 1.0f)),
                  1.0f));
    CHECK(seen.x < -0.1f);
    CHECK(seen.y < -0.3f);
    // Not the player: no arms.
    characters.set_player(entt::null);
    characters.update(scene, 0.0f);
    CHECK_FALSE(characters.first_person(scene, player, camera));
}

TEST_CASE("first-person arms of their own follow the body's graph") {
    const auto hero = read("superhero_male.glb"), library = read("animation_library.glb");
    const auto arms = chain_glb();  // (its clip: "Bend")
    midgard::Scene scene;
    const json body_graph = json::parse(R"({"parameters": {"go": 0}, "layers": [{"name": "base",
        "start": "still", "states": {"still": {"clip": "Idle_Loop"},
                                     "moving": {"clip": "Walk_Loop"}},
        "transitions": [{"from": "still", "to": "moving", "when": [["go"]], "blend": 0}]}]})");
    const json arms_graph = json::parse(R"({"layers": [{"name": "arms", "start": "still",
        "states": {"still": {"clip": "Bend", "speed": 0}, "moving": {"clip": "Bend"}}}]})");
    json c = {{"model", "asset:hero.glb"}, {"clips", "asset:library.glb"}, {"graph", body_graph}};
    c["first_person"] = {{"model", "asset:arms.glb"}, {"graph", arms_graph}, {"eye", "j2"}};
    json components = json::object();
    components["transform"] = json::object();
    components["character"] = c;
    scene.put_entity("player", "player", "", components);
    const entt::entity player = scene.find("player");
    midgard::Characters characters;
    REQUIRE(
        characters
            .load(scene,
                  [&](const std::string& a) {
                      return a == "asset:hero.glb" ? hero : a == "asset:arms.glb" ? arms : library;
                  })
            .empty());
    characters.set_player(player);
    characters.update(scene, 0.1f);
    auto view = characters.first_person(scene, player, glm::mat4(1.0f));
    REQUIRE(view);
    CHECK(view->state == "still");
    CHECK(view->mesh->vertices.size() == 3);  // (its own mesh, whole)
    characters.set_parameter(player, "go", 1.0f);
    characters.update(scene, 0.1f);
    CHECK(characters.state_of(player) == "moving");
    view = characters.first_person(scene, player, glm::mat4(1.0f));
    CHECK(view->state == "moving");  // in step
}
