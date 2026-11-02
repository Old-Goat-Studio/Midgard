// SPDX-License-Identifier: Apache-2.0
#include "midgard/characters.h"

#include <algorithm>

#include "midgard/gltf_skin.h"

namespace midgard {

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
            auto m = std::make_shared<Model>(
                Model{std::move(built.skeleton), std::move(skinned.skeleton),
                      std::move(built.clips), std::move(built.skin_to_skeleton),
                      std::move(skinned.inverse_binds), skinned.armature, std::move(skinned.mesh),
                      std::move(skinned.joints), std::move(skinned.weights)});
            models_.emplace(asset, m);
            return m;
        } catch (const std::exception& e) {
            fail(asset, "character model '" + asset + "': " + e.what());
            return nullptr;
        }
    };
    for (entt::entity e : scene.registry.view<const CharacterRef>()) {
        const CharacterRef& c = scene.registry.get<CharacterRef>(e);
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
}

void Characters::renderer_gone() {
    for (auto& [asset, m] : models_) m->uploaded = false;
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
            s.player->set(kSpeedParameter, s.speed);
            s.player->set(kVerticalSpeedParameter, s.vertical_speed);
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
        // The skin's joint matrices: each joint's pose times its inverse bind matrix.
        const std::vector<glm::mat4>& model = s.pose->model();
        s.palette.resize(s.m->inverse_binds.size());
        for (size_t j = 0; j < s.palette.size(); ++j) {
            const int k = s.m->skin_to_skeleton[j];
            s.palette[j] = (k >= 0 ? model[size_t(k)] : glm::mat4(1.0f)) * s.m->inverse_binds[j];
        }
    }
}

void Characters::draws(const Scene& scene, const glm::dvec3& origin, const MaterialCache& materials,
                       entt::entity hidden, std::vector<render::DrawItem>& out) const {
    for (const auto& [e, s] : states_) {
        if (e == hidden || !s.m || !s.m->uploaded || s.palette.empty()) continue;
        if (!scene.registry.valid(e)) continue;
        render::DrawItem d = scene.draw_item(e, s.m->gpu, origin);
        d.model = d.model * s.m->armature;
        if (const Material* m = scene.registry.try_get<Material>(e)) materials.apply(*m, d);
        d.skin = s.palette;
        out.push_back(d);
    }
}

const Pose* Characters::pose(entt::entity e) const {
    const auto it = states_.find(e);
    return it == states_.end() ? nullptr : it->second.pose.get();
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
