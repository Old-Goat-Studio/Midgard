// SPDX-License-Identifier: Apache-2.0
// A basic third-person camera: it orbits the player, and movement is relative to where the
// camera looks. Driven by actions (look_x, look_y, zoom, move_x, move_y), never raw keys.
#pragma once

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>

#include "midgard/input.h"
#include "midgard/scene.h"

namespace midgard {

class ThirdPersonCamera {
public:
    // Finds the scene's player and first camera. The starting yaw, elevation and distance come
    // from where the designer placed the camera relative to the player.
    explicit ThirdPersonCamera(Scene& scene);

    bool active() const { return player_ != entt::null && camera_ != entt::null; }

    // Moves the player and the camera for one frame.
    void update(Scene& scene, const Actions& actions, float dt);

    float yaw() const { return yaw_; }              // degrees around the player
    float elevation() const { return elevation_; }  // degrees above the horizon
    float distance() const { return distance_; }    // metres

    static constexpr float kMinElevation = -10.0f, kMaxElevation = 80.0f;
    static constexpr float kMinDistance = 1.5f, kMaxDistance = 60.0f;

private:
    void place_camera(Scene& scene) const;
    glm::vec3 target(const Scene& scene) const;

    entt::entity player_ = entt::null;
    entt::entity camera_ = entt::null;
    float yaw_ = 0.0f, elevation_ = 25.0f, distance_ = 6.0f;
    glm::vec3 look_offset_{0.0f, 0.5f, 0.0f};  // look at the player's middle, not its feet
};

// Euler degrees [pitch, yaw, 0] pointing an entity's -Z from `from` to `to` (R = Ry·Rx·Rz).
glm::vec3 look_rotation(const glm::vec3& from, const glm::vec3& to);

}  // namespace midgard
