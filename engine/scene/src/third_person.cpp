// SPDX-License-Identifier: Apache-2.0
#include "midgard/third_person.h"

#include <algorithm>
#include <cmath>

#include <glm/gtc/quaternion.hpp>

#include "midgard/scene_physics.h"

namespace midgard {
namespace {

template <typename Component>
entt::entity first_with(const entt::registry& reg) {
    entt::entity found = entt::null;
    for (entt::entity e : reg.view<const Component>())
        if (found == entt::null || entt::to_integral(e) < entt::to_integral(found)) found = e;
    return found;
}

// `v` made level (perpendicular to `up`) and unit length, or `fallback` if it points up or down.
glm::vec3 level_of(const glm::vec3& v, const glm::vec3& up, const glm::vec3& fallback) {
    const glm::vec3 flat = v - up * glm::dot(v, up);
    if (glm::dot(flat, flat) > 1e-8f) return glm::normalize(flat);
    const glm::vec3 any = std::abs(up.x) < 0.9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 0, 1);
    return fallback == glm::vec3(0.0f) ? glm::normalize(glm::cross(up, any)) : fallback;
}

}  // namespace

glm::vec3 look_rotation(const glm::vec3& from, const glm::vec3& to) {
    const glm::vec3 d = to - from;
    const float yaw = glm::degrees(std::atan2(-d.x, -d.z));
    const float pitch = glm::degrees(std::atan2(d.y, std::hypot(d.x, d.z)));
    return {pitch, yaw, 0.0f};
}

glm::vec3 upright_rotation(const glm::vec3& forward, const glm::vec3& up) {
    const glm::vec3 z = -glm::normalize(forward);
    glm::vec3 x = glm::cross(up, z);
    if (glm::dot(x, x) < 1e-10f) x = glm::cross(glm::vec3(0, 0, 1), z);  // straight up or down
    x = glm::normalize(x);
    const glm::vec3 y = glm::cross(z, x);
    return euler_degrees(glm::quat_cast(glm::mat3(x, y, z)));
}

glm::vec3 ThirdPersonCamera::up_at(const Scene& scene, const glm::dvec3& p) const {
    return scene.settings.body ? glm::vec3(scene.settings.body->up(p)) : glm::vec3(0, 1, 0);
}

glm::dvec3 ThirdPersonCamera::target(const Scene& scene) const {
    const glm::dvec3 p = scene.world_position(player_);
    return p + glm::dvec3(up_at(scene, p)) * double(look_height_);
}

ThirdPersonCamera::Level ThirdPersonCamera::level(const Scene& scene, float yaw) {
    const glm::vec3 up = up_at(scene, scene.world_position(player_));
    reference_ = level_of(reference_, up, glm::vec3(0.0f));  // carried along as up turns
    const glm::vec3 right0 = glm::normalize(glm::cross(reference_, up));
    const float y = glm::radians(yaw);
    return {up, reference_ * std::cos(y) - right0 * std::sin(y),
            right0 * std::cos(y) + reference_ * std::sin(y)};
}

ThirdPersonCamera::ThirdPersonCamera(Scene& scene) {
    player_ = first_with<Player>(scene.registry);
    camera_ = first_with<CameraLens>(scene.registry);
    if (!active()) return;
    // Where the designer put the camera, as an orbit in the level frame at the player.
    const Level l = level(scene, 0.0f);
    const glm::vec3 offset(scene.registry.get<Transform>(camera_).position - target(scene));
    const float length = std::max(glm::length(offset), 1e-4f);
    distance_ = std::clamp(length, kMinDistance, kMaxDistance);
    elevation_ = std::clamp(
        glm::degrees(std::asin(std::clamp(glm::dot(offset, l.up) / length, -1.0f, 1.0f))),
        kMinElevation, kMaxElevation);
    const glm::vec3 flat = offset - l.up * glm::dot(offset, l.up);
    yaw_ = glm::degrees(std::atan2(glm::dot(flat, l.right), glm::dot(-flat, l.forward)));
    // The player faces the way its own rotation points.
    const glm::mat3 turn(rotation_matrix(scene.registry.get<Transform>(player_).rotation));
    facing_ = level_of(turn * glm::vec3(0, 0, -1), l.up, l.forward);
    follow(scene);
}

void ThirdPersonCamera::follow(Scene& scene) {
    if (!active()) return;
    const Level l = level(scene, yaw_);
    const float el = glm::radians(elevation_);
    const glm::vec3 offset = (-l.forward * std::cos(el) + l.up * std::sin(el)) * distance_;
    Transform& cam = scene.registry.get<Transform>(camera_);
    cam.position = target(scene) + glm::dvec3(offset);
    // The direction is small and exact relative to the camera, so floats are enough for it.
    cam.rotation = upright_rotation(-offset, l.up);
}

void ThirdPersonCamera::set_view(Scene& scene, float yaw, float elevation, float distance) {
    if (!active()) return;
    yaw_ = yaw;
    elevation_ = std::clamp(elevation, kMinElevation, kMaxElevation);
    distance_ = std::clamp(distance, kMinDistance, kMaxDistance);
    follow(scene);
}

PlayerIntent ThirdPersonCamera::steer(Scene& scene, const Actions& actions, float dt) {
    PlayerIntent intent;
    if (!active()) return intent;
    // Look: degrees this frame (mouse deltas, or stick rates × dt).
    yaw_ -= actions.axis("look_x", dt);
    elevation_ = std::clamp(elevation_ - actions.axis("look_y", dt), kMinElevation, kMaxElevation);
    distance_ =
        std::clamp(distance_ * std::pow(1.1f, actions.axis("zoom")), kMinDistance, kMaxDistance);

    // Move along the ground, relative to the camera's facing.
    const Level l = level(scene, yaw_);
    glm::vec3 move = l.right * actions.axis("move_x") + l.forward * actions.axis("move_y");
    if (const float len = glm::length(move); len > 1.0f) move /= len;
    facing_ = level_of(facing_, l.up, l.forward);
    if (glm::length(move) > 1e-3f) {
        facing_ = glm::normalize(move);
        // Face where it goes, upright on the ground under it.
        scene.registry.get<Transform>(player_).rotation = upright_rotation(facing_, l.up);
    }
    intent.walk = move * scene.registry.get<Player>(player_).speed;
    intent.jump = actions.pressed("jump");
    return intent;
}

void ThirdPersonCamera::update(Scene& scene, const Actions& actions, float dt) {
    const PlayerIntent intent = steer(scene, actions, dt);
    if (active()) scene.registry.get<Transform>(player_).position += glm::dvec3(intent.walk * dt);
    follow(scene);
}

}  // namespace midgard
