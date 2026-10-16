// SPDX-License-Identifier: Apache-2.0
// A basic third-person camera: it orbits the player, and movement is relative to where the
// camera looks. Driven by actions (look_x, look_y, zoom, move_x, move_y, jump), never raw keys.
//
// "Up" comes from the world's shape (the scene's body), so on a planet or inside a cylinder the
// orbit stays level with the ground under the player and "forward" keeps pointing the same way
// along it as the player walks on.
#pragma once

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

class ThirdPersonCamera {
public:
    // Finds the scene's player and first camera. The starting yaw, elevation and distance come
    // from where the designer placed the camera relative to the player.
    explicit ThirdPersonCamera(Scene& scene);

    bool active() const { return player_ != entt::null && camera_ != entt::null; }

    // Look and zoom from the actions, and where the player wants to go (relative to the camera);
    // the player turns to face that way. Nothing moves yet: physics walks the player.
    PlayerIntent steer(Scene& scene, const Actions& actions, float dt);
    // Puts the camera at its orbit around the player (after the player moved).
    void follow(Scene& scene);
    // Both, moving the player straight there (no physics).
    void update(Scene& scene, const Actions& actions, float dt);

    // Puts the camera at this orbit (clamped) around the player.
    void set_view(Scene& scene, float yaw, float elevation, float distance);

    float yaw() const { return yaw_; }              // degrees around the player
    float elevation() const { return elevation_; }  // degrees above the horizon
    float distance() const { return distance_; }    // metres

    static constexpr float kMinElevation = -10.0f, kMaxElevation = 80.0f;
    static constexpr float kMinDistance = 1.5f, kMaxDistance = 60.0f;

private:
    // The level directions at the player: up, and forward and right for this yaw.
    struct Level {
        glm::vec3 up, forward, right;
    };
    Level level(const Scene& scene, float yaw);
    glm::dvec3 target(const Scene& scene) const;
    glm::vec3 up_at(const Scene& scene, const glm::dvec3& p) const;

    entt::entity player_ = entt::null;
    entt::entity camera_ = entt::null;
    float yaw_ = 0.0f, elevation_ = 25.0f, distance_ = 6.0f;
    float look_height_ = 0.5f;  // look at the player's middle, not its feet
    // "Forward" at yaw 0, kept level as up turns underfoot; and the way the player faces.
    glm::vec3 reference_{0.0f, 0.0f, -1.0f};
    glm::vec3 facing_{0.0f, 0.0f, -1.0f};
};

// Euler degrees [pitch, yaw, 0] pointing an entity's -Z from `from` to `to` (R = Ry·Rx·Rz).
glm::vec3 look_rotation(const glm::vec3& from, const glm::vec3& to);
// Euler degrees for an entity whose -Z points along `forward` and whose Y is `up`.
glm::vec3 upright_rotation(const glm::vec3& forward, const glm::vec3& up);

}  // namespace midgard
