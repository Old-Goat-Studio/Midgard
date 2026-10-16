// SPDX-License-Identifier: Apache-2.0
// Quality tiers: what the renderer spends on a frame. Desktop is the agreed look; Phone keeps
// the same look within a phone's power and memory (fewer, smaller shadow maps over a shorter
// distance, harder shadow edges, smaller textures). Phones use Phone by default.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "midgard/render/shadows.h"

namespace midgard::render {

enum class Quality { Desktop, Phone };

struct QualitySettings {
    Quality tier = Quality::Desktop;
    ShadowSettings shadows;
    // Textures larger than this drop their largest mip levels when uploaded.
    uint32_t max_texture_size = 4096;
    // Shadow edges from 3 x 3 filtered lookups; otherwise one (the GPU still blends 2 x 2).
    bool soft_shadows = true;
};

QualitySettings quality_settings(Quality tier);
Quality default_quality();               // Phone on Android, Desktop elsewhere
const char* quality_name(Quality tier);  // "desktop" or "phone"
std::optional<Quality> parse_quality(const std::string& name);

}  // namespace midgard::render
