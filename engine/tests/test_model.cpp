// SPDX-License-Identifier: Apache-2.0
#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include "midgard/model.h"
#include "midgard/pak.h"
#include "midgard/render/renderer.h"

namespace fs = std::filesystem;
using midgard::AssetError;
using midgard::ImportSettings;
using midgard::Model;
using nlohmann::json;

namespace {

// A unit quad in the XZ plane facing up (+Y), with uvs and no tangents: 4 vertices, 2 triangles.
std::vector<uint8_t> quad_buffer() {
    const float positions[] = {-0.5f, 0, 0.5f, 0.5f, 0, 0.5f, 0.5f, 0, -0.5f, -0.5f, 0, -0.5f};
    const float normals[] = {0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0};
    const float uvs[] = {0, 1, 1, 1, 1, 0, 0, 0};
    const uint16_t indices[] = {0, 1, 2, 0, 2, 3};
    std::vector<uint8_t> out;
    const auto add = [&](const void* data, size_t size) {
        const auto* b = static_cast<const uint8_t*>(data);
        out.insert(out.end(), b, b + size);
    };
    add(positions, sizeof positions);  // 48 bytes at 0
    add(normals, sizeof normals);      // 48 at 48
    add(uvs, sizeof uvs);              // 32 at 96
    add(indices, sizeof indices);      // 12 at 128
    return out;
}

// The glTF JSON for two quads: one at x = -2 with a textured material, one at x = +2 mirrored
// (scale -1 on x) with a plain red one. `image` is the image's entry.
json quad_gltf(const json& buffer, const json& image) {
    json doc = json::parse(R"({
      "asset": {"version": "2.0"},
      "scene": 0,
      "scenes": [{"nodes": [0, 1]}],
      "nodes": [
        {"name": "left", "mesh": 0, "translation": [-2, 0, 0]},
        {"name": "right", "mesh": 1, "translation": [2, 0, 0], "scale": [-1, 1, 1]}
      ],
      "meshes": [
        {"name": "textured", "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1,
          "TEXCOORD_0": 2}, "indices": 3, "material": 0}]},
        {"name": "red", "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1,
          "TEXCOORD_0": 2}, "indices": 3, "material": 1}]}
      ],
      "materials": [
        {"name": "checker", "pbrMetallicRoughness": {"baseColorTexture": {"index": 0},
          "metallicFactor": 0.0, "roughnessFactor": 0.6}},
        {"name": "red", "pbrMetallicRoughness": {"baseColorFactor": [1, 0, 0, 1],
          "metallicFactor": 1.0, "roughnessFactor": 0.2}, "emissiveFactor": [0.5, 0.25, 0],
          "doubleSided": true}
      ],
      "textures": [{"source": 0}],
      "bufferViews": [
        {"buffer": 0, "byteOffset": 0, "byteLength": 48},
        {"buffer": 0, "byteOffset": 48, "byteLength": 48},
        {"buffer": 0, "byteOffset": 96, "byteLength": 32},
        {"buffer": 0, "byteOffset": 128, "byteLength": 12}
      ],
      "accessors": [
        {"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3",
         "min": [-0.5, 0, -0.5], "max": [0.5, 0, 0.5]},
        {"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3"},
        {"bufferView": 2, "componentType": 5126, "count": 4, "type": "VEC2"},
        {"bufferView": 3, "componentType": 5123, "count": 6, "type": "SCALAR"}
      ]
    })");
    doc["buffers"] = json::array({buffer});
    doc["images"] = json::array({image});
    return doc;
}

std::vector<uint8_t> checker_png(const fs::path& where) {
    std::vector<uint8_t> rgba;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            const uint8_t v = ((x + y) % 2) ? 255 : 30;
            rgba.insert(rgba.end(), {v, v, v, 255});
        }
    REQUIRE(midgard::render::write_png(where.string(), 8, 8, rgba));
    std::ifstream in(where, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

struct Folder {
    fs::path dir;
    Folder() {
        std::random_device rd;
        dir = fs::temp_directory_path() / ("midgard_model_" + std::to_string(rd()));
        fs::create_directories(dir);
    }
    ~Folder() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    fs::path write(const std::string& name, const std::vector<uint8_t>& bytes) const {
        std::ofstream(dir / name, std::ios::binary)
            .write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        return dir / name;
    }
    fs::path write(const std::string& name, const std::string& text) const {
        return write(name, std::vector<uint8_t>(text.begin(), text.end()));
    }
};

// A .glb: the JSON chunk and a BIN chunk holding the quad and the PNG.
std::vector<uint8_t> glb(json doc, const std::vector<uint8_t>& png) {
    std::vector<uint8_t> bin = quad_buffer();
    const size_t image_at = bin.size();
    bin.insert(bin.end(), png.begin(), png.end());
    while (bin.size() % 4) bin.push_back(0);
    doc["buffers"] = json::array({json{{"byteLength", bin.size()}}});
    doc["bufferViews"].push_back(
        json{{"buffer", 0}, {"byteOffset", image_at}, {"byteLength", png.size()}});
    doc["images"] = json::array({json{{"bufferView", 4}, {"mimeType", "image/png"}}});
    std::string text = doc.dump();
    while (text.size() % 4) text.push_back(' ');
    std::vector<uint8_t> out;
    const auto u32 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(uint8_t(v >> (8 * i)));
    };
    out.insert(out.end(), {'g', 'l', 'T', 'F'});
    u32(2);
    u32(uint32_t(12 + 8 + text.size() + 8 + bin.size()));
    u32(uint32_t(text.size()));
    out.insert(out.end(), {'J', 'S', 'O', 'N'});
    out.insert(out.end(), text.begin(), text.end());
    u32(uint32_t(bin.size()));
    out.insert(out.end(), {'B', 'I', 'N', '\0'});
    out.insert(out.end(), bin.begin(), bin.end());
    return out;
}

glm::vec3 position(const midgard::render::Vertex& v) {
    return {v.position[0], v.position[1], v.position[2]};
}

}  // namespace

TEST_CASE("a .gltf with its .bin and texture: parts per material, transforms, materials") {
    Folder f;
    const std::vector<uint8_t> png = checker_png(f.dir / "checker.png");
    f.write("quad.bin", quad_buffer());
    const fs::path file = f.write(
        "quads.gltf",
        quad_gltf(json{{"uri", "quad.bin"}, {"byteLength", 140}}, json{{"uri", "checker.png"}})
            .dump());
    const Model model = midgard::load_model(file);
    CHECK(model.warnings.empty());
    REQUIRE(model.parts.size() == 2);
    REQUIRE(model.materials.size() == 2);
    REQUIRE(model.textures.size() == 1);

    // The textured quad, moved to x = -2; its texture decoded as sRGB, with mipmaps.
    const auto& left = model.parts[0];
    CHECK(left.name == "checker");
    CHECK(left.mesh.indices.size() == 6);
    for (const auto& v : left.mesh.vertices) CHECK(std::abs(position(v).x + 2.0f) <= 0.5f);
    CHECK(model.materials[0].base_color_texture == 0);
    CHECK(model.materials[0].roughness == doctest::Approx(0.6f));
    CHECK(model.textures[0].data.srgb);
    CHECK(model.textures[0].data.width == 8);
    CHECK(model.textures[0].data.levels.size() == 4);  // 8, 4, 2, 1
    CHECK(left.mesh.vertices[0].tangent[3] != 0.0f);   // tangents made (it had uvs, no tangents)

    // The red quad, mirrored at x = +2: still facing up (its triangles turned back).
    const auto& right = model.parts[1];
    const glm::vec3 a = position(right.mesh.vertices[right.mesh.indices[0]]);
    const glm::vec3 b = position(right.mesh.vertices[right.mesh.indices[1]]);
    const glm::vec3 c = position(right.mesh.vertices[right.mesh.indices[2]]);
    CHECK(glm::cross(b - a, c - a).y > 0.0f);
    CHECK(right.mesh.vertices[0].normal[1] == doctest::Approx(1.0f));
    CHECK(model.materials[1].base_color == glm::vec4(1, 0, 0, 1));
    CHECK(model.materials[1].metallic == 1.0f);
    CHECK(model.materials[1].emissive == glm::vec3(0.5f, 0.25f, 0.0f));
    CHECK(model.materials[1].double_sided);
    CHECK(model.bounds_min.x == doctest::Approx(-2.5f));
    CHECK(model.bounds_max.x == doctest::Approx(2.5f));
}

TEST_CASE("a .glb with its image inside, and the import settings") {
    Folder f;
    const std::vector<uint8_t> png = checker_png(f.dir / "checker.png");
    const std::vector<uint8_t> bytes = glb(quad_gltf(json::object(), json::object()), png);
    const Model model = midgard::load_model(bytes);
    CHECK(model.warnings.empty());
    CHECK(model.textures.size() == 1);
    CHECK(model.parts.size() == 2);

    // Centimetres and +Z up: a model 2.5 m to the side becomes 2.5 cm, and +Z becomes +Y.
    ImportSettings settings;
    settings.scale = 0.01f;
    settings.z_up = true;
    const Model small = midgard::load_model(bytes, settings);
    CHECK(small.bounds_max.x == doctest::Approx(0.025f));
    // The quads faced +Y in the file; Z up turns that to -Z... and the quads' depth (file Z)
    // becomes height (Y).
    CHECK(small.parts[0].mesh.vertices[0].normal[2] == doctest::Approx(-1.0f));
    CHECK(small.bounds_max.y == doctest::Approx(0.005f));
}

TEST_CASE("awkward files: missing textures, paths outside the folder, no triangles") {
    Folder f;
    f.write("quad.bin", quad_buffer());
    // A missing texture: imported without it, and a warning that says so.
    const fs::path missing =
        f.write("missing.gltf",
                quad_gltf(json{{"uri", "quad.bin"}, {"byteLength", 140}}, json{{"uri", "gone.png"}})
                    .dump());
    Model model = midgard::load_model(missing);
    CHECK(model.materials[0].base_color_texture == -1);
    REQUIRE(model.warnings.size() == 1);
    CHECK(model.warnings[0].find("gone.png") != std::string::npos);
    CHECK(model.warnings[0].find("missing") != std::string::npos);

    // A texture or a buffer outside the model's folder is never read.
    const fs::path escaping = f.write(
        "escaping.gltf",
        quad_gltf(json{{"uri", "quad.bin"}, {"byteLength", 140}}, json{{"uri", "../secret.png"}})
            .dump());
    model = midgard::load_model(escaping);
    CHECK(model.warnings[0].find("outside") != std::string::npos);
    const fs::path outside_bin =
        f.write("outside_bin.gltf",
                quad_gltf(json{{"uri", "../quad.bin"}, {"byteLength", 140}}, json{{"uri", "x.png"}})
                    .dump());
    CHECK_THROWS_WITH_AS(midgard::load_model(outside_bin), doctest::Contains("missing"),
                         AssetError);

    // A .bin shorter than the file says, no triangles, not glTF, glTF 1.0, needs its files.
    std::vector<uint8_t> short_bin = quad_buffer();
    short_bin.resize(100);
    f.write("short.bin", short_bin);
    const fs::path short_file = f.write(
        "short.gltf",
        quad_gltf(json{{"uri", "short.bin"}, {"byteLength", 140}}, json{{"uri", "x.png"}}).dump());
    CHECK_THROWS_AS(midgard::load_model(short_file), AssetError);
    const json empty = json::parse(R"({"asset": {"version": "2.0"}})");
    CHECK_THROWS_WITH_AS(midgard::load_model(f.write("empty.gltf", empty.dump())),
                         doctest::Contains("no triangles"), AssetError);
    CHECK_THROWS_WITH_AS(midgard::load_model(f.write(
                             "text.gltf", std::string("this is plain text, not a glTF file"))),
                         doctest::Contains("JSON"), AssetError);
    CHECK_THROWS_WITH_AS(
        midgard::load_model(f.write("old.gltf", std::string(R"({"asset":{"version":"1.0"}})"))),
        doctest::Contains("1.0"), AssetError);
    const std::string needs_bin =
        quad_gltf(json{{"uri", "quad.bin"}, {"byteLength", 140}}, json{{"uri", "x.png"}}).dump();
    CHECK_THROWS_WITH_AS(
        midgard::load_model(std::vector<uint8_t>(needs_bin.begin(), needs_bin.end())),
        doctest::Contains("beside it"), AssetError);
}

TEST_CASE("fuzzing: damaged models are refused with an error, never a crash") {
    Folder f;
    const std::vector<uint8_t> png = checker_png(f.dir / "checker.png");
    const std::vector<uint8_t> good_glb = glb(quad_gltf(json::object(), json::object()), png);
    f.write("quad.bin", quad_buffer());
    const std::string good_json =
        quad_gltf(json{{"uri", "quad.bin"}, {"byteLength", 140}}, json{{"uri", "checker.png"}})
            .dump();
    std::mt19937 rng(20261016);
    int refused = 0, loaded = 0;
    const auto damage = [&](std::vector<uint8_t> bytes) {
        std::uniform_int_distribution<size_t> at(0, bytes.size() - 1);
        const int kind = int(rng() % 4);
        if (kind == 0) {
            bytes.resize(at(rng));  // cut short
        } else {
            const int flips = 1 + int(rng() % 8);
            for (int i = 0; i < flips; ++i)
                bytes[at(rng)] =
                    kind == 1 ? uint8_t(rng()) : uint8_t(bytes[at(rng)] ^ (1u << (rng() % 8)));
        }
        return bytes;
    };
    for (int i = 0; i < 3000; ++i) {
        try {
            (void)midgard::load_model(damage(good_glb));
            ++loaded;
        } catch (const AssetError&) {
            ++refused;
        }
    }
    for (int i = 0; i < 400; ++i) {
        const auto bytes = damage(std::vector<uint8_t>(good_json.begin(), good_json.end()));
        try {
            (void)midgard::load_model(f.write("fuzz.gltf", bytes));
            ++loaded;
        } catch (const AssetError&) {
            ++refused;
        }
    }
    CHECK(refused + loaded == 3400);  // every one came back: loaded, or refused with a reason
    CHECK(refused > 1000);
}
