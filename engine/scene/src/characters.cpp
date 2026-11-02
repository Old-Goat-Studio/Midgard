// SPDX-License-Identifier: Apache-2.0
#include "midgard/characters.h"

#include <algorithm>

#include <glm/gtc/matrix_transform.hpp>

#include "midgard/gltf_skin.h"
#include "midgard/ik.h"

namespace midgard {

namespace {

// Where a character stands: its entity's place, or for the player (whose entity is the middle of
// its capsule) the bottom of the capsule, its feet on the ground.
glm::dmat4 standing(const Scene& scene, entt::entity e) {
    const glm::dmat4 world = scene.world_matrix(e);
    const Player* p = scene.registry.try_get<Player>(e);
    return p ? world * glm::translate(glm::dmat4(1.0), glm::dvec3(0.0, -0.5 * p->height, 0.0))
             : world;
}

// A model's front (glTF's +Z) turned to the entity's front (-Z, which a player faces as it
// walks, as cameras, spot lights and arrivals do): half a turn about its up.
const glm::mat4 kFacing = glm::rotate(glm::mat4(1.0f), glm::pi<float>(), glm::vec3(0, 1, 0));

glm::mat4 standing_offset(const Scene& scene, entt::entity e) {
    const Player* p = scene.registry.try_get<Player>(e);
    return p ? glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -0.5f * p->height, 0.0f))
             : glm::mat4(1.0f);
}

}  // namespace

std::vector<std::string> Characters::load(const Scene& scene, const Bytes& bytes) {
    std::vector<std::string> problems;
    const auto fail = [&](const std::string& asset, const std::string& why) {
        if (failed_.emplace(asset, why).second) problems.push_back(why);
    };
    const auto read_model = [&](const std::string& asset) -> std::shared_ptr<Model> {
        if (const auto it = models_.find(asset); it != models_.end()) return it->second;
        if (failed_.contains(asset)) return nullptr;
        try {
            const std::vector<std::uint8_t> data = bytes(asset);
            SkinnedModel skinned = load_skinned_glb(data);
            BuiltAnimation built = build_animation(skinned);
            auto m = std::make_shared<Model>(Model{
                std::move(built.skeleton), std::move(skinned.skeleton), std::move(built.clips),
                std::move(built.skin_to_skeleton), std::move(skinned.inverse_binds),
                kFacing * skinned.armature, std::move(skinned.mesh), std::move(skinned.joints),
                std::move(skinned.weights)});
            models_.emplace(asset, m);
            return m;
        } catch (const std::exception& e) {
            fail(asset, "character model '" + asset + "': " + e.what());
            return nullptr;
        }
    };
    // Each character, and a player's first-person rig of its own.
    std::vector<CharacterRef> wanted;
    for (entt::entity e : scene.registry.view<const CharacterRef>()) {
        const CharacterRef& c = scene.registry.get<CharacterRef>(e);
        wanted.push_back(c);
        const CharacterRef::FirstPerson& fp = c.first_person;
        if (fp.on && !fp.model.empty() && fp.model != c.model) {
            CharacterRef arms;
            arms.model = fp.model;
            arms.clips = fp.clips;
            arms.graph = fp.graph;
            wanted.push_back(arms);
        }
    }
    for (const CharacterRef& c : wanted) {
        if (c.model.empty()) continue;
        const std::shared_ptr<Model> m = read_model(c.model);
        if (!m) continue;
        // Clips from another file: read once, retargeted to this skeleton if both are
        // humanoids, else built for it by joint name.
        const LibraryKey key{c.clips, c.model, c.rig};
        if (!c.clips.empty() && c.clips != c.model && !libraries_.contains(key) &&
            !failed_.contains(c.clips)) {
            HumanoidMap to_map;
            try {
                to_map = map_humanoid(m->raw, c.rig);
            } catch (const AnimationError& ex) {  // (a joint it hasn't got: as if unnamed)
                if (failed_.emplace(c.model + "#rig", ex.what()).second)
                    problems.push_back("character model '" + c.model + "': " + ex.what());
                to_map = map_humanoid(m->raw);
            }
            try {
                const SkinnedModel source = load_skinned_glb(bytes(c.clips));
                const HumanoidMap from_map = map_humanoid(source.skeleton);
                const bool humanoids = to_map.missing().empty() && from_map.missing().empty();
                std::vector<Clip> built;
                for (const RawClip& raw : source.clips)
                    built.push_back(Clip::build(
                        m->skeleton, humanoids
                                         ? retarget(raw, source.skeleton, from_map, m->raw, to_map)
                                         : raw));
                libraries_.emplace(key, std::move(built));
            } catch (const std::exception& ex) {
                fail(c.clips, "character clips '" + c.clips + "': " + ex.what());
            }
        }
        // Its graph: read once; the clips it names that it hasn't got, said once.
        if (c.graph.empty()) continue;
        if (!graphs_.contains(c.graph)) {
            try {
                graphs_.emplace(c.graph,
                                std::make_shared<const AnimGraph>(AnimGraph::parse(c.graph)));
            } catch (const AnimationError& ex) {
                graphs_.emplace(c.graph, nullptr);
                problems.push_back("character model '" + c.model + "': " + ex.what());
                continue;
            }
        }
        const auto& graph = graphs_.at(c.graph);
        if (!graph) continue;
        State probe;
        probe.model = c.model;
        probe.clips = c.clips;
        probe.rig = c.rig;
        probe.m = m;
        const std::string& source = c.clips.empty() || c.clips == c.model ? c.model : c.clips;
        for (const std::string& clip : graph->clips())
            if (!find_clip(probe, clip) &&
                failed_.emplace(c.model + "|" + c.clips + "|clip:" + clip, "").second)
                problems.push_back("character model '" + c.model +
                                   "': its animation graph plays '" + clip + "', which " + source +
                                   " hasn't got");
    }
    return problems;
}

void Characters::upload(render::Renderer& renderer) {
    for (auto& [asset, m] : models_) {
        if (m->uploaded) continue;
        m->gpu = renderer.upload_skinned_mesh(m->mesh, m->joints, m->weights);
        m->uploaded = true;
    }
    for (auto& [e, s] : states_) {
        if (!s.fp || s.fp->uploaded || s.fp->mesh.vertices.empty()) continue;
        s.fp->gpu = renderer.upload_skinned_mesh(s.fp->mesh, s.fp->joints, s.fp->weights);
        s.fp->uploaded = true;
    }
}

void Characters::renderer_gone() {
    for (auto& [asset, m] : models_) m->uploaded = false;
    for (auto& [e, s] : states_)
        if (s.fp) s.fp->uploaded = false;
}

void Characters::cut_skinned(const render::MeshData& mesh, std::span<const glm::u16vec4> joints,
                             std::span<const glm::vec4> weights, const std::vector<bool>& keep,
                             render::MeshData& out_mesh, std::vector<glm::u16vec4>& out_joints,
                             std::vector<glm::vec4>& out_weights) {
    // A vertex is kept's if its heaviest joint is.
    const auto kept = [&](std::uint32_t v) {
        int best = 0;
        for (int k = 1; k < 4; ++k)
            if (weights[v][k] > weights[v][best]) best = k;
        const size_t j = joints[v][best];
        return j < keep.size() && keep[j];
    };
    out_mesh = render::MeshData{};
    out_joints.clear();
    out_weights.clear();
    std::vector<std::int64_t> renumbered(mesh.vertices.size(), -1);
    for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
        const std::uint32_t a = mesh.indices[t], b = mesh.indices[t + 1], c = mesh.indices[t + 2];
        if (!kept(a) || !kept(b) || !kept(c)) continue;
        for (const std::uint32_t v : {a, b, c}) {
            if (renumbered[v] < 0) {
                renumbered[v] = std::int64_t(out_mesh.vertices.size());
                out_mesh.vertices.push_back(mesh.vertices[v]);
                out_joints.push_back(joints[v]);
                out_weights.push_back(weights[v]);
            }
            out_mesh.indices.push_back(std::uint32_t(renumbered[v]));
        }
    }
}

const std::vector<Clip>* Characters::clips_for(
    const std::string& clips, const std::string& model,
    const std::map<std::string, std::string>& rig) const {
    if (!clips.empty() && clips != model) {
        const auto it = libraries_.find(LibraryKey{clips, model, rig});
        return it == libraries_.end() ? nullptr : &it->second;
    }
    const auto it = models_.find(model);
    return it == models_.end() ? nullptr : &it->second->clips;
}

// The player's arms as first person shows them: cut from its body and posed as the body is
// (kept in step by being the body's own joints), or a rig of their own whose graph follows the
// body's.
void Characters::update_first_person(const CharacterRef& c, State& s, float dt) {
    const CharacterRef::FirstPerson& fp = c.first_person;
    const bool own = !fp.model.empty() && fp.model != c.model;
    const std::string key =
        fp.model + "|" + fp.clips + "|" + fp.graph + "|" + fp.eye + "|" + c.model + "|" + c.clips;
    if (!s.fp || s.fp->key != key) {
        auto f = std::make_unique<State::FirstPerson>();
        f->key = key;
        f->own = own;
        if (!own) {
            // The arms: everything under the shoulders (or the upper arms, without them).
            f->m = s.m;
            const int n = s.m->skeleton.joint_count();
            std::vector<bool> under(size_t(n), false);
            for (const auto [shoulder, arm] :
                 {std::pair{HumanBone::LeftShoulder, HumanBone::LeftUpperArm},
                  std::pair{HumanBone::RightShoulder, HumanBone::RightUpperArm}}) {
                const int root = s.bones[size_t(shoulder)] >= 0 ? s.bones[size_t(shoulder)]
                                                                : s.bones[size_t(arm)];
                if (root < 0) continue;
                for (int j = 0; j < n; ++j) {  // (parents before children)
                    const int p = s.m->skeleton.parents()[size_t(j)];
                    if (j == root || (p >= 0 && under[size_t(p)])) under[size_t(j)] = true;
                }
            }
            std::vector<bool> keep(s.m->skin_to_skeleton.size(), false);
            for (size_t k = 0; k < keep.size(); ++k) {
                const int j = s.m->skin_to_skeleton[k];
                keep[k] = j >= 0 && under[size_t(j)];
            }
            cut_skinned(s.m->mesh, s.m->joints, s.m->weights, keep, f->mesh, f->joints, f->weights);
            f->eye = std::max(0, s.bones[size_t(HumanBone::Head)]);
            if (!fp.eye.empty() && s.m->skeleton.find(fp.eye) >= 0)
                f->eye = s.m->skeleton.find(fp.eye);
        } else {
            const auto it = models_.find(fp.model);
            if (it == models_.end()) return;  // (not read, or it failed: said when loaded)
            f->m = it->second;
            f->mesh = f->m->mesh;
            f->joints = f->m->joints;
            f->weights = f->m->weights;
            f->pose = std::make_unique<Pose>(f->m->skeleton);
            // Its eye: the joint named, else a humanoid's head, else its first joint.
            f->eye = f->m->skeleton.find(fp.eye);
            if (f->eye < 0) {
                const HumanoidMap rig = map_humanoid(f->m->raw);
                const int head = rig[HumanBone::Head];
                f->eye = head >= 0 ? f->m->skeleton.find(f->m->raw.names[size_t(head)]) : 0;
            }
            if (const auto g = graphs_.find(fp.graph); g != graphs_.end() && g->second) {
                const std::vector<Clip>* clips = clips_for(fp.clips, fp.model, {});
                f->player = std::make_unique<AnimPlayer>(
                    g->second, f->m->skeleton, [clips](const std::string& name) -> const Clip* {
                        if (!clips) return nullptr;
                        for (const Clip& clip : *clips)
                            if (clip.name() == name) return &clip;
                        return nullptr;
                    });
            }
        }
        s.fp = std::move(f);
    }
    State::FirstPerson& f = *s.fp;
    if (!f.own) return;  // (drawn with the body's own joints)
    // Its own rig: its graph kept in step with the body's, or the body's clip by name.
    if (f.player) {
        if (s.player) f.player->follow(*s.player);
        f.player->update(dt * c.speed, *f.pose);
    } else {
        f.time = s.time;
        const std::vector<Clip>* clips = clips_for(fp.clips, fp.model, {});
        const Clip* playing = nullptr;
        const std::string name = s.playing ? s.playing->name() : std::string();
        if (clips)
            for (const Clip& clip : *clips)
                if (clip.name() == name) playing = &clip;
        if (playing)
            f.pose->sample(*playing, f.time, c.loop);
        else
            f.pose->rest();
    }
    const std::vector<glm::mat4>& model = f.pose->model();
    f.palette.resize(f.m->inverse_binds.size());
    for (size_t j = 0; j < f.palette.size(); ++j) {
        const int k = f.m->skin_to_skeleton[j];
        f.palette[j] = (k >= 0 ? model[size_t(k)] : glm::mat4(1.0f)) * f.m->inverse_binds[j];
    }
}

std::optional<Characters::FirstPersonView> Characters::first_person(const Scene& scene,
                                                                    entt::entity player,
                                                                    const glm::mat4& camera) const {
    const auto it = states_.find(player);
    if (it == states_.end() || !it->second.fp || !scene.registry.valid(player)) return {};
    const State& s = it->second;
    const State::FirstPerson& f = *s.fp;
    const CharacterRef* c = scene.registry.try_get<CharacterRef>(player);
    const Pose* pose = f.own ? f.pose.get() : s.pose.get();
    const std::span<const glm::mat4> palette = f.own ? f.palette : s.palette;
    if (!c || !pose || palette.empty() || f.mesh.vertices.empty()) return {};
    // The eye at the camera: the model's front (the entity's -Z, kFacing) along the camera's
    // -Z, its up the camera's up; moved by the offset in the camera's frame.
    const glm::vec3 eye = glm::vec3(f.m->armature * pose->model()[size_t(f.eye)][3]);
    FirstPersonView view;
    view.mesh = &f.mesh;
    view.model = camera * glm::translate(glm::mat4(1.0f), c->first_person.offset) *
                 glm::translate(glm::mat4(1.0f), -eye) * f.m->armature;
    view.skin = palette;
    if (f.player) view.state = f.player->state();
    return view;
}

void Characters::first_person_draws(const Scene& scene, entt::entity player,
                                    const glm::mat4& camera, const MaterialCache& materials,
                                    std::vector<render::DrawItem>& out) const {
    const auto view = first_person(scene, player, camera);
    const auto it = states_.find(player);
    if (!view || !it->second.fp->uploaded) return;
    // The player's own material (its colour and surface), held at the camera.
    render::DrawItem d = scene.draw_item(player, it->second.fp->gpu, glm::dvec3(0.0));
    d.model = view->model;
    if (const Material* m = scene.registry.try_get<Material>(player)) materials.apply(*m, d);
    d.casts_shadow = false;  // (it hangs at the camera: its shadow would be the camera's)
    d.skin = view->skin;
    out.push_back(d);
}

void Characters::forget(const std::string& asset) {
    models_.erase(asset);
    failed_.erase(asset);
    failed_.erase(asset + "#rig");
    std::erase_if(libraries_, [&](const auto& kv) {
        return std::get<0>(kv.first) == asset || std::get<1>(kv.first) == asset;
    });
    for (auto& [e, s] : states_)
        if (s.model == asset || s.clips == asset) s = State{};
}

const Clip* Characters::find_clip(const State& s, const std::string& name) const {
    const std::vector<Clip>* from = &s.m->clips;
    if (!s.clips.empty() && s.clips != s.model) {
        const auto it = libraries_.find(LibraryKey{s.clips, s.model, s.rig});
        if (it == libraries_.end()) return nullptr;
        from = &it->second;
    }
    for (const Clip& c : *from)
        if (c.name() == name) return &c;
    return name.empty() && !from->empty() ? &from->front() : nullptr;
}

const Clip* Characters::find_clip(const State& s) {
    return find_clip(s, s.clip);
}

void Characters::start_graph(const CharacterRef& c, State& s) {
    s.player.reset();
    s.graph = c.graph;
    const auto it = graphs_.find(c.graph);
    if (c.graph.empty() || it == graphs_.end() || !it->second) return;
    // Root motion is its hips' (a humanoid's), else its first joint's.
    int motion = 0;
    try {
        const HumanoidMap rig = map_humanoid(s.m->raw, c.rig);
        if (const int hips = rig[HumanBone::Hips]; hips >= 0)
            motion = std::max(0, s.m->skeleton.find(s.m->raw.names[size_t(hips)]));
    } catch (const AnimationError&) {  // (said when loaded)
    }
    s.player = std::make_unique<AnimPlayer>(
        it->second, s.m->skeleton,
        [this, &s](const std::string& name) { return find_clip(s, name); }, motion, s.m->armature);
    for (const auto& [name, value] : s.waiting) {
        try {
            s.player->set(name, value);
        } catch (const AnimationError&) {  // (not this graph's: forgotten)
        }
    }
    for (const std::string& name : s.triggered) {
        try {
            s.player->trigger(name);
        } catch (const AnimationError&) {
        }
    }
    s.waiting.clear();
    s.triggered.clear();
}

void Characters::update(const Scene& scene, float dt) {
    const auto& reg = scene.registry;
    events_.clear();
    motions_.clear();
    std::erase_if(states_, [&](const auto& kv) {
        return !reg.valid(kv.first) || !reg.all_of<CharacterRef>(kv.first);
    });
    for (entt::entity e : reg.view<const CharacterRef>()) {
        const CharacterRef& c = reg.get<CharacterRef>(e);
        const auto it = models_.find(c.model);
        if (it == models_.end()) continue;  // (not read, or it failed)
        State& s = states_[e];
        if (s.m != it->second || s.model != c.model) {  // a new character, or another model
            auto waiting = std::move(s.waiting);        // (a script's, before it started)
            auto triggered = std::move(s.triggered);
            s = State{};
            s.waiting = std::move(waiting);
            s.triggered = std::move(triggered);
            s.m = it->second;
            s.model = c.model;
            s.pose = std::make_unique<Pose>(s.m->skeleton);
        }
        const bool changed = s.clips != c.clips || s.clip != c.clip || s.rig != c.rig;
        if (changed || !s.playing) {
            s.clips = c.clips;
            s.clip = c.clip;
            s.rig = c.rig;
            s.playing = find_clip(s);
            s.time = 0.0f;
        }
        if (changed || s.graph != c.graph || (!s.player && !c.graph.empty())) start_graph(c, s);
        if (changed || !s.bones_known) find_bones(c, s);
        // How it moves: its speed along the ground and up (its own up), gently smoothed.
        const glm::dvec3 at = scene.world_position(e);
        if (s.placed && dt > 0.0f) {
            const glm::dvec3 up = glm::normalize(glm::dvec3(scene.world_matrix(e)[1]));
            const glm::dvec3 moved = at - s.last;
            const double rise = glm::dot(moved, up);
            const float k = std::min(1.0f, dt / 0.1f);
            s.speed += (float(glm::length(moved - up * rise) / dt) - s.speed) * k;
            s.vertical_speed += (float(rise / dt) - s.vertical_speed) * k;
        }
        s.last = at;
        s.placed = true;
        if (s.player) {
            if (automatic_) {
                s.player->set(kSpeedParameter, s.speed);
                s.player->set(kVerticalSpeedParameter, s.vertical_speed);
            }
            const AnimPlayer::Step step = s.player->update(dt * c.speed, *s.pose);
            for (const std::string& name : step.events) events_.push_back({e, name});
            if (step.motion != glm::vec3(0.0f)) motions_.emplace_back(e, step.motion);
        } else {
            s.time += dt * c.speed;
            if (s.playing)
                s.pose->sample(*s.playing, s.time, c.loop);
            else
                s.pose->rest();
        }
        apply_ik(scene, e, c, s, dt);
        // The skin's joint matrices: each joint's pose times its inverse bind matrix.
        const std::vector<glm::mat4>& model = s.pose->model();
        s.palette.resize(s.m->inverse_binds.size());
        for (size_t j = 0; j < s.palette.size(); ++j) {
            const int k = s.m->skin_to_skeleton[j];
            s.palette[j] = (k >= 0 ? model[size_t(k)] : glm::mat4(1.0f)) * s.m->inverse_binds[j];
        }
        // The player's first-person arms (after its body: kept in step with it).
        if (e == player_ && c.first_person.on)
            update_first_person(c, s, dt);
        else
            s.fp.reset();
    }
}

void Characters::draws(const Scene& scene, const glm::dvec3& origin, const MaterialCache& materials,
                       entt::entity hidden, std::vector<render::DrawItem>& out) const {
    for (const auto& [e, s] : states_) {
        if (e == hidden || !s.m || !s.m->uploaded || s.palette.empty()) continue;
        if (!scene.registry.valid(e)) continue;
        render::DrawItem d = scene.draw_item(e, s.m->gpu, origin);
        d.model = d.model * standing_offset(scene, e) * s.m->armature;
        if (const Material* m = scene.registry.try_get<Material>(e)) materials.apply(*m, d);
        d.skin = s.palette;
        out.push_back(d);
    }
}

const Pose* Characters::pose(entt::entity e) const {
    const auto it = states_.find(e);
    return it == states_.end() ? nullptr : it->second.pose.get();
}

void Characters::restart(entt::entity e) {
    const auto it = states_.find(e);
    if (it == states_.end()) return;
    it->second.time = 0.0f;
    it->second.graph.clear();  // (started again at the next update)
    it->second.player.reset();
}

void Characters::find_bones(const CharacterRef& c, State& s) const {
    s.bones.fill(-1);
    s.bones_known = true;
    HumanoidMap rig;
    try {
        rig = map_humanoid(s.m->raw, c.rig);
    } catch (const AnimationError&) {  // (said when loaded)
        rig = map_humanoid(s.m->raw);
    }
    for (int b = 0; b < kHumanBones; ++b)
        if (const int j = rig.joint[size_t(b)]; j >= 0)
            s.bones[size_t(b)] = s.m->skeleton.find(s.m->raw.names[size_t(j)]);
}

void Characters::look_at(entt::entity e, std::optional<glm::dvec3> point, entt::entity target) {
    State& s = graph_state(e);
    s.look_point = point;
    s.look_entity = target;
}

void Characters::reach(entt::entity e, bool right, std::optional<glm::dvec3> point, float weight) {
    State::Reach& r = graph_state(e).reach[right ? 1 : 0];
    r.point = point;
    r.weight = std::clamp(weight, 0.0f, 1.0f);
}

std::optional<glm::dvec3> Characters::look_target(const Scene& scene, const CharacterRef& c,
                                                  const State& s) const {
    if (s.look_point) return s.look_point;
    entt::entity target = s.look_entity;
    if (target == entt::null && !c.look.empty())
        target = c.look == "player" ? player_ : scene.find(c.look);
    if (target == entt::null || !scene.registry.valid(target)) return std::nullopt;
    // A character's head (its eyes, near enough), else where the entity is.
    if (const auto it = states_.find(target);
        it != states_.end() && it->second.pose && it->second.bones[size_t(HumanBone::Head)] >= 0) {
        const State& t = it->second;
        const glm::dmat4 w = standing(scene, target);
        const glm::vec4 head =
            t.m->armature * t.pose->model()[size_t(t.bones[size_t(HumanBone::Head)])][3];
        return glm::dvec3(w * glm::dvec4(glm::dvec3(head), 1.0));
    }
    return scene.world_position(target);
}

// Feet on the ground under them, the head turned to what it looks at, hands reaching: each
// blended in and out over a moment, all along the character's own up (so on a planet too).
void Characters::apply_ik(const Scene& scene, entt::entity e, const CharacterRef& c, State& s,
                          float dt) {
    const auto& b = s.bones;
    const auto bone = [&](HumanBone h) { return b[size_t(h)]; };
    const auto ease = [&](float& value, float target, float seconds) {
        value = dt <= 0.0f ? target : value + (target - value) * std::min(1.0f, dt / seconds);
    };
    const std::optional<glm::dvec3> look = look_target(scene, c, s);
    ease(s.look_weight, look ? 1.0f : 0.0f, 0.3f);
    for (State::Reach& r : s.reach) ease(r.blend, r.point ? r.weight : 0.0f, 0.25f);
    const bool feet = c.feet && ground_ && bone(HumanBone::Hips) >= 0;
    if (!feet && s.look_weight < 1e-3f && s.reach[0].blend < 1e-3f && s.reach[1].blend < 1e-3f)
        return;

    // The model's frame in the world: around the entity's place (kept in doubles there).
    const glm::dmat4 world = standing(scene, e);
    const glm::dvec3 origin(world[3]);
    glm::mat4 frame(world);
    frame[3] = glm::vec4(0, 0, 0, 1);
    const glm::mat4& armature = s.m->armature;
    const glm::mat4 to_entity_from_model = armature;
    const glm::mat4 to_model_from_entity = glm::inverse(armature);
    const glm::mat4 to_entity_from_world = glm::inverse(frame);
    const glm::vec3 offset = s.pose->offset();  // (root motion's: model = joints + offset)
    // Unshifted model space (the joints'), and the entity's frame (Y its up).
    const auto entity_of = [&](const glm::vec3& joints) {
        return glm::vec3(to_entity_from_model * glm::vec4(joints + offset, 1.0f));
    };
    const auto joints_of = [&](const glm::vec3& in_entity) {
        return glm::vec3(to_model_from_entity * glm::vec4(in_entity, 1.0f)) - offset;
    };
    const auto joints_dir = [&](const glm::vec3& in_entity) {
        return glm::normalize(glm::mat3(to_model_from_entity) * in_entity);
    };
    const auto world_of = [&](const glm::vec3& in_entity) {
        return origin + glm::dvec3(glm::vec3(frame * glm::vec4(in_entity, 1.0f)));
    };
    const auto entity_from_world = [&](const glm::dvec3& w) {
        return glm::vec3(to_entity_from_world * glm::vec4(glm::vec3(w - origin), 1.0f));
    };
    const glm::vec3 up_world = glm::normalize(glm::vec3(frame[1]));
    const std::span<const int> parents = s.m->skeleton.parents();
    std::vector<JointTransform> local = s.pose->local();
    const auto at = [&](const std::vector<glm::mat4>& model, int j) {
        return glm::vec3(model[size_t(j)][3]);
    };
    // One joint now (its chain only: cheaper than the whole pose).
    const auto position_at = [&](int j) { return glm::vec3(joint_model(local, parents, j)[3]); };
    const auto rotation_at = [&](int j) {
        glm::mat3 r(joint_model(local, parents, j));
        for (int col = 0; col < 3; ++col) r[col] = glm::normalize(r[col]);
        return glm::normalize(glm::quat_cast(r));
    };

    if (feet) {
        const std::vector<glm::mat4> model = model_of(local, parents);
        const HumanBone legs[2][3] = {
            {HumanBone::LeftUpperLeg, HumanBone::LeftLowerLeg, HumanBone::LeftFoot},
            {HumanBone::RightUpperLeg, HumanBone::RightLowerLeg, HumanBone::RightFoot}};
        float height[2] = {0, 0};  // each foot above the character's ground, as animated
        glm::vec3 normal[2] = {glm::vec3(0, 1, 0), glm::vec3(0, 1, 0)};  // (entity frame)
        bool found[2] = {false, false};
        float reach = 0.5f;
        if (bone(legs[0][0]) >= 0 && bone(legs[0][2]) >= 0)
            reach = 0.5f * glm::length(at(model, bone(legs[0][0])) - at(model, bone(legs[0][2])));
        for (int side = 0; side < 2; ++side) {
            if (bone(legs[side][0]) < 0 || bone(legs[side][1]) < 0 || bone(legs[side][2]) < 0)
                continue;
            const glm::vec3 foot = entity_of(at(model, bone(legs[side][2])));
            height[side] = foot.y;
            // The ground under the foot, along the character's up.
            const glm::vec3 above(foot.x, reach, foot.z);
            const auto hit = ground_(e, world_of(above), -up_world, 2.0 * reach);
            float target = 0.0f;
            if (hit) {
                target = entity_from_world(hit->point).y;
                normal[side] = glm::normalize(glm::mat3(to_entity_from_world) * hit->normal);
                found[side] = true;
            }
            ease(s.foot_offset[side], target, 0.08f);
        }
        // The hips lowered as far as the lower foot needs (never raised: legs don't stretch).
        const float drop = std::max(-reach, std::min({0.0f, s.foot_offset[0], s.foot_offset[1]}));
        ease(s.drop, drop, 0.1f);
        const int hips = bone(HumanBone::Hips);
        const int hp = parents[size_t(hips)];
        const glm::vec3 down = glm::mat3(to_model_from_entity) * glm::vec3(0, s.drop, 0);
        const glm::mat3 parent = hp < 0 ? glm::mat3(1.0f) : glm::mat3(model[size_t(hp)]);
        local[size_t(hips)].translation += glm::inverse(parent) * down;
        // Each foot to the ground under it, as high above it as its animation lifts it; its sole
        // along the slope while it is down. (Its knees bend forward: the entity's -Z.)
        const glm::vec3 forward = joints_dir({0, 0, -1});
        for (int side = 0; side < 2; ++side) {
            if (bone(legs[side][2]) < 0 || bone(legs[side][1]) < 0) continue;
            const int thigh = bone(legs[side][0]), knee = bone(legs[side][1]),
                      foot = bone(legs[side][2]);
            glm::vec3 target = entity_of(position_at(foot));
            target.y = height[side] + s.foot_offset[side];
            const glm::vec3 pole = position_at(knee) + forward * 0.5f;
            two_bone_ik(local, parents, thigh, knee, foot, joints_of(target), pole);
            if (found[side]) {
                const float down_weight =
                    std::clamp(1.0f - (height[side] - 0.05f) / 0.15f, 0.f, 1.f);
                const glm::vec3 axis = glm::inverse(rotation_at(foot)) * joints_dir({0, 1, 0});
                orient(local, parents, foot, axis, joints_dir(normal[side]), glm::radians(35.0f),
                       down_weight);
            }
        }
    }

    // Looking: the neck a third of the way there, the head the rest, in all no further than
    // the limit from where the animation has the head facing (forward, the entity's -Z, as
    // animated).
    if (look && s.look_weight > 1e-3f && bone(HumanBone::Head) >= 0) {
        const glm::vec3 target = joints_of(entity_from_world(*look));
        const glm::vec3 forward = joints_dir({0, 0, -1});
        const float limit = glm::radians(c.look_limit);
        const int neck = bone(HumanBone::Neck), head = bone(HumanBone::Head);
        const glm::quat head_before = rotation_at(head);
        const glm::vec3 head_axis = glm::inverse(head_before) * forward;  // (its own facing)
        if (neck >= 0) {
            const glm::vec3 neck_axis = glm::inverse(rotation_at(neck)) * forward;
            aim(local, parents, neck, neck_axis, target, limit, 0.35f * s.look_weight);
        }
        const glm::vec3 now = rotation_at(head) * head_axis;
        const float used = std::acos(std::clamp(glm::dot(now, forward), -1.0f, 1.0f));
        aim(local, parents, head, head_axis, target, std::max(0.0f, limit - used), s.look_weight);
    }

    // Hands: the elbow bending down and back (behind: the entity's +Z).
    const HumanBone arms[2][3] = {
        {HumanBone::LeftUpperArm, HumanBone::LeftLowerArm, HumanBone::LeftHand},
        {HumanBone::RightUpperArm, HumanBone::RightLowerArm, HumanBone::RightHand}};
    for (int side = 0; side < 2; ++side) {
        const State::Reach& r = s.reach[side];
        if (r.blend < 1e-3f || !r.point || bone(arms[side][2]) < 0) continue;
        const glm::vec3 pole = position_at(bone(arms[side][1])) + joints_dir({0, -1, 1}) * 0.5f;
        two_bone_ik(local, parents, bone(arms[side][0]), bone(arms[side][1]), bone(arms[side][2]),
                    joints_of(entity_from_world(*r.point)), pole, r.blend);
    }
    s.pose->set_local(local);
}

Characters::State& Characters::graph_state(entt::entity e) {
    return states_[e];  // (one not updated yet keeps what is set for its graph)
}

void Characters::set_parameter(entt::entity e, const std::string& name, float value) {
    State& s = graph_state(e);
    if (s.player)
        s.player->set(name, value);
    else
        s.waiting[name] = value;
}

void Characters::trigger(entt::entity e, const std::string& name) {
    State& s = graph_state(e);
    if (s.player)
        s.player->trigger(name);
    else
        s.triggered.insert(name);
}

float Characters::parameter(entt::entity e, const std::string& name) const {
    const auto it = states_.find(e);
    if (it != states_.end() && it->second.player) return it->second.player->get(name);
    if (it != states_.end())
        if (const auto w = it->second.waiting.find(name); w != it->second.waiting.end())
            return w->second;
    throw AnimationError("the character has no animation graph playing");
}

std::optional<glm::dvec3> Characters::bone_position(const Scene& scene, entt::entity e,
                                                    HumanBone bone) const {
    const auto it = states_.find(e);
    if (it == states_.end() || !it->second.pose || !it->second.bones_known) return std::nullopt;
    const State& s = it->second;
    const int j = s.bones[size_t(bone)];
    if (j < 0) return std::nullopt;
    const glm::vec4 p = s.m->armature * s.pose->model()[size_t(j)][3];
    return glm::dvec3(standing(scene, e) * glm::dvec4(glm::dvec3(p), 1.0));
}

std::string Characters::state_of(entt::entity e, size_t layer) const {
    const auto it = states_.find(e);
    return it == states_.end() || !it->second.player ? std::string()
                                                     : it->second.player->state(layer);
}

std::string Characters::clip_of(entt::entity e) const {
    const auto it = states_.find(e);
    return it == states_.end() || !it->second.playing ? std::string() : it->second.playing->name();
}

}  // namespace midgard
