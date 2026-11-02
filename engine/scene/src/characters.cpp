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
                Model{std::move(built.skeleton), std::move(built.clips),
                      std::move(built.skin_to_skeleton), std::move(skinned.inverse_binds),
                      skinned.armature, std::move(skinned.mesh), std::move(skinned.joints),
                      std::move(skinned.weights)});
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
        // Clips from another file: read once, built for this skeleton by joint name.
        if (!c.clips.empty() && c.clips != c.model && !libraries_.contains({c.clips, c.model}) &&
            !failed_.contains(c.clips)) {
            try {
                const SkinnedModel source = load_skinned_glb(bytes(c.clips));
                std::vector<Clip> built;
                for (const RawClip& raw : source.clips)
                    built.push_back(Clip::build(m->skeleton, raw));
                libraries_.emplace(std::pair{c.clips, c.model}, std::move(built));
            } catch (const std::exception& ex) {
                fail(c.clips, "character clips '" + c.clips + "': " + ex.what());
            }
        }
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
    std::erase_if(libraries_, [&](const auto& kv) {
        return kv.first.first == asset || kv.first.second == asset;
    });
    for (auto& [e, s] : states_)
        if (s.model == asset || s.clips == asset) s = State{};
}

const Clip* Characters::find_clip(const State& s) {
    const std::vector<Clip>* from = &s.m->clips;
    if (!s.clips.empty() && s.clips != s.model) {
        const auto it = libraries_.find({s.clips, s.model});
        if (it == libraries_.end()) return nullptr;
        from = &it->second;
    }
    for (const Clip& c : *from)
        if (c.name() == s.clip) return &c;
    return s.clip.empty() && !from->empty() ? &from->front() : nullptr;
}

void Characters::update(const Scene& scene, float dt) {
    const auto& reg = scene.registry;
    std::erase_if(states_, [&](const auto& kv) {
        return !reg.valid(kv.first) || !reg.all_of<CharacterRef>(kv.first);
    });
    for (entt::entity e : reg.view<const CharacterRef>()) {
        const CharacterRef& c = reg.get<CharacterRef>(e);
        const auto it = models_.find(c.model);
        if (it == models_.end()) continue;  // (not read, or it failed)
        State& s = states_[e];
        if (s.m != it->second || s.model != c.model) {  // a new character, or another model
            s = State{};
            s.m = it->second;
            s.model = c.model;
            s.pose = std::make_unique<Pose>(s.m->skeleton);
        }
        if (s.clips != c.clips || s.clip != c.clip || !s.playing) {
            s.clips = c.clips;
            s.clip = c.clip;
            s.playing = find_clip(s);
            s.time = 0.0f;
        }
        s.time += dt * c.speed;
        if (s.playing)
            s.pose->sample(*s.playing, s.time, c.loop);
        else
            s.pose->rest();
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

std::string Characters::clip_of(entt::entity e) const {
    const auto it = states_.find(e);
    return it == states_.end() || !it->second.playing ? std::string() : it->second.playing->name();
}

}  // namespace midgard
