// SPDX-License-Identifier: Apache-2.0
#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#include "midgard/gltf.h"

#include <algorithm>
#include <cstdio>
#include <string>

#include "midgard/pak.h"

namespace midgard {

render::MeshData load_glb(std::span<const uint8_t> bytes) {
    cgltf_options options{};
    cgltf_data* data = nullptr;
    if (cgltf_parse(&options, bytes.data(), bytes.size(), &data) != cgltf_result_success)
        throw AssetError("not a valid glTF file");
    struct Free {
        cgltf_data* d;
        ~Free() { cgltf_free(d); }
    } guard{data};
    // For .glb files, buffers without a URI come from the embedded BIN chunk.
    if (cgltf_load_buffers(&options, data, nullptr) != cgltf_result_success)
        throw AssetError("glTF buffers could not be loaded (external files are not supported)");

    render::MeshData mesh;
    // Tangents are generated (MikkTSpace, as glTF recommends) when the file has uvs but none.
    bool all_tangents = true, all_uvs = true;
    for (cgltf_size m = 0; m < data->meshes_count; ++m) {
        const cgltf_mesh& gm = data->meshes[m];
        for (cgltf_size p = 0; p < gm.primitives_count; ++p) {
            const cgltf_primitive& prim = gm.primitives[p];
            if (prim.type != cgltf_primitive_type_triangles) continue;
            const cgltf_accessor* positions = nullptr;
            const cgltf_accessor* normals = nullptr;
            const cgltf_accessor* uvs = nullptr;
            const cgltf_accessor* tangents = nullptr;
            for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
                const cgltf_attribute& attr = prim.attributes[a];
                if (attr.type == cgltf_attribute_type_position) positions = attr.data;
                if (attr.type == cgltf_attribute_type_normal) normals = attr.data;
                if (attr.type == cgltf_attribute_type_texcoord && attr.index == 0) uvs = attr.data;
                if (attr.type == cgltf_attribute_type_tangent) tangents = attr.data;
            }
            if (!positions) continue;
            all_uvs = all_uvs && uvs;
            all_tangents = all_tangents && tangents;
            const auto base = static_cast<uint32_t>(mesh.vertices.size());
            for (cgltf_size i = 0; i < positions->count; ++i) {
                render::Vertex v{{0, 0, 0}, {0, 1, 0}, {0, 0}, {0, 0, 0, 0}};
                cgltf_accessor_read_float(positions, i, v.position, 3);
                if (normals) cgltf_accessor_read_float(normals, i, v.normal, 3);
                if (uvs) cgltf_accessor_read_float(uvs, i, v.uv, 2);
                if (tangents) cgltf_accessor_read_float(tangents, i, v.tangent, 4);
                mesh.vertices.push_back(v);
            }
            if (prim.indices) {
                for (cgltf_size i = 0; i < prim.indices->count; ++i)
                    mesh.indices.push_back(
                        base + static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, i)));
            } else {
                for (cgltf_size i = 0; i < positions->count; ++i)
                    mesh.indices.push_back(base + static_cast<uint32_t>(i));
            }
        }
    }
    if (mesh.indices.empty()) throw AssetError("the glTF file contains no triangles");
    if (!all_tangents && all_uvs) render::generate_tangents(mesh);
    return mesh;
}

std::vector<uint8_t> save_glb(const render::MeshData& mesh) {
    std::vector<uint8_t> bin;
    const auto append = [&](const void* data, size_t bytes) {
        const auto* b = static_cast<const uint8_t*>(data);
        bin.insert(bin.end(), b, b + bytes);
        while (bin.size() % 4) bin.push_back(0);
    };
    std::vector<float> pos, nrm, uv, tan;
    float lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        const render::Vertex& v = mesh.vertices[i];
        pos.insert(pos.end(), v.position, v.position + 3);
        nrm.insert(nrm.end(), v.normal, v.normal + 3);
        uv.insert(uv.end(), v.uv, v.uv + 2);
        tan.insert(tan.end(), v.tangent, v.tangent + 4);
        for (int k = 0; k < 3; ++k) {
            lo[k] = i ? std::min(lo[k], v.position[k]) : v.position[k];
            hi[k] = i ? std::max(hi[k], v.position[k]) : v.position[k];
        }
    }
    std::string views, accessors;
    const auto view = [&](const void* data, size_t bytes, const char* type, int component,
                          size_t count, bool bounds) {
        const size_t offset = bin.size();
        append(data, bytes);
        const size_t index = std::count(views.begin(), views.end(), '{');
        if (!views.empty()) views += ",";
        views += "{\"buffer\":0,\"byteOffset\":" + std::to_string(offset) +
                 ",\"byteLength\":" + std::to_string(bytes) + "}";
        if (!accessors.empty()) accessors += ",";
        accessors += "{\"bufferView\":" + std::to_string(index) +
                     ",\"componentType\":" + std::to_string(component) +
                     ",\"count\":" + std::to_string(count) + ",\"type\":\"" + type + "\"";
        if (bounds) {
            char text[160];
            std::snprintf(text, sizeof text, ",\"min\":[%.9g,%.9g,%.9g],\"max\":[%.9g,%.9g,%.9g]",
                          lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
            accessors += text;
        }
        accessors += "}";
    };
    const size_t n = mesh.vertices.size();
    view(pos.data(), pos.size() * 4, "VEC3", 5126, n, true);
    view(nrm.data(), nrm.size() * 4, "VEC3", 5126, n, false);
    view(uv.data(), uv.size() * 4, "VEC2", 5126, n, false);
    view(tan.data(), tan.size() * 4, "VEC4", 5126, n, false);
    view(mesh.indices.data(), mesh.indices.size() * 4, "SCALAR", 5125, mesh.indices.size(), false);
    std::string text =
        "{\"asset\":{\"version\":\"2.0\",\"generator\":\"Midgard\"},\"scene\":0,"
        "\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,"
        "\"TEXCOORD_0\":2,\"TANGENT\":3},\"indices\":4}]}],"
        "\"buffers\":[{\"byteLength\":" +
        std::to_string(bin.size()) +
        "}],"
        "\"bufferViews\":[" +
        views + "],\"accessors\":[" + accessors + "]}";
    while (text.size() % 4) text.push_back(' ');
    std::vector<uint8_t> out;
    const auto u32 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(uint8_t(v >> (8 * i)));
    };
    u32(0x46546C67);  // "glTF"
    u32(2);
    u32(uint32_t(12 + 8 + text.size() + 8 + bin.size()));
    u32(uint32_t(text.size()));
    u32(0x4E4F534A);  // "JSON"
    out.insert(out.end(), text.begin(), text.end());
    u32(uint32_t(bin.size()));
    u32(0x004E4942);  // "BIN"
    out.insert(out.end(), bin.begin(), bin.end());
    return out;
}

}  // namespace midgard
