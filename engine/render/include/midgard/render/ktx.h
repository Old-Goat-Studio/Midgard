// SPDX-License-Identifier: Apache-2.0
// Compressed textures: KTX2 files holding Basis Universal UASTC data (KTX-Software). The editor
// encodes a texture once; each device transcodes it to the compressed format its GPU reads (BC7
// on PCs, ASTC or ETC2 on phones), or to plain RGBA where it reads none of them.
#pragma once

#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "midgard/render/mesh_data.h"

namespace midgard::render {

class TextureError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct EncodeOptions {
    int zstd_level = 18;  // supercompression of the file (0: none)
};

// A texture (all its mip levels) as a KTX2 file: UASTC, sRGB or linear as the texture is.
// Normal maps are encoded as plain linear RGB, not KTX's two-channel normal layout (X in RGB,
// Y in alpha), so materials read every normal map the same way whatever its source.
std::vector<uint8_t> encode_ktx2(const TextureData& texture, const EncodeOptions& options = {});

enum class GpuFormat { BC7, ASTC4x4, ETC2, RGBA8 };
const char* gpu_format_name(GpuFormat format);

// GPU-ready texture data: every mip level in one format.
struct TranscodedTexture {
    uint32_t width = 0, height = 0;
    bool srgb = true;
    uint32_t vk_format = 0;  // a VkFormat
    std::vector<std::vector<uint8_t>> levels;
};

TranscodedTexture transcode_ktx2(std::span<const uint8_t> ktx2, GpuFormat format);
// The texture as plain RGBA8 again (tests, and devices with no compressed format).
TextureData decode_ktx2(std::span<const uint8_t> ktx2);

}  // namespace midgard::render
