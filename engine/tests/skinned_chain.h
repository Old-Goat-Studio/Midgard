// SPDX-License-Identifier: Apache-2.0
// A tiny skinned glTF for tests (milestone 5.13), built in code.
#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace {

// A three-joint chain (j0 at the origin, j1 a metre up, j2 a metre above that) under an
// armature moved 5 m along X, one triangle skinned to it (a vertex on each joint), and a clip
// "Bend" turning j1 from rest to 90 degrees about Z over a second.
inline std::vector<std::uint8_t> chain_glb() {
    std::vector<std::uint8_t> bin;
    const auto put = [&](const void* p, size_t n) {
        const auto* b = static_cast<const std::uint8_t*>(p);
        bin.insert(bin.end(), b, b + n);
    };
    const float positions[9] = {0, 0, 0, 0.1f, 1, 0, 0, 2, 0};  // 0: 36 bytes
    put(positions, sizeof positions);
    const std::uint16_t joints[12] = {0, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0};  // 36: 24
    put(joints, sizeof joints);
    const float weights[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};  // 60: 48
    put(weights, sizeof weights);
    for (int j = 0; j < 3; ++j) {  // 108: 192 (inverse binds: translate by -j up)
        const glm::mat4 m = glm::translate(glm::mat4(1.0f), glm::vec3(0, -float(j), 0));
        put(&m[0][0], 64);
    }
    const float times[2] = {0, 1};  // 300: 8
    put(times, sizeof times);
    const float s = std::sqrt(0.5f);
    const float turns[8] = {0, 0, 0, 1, 0, 0, s, s};  // 308: 32
    put(turns, sizeof turns);
    const std::string json = R"({
      "asset": {"version": "2.0"},
      "scene": 0, "scenes": [{"nodes": [0]}],
      "nodes": [
        {"name": "Armature", "translation": [5, 0, 0], "children": [1, 4]},
        {"name": "j0", "children": [2]},
        {"name": "j1", "translation": [0, 1, 0], "children": [3]},
        {"name": "j2", "translation": [0, 1, 0]},
        {"name": "Body", "mesh": 0, "skin": 0}],
      "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "JOINTS_0": 1,
                                                  "WEIGHTS_0": 2}}]}],
      "skins": [{"joints": [1, 2, 3], "inverseBindMatrices": 3}],
      "animations": [{"name": "Bend", "channels": [{"sampler": 0,
                       "target": {"node": 2, "path": "rotation"}}],
                      "samplers": [{"input": 4, "output": 5, "interpolation": "LINEAR"}]}],
      "accessors": [
        {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
         "min": [0, 0, 0], "max": [0.1, 2, 0]},
        {"bufferView": 1, "componentType": 5123, "count": 3, "type": "VEC4"},
        {"bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC4"},
        {"bufferView": 3, "componentType": 5126, "count": 3, "type": "MAT4"},
        {"bufferView": 4, "componentType": 5126, "count": 2, "type": "SCALAR",
         "min": [0], "max": [1]},
        {"bufferView": 5, "componentType": 5126, "count": 2, "type": "VEC4"}],
      "bufferViews": [
        {"buffer": 0, "byteOffset": 0, "byteLength": 36},
        {"buffer": 0, "byteOffset": 36, "byteLength": 24},
        {"buffer": 0, "byteOffset": 60, "byteLength": 48},
        {"buffer": 0, "byteOffset": 108, "byteLength": 192},
        {"buffer": 0, "byteOffset": 300, "byteLength": 8},
        {"buffer": 0, "byteOffset": 308, "byteLength": 32}],
      "buffers": [{"byteLength": 340}]})";
    std::string padded = json;
    while (padded.size() % 4) padded += ' ';
    std::vector<std::uint8_t> out;
    const auto u32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(std::uint8_t(v >> (8 * i)));
    };
    u32(0x46546C67);  // glTF
    u32(2);
    u32(std::uint32_t(12 + 8 + padded.size() + 8 + bin.size()));
    u32(std::uint32_t(padded.size()));
    u32(0x4E4F534A);  // JSON
    out.insert(out.end(), padded.begin(), padded.end());
    u32(std::uint32_t(bin.size()));
    u32(0x004E4942);  // BIN
    out.insert(out.end(), bin.begin(), bin.end());
    return out;
}

}  // namespace
