// SPDX-License-Identifier: Apache-2.0
#include "midgard/camera_rig.h"

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

// A camera's turn as a quaternion (its -Z along forward, its Y towards up), and back.
glm::quat turn_of(const glm::vec3& forward, const glm::vec3& up) {
    const glm::vec3 z = -glm::normalize(forward);
    glm::vec3 x = glm::cross(up, z);
    if (glm::dot(x, x) < 1e-10f) x = glm::cross(glm::vec3(0, 0, 1), z);
    x = glm::normalize(x);
    return glm::normalize(glm::quat_cast(glm::mat3(x, glm::cross(z, x), z)));
}

float smoothstep(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

CameraRig::Pose mix(const CameraRig::Pose& a, const CameraRig::Pose& b, float s) {
    CameraRig::Pose out;
    out.position = a.position + (b.position - a.position) * double(s);
    const glm::quat q = glm::slerp(turn_of(a.forward, a.up), turn_of(b.forward, b.up), s);
    out.forward = q * glm::vec3(0, 0, -1);
    out.up = q * glm::vec3(0, 1, 0);
    out.fov = a.fov + (b.fov - a.fov) * s;
    return out;
}

}  // namespace

glm::vec3 look_rotation(const glm::vec3& from, const glm::vec3& to) {
    const glm::vec3 d = to - from;
    const float yaw = glm::degrees(std::atan2(-d.x, -d.z));
    const float pitch = glm::degrees(std::atan2(d.y, std::hypot(d.x, d.z)));
    return {pitch, yaw, 0.0f};
}

glm::vec3 upright_rotation(const glm::vec3& forward, const glm::vec3& up) {
    return euler_degrees(turn_of(forward, up));
}

glm::vec3 CameraRig::up_at(const Scene& scene, const glm::dvec3& p) const {
    return scene.settings.body ? glm::vec3(scene.settings.body->up(p)) : glm::vec3(0, 1, 0);
}

const ViewSettings& CameraRig::settings(CameraView view) const {
    switch (view) {
        case CameraView::First:
            return settings_.first;
        case CameraView::Top:
            return settings_.top;
        default:
            return settings_.third;
    }
}

CameraRig::Level CameraRig::level(const Scene& scene, float yaw) {
    const glm::vec3 up = up_at(scene, scene.world_position(player_));
    reference_ = level_of(reference_, up, glm::vec3(0.0f));  // carried along as up turns
    const glm::vec3 right0 = glm::normalize(glm::cross(reference_, up));
    const float y = glm::radians(yaw);
    return {up, reference_ * std::cos(y) - right0 * std::sin(y),
            right0 * std::cos(y) + reference_ * std::sin(y)};
}

// The yaw whose forward points along `direction` (made level).
float CameraRig::yaw_toward(const Scene& scene, const glm::vec3& direction) {
    const Level l = level(scene, 0.0f);
    const glm::vec3 d = level_of(direction, l.up, l.forward);
    return glm::degrees(std::atan2(-glm::dot(d, l.right), glm::dot(d, l.forward)));
}

void CameraRig::clamp_state() {
    const ViewSettings &first = settings_.first, &third = settings_.third, &top = settings_.top;
    state_.pitch = std::clamp(state_.pitch, first.min_pitch, first.max_pitch);
    state_.elevation = std::clamp(state_.elevation, third.min_pitch, third.max_pitch);
    state_.third_distance =
        std::clamp(state_.third_distance, third.min_distance, third.max_distance);
    state_.top_distance = std::clamp(state_.top_distance, top.min_distance, top.max_distance);
}

CameraRig::CameraRig(Scene& scene) {
    player_ = first_with<Player>(scene.registry);
    camera_ = first_with<CameraLens>(scene.registry);
    if (!active()) return;
    settings_ = scene.registry.get<CameraLens>(camera_).rig;
    state_.view = settings_.start;
    state_.top_distance = settings_.top.distance;
    // Where the designer put the camera, as the third-person orbit in the level frame.
    const Level l = level(scene, 0.0f);
    const glm::dvec3 target =
        scene.world_position(player_) + glm::dvec3(l.up * settings_.third.height);
    const glm::vec3 offset(scene.registry.get<Transform>(camera_).position - target);
    const float length = std::max(glm::length(offset), 1e-4f);
    state_.third_distance = settings_.third_distance_set ? settings_.third.distance : length;
    state_.elevation =
        glm::degrees(std::asin(std::clamp(glm::dot(offset, l.up) / length, -1.0f, 1.0f)));
    const glm::vec3 flat = offset - l.up * glm::dot(offset, l.up);
    state_.yaw = glm::degrees(std::atan2(glm::dot(flat, l.right), glm::dot(-flat, l.forward)));
    clamp_state();
    // The player faces the way its own rotation points.
    const glm::mat3 turn(rotation_matrix(scene.registry.get<Transform>(player_).rotation));
    facing_ = level_of(turn * glm::vec3(0, 0, -1), l.up, l.forward);
    if (state_.view == CameraView::First) state_.yaw = yaw_toward(scene, facing_);
    follow(scene, 0.0f);
}

bool CameraRig::hides_player() const {
    return active() && state_.view == CameraView::First && !blending() && !scripted_.active;
}

void CameraRig::start_blend() {
    blend_from_ = pose_;
    blend_length_ = settings_.blend;
    blend_left_ = settings_.blend;
}

void CameraRig::set_view(Scene& scene, CameraView view, bool blend) {
    if (!active() || view == state_.view) return;
    // The player keeps facing the same way: first person looks along it, third person and
    // top-down start behind it.
    if (view == CameraView::First || state_.view == CameraView::First)
        state_.yaw = yaw_toward(scene, facing_);
    if (view == CameraView::First) state_.pitch = 0.0f;
    state_.view = view;
    clamp_state();
    if (blend && settings_.blend > 0.0f) start_blend();
}

void CameraRig::set_orbit(Scene& scene, float yaw, float elevation, float distance) {
    if (!active()) return;
    state_.yaw = yaw;
    state_.elevation = elevation;
    state_.third_distance = distance;
    clamp_state();
    follow(scene, 0.0f);
}

void CameraRig::restore(Scene& scene, const State& state) {
    if (!active()) return;
    state_ = state;
    clamp_state();
    follow(scene, 0.0f);
}

void CameraRig::shake(float strength, float seconds) {
    shake_strength_ = std::max(strength, 0.0f);
    shake_length_ = shake_left_ = std::max(seconds, 0.0f);
}

void CameraRig::look(const glm::dvec3& position, const glm::dvec3& target, float seconds) {
    scripted_ = {true, false, position, target, std::max(seconds, 0.0f)};
    start_blend();
}

void CameraRig::release() {
    if (!scripted_.active || scripted_.returning) return;
    scripted_.returning = true;
    start_blend();  // from wherever the camera is now
}

PlayerIntent CameraRig::steer(Scene& scene, const Actions& actions, float dt) {
    PlayerIntent intent;
    if (!active()) return intent;
    // View changes: the view action steps third → first → top-down → third; the zoom passes
    // between first and third person.
    const float zoom = actions.axis("zoom");
    if (actions.pressed("view")) {
        set_view(scene, state_.view == CameraView::Third   ? CameraView::First
                        : state_.view == CameraView::First ? CameraView::Top
                                                           : CameraView::Third);
    } else if (state_.view == CameraView::Third && zoom < 0.0f &&
               state_.third_distance <= settings_.third.min_distance + 1e-4f) {
        set_view(scene, CameraView::First);
    } else if (state_.view == CameraView::First && zoom > 0.0f) {
        state_.third_distance = settings_.third.min_distance;
        set_view(scene, CameraView::Third);
    } else if (state_.view == CameraView::Third) {
        state_.third_distance *= std::pow(1.1f, zoom);
    } else if (state_.view == CameraView::Top) {
        state_.top_distance *= std::pow(1.1f, zoom);
    }
    // Look: degrees this frame (mouse deltas, or stick rates × dt). Sideways turns every view
    // (top-down turns round the player, so the mouse steers where "up the screen" walks);
    // up and down tilt first and third person, and top-down keeps its fixed angle.
    state_.yaw -= actions.axis("look_x", dt);
    if (state_.view == CameraView::First) {
        state_.pitch += actions.axis("look_y", dt);  // mouse up: look up
    } else if (state_.view == CameraView::Third) {
        state_.elevation -= actions.axis("look_y", dt);  // look up: the camera goes lower
    }
    clamp_state();

    // Move along the ground, relative to the camera's facing.
    const Level l = level(scene, state_.yaw);
    glm::vec3 move = l.right * actions.axis("move_x") + l.forward * actions.axis("move_y");
    if (const float len = glm::length(move); len > 1.0f) move /= len;
    facing_ = level_of(facing_, l.up, l.forward);
    if (state_.view == CameraView::First) {
        facing_ = l.forward;  // the player faces where it looks
        scene.registry.get<Transform>(player_).rotation = upright_rotation(facing_, l.up);
    } else if (glm::length(move) > 1e-3f) {
        facing_ = glm::normalize(move);  // face where it goes, upright on the ground under it
        scene.registry.get<Transform>(player_).rotation = upright_rotation(facing_, l.up);
    }
    const Player& p = scene.registry.get<Player>(player_);
    intent.walk = move * (p.speed * (actions.down("run") ? p.run : 1.0f));
    intent.jump = actions.pressed("jump");
    return intent;
}

CameraRig::Pose CameraRig::desired(Scene& scene, float dt, const CameraSweep& sweep) {
    const ViewSettings& s = settings(state_.view);
    const Level l = level(scene, state_.yaw);
    // The followed point, lagging behind the player by the view's smoothing.
    const glm::dvec3 player = scene.world_position(player_);
    if (!smoothed_set_ || s.smoothing <= 0.0f || dt <= 0.0f) {
        smoothed_ = player;
        smoothed_set_ = true;
    } else {
        smoothed_ += (player - smoothed_) * double(1.0f - std::exp(-dt / s.smoothing));
    }
    Pose out;
    out.fov = s.fov;
    out.up = l.up;
    if (state_.view == CameraView::First) {
        const float p = glm::radians(state_.pitch);
        out.position = smoothed_ + glm::dvec3(l.up * s.height);
        out.forward = l.forward * std::cos(p) + l.up * std::sin(p);
        return out;
    }
    // Third person and top-down: back from the point looked at, kept out of walls.
    const bool top = state_.view == CameraView::Top;
    const float elevation = glm::radians(top ? s.min_pitch : state_.elevation);
    const float distance = top ? state_.top_distance : state_.third_distance;
    const glm::dvec3 target =
        smoothed_ + glm::dvec3(l.up * s.height + l.right * (top ? 0.0f : s.shoulder));
    const glm::vec3 back = -l.forward * std::cos(elevation) + l.up * std::sin(elevation);
    double reach = distance;
    if (sweep && distance > 0.0f)
        reach = std::clamp(sweep(target, back, distance, s.radius), 0.0, double(distance));
    out.position = target + glm::dvec3(back) * reach;
    out.forward = -back;
    return out;
}

void CameraRig::follow(Scene& scene, float dt, const CameraSweep& sweep) {
    if (!active()) return;
    Pose wanted = desired(scene, dt, sweep);
    // A scripted look holds for its time, then the player's view comes back (blended).
    if (scripted_.active) {
        if (!scripted_.returning) {
            Pose look;
            look.position = scripted_.position;
            look.up = up_at(scene, scripted_.position);
            look.forward = glm::normalize(glm::vec3(scripted_.target - scripted_.position));
            look.fov = wanted.fov;
            wanted = look;
            scripted_.left -= dt;
            if (scripted_.left <= 0.0f && blend_left_ <= 0.0f) {
                scripted_.returning = true;
                pose_ = look;
                start_blend();
            }
        } else if (blend_left_ <= 0.0f) {
            scripted_.active = false;
        }
    }
    if (blend_left_ > 0.0f && blend_length_ > 0.0f) {
        blend_left_ = std::max(blend_left_ - dt, 0.0f);
        pose_ = mix(blend_from_, wanted, smoothstep(1.0f - blend_left_ / blend_length_));
    } else {
        blend_left_ = 0.0f;
        pose_ = wanted;
    }
    if (scripted_.active && scripted_.returning && blend_left_ <= 0.0f) scripted_.active = false;
    // A shake: fading, smooth wobbles across the view.
    Pose shown = pose_;
    if (shake_left_ > 0.0f && shake_length_ > 0.0f) {
        shake_time_ += dt;
        shake_left_ = std::max(shake_left_ - dt, 0.0f);
        const float amount = shake_strength_ * shake_left_ / shake_length_;
        const glm::vec3 right = glm::normalize(glm::cross(shown.forward, shown.up));
        shown.position += glm::dvec3(right * (amount * std::sin(shake_time_ * 47.0f)) +
                                     shown.up * (amount * std::sin(shake_time_ * 61.0f + 1.3f)));
    }
    Transform& cam = scene.registry.get<Transform>(camera_);
    cam.position = shown.position;
    // The direction is small and exact relative to the camera, so floats are enough for it.
    cam.rotation = upright_rotation(shown.forward, shown.up);
    scene.registry.get<CameraLens>(camera_).fov = shown.fov;
}

void CameraRig::update(Scene& scene, const Actions& actions, float dt, const CameraSweep& sweep) {
    const PlayerIntent intent = steer(scene, actions, dt);
    if (active()) scene.registry.get<Transform>(player_).position += glm::dvec3(intent.walk * dt);
    follow(scene, dt, sweep);
}

}  // namespace midgard
