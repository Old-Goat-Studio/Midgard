// SPDX-License-Identifier: Apache-2.0
// The renderer's materials for a scene's textured Material components, shared by the game and
// the editor's viewport: each KTX2 texture is uploaded once by its content id, and each set of
// textures becomes one renderer material.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "midgard/render/renderer.h"
#include "midgard/scene.h"

namespace midgard {

class MaterialCache {
public:
    // A texture's KTX2 bytes by content id. Throws (AssetError) for one it doesn't have.
    using Read = std::function<std::vector<uint8_t>(const std::string& texture)>;

    // Uploads the textures and materials the scene's Material components use that aren't on
    // the GPU yet. A texture that fails is left out of its material (and not tried again until
    // forgotten); the returned messages say why.
    std::vector<std::string> upload(const Scene& scene, render::Renderer& renderer,
                                    const Read& read);
    // The draw uses the material's textures, if they have been uploaded.
    void apply(const Material& material, render::DrawItem& draw) const;
    // A texture changed: it and the materials using it are made again on the next upload.
    void forget(const std::string& texture);
    // A new renderer: everything is made again.
    void clear();

private:
    static std::string key(const Material& material);

    std::unordered_map<std::string, render::TextureId> textures_;
    std::unordered_map<std::string, render::MaterialId> materials_;
    std::unordered_set<std::string> failed_;
};

}  // namespace midgard
