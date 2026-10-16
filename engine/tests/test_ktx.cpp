// SPDX-License-Identifier: Apache-2.0
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "midgard/render/ktx.h"
#include "midgard/render/mesh_data.h"

using namespace midgard::render;

namespace {

// A 64 x 64 test texture: smooth gradients with a checker on top, plus its mipmaps.
TextureData test_texture(bool srgb) {
    std::vector<uint8_t> px;
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            const bool odd = ((x / 8) + (y / 8)) % 2;
            px.push_back(static_cast<uint8_t>(x * 4));
            px.push_back(static_cast<uint8_t>(y * 4));
            px.push_back(odd ? 200 : 40);
            px.push_back(255);
        }
    return make_texture(64, 64, srgb, std::move(px));
}

// Peak signal-to-noise ratio between two RGBA8 images, in decibels (higher is closer).
double psnr(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    double sum = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        const double d = double(a[i]) - double(b[i]);
        sum += d * d;
    }
    const double mse = sum / double(a.size());
    return mse == 0.0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

}  // namespace

TEST_CASE("textures survive encoding to KTX2 (UASTC) and decoding") {
    for (bool srgb : {true, false}) {
        const TextureData original = test_texture(srgb);
        const std::vector<uint8_t> file = encode_ktx2(original);
        REQUIRE(file.size() > 12);
        CHECK(std::memcmp(file.data(), "\xABKTX 20\xBB\r\n\x1A\n", 12) == 0);  // the KTX2 magic
        const TextureData back = decode_ktx2(file);
        CHECK(back.width == 64);
        CHECK(back.height == 64);
        CHECK(back.srgb == srgb);
        REQUIRE(back.levels.size() == original.levels.size());  // 64 down to 1: 7 levels
        CHECK(back.levels.size() == 7);
        CHECK(psnr(back.levels[0], original.levels[0]) > 38.0);  // visually lossless
        CHECK(psnr(back.levels[1], original.levels[1]) > 32.0);
        // At 8 x 8 every pixel is a different checker square, more than 4 x 4 blocks can keep.
        CHECK(psnr(back.levels[3], original.levels[3]) > 20.0);
    }
}

TEST_CASE("the same texture encodes to the same file (reproducible cooks)") {
    const TextureData t = test_texture(true);
    CHECK(encode_ktx2(t) == encode_ktx2(t));
}

TEST_CASE("KTX2 transcodes to the GPUs' compressed formats, in whole 4 x 4 blocks") {
    const std::vector<uint8_t> file = encode_ktx2(test_texture(true));
    for (GpuFormat f : {GpuFormat::BC7, GpuFormat::ASTC4x4, GpuFormat::ETC2}) {
        const TranscodedTexture t = transcode_ktx2(file, f);
        REQUIRE(t.levels.size() == 7);
        CHECK(t.srgb);
        CHECK(t.levels[0].size() == (64 / 4) * (64 / 4) * 16);  // 16 bytes per 4 x 4 block
        CHECK(t.levels[6].size() == 16);                        // a 1 x 1 level is still one block
        CHECK(t.vk_format != 0);
    }
    // BC7, ASTC and ETC2 are different formats.
    CHECK(transcode_ktx2(file, GpuFormat::BC7).vk_format !=
          transcode_ktx2(file, GpuFormat::ASTC4x4).vk_format);
    CHECK(std::string(gpu_format_name(GpuFormat::ASTC4x4)) == "ASTC 4x4");
}

TEST_CASE("normal maps keep their RGB layout, and bad input is refused") {
    std::vector<uint8_t> flat;
    for (int i = 0; i < 16 * 16; ++i) flat.insert(flat.end(), {128, 128, 255, 255});
    const TextureData decoded = decode_ktx2(encode_ktx2(make_texture(16, 16, false, flat)));
    // Still (128, 128, 255): pointing straight out, read as RGB like any other normal map.
    CHECK(std::abs(int(decoded.levels[0][0]) - 128) <= 2);
    CHECK(std::abs(int(decoded.levels[0][1]) - 128) <= 2);
    CHECK(std::abs(int(decoded.levels[0][2]) - 255) <= 2);
    const std::vector<uint8_t> junk(100, 7);
    CHECK_THROWS_AS(decode_ktx2(junk), TextureError);
    CHECK_THROWS_AS(encode_ktx2(TextureData{}), TextureError);
}
