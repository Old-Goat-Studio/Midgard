// SPDX-License-Identifier: Apache-2.0
// Kept apart from the renderer: KTX's headers declare Vulkan's format list themselves, which
// would clash with the Vulkan headers the renderer uses.
#include "midgard/render/ktx.h"

#include <cstdlib>
#include <memory>

#include <ktx.h>

namespace midgard::render {

namespace {

constexpr uint32_t kVkR8G8B8A8Unorm = 37;  // VkFormat values
constexpr uint32_t kVkR8G8B8A8Srgb = 43;

struct KtxDeleter {
    void operator()(ktxTexture2* t) const { ktxTexture_Destroy(ktxTexture(t)); }
};
using KtxPtr = std::unique_ptr<ktxTexture2, KtxDeleter>;

void check(KTX_error_code code, const char* what) {
    if (code != KTX_SUCCESS) throw TextureError(std::string(what) + ": " + ktxErrorString(code));
}

KtxPtr open(std::span<const uint8_t> bytes) {
    ktxTexture2* raw = nullptr;
    check(ktxTexture2_CreateFromMemory(bytes.data(), bytes.size(),
                                       KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &raw),
          "Reading a KTX2 texture");
    return KtxPtr(raw);
}

// The levels of a texture already in its final format.
TranscodedTexture levels_of(ktxTexture2* tex) {
    TranscodedTexture out;
    out.width = tex->baseWidth;
    out.height = tex->baseHeight;
    out.vk_format = tex->vkFormat;
    out.srgb = ktxTexture2_GetOETF(tex) == KHR_DF_TRANSFER_SRGB;
    const ktx_uint8_t* data = ktxTexture_GetData(ktxTexture(tex));
    for (ktx_uint32_t level = 0; level < tex->numLevels; ++level) {
        ktx_size_t offset = 0;
        check(ktxTexture_GetImageOffset(ktxTexture(tex), level, 0, 0, &offset),
              "Reading a texture level");
        const ktx_size_t size = ktxTexture_GetImageSize(ktxTexture(tex), level);
        out.levels.emplace_back(data + offset, data + offset + size);
    }
    return out;
}

}  // namespace

const char* gpu_format_name(GpuFormat format) {
    switch (format) {
        case GpuFormat::BC7:
            return "BC7";
        case GpuFormat::ASTC4x4:
            return "ASTC 4x4";
        case GpuFormat::ETC2:
            return "ETC2";
        case GpuFormat::RGBA8:
            return "RGBA8 (uncompressed)";
    }
    return "?";
}

std::vector<uint8_t> encode_ktx2(const TextureData& t, const EncodeOptions& options) {
    if (t.levels.empty() || t.width == 0 || t.height == 0)
        throw TextureError("encode_ktx2: an empty texture");
    ktxTextureCreateInfo info{};
    info.vkFormat = t.srgb ? kVkR8G8B8A8Srgb : kVkR8G8B8A8Unorm;
    info.baseWidth = t.width;
    info.baseHeight = t.height;
    info.baseDepth = 1;
    info.numDimensions = 2;
    info.numLevels = static_cast<ktx_uint32_t>(t.levels.size());
    info.numLayers = 1;
    info.numFaces = 1;
    info.isArray = KTX_FALSE;
    info.generateMipmaps = KTX_FALSE;
    ktxTexture2* raw = nullptr;
    check(ktxTexture2_Create(&info, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &raw),
          "Creating a KTX2 texture");
    KtxPtr tex(raw);
    uint32_t w = t.width, h = t.height;
    for (ktx_uint32_t level = 0; level < info.numLevels; ++level) {
        if (t.levels[level].size() != size_t(w) * h * 4)
            throw TextureError("encode_ktx2: mip level " + std::to_string(level) +
                               " has the wrong size");
        check(ktxTexture_SetImageFromMemory(ktxTexture(tex.get()), level, 0, 0,
                                            t.levels[level].data(), t.levels[level].size()),
              "Setting a texture level");
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    ktxBasisParams params{};
    params.structSize = sizeof(params);
    params.uastc = KTX_TRUE;  // high quality; transcodes almost losslessly to BC7 and ASTC
    params.uastcFlags = KTX_PACK_UASTC_LEVEL_DEFAULT;
    params.threadCount = 1;  // the same file every time the editor cooks it
    check(ktxTexture2_CompressBasisEx(tex.get(), &params), "Encoding a texture (Basis UASTC)");
    if (options.zstd_level > 0)
        check(ktxTexture2_DeflateZstd(tex.get(), static_cast<ktx_uint32_t>(options.zstd_level)),
              "Supercompressing a texture (zstd)");
    ktx_uint8_t* bytes = nullptr;
    ktx_size_t size = 0;
    check(ktxTexture_WriteToMemory(ktxTexture(tex.get()), &bytes, &size), "Writing a KTX2 file");
    std::vector<uint8_t> out(bytes, bytes + size);
    std::free(bytes);
    return out;
}

TranscodedTexture transcode_ktx2(std::span<const uint8_t> ktx2, GpuFormat format) {
    KtxPtr tex = open(ktx2);
    if (ktxTexture2_NeedsTranscoding(tex.get())) {
        ktx_transcode_fmt_e target = KTX_TTF_RGBA32;
        switch (format) {
            case GpuFormat::BC7:
                target = KTX_TTF_BC7_RGBA;
                break;
            case GpuFormat::ASTC4x4:
                target = KTX_TTF_ASTC_4x4_RGBA;
                break;
            case GpuFormat::ETC2:
                target = KTX_TTF_ETC2_RGBA;
                break;
            case GpuFormat::RGBA8:
                target = KTX_TTF_RGBA32;
                break;
        }
        check(ktxTexture2_TranscodeBasis(tex.get(), target, 0), "Transcoding a texture");
    }
    return levels_of(tex.get());
}

TextureData decode_ktx2(std::span<const uint8_t> ktx2) {
    TranscodedTexture t = transcode_ktx2(ktx2, GpuFormat::RGBA8);
    if (t.vk_format != kVkR8G8B8A8Unorm && t.vk_format != kVkR8G8B8A8Srgb)
        throw TextureError("decode_ktx2: the texture isn't Basis-encoded or RGBA8");
    TextureData out;
    out.width = t.width;
    out.height = t.height;
    out.srgb = t.srgb;
    out.levels = std::move(t.levels);
    return out;
}

}  // namespace midgard::render
