// SPDX-License-Identifier: Apache-2.0
#include "midgard/render/quality.h"

namespace midgard::render {

QualitySettings quality_settings(Quality tier) {
    QualitySettings q;
    q.tier = tier;
    if (tier == Quality::Phone) {
        // Two 1024 x 1024 maps over 30 m: an eighth of the desktop's shadow memory and fill.
        q.shadows.cascades = 2;
        q.shadows.resolution = 1024;
        q.shadows.distance = 30.0f;
        q.shadows.split_blend = 0.6f;
        q.max_texture_size = 1024;
        q.soft_shadows = false;
    }
    return q;
}

Quality default_quality() {
#ifdef __ANDROID__
    return Quality::Phone;
#else
    return Quality::Desktop;
#endif
}

const char* quality_name(Quality tier) {
    return tier == Quality::Phone ? "phone" : "desktop";
}

std::optional<Quality> parse_quality(const std::string& name) {
    if (name == "desktop") return Quality::Desktop;
    if (name == "phone") return Quality::Phone;
    return std::nullopt;
}

}  // namespace midgard::render
