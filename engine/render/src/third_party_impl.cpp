// SPDX-License-Identifier: Apache-2.0
// Single-file library implementations, compiled once.
#include <volk.h>

#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vk_mem_alloc.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <string>
#include <vector>

#include "midgard/render/renderer.h"

namespace midgard::render {

bool write_png(const std::string& path, uint32_t width, uint32_t height,
               const std::vector<uint8_t>& rgba) {
    if (rgba.size() < size_t(width) * height * 4) return false;
    return stbi_write_png(path.c_str(), int(width), int(height), 4, rgba.data(), int(width * 4)) !=
           0;
}

}  // namespace midgard::render
