// SPDX-License-Identifier: Apache-2.0
// Animation graphs (milestone 5.13): what a character plays, decided as it plays.
//
// A graph has parameters (numbers; true and false are 1 and 0) and triggers (set by a script,
// used up by the transition they fire), and layers. Each layer is a state machine: states play
// a clip, or blend clips by a parameter (a blend space: Idle at 0 m/s, Walk at 1.4, Run at 4.5,
// played in step so their feet agree); transitions move between them when their conditions
// hold (and, optionally, once enough of the state has played), cross-fading over a time. Layers
// above the first replace its pose on the joints under their mask (an upper body waving while
// the legs walk), by their weight. Events fire as a state passes points in its cycle (a
// footstep); root motion moves the character as its clips move their hips, and keeps them in
// place in the pose.
//
//   {"parameters": {"speed": 0, "crouch": false}, "triggers": ["jump"], "root_motion": false,
//    "layers": [{"name": "base", "start": "move",
//      "states": {
//        "move": {"blend": "speed", "clips": [["Idle_Loop", 0], ["Walk_Loop", 1.4],
//                                             ["Sprint_Loop", 6.0]],
//                 "events": [[0.0, "step"], [0.5, "step"]]},
//        "jump": {"clip": "Jump_Start", "loop": false, "speed": 1.2}},
//      "transitions": [
//        {"from": "move", "to": "jump", "when": [["jump"]], "blend": 0.1},
//        {"from": "jump", "to": "move", "after": 1.0, "blend": 0.25},
//        {"from": "*", "to": "move", "when": [["speed", ">", 9]]}]},
//     {"name": "wave", "mask": "spine_02", "weight": "waving", ...}]}
//
// A condition is [parameter] (not 0; a trigger: set) or [parameter, op, number] with op one of
// == != < <= > >=. A layer of more than one state names the one it starts in. "after" is how many
// of the state's cycles must have played (1: all of a clip that doesn't loop). "speed" and
// "vertical_speed" (metres a second, along the character's up) are set by the game from how the
// character moves, whether declared or not.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

#include "midgard/animation.h"

namespace midgard {

struct AnimCondition {
    enum class Op { Set, Eq, Ne, Lt, Le, Gt, Ge };
    std::string parameter;
    Op op = Op::Set;
    float value = 0.0f;
};

struct AnimState {
    std::string name;
    std::string clip;  // a clip; or a blend of clips by a parameter:
    std::string blend;
    std::vector<std::pair<std::string, float>> clips;  // (clip, where on the parameter), sorted
    float speed = 1.0f;
    std::string speed_parameter;  // (multiplies speed; empty: none)
    bool loop = true;
    std::vector<std::pair<float, std::string>> events;  // (point in the cycle 0..1, name)
};

struct AnimTransition {
    int from = -1;  // a state; -1: any (but the one it leads to)
    int to = 0;
    std::vector<AnimCondition> when;
    float blend = 0.2f;  // seconds
    std::optional<float> after;
};

struct AnimLayer {
    std::string name;
    std::string mask;  // the joint whose subtree it moves (empty: all)
    float weight = 1.0f;
    std::string weight_parameter;  // (empty: weight)
    int start = 0;
    std::vector<AnimState> states;
    std::vector<AnimTransition> transitions;
};

struct AnimGraph {
    std::map<std::string, float> parameters;  // with their starting values
    std::set<std::string> triggers;
    std::vector<AnimLayer> layers;
    bool root_motion = false;

    // Throws AnimationError saying what is wrong (a state, parameter or clip missing...).
    static AnimGraph parse(const nlohmann::json& json);
    static AnimGraph parse(const std::string& json_text);
    std::set<std::string> clips() const;  // every clip it names
};

// The parameters the game sets from how a character moves.
inline constexpr const char* kSpeedParameter = "speed";
inline constexpr const char* kVerticalSpeedParameter = "vertical_speed";

// A graph playing on one character.
class AnimPlayer {
public:
    using FindClip = std::function<const Clip*(const std::string& name)>;
    // `motion_joint`: the joint whose horizontal movement is root motion (its hips; -1: none);
    // `armature`: the skeleton's place in the model, whose Y is up.
    AnimPlayer(std::shared_ptr<const AnimGraph> graph, const Skeleton& skeleton, FindClip find,
               int motion_joint = -1, const glm::mat4& armature = glm::mat4(1.0f));

    // Throws AnimationError for a parameter (or trigger) the graph hasn't got.
    void set(const std::string& parameter, float value);
    void trigger(const std::string& name);
    float get(const std::string& parameter) const;
    bool has(const std::string& parameter) const { return values_.contains(parameter); }
    const std::string& state(size_t layer = 0) const;

    // Kept in step with another graph (a first-person rig's with its body's): its parameters
    // of the same names copied, and each layer whose leader's state it has by name put in that
    // state at the same point in its cycle (those layers then move only as the leader's do).
    void follow(const AnimPlayer& leader);

    struct Step {
        glm::vec3 motion{0.0f};  // root motion: metres in the model (Y up), this step
        std::vector<std::string> events;
    };
    // Moves time on by dt, and poses `pose` (its skeleton's).
    Step update(float dt, Pose& pose);

private:
    struct Playing {
        int state = -1;
        double cycles = 0.0;  // how many of its cycles have played
    };
    struct LayerState {
        Playing now, before;  // (before: fading out)
        float fade = 1.0f, fade_time = 0.0f;
        std::vector<float> mask;  // each joint's share
        bool led = false;         // (follow: in step with a leader's layer)
    };
    struct RootTrack {  // a clip's motion joint, horizontally, from its first frame
        std::vector<glm::vec3> at;
        float step = 1.0f / 30.0f;
        glm::vec3 offset(float time) const;
        glm::vec3 cycle() const { return at.empty() ? glm::vec3(0.0f) : at.back(); }
    };
    float cycle_time(const AnimState& s) const;  // seconds a cycle takes
    float state_speed(const AnimState& s) const;
    bool holds(const AnimCondition& c) const;
    // The state's joints at a point in it, into `out`.
    void sample(const AnimState& s, double cycles, Pose& pose, std::vector<JointTransform>& out);
    // Its clips' root offsets at a point (by their weights), and how far they moved from one
    // point to another.
    glm::vec3 root_offset(const AnimState& s, double cycles, Pose& pose);
    glm::vec3 root_moved(const AnimState& s, double from, double to, Pose& pose);
    const RootTrack& root_track(const Clip& clip, Pose& pose);
    std::vector<std::pair<const Clip*, float>> blend_weights(const AnimState& s) const;
    float clip_time(const AnimState& s, const Clip& c, double cycles) const;

    std::shared_ptr<const AnimGraph> graph_;
    const Skeleton* skeleton_;
    FindClip find_;
    int motion_joint_;
    glm::mat4 armature_;
    std::map<std::string, float> values_;
    std::set<std::string> set_triggers_;
    std::vector<LayerState> layers_;
    std::map<const Clip*, RootTrack> roots_;
    std::vector<JointTransform> result_, layer_, a_, b_;
};

}  // namespace midgard
