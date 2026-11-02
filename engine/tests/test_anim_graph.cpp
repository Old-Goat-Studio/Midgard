// SPDX-License-Identifier: Apache-2.0
// Animation graphs (midgard/anim_graph.h; milestone 5.13): graphs read and refused; blend
// spaces in step; transitions on parameters, triggers and time, cross-fading; layers under a
// mask; events; root motion; and characters playing graphs, their speed set from how they move.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <glm/gtc/quaternion.hpp>

#include "midgard/anim_graph.h"
#include "midgard/characters.h"
#include "midgard/gltf_skin.h"
#include "skinned_chain.h"

using nlohmann::json;

namespace {

// hips (a metre up) > spine (half a metre above) > arm (half a metre along +X); and > leg.
midgard::RawSkeleton body() {
    midgard::RawSkeleton s;
    s.names = {"hips", "spine", "arm", "leg"};
    s.parents = {-1, 0, 1, 0};
    s.rest = {{{0, 1, 0}}, {{0, 0.5f, 0}}, {{0.5f, 0, 0}}, {{0, -0.5f, 0}}};
    return s;
}

// A clip holding `joint` turned `degrees` about Z for `duration` seconds.
midgard::RawClip turned(const std::string& name, const std::string& joint, float degrees,
                        float duration = 1.0f) {
    const glm::quat q = glm::angleAxis(glm::radians(degrees), glm::vec3(0, 0, 1));
    return {name, duration, {{joint, {}, {{0, q}, {duration, q}}, {}}}};
}

struct Rig {
    midgard::Skeleton skeleton = midgard::Skeleton::build(body());
    std::vector<midgard::Clip> clips;
    midgard::Pose pose{skeleton};

    explicit Rig(const std::vector<midgard::RawClip>& raw) {
        for (const auto& r : raw) clips.push_back(midgard::Clip::build(skeleton, r));
    }
    midgard::AnimPlayer player(const json& graph) {
        return midgard::AnimPlayer(
            std::make_shared<const midgard::AnimGraph>(midgard::AnimGraph::parse(graph)), skeleton,
            [this](const std::string& name) -> const midgard::Clip* {
                for (const auto& c : clips)
                    if (c.name() == name) return &c;
                return nullptr;
            },
            skeleton.find("hips"));
    }
    glm::vec3 at(const char* joint) const {
        return glm::vec3(pose.model()[size_t(skeleton.find(joint))][3]);
    }
    // The spine's turn about Z, in degrees (from where the arm is).
    float spine_turn() const {
        const glm::vec3 d = at("arm") - at("spine");
        return glm::degrees(std::atan2(d.y, d.x));
    }
};

// A graph of one layer ("base") from its states and transitions, as JSON text.
json one_layer(const std::string& start, const char* states, const char* transitions = "[]",
               const char* extra = "{}") {
    json g = json::parse(extra);
    json layer = json::object();
    layer["name"] = "base";
    layer["start"] = start;
    layer["states"] = json::parse(states);
    layer["transitions"] = json::parse(transitions);
    g["layers"] = json::array({layer});
    return g;
}

// The angle ozz's linear blend (then normalised) gives t of the way from 0 to `to` degrees.
float blended(float to, float t) {
    const glm::quat q = glm::normalize(glm::quat(1, 0, 0, 0) * (1.0f - t) +
                                       glm::angleAxis(glm::radians(to), glm::vec3(0, 0, 1)) * t);
    return glm::degrees(glm::angle(q));
}

}  // namespace

TEST_CASE("animation graphs read, and refused with a reason") {
    const json g = one_layer("idle", R"({
        "idle": {"clip": "Idle"},
        "move": {"blend": "speed", "clips": [["Run", 4.0], ["Walk", 1.4]]}})",
                             R"([
        {"from": "idle", "to": "move", "when": [["speed", ">", 0.1]]},
        {"from": "*", "to": "idle", "when": [["stop"]], "blend": 0.5}])",
                             R"({"parameters": {"crouch": false}, "triggers": ["stop"]})");
    const midgard::AnimGraph graph = midgard::AnimGraph::parse(g);
    REQUIRE(graph.layers.size() == 1);
    const midgard::AnimLayer& l = graph.layers[0];
    CHECK(l.states.size() == 2);
    CHECK(l.states[size_t(l.start)].name == "idle");
    CHECK(l.states[1].clips.front().first == "Walk");  // (sorted by where)
    CHECK(l.transitions[1].from == -1);
    CHECK(l.transitions[1].blend == 0.5f);
    CHECK(graph.parameters.at("crouch") == 0.0f);
    CHECK(graph.parameters.contains("speed"));  // (always there)
    CHECK(graph.clips() == std::set<std::string>{"Idle", "Run", "Walk"});
    const auto refused = [](const json& bad, const char* says) {
        try {
            (void)midgard::AnimGraph::parse(bad);
            FAIL("read: " << bad.dump());
        } catch (const midgard::AnimationError& e) {
            CHECK_MESSAGE(std::string(e.what()).find(says) != std::string::npos, e.what());
        }
    };
    const char* one = R"({"a": {"clip": "A"}})";
    refused(json::object(), "needs 'layers'");
    refused(one_layer("a", one, R"([{"from": "a", "to": "b", "after": 1}])"), "no state 'b'");
    refused(one_layer("a", R"({"a": {"blend": "height", "clips": [["A", 0]]}})"),
            "no parameter 'height'");
    refused(one_layer("a", R"({"a": {"clip": "A", "blend": "speed"}})"), "plays a 'clip', or");
    refused(one_layer("a", one, R"([{"from": "a", "to": "a"}])"), "needs a condition");
    refused(one_layer("a", one, R"([{"to": "a", "when": [["jump", ">", 1]]}])",
                      R"({"triggers": ["jump"]})"),
            "is only [name]");
    refused(one_layer("a", one, R"([{"to": "a", "when": [["speed", "~", 1]]}])"), "op is one of");
    refused(one_layer("", R"({"a": {"clip": "A"}, "b": {"clip": "B"}})"), "needs a 'start'");
    CHECK_THROWS_AS(midgard::AnimGraph::parse(std::string("{nope")), midgard::AnimationError);
}

TEST_CASE("a blend space plays its clips in step, weighted by a parameter") {
    Rig rig({turned("Low", "spine", 0.0f, 1.0f), turned("High", "spine", 60.0f, 2.0f)});
    midgard::AnimPlayer p = rig.player(
        one_layer("move", R"({"move": {"blend": "lean", "clips": [["Low", 0], ["High", 10]]}})",
                  "[]", R"({"parameters": {"lean": 0}})"));
    p.update(0.0f, rig.pose);
    CHECK(rig.spine_turn() == doctest::Approx(0.0f).epsilon(0.01));
    p.set("lean", 10.0f);
    p.update(0.0f, rig.pose);
    CHECK(rig.spine_turn() == doctest::Approx(60.0f).epsilon(0.01));
    p.set("lean", 20.0f);  // beyond the last: the last
    p.update(0.0f, rig.pose);
    CHECK(rig.spine_turn() == doctest::Approx(60.0f).epsilon(0.01));
    p.set("lean", 5.0f);  // half way: blended
    p.update(0.0f, rig.pose);
    CHECK(rig.spine_turn() == doctest::Approx(blended(60.0f, 0.5f)).epsilon(0.01));
    CHECK_THROWS_AS(p.set("height", 1.0f), midgard::AnimationError);
    CHECK(p.get("lean") == 5.0f);
}

TEST_CASE("transitions: on parameters, triggers and time, cross-fading") {
    Rig rig({turned("Rest", "spine", 0.0f), turned("Bend", "spine", 90.0f),
             turned("Once", "spine", 45.0f, 0.5f)});
    midgard::AnimPlayer p = rig.player(one_layer("rest", R"({
        "rest": {"clip": "Rest"},
        "bend": {"clip": "Bend"},
        "once": {"clip": "Once", "loop": false}})",
                                                 R"([
        {"from": "rest", "to": "bend", "when": [["bent", "==", true]], "blend": 0.4},
        {"from": "bend", "to": "once", "when": [["go"]], "blend": 0},
        {"from": "once", "to": "rest", "after": 1.0, "blend": 0},
        {"from": "*", "to": "rest", "when": [["speed", ">", 9]], "blend": 0}])",
                                                 R"({"parameters": {"bent": false},
                                                     "triggers": ["go"]})"));
    p.update(0.1f, rig.pose);
    CHECK(p.state() == "rest");
    p.set("bent", 1.0f);
    p.update(0.0f, rig.pose);  // into bend, fading in over 0.4 s: nothing yet
    CHECK(p.state() == "bend");
    CHECK(rig.spine_turn() == doctest::Approx(0.0f).epsilon(0.01));
    p.update(0.2f, rig.pose);  // half way
    CHECK(rig.spine_turn() == doctest::Approx(blended(90.0f, 0.5f)).epsilon(0.01));
    p.update(0.3f, rig.pose);  // and done
    CHECK(rig.spine_turn() == doctest::Approx(90.0f).epsilon(0.01));
    // A trigger: used up by the transition it fires.
    p.trigger("go");
    CHECK(p.get("go") == 1.0f);
    p.update(0.1f, rig.pose);
    CHECK(p.state() == "once");
    CHECK(p.get("go") == 0.0f);
    CHECK_THROWS_AS(p.trigger("jump"), midgard::AnimationError);
    // "after": once all of it has played (0.5 s), back to rest.
    p.update(0.3f, rig.pose);
    CHECK(p.state() == "once");
    p.update(0.3f, rig.pose);  // (0.7 s played)
    p.update(0.0f, rig.pose);
    CHECK(p.state() == "rest");
    // From anywhere (but not to itself).
    p.update(0.5f, rig.pose);
    CHECK(p.state() == "bend");  // (still bent)
    p.set("speed", 10.0f);
    p.update(0.0f, rig.pose);
    CHECK(p.state() == "rest");
}

TEST_CASE("a layer over the first moves the joints under its mask, by its weight") {
    Rig rig({turned("Lean", "hips", 30.0f), turned("Wave", "spine", 90.0f)});
    json g = one_layer("lean", R"({"lean": {"clip": "Lean"}})", "[]",
                       R"({"parameters": {"waving": 0}})");
    g["layers"].push_back(json::parse(R"({"name": "upper", "mask": "spine", "weight": "waving",
                                          "states": {"wave": {"clip": "Wave"}}})"));
    midgard::AnimPlayer p = rig.player(g);
    const auto leg = [&] {  // the leg's direction from the hips (they lean 30 degrees)
        const glm::vec3 d = rig.at("leg") - rig.at("hips");
        return glm::degrees(std::atan2(d.x, -d.y));
    };
    p.update(0.0f, rig.pose);
    CHECK(rig.spine_turn() == doctest::Approx(30.0f).epsilon(0.01));  // the spine follows
    p.set("waving", 1.0f);
    p.update(0.0f, rig.pose);
    // The spine waves (90 from the hips: 120 in all); the leg, not under the mask, doesn't.
    CHECK(rig.spine_turn() == doctest::Approx(120.0f).epsilon(0.01));
    CHECK(leg() == doctest::Approx(30.0f).epsilon(0.01));
    CHECK(p.state(1) == "wave");
    p.set("waving", 0.5f);  // half its weight: half way
    p.update(0.0f, rig.pose);
    CHECK(rig.spine_turn() == doctest::Approx(30.0f + blended(90.0f, 0.5f)).epsilon(0.01));
}

TEST_CASE("events fire as a state passes them") {
    Rig rig({turned("Walk", "spine", 0.0f, 1.0f), turned("Jump", "spine", 0.0f, 1.0f)});
    midgard::AnimPlayer p = rig.player(one_layer("walk", R"({
        "walk": {"clip": "Walk", "events": [[0.0, "left"], [0.5, "right"]]},
        "jump": {"clip": "Jump", "loop": false, "events": [[0.25, "push"]]}})",
                                                 R"([
        {"from": "walk", "to": "jump", "when": [["jumping"]], "blend": 0}])",
                                                 R"({"parameters": {"jumping": false}})"));
    CHECK(p.update(0.1f, rig.pose).events == std::vector<std::string>{"left"});
    CHECK(p.update(0.3f, rig.pose).events.empty());
    CHECK(p.update(0.2f, rig.pose).events == std::vector<std::string>{"right"});  // (0.6)
    // A long step passes several (and round the loop).
    CHECK(p.update(1.0f, rig.pose).events == std::vector<std::string>{"left", "right"});
    p.set("jumping", 1.0f);
    CHECK(p.update(0.5f, rig.pose).events == std::vector<std::string>{"push"});
    CHECK(p.update(2.0f, rig.pose).events.empty());  // (once: it doesn't loop)
}

TEST_CASE("root motion moves the character as its clip moves its hips") {
    // Hips 2 m forward over each second (and back to the start as it loops).
    midgard::RawClip walk{"Walk", 1.0f, {{"hips", {{0, {0, 1, 0}}, {1, {0, 1, 2}}}, {}, {}}}};
    Rig rig({walk});
    const char* states = R"({"walk": {"clip": "Walk"}})";
    midgard::AnimPlayer p = rig.player(one_layer("walk", states, "[]", R"({"root_motion": true})"));
    glm::vec3 moved(0.0f);
    for (int i = 0; i < 15; ++i) {
        moved += p.update(0.1f, rig.pose).motion;
        // In place: the hips stay over the character's origin.
        CHECK(glm::length(glm::vec3(rig.at("hips").x, 0, rig.at("hips").z)) < 5e-3f);
    }
    CHECK(moved.z == doctest::Approx(3.0f).epsilon(0.01));  // 1.5 s at 2 m/s, through a loop
    CHECK(std::abs(moved.x) < 1e-3f);
    // Without it: the pose moves, the character doesn't.
    midgard::AnimPlayer still = rig.player(one_layer("walk", states));
    CHECK(still.update(0.5f, rig.pose).motion == glm::vec3(0.0f));
    CHECK(rig.at("hips").z == doctest::Approx(1.0f).epsilon(0.01));
}

TEST_CASE("characters play graphs: their speed from how they move, scripts' parameters") {
    const std::vector<std::uint8_t> glb = chain_glb();  // (its clip: "Bend")
    const midgard::Characters::Bytes bytes = [&](const std::string&) { return glb; };
    midgard::Scene scene;
    const json graph = one_layer("still", R"({
        "still": {"clip": "Bend", "speed": 0},
        "moving": {"clip": "Bend", "events": [[0.5, "half"]]}})",
                                 R"([
        {"from": "still", "to": "moving", "when": [["speed", ">", 1]], "blend": 0},
        {"from": "moving", "to": "still", "when": [["stop"]], "blend": 0}])",
                                 R"({"triggers": ["stop"], "parameters": {"mood": 0}})");
    const auto add = [&](const char* id, const json& g) {
        json character = json::object();
        character["model"] = "asset:chain.glb";
        character["graph"] = g;
        json components = json::object();
        components["transform"] = json::object();
        components["character"] = character;
        scene.put_entity(id, id, "", components);
        return scene.find(id);
    };
    const entt::entity a = add("a", graph);
    json broken = graph;
    broken["layers"][0]["start"] = "flying";
    add("b", broken);
    json missing = graph;
    missing["layers"][0]["states"]["still"]["clip"] = "Sit";
    add("c", missing);
    midgard::Characters characters;
    const std::vector<std::string> problems = characters.load(scene, bytes);
    REQUIRE(problems.size() == 2);
    const auto said = [&](const char* what) {
        return std::any_of(problems.begin(), problems.end(),
                           [&](const std::string& p) { return p.find(what) != std::string::npos; });
    };
    CHECK(said("no state 'flying'"));
    CHECK(said("plays 'Sit'"));
    characters.set_parameter(a, "mood", 3.0f);  // (before it starts: kept for it)
    characters.update(scene, 0.0f);
    CHECK(characters.state_of(a) == "still");
    CHECK(characters.parameter(a, "mood") == 3.0f);
    CHECK(characters.state_of(scene.find("b")).empty());  // (its graph is broken: plays clip)
    // It moves at 2 m/s: its speed follows, and it starts moving.
    auto& t = scene.registry.get<midgard::Transform>(a);
    for (int i = 0; i < 20; ++i) {
        t.position.x += 0.2;
        characters.update(scene, 0.1f);
    }
    CHECK(characters.parameter(a, "speed") == doctest::Approx(2.0f).epsilon(0.02));
    CHECK(characters.state_of(a) == "moving");
    bool half = false;
    for (int i = 0; i < 10 && !half; ++i) {
        characters.update(scene, 0.1f);
        for (const auto& ev : characters.events()) half |= ev.entity == a && ev.name == "half";
    }
    CHECK(half);
    characters.trigger(a, "stop");
    characters.update(scene, 0.1f);
    CHECK(characters.state_of(a) == "still");
    CHECK_THROWS_AS(characters.set_parameter(a, "nope", 1.0f), midgard::AnimationError);
}

TEST_CASE("a 2D blend space weighs its clips by how near their points are") {
    Rig rig({turned("Ahead", "spine", 0.0f, 1.0f), turned("Side", "spine", 60.0f, 2.0f),
             turned("Back", "spine", -60.0f, 1.0f)});
    midgard::AnimPlayer p = rig.player(one_layer("move", R"({"move": {"blend": ["x", "y"],
                    "clips": [["Ahead", 0, 1], ["Side", 1, 0], ["Back", 0, -1]]}})",
                                                 "[]", R"({"parameters": {"x": 0, "y": 1}})"));
    p.update(0.0f, rig.pose);
    CHECK(rig.spine_turn() == doctest::Approx(0.0f).epsilon(0.01));  // on Ahead's point
    p.set("x", 1.0f);
    p.set("y", 0.0f);
    p.update(0.0f, rig.pose);
    CHECK(rig.spine_turn() == doctest::Approx(60.0f).epsilon(0.01));  // on Side's
    // Half way between Ahead and Side: as near each (and Back far off, a little of it).
    p.set("x", 0.5f);
    p.set("y", 0.5f);
    p.update(0.0f, rig.pose);
    const float between = rig.spine_turn();
    CHECK(between > 20.0f);
    CHECK(between < 30.0f);  // (Back pulls it below the even 30)
    // Refused: a blend by three, a clip without its y.
    const auto refused = [](const char* states, const char* says) {
        try {
            (void)midgard::AnimGraph::parse(
                one_layer("a", states, "[]", R"({"parameters": {"x": 0, "y": 0}})"));
            FAIL("read");
        } catch (const midgard::AnimationError& e) {
            CHECK_MESSAGE(std::string(e.what()).find(says) != std::string::npos, e.what());
        }
    };
    refused(R"({"a": {"blend": ["x", "y", "x"], "clips": [["A", 0, 0]]}})", "or two of them");
    refused(R"({"a": {"blend": ["x", "y"], "clips": [["A", 0]]}})", "[clip, x, y]");
    refused(R"({"a": {"blend": ["x", "z"], "clips": [["A", 0, 0]]}})", "no parameter 'z'");
}
