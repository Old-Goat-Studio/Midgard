// SPDX-License-Identifier: Apache-2.0
#include "midgard/third_person.h"

#include <algorithm>
#include <cmath>

namespace midgard {
namespace {

template <typename Component>
entt::entity first_with(const entt::registry& reg) {
    entt::entity found = entt::null;
    for (entt::entity e : reg.view<const Component>())
        if (found == entt::null || entt::to_integral(e) < entt::to_integral(found)) found = e;
    return found;
}

}  // namespace

glm::vec3 look_rotation(const glm::vec3& from, const glm::vec3& to) {
    const glm::vec3 d = to - from;
    const float yaw = glm::degrees(std::atan2(-d.x, -d.z));
    const float pitch = glm::degrees(std::atan2(d.y, std::hypot(d.x, d.z)));
    return {pitch, yaw, 0.0f};
}

ThirdPersonCamera::ThirdPersonCamera(Scene& scene) {
    player_ = first_with<Player>(scene.registry);
    camera_ = first_with<CameraLens>(scene.registry);
    if (!active()) return;
    const glm::vec3 offset = scene.registry.get<Transform>(camera_).position - target(scene);
    distance_ = std::clamp(glm::length(offset), kMinDistance, kMaxDistance);
    elevation_ =
        std::clamp(glm::degrees(std::asin(offset.y / std::max(glm::length(offset), 1e-4f))),
                   kMinElevation, kMaxElevation);
    yaw_ = glm::degrees(std::atan2(offset.x, offset.z));
    place_camera(scene);
}

glm::vec3 ThirdPersonCamera::target(const Scene& scene) const {
    return scene.registry.get<Transform>(player_).position + look_offset_;
}

void ThirdPersonCamera::place_camera(Scene& scene) const {
    const float yaw = glm::radians(yaw_), el = glm::radians(elevation_);
    const glm::vec3 offset(std::sin(yaw) * std::cos(el), std::sin(el),
                           std::cos(yaw) * std::cos(el));
    Transform& cam = scene.registry.get<Transform>(camera_);
    cam.position = target(scene) + offset * distance_;
    cam.rotation = look_rotation(cam.position, target(scene));
}

void ThirdPersonCamera::set_view(Scene& scene, float yaw, float elevation, float distance) {
    if (!active()) return;
    yaw_ = yaw;
    elevation_ = std::clamp(elevation, kMinElevation, kMaxElevation);
    distance_ = std::clamp(distance, kMinDistance, kMaxDistance);
    place_camera(scene);
}

void ThirdPersonCamera::update(Scene& scene, const Actions& actions, float dt) {
    if (!active()) return;
    // Look: degrees this frame (mouse deltas, or stick rates × dt).
    yaw_ -= actions.axis("look_x", dt);
    elevation_ = std::clamp(elevation_ - actions.axis("look_y", dt), kMinElevation, kMaxElevation);
    distance_ =
        std::clamp(distance_ * std::pow(1.1f, actions.axis("zoom")), kMinDistance, kMaxDistance);

    // Move on the ground plane, relative to the camera's facing.
    const float yaw = glm::radians(yaw_);
    const glm::vec3 forward(-std::sin(yaw), 0.0f, -std::cos(yaw));
    const glm::vec3 right(std::cos(yaw), 0.0f, -std::sin(yaw));
    glm::vec3 move = right * actions.axis("move_x") + forward * actions.axis("move_y");
    if (const float len = glm::length(move); len > 1.0f) move /= len;
    Transform& player = scene.registry.get<Transform>(player_);
    if (glm::length(move) > 1e-3f) {
        player.position += move * scene.registry.get<Player>(player_).speed * dt;
        player.rotation.y = glm::degrees(std::atan2(-move.x, -move.z));  // face where it goes
    }
    place_camera(scene);
}

}  // namespace midgard
