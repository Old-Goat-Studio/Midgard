// SPDX-License-Identifier: Apache-2.0
#include "midgard/materials.h"

#include <cstring>
#include <stdexcept>

namespace midgard {

std::string MaterialCache::key(const Material& material) {
    std::string key;
    for (const std::string& t : material.textures) key += t + '\n';
    return key;
}

std::vector<std::string> MaterialCache::upload(const Scene& scene, render::Renderer& renderer,
                                               const Read& read) {
    std::vector<std::string> errors;
    for (entt::entity e : scene.registry.view<const Material>()) {
        const Material& m = scene.registry.get<Material>(e);
        if (!m.shader.empty() && !shaders_.contains(m.shader)) {
            render::ShaderId id = render::kErrorShader;
            try {
                const std::vector<uint8_t> bytes = read(m.shader);
                if (bytes.size() % 4 != 0) throw std::runtime_error("it isn't SPIR-V");
                std::vector<uint32_t> words(bytes.size() / 4);
                std::memcpy(words.data(), bytes.data(), bytes.size());
                id = renderer.create_surface_shader(words);
                if (id == render::kErrorShader)
                    errors.push_back("surface shader '" + m.shader +
                                     "': the GPU can't use it (drawn magenta)");
            } catch (const std::exception& ex) {
                errors.push_back("surface shader '" + m.shader + "': " + ex.what() +
                                 " (drawn magenta)");
            }
            shaders_.emplace(m.shader, id);
        }
        if (!m.textured() || materials_.contains(key(m))) continue;
        render::MaterialTextures set;
        render::TextureId* slots[5] = {&set.base_color, &set.metallic_roughness, &set.normal,
                                       &set.occlusion, &set.emissive};
        for (size_t i = 0; i < 5; ++i) {
            const std::string& texture = m.textures[i];
            if (texture.empty() || failed_.contains(texture)) continue;
            if (const auto it = textures_.find(texture); it != textures_.end()) {
                *slots[i] = it->second;
                continue;
            }
            try {
                const std::vector<uint8_t> bytes = read(texture);
                *slots[i] = textures_.emplace(texture, renderer.upload_ktx2(bytes)).first->second;
            } catch (const std::exception& ex) {
                failed_.insert(texture);
                errors.push_back("texture '" + texture + "': " + ex.what());
            }
        }
        materials_.emplace(key(m), renderer.create_material(set));
    }
    return errors;
}

void MaterialCache::apply(const Material& material, render::DrawItem& draw) const {
    if (!material.shader.empty()) {
        const auto it = shaders_.find(material.shader);
        draw.shader = it != shaders_.end() ? it->second : render::kErrorShader;
    }
    if (!material.textured()) return;
    if (const auto it = materials_.find(key(material)); it != materials_.end())
        draw.material = it->second;
}

void MaterialCache::forget(const std::string& texture) {
    // (The old texture stays on the GPU until the renderer goes; edits are rare.)
    textures_.erase(texture);
    shaders_.erase(texture);  // (a surface shader: made again; the old one stays till then)
    failed_.erase(texture);
    const std::string line = texture + '\n';
    for (auto it = materials_.begin(); it != materials_.end();) {
        const std::string& k = it->first;
        bool uses = false;
        for (size_t at = 0; !uses && (at = k.find(line, at)) != std::string::npos; ++at)
            uses = at == 0 || k[at - 1] == '\n';
        it = uses ? materials_.erase(it) : std::next(it);
    }
}

void MaterialCache::clear() {
    textures_.clear();
    materials_.clear();
    shaders_.clear();
    failed_.clear();
}

}  // namespace midgard
