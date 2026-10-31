// SPDX-License-Identifier: Apache-2.0
// The player's camera (roadmap §8, Cameras; milestone 5.6): three views of the player, switched
// at any time with the "view" action (V), or the zoom between first and third person.
//
//   first person   at the player's eyes; the player faces where it looks
//   third person   orbiting the player, or over its shoulder (a shoulder offset)
//   top-down       above and behind at a fixed angle, zooming in and out
//
// Changing view blends smoothly. The camera never passes through solid things: it is swept
// from the player outwards as a sphere through the physics world and stops short of what it
// meets. "Up" comes from the world's shape, so every view stays level on a planet or inside a
// cylinder. Scripts can shake the camera and take it for short moves (a conversation's close-up),
// after which the player's own view comes back. Settings per view are in the camera component.
#pragma once

#include <functional>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>

#include "midgard/input.h"
#include "midgard/scene.h"

namespace midgard {

// What the player wants to do this frame.
struct PlayerIntent {
    glm::vec3 walk{0.0f};  // metres per second, along the ground
    bool jump = false;
};

// How far a sphere of `radius` can go from `from` along `direction` (unit) before it touches
// something solid, up to `distance`. Without one, nothing is in the way.
using CameraSweep = std::function<double(const glm::dvec3& from, const glm::vec3& direction,
                                         double distance, float radius)>;

class CameraRig {
public:
    // Where a camera is and which way it looks.
    struct Pose {
        glm::dvec3 position{0.0};
        glm::vec3 forward{0.0f, 0.0f, -1.0f};
        glm::vec3 up{0.0f, 1.0f, 0.0f};
        float fov = 55.0f;
    };
    // The player's view, kept when the rig is made again (an edit while playing) and in the
    // player's profile.
    struct State {
        CameraView view = CameraView::Third;
        float yaw = 0.0f, pitch = 0.0f, elevation = 25.0f;
        float third_distance = 6.0f, top_distance = 18.0f;
    };

    // Finds the scene's player and first camera. The starting third-person orbit comes from
    // where the designer placed the camera relative to the player; the starting view from the
    // camera's settings.
    explicit CameraRig(Scene& scene);

    bool active() const { return player_ != entt::null && camera_ != entt::null; }

    // Look, zoom and view changes from the actions, and where the player wants to go (relative
    // to the camera); the player turns to face that way. Nothing moves yet: physics walks it.
    PlayerIntent steer(Scene& scene, const Actions& actions, float dt);
    // Puts the camera where the view wants it (after the player moved), blending and kept out
    // of walls by `sweep`; dt drives blends, smoothing and shakes.
    void follow(Scene& scene, float dt, const CameraSweep& sweep = {});
    // Both, moving the player straight there (no physics).
    void update(Scene& scene, const Actions& actions, float dt, const CameraSweep& sweep = {});

    CameraView view() const { return state_.view; }
    // Another view, blended to (or at once). The player keeps facing the same way.
    void set_view(Scene& scene, CameraView view, bool blend = true);
    // The third-person orbit (clamped).
    void set_orbit(Scene& scene, float yaw, float elevation, float distance);
    State state() const { return state_; }
    void restore(Scene& scene, const State& state);

    // Shakes the camera for a while, fading out (strength in metres).
    void shake(float strength, float seconds);
    // Takes the camera to look from `position` at `target` for `seconds`, then gives the
    // player's view back (both blended).
    void look(const glm::dvec3& position, const glm::dvec3& target, float seconds);
    bool scripted() const { return scripted_.active; }

    // Where the camera is now, after blends and shakes.
    const Pose& pose() const { return pose_; }
    bool blending() const { return blend_left_ > 0.0f; }
    // In first person, the player's own model would fill the view: it isn't drawn.
    bool hides_player() const;
    entt::entity player() const { return player_; }

    float yaw() const { return state_.yaw; }                  // degrees around the player
    float elevation() const { return state_.elevation; }      // third person, degrees above level
    float distance() const { return state_.third_distance; }  // third person, metres

private:
    struct Level {
        glm::vec3 up, forward, right;
    };
    Level level(const Scene& scene, float yaw);
    glm::vec3 up_at(const Scene& scene, const glm::dvec3& p) const;
    const ViewSettings& settings(CameraView view) const;
    float yaw_toward(const Scene& scene, const glm::vec3& direction);
    Pose desired(Scene& scene, float dt, const CameraSweep& sweep);
    void start_blend();
    void clamp_state();

    entt::entity player_ = entt::null;
    entt::entity camera_ = entt::null;
    CameraRigSettings settings_;
    State state_;
    // "Forward" at yaw 0, kept level as up turns underfoot; and the way the player faces.
    glm::vec3 reference_{0.0f, 0.0f, -1.0f};
    glm::vec3 facing_{0.0f, 0.0f, -1.0f};
    glm::dvec3 smoothed_{0.0};  // the followed point, lagging by the view's smoothing
    bool smoothed_set_ = false;
    Pose pose_, blend_from_;
    float blend_left_ = 0.0f, blend_length_ = 0.0f;
    float shake_strength_ = 0.0f, shake_left_ = 0.0f, shake_length_ = 0.0f, shake_time_ = 0.0f;
    struct Scripted {
        bool active = false, returning = false;
        glm::dvec3 position{0.0}, target{0.0};
        float left = 0.0f;
    } scripted_;
};

// Euler degrees [pitch, yaw, 0] pointing an entity's -Z from `from` to `to` (R = Ry·Rx·Rz).
glm::vec3 look_rotation(const glm::vec3& from, const glm::vec3& to);
// Euler degrees for an entity whose -Z points along `forward` and whose Y is `up`.
glm::vec3 upright_rotation(const glm::vec3& forward, const glm::vec3& up);

}  // namespace midgard
