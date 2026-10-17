// SPDX-License-Identifier: Apache-2.0
#include "midgard/model.h"

#include <cgltf.h>      // implemented in gltf.cpp
#include <stb_image.h>  // implemented in render/src/third_party_impl.cpp (PNG and JPEG only)

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "midgard/pak.h"

namespace midgard {
namespace {

std::vector<uint8_t> read_file(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// A path inside `dir` for a relative URI (percent-decoded), or nothing if it would lead
// outside it (an absolute path, a drive, or "..").
std::optional<std::filesystem::path> inside(const std::filesystem::path& dir, std::string uri) {
    cgltf_decode_uri(uri.data());
    uri.resize(std::strlen(uri.c_str()));
    const std::filesystem::path relative =
        std::filesystem::path(std::u8string(uri.begin(), uri.end()));
    if (relative.empty() || relative.is_absolute() || relative.has_root_name() ||
        relative.has_root_directory())
        return std::nullopt;
    for (const auto& part : relative)
        if (part == "..") return std::nullopt;
    return dir / relative;
}

// cgltf's reader for external buffers, kept inside the model's folder.
struct FileAccess {
    std::filesystem::path dir;
};

cgltf_result read_inside(const cgltf_memory_options* memory, const cgltf_file_options* file,
                         const char* path, cgltf_size* size, void** data) {
    const auto* access = static_cast<const FileAccess*>(file->user_data);
    // cgltf joins the model's path and the URI; take the URI part back off.
    std::string uri = path;
    const std::string base =
        access->dir.u8string().empty()
            ? std::string()
            : std::string(reinterpret_cast<const char*>(access->dir.u8string().c_str()));
    if (!base.empty() && uri.rfind(base, 0) == 0) uri = uri.substr(base.size());
    while (!uri.empty() && (uri.front() == '/' || uri.front() == '\\')) uri.erase(0, 1);
    const auto where = inside(access->dir, uri);
    if (!where) return cgltf_result_file_not_found;
    std::vector<uint8_t> bytes = read_file(*where);
    if (bytes.empty() && !std::filesystem::exists(*where)) return cgltf_result_file_not_found;
    if (*size > bytes.size()) return cgltf_result_data_too_short;  // shorter than it claims
    void* out = memory->alloc_func ? memory->alloc_func(memory->user_data, bytes.size())
                                   : std::malloc(bytes.size());
    if (!out && !bytes.empty()) return cgltf_result_out_of_memory;
    std::memcpy(out, bytes.data(), bytes.size());
    *size = bytes.size();
    *data = out;
    return cgltf_result_success;
}

void release(const cgltf_memory_options* memory, const cgltf_file_options*, void* data) {
    if (memory->free_func)
        memory->free_func(memory->user_data, data);
    else
        std::free(data);
}

struct Data {
    cgltf_data* data = nullptr;
    ~Data() {
        if (data) cgltf_free(data);
    }
};

class Importer {
public:
    Importer(cgltf_data* data, std::optional<std::filesystem::path> dir,
             const ImportSettings& settings)
        : data_(data), dir_(std::move(dir)), settings_(settings) {}

    Model run() {
        for (cgltf_size m = 0; m < data_->materials_count; ++m)
            model_.materials.push_back(material(data_->materials[m]));
        glm::mat4 root = glm::scale(glm::mat4(1.0f), glm::vec3(settings_.scale));
        if (settings_.z_up)  // +Z up becomes +Y up
            root = root * glm::rotate(glm::mat4(1.0f), glm::radians(-90.0f), glm::vec3(1, 0, 0));
        for (cgltf_size n = 0; n < data_->nodes_count; ++n) {
            const cgltf_node& node = data_->nodes[n];
            if (!node.mesh || !in_scene(node)) continue;
            float world[16];
            cgltf_node_transform_world(&node, world);
            add_mesh(*node.mesh, root * glm::make_mat4(world),
                     node.name ? node.name : (node.mesh->name ? node.mesh->name : "mesh"));
        }
        if (model_.parts.empty()) throw AssetError("the glTF file contains no triangles");
        bool first = true;
        for (size_t i = 0; i < model_.parts.size(); ++i) {
            ModelPart& part = model_.parts[i];
            // MikkTSpace tangents (as glTF recommends) where the file has uvs but no tangents.
            const auto [tangents, uvs] = part_tangents_[i];
            if (!tangents && uvs) render::generate_tangents(part.mesh);
            for (const render::Vertex& v : part.mesh.vertices) {
                const glm::vec3 p(v.position[0], v.position[1], v.position[2]);
                model_.bounds_min = first ? p : glm::min(model_.bounds_min, p);
                model_.bounds_max = first ? p : glm::max(model_.bounds_max, p);
                first = false;
            }
        }
        return std::move(model_);
    }

private:
    // Nodes of the default scene (all of them if the file names no scene).
    bool in_scene(const cgltf_node& node) const {
        const cgltf_scene* scene =
            data_->scene ? data_->scene : (data_->scenes_count ? data_->scenes : nullptr);
        if (!scene) return true;
        const cgltf_node* top = &node;
        while (top->parent) top = top->parent;
        for (cgltf_size i = 0; i < scene->nodes_count; ++i)
            if (scene->nodes[i] == top) return true;
        return false;
    }

    int texture(const cgltf_texture_view& view, bool srgb, const char* slot) {
        if (!view.texture || !view.texture->image) return -1;
        const cgltf_image* image = view.texture->image;
        const auto key = std::make_pair(image, srgb);
        if (const auto it = textures_.find(key); it != textures_.end()) return it->second;
        const std::string name = image->name  ? image->name
                                 : image->uri ? (std::strncmp(image->uri, "data:", 5) == 0
                                                     ? std::string("embedded image")
                                                     : std::string(image->uri))
                                              : std::string("embedded image");
        std::vector<uint8_t> bytes;
        if (image->buffer_view) {
            const cgltf_buffer_view* bv = image->buffer_view;
            if (bv->buffer && bv->buffer->data && bv->offset + bv->size <= bv->buffer->size) {
                const auto* begin = static_cast<const uint8_t*>(bv->buffer->data) + bv->offset;
                bytes.assign(begin, begin + bv->size);
            }
        } else if (image->uri && std::strncmp(image->uri, "data:", 5) == 0) {
            const char* comma = std::strstr(image->uri, ";base64,");
            if (comma) {
                const char* b64 = comma + 8;
                const size_t n = std::strlen(b64);
                const size_t size =
                    n / 4 * 3 - (n > 0 && b64[n - 1] == '=') - (n > 1 && b64[n - 2] == '=');
                void* out = nullptr;
                cgltf_options options{};
                if (cgltf_load_buffer_base64(&options, size, b64, &out) == cgltf_result_success) {
                    bytes.assign(static_cast<uint8_t*>(out), static_cast<uint8_t*>(out) + size);
                    std::free(out);
                }
            }
        } else if (image->uri) {
            const auto where = dir_ ? inside(*dir_, image->uri) : std::nullopt;
            if (!where) {
                warn(std::string(slot) + " texture '" + name +
                     "' is outside the model's folder (or the model has no folder): left out");
                return textures_[key] = -1;
            }
            bytes = read_file(*where);
            if (bytes.empty()) {
                warn(std::string(slot) + " texture '" + name + "' is missing: left out");
                return textures_[key] = -1;
            }
        }
        int w = 0, h = 0, channels = 0;
        stbi_uc* pixels = bytes.empty() ? nullptr
                                        : stbi_load_from_memory(bytes.data(), int(bytes.size()), &w,
                                                                &h, &channels, 4);
        if (!pixels) {
            warn(std::string(slot) + " texture '" + name + "' can't be read (" +
                 (stbi_failure_reason() ? stbi_failure_reason() : "no data") +
                 "; PNG and JPEG are supported): left out");
            return textures_[key] = -1;
        }
        std::vector<uint8_t> rgba(pixels, pixels + size_t(w) * size_t(h) * 4);
        stbi_image_free(pixels);
        model_.textures.push_back(
            {name, render::make_texture(uint32_t(w), uint32_t(h), srgb, std::move(rgba))});
        return textures_[key] = int(model_.textures.size() - 1);
    }

    ModelMaterial material(const cgltf_material& m) {
        ModelMaterial out;
        out.name = m.name ? m.name : "material";
        if (m.has_pbr_metallic_roughness) {
            const auto& pbr = m.pbr_metallic_roughness;
            out.base_color = glm::make_vec4(pbr.base_color_factor);
            out.metallic = pbr.metallic_factor;
            out.roughness = pbr.roughness_factor;
            out.base_color_texture = texture(pbr.base_color_texture, true, "base colour");
            out.metallic_roughness_texture =
                texture(pbr.metallic_roughness_texture, false, "metal/roughness");
        }
        out.emissive = glm::make_vec3(m.emissive_factor);
        if (m.has_emissive_strength) out.emissive_strength = m.emissive_strength.emissive_strength;
        out.emissive_texture = texture(m.emissive_texture, true, "emissive");
        out.normal_texture = texture(m.normal_texture, false, "normal");
        out.normal_scale = m.normal_texture.texture ? m.normal_texture.scale : 1.0f;
        out.occlusion_texture = texture(m.occlusion_texture, false, "occlusion");
        out.occlusion_strength = m.occlusion_texture.texture ? m.occlusion_texture.scale : 1.0f;
        out.transparent = m.alpha_mode == cgltf_alpha_mode_blend;
        out.double_sided = m.double_sided;
        return out;
    }

    // A part per material; the mesh's triangles go into theirs, moved by `transform`.
    void add_mesh(const cgltf_mesh& mesh, const glm::mat4& transform, const std::string& name) {
        const glm::mat3 normals = glm::transpose(glm::inverse(glm::mat3(transform)));
        const bool mirrored = glm::determinant(glm::mat3(transform)) < 0.0f;
        for (cgltf_size p = 0; p < mesh.primitives_count; ++p) {
            const cgltf_primitive& prim = mesh.primitives[p];
            if (prim.type != cgltf_primitive_type_triangles) {
                warn("mesh '" + name + "': a primitive of lines or points was left out");
                continue;
            }
            const cgltf_accessor *positions = nullptr, *normal = nullptr, *uvs = nullptr,
                                 *tangents = nullptr;
            for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
                const cgltf_attribute& attr = prim.attributes[a];
                if (attr.type == cgltf_attribute_type_position) positions = attr.data;
                if (attr.type == cgltf_attribute_type_normal) normal = attr.data;
                if (attr.type == cgltf_attribute_type_texcoord && attr.index == 0) uvs = attr.data;
                if (attr.type == cgltf_attribute_type_tangent) tangents = attr.data;
            }
            if (!positions) continue;
            const int material = prim.material ? int(prim.material - data_->materials) : -1;
            auto [it, added] = parts_.try_emplace(material, model_.parts.size());
            if (added) {
                model_.parts.push_back({material >= 0 ? model_.materials[size_t(material)].name
                                                      : std::string("default"),
                                        {},
                                        material});
                part_tangents_[it->second] = {true, true};
            }
            const size_t index = it->second;
            ModelPart& part = model_.parts[index];
            part_tangents_[index].first = part_tangents_[index].first && tangents;
            part_tangents_[index].second = part_tangents_[index].second && uvs;
            const auto base = static_cast<uint32_t>(part.mesh.vertices.size());
            for (cgltf_size i = 0; i < positions->count; ++i) {
                render::Vertex v{{0, 0, 0}, {0, 1, 0}, {0, 0}, {0, 0, 0, 0}};
                float p3[3] = {0, 0, 0}, n3[3] = {0, 1, 0}, t4[4] = {0, 0, 0, 0};
                cgltf_accessor_read_float(positions, i, p3, 3);
                if (normal) cgltf_accessor_read_float(normal, i, n3, 3);
                if (uvs) cgltf_accessor_read_float(uvs, i, v.uv, 2);
                if (tangents) cgltf_accessor_read_float(tangents, i, t4, 4);
                const glm::vec3 pos(transform * glm::vec4(glm::make_vec3(p3), 1.0f));
                glm::vec3 nrm = normals * glm::make_vec3(n3);
                nrm = glm::dot(nrm, nrm) > 0.0f ? glm::normalize(nrm) : glm::vec3(0, 1, 0);
                glm::vec3 tan = glm::mat3(transform) * glm::make_vec3(t4);
                if (glm::dot(tan, tan) > 0.0f) tan = glm::normalize(tan);
                for (int k = 0; k < 3; ++k) {
                    v.position[k] = pos[k];
                    v.normal[k] = nrm[k];
                    v.tangent[k] = tan[k];
                }
                v.tangent[3] = mirrored ? -t4[3] : t4[3];
                part.mesh.vertices.push_back(v);
            }
            std::vector<uint32_t> indices;
            if (prim.indices) {
                for (cgltf_size i = 0; i < prim.indices->count; ++i) {
                    const cgltf_size k = cgltf_accessor_read_index(prim.indices, i);
                    if (k >= positions->count)
                        throw AssetError("mesh '" + name + "' has an index past its vertices");
                    indices.push_back(base + uint32_t(k));
                }
            } else {
                for (cgltf_size i = 0; i < positions->count; ++i)
                    indices.push_back(base + uint32_t(i));
            }
            indices.resize(indices.size() / 3 * 3);
            if (mirrored)  // a mirrored node turns its triangles inside out: turn them back
                for (size_t i = 0; i < indices.size(); i += 3)
                    std::swap(indices[i + 1], indices[i + 2]);
            part.mesh.indices.insert(part.mesh.indices.end(), indices.begin(), indices.end());
        }
    }

    void warn(const std::string& text) {
        if (std::find(model_.warnings.begin(), model_.warnings.end(), text) ==
            model_.warnings.end())
            model_.warnings.push_back(text);
    }

    cgltf_data* data_;
    std::optional<std::filesystem::path> dir_;
    ImportSettings settings_;
    Model model_;
    std::map<int, size_t> parts_;                            // material index -> part
    std::map<size_t, std::pair<bool, bool>> part_tangents_;  // part -> (all tangents, all uvs)
    std::map<std::pair<const cgltf_image*, bool>, int> textures_;
};

const char* explain(cgltf_result r) {
    switch (r) {
        case cgltf_result_data_too_short:
            return "the file is cut short";
        case cgltf_result_unknown_format:
            return "not a glTF file";
        case cgltf_result_invalid_json:
            return "its JSON is broken";
        case cgltf_result_invalid_gltf:
            return "it breaks the glTF rules";
        case cgltf_result_file_not_found:
            return "a file it needs is missing (or outside its folder)";
        case cgltf_result_io_error:
            return "a file it needs can't be read";
        case cgltf_result_out_of_memory:
            return "it is too big";
        case cgltf_result_legacy_gltf:
            return "it is glTF 1.0 (only 2.0 is supported)";
        default:
            return "it can't be read";
    }
}

Model finish(Data& d, cgltf_options& options, const char* path,
             std::optional<std::filesystem::path> dir, const ImportSettings& settings) {
    if (const cgltf_result r = cgltf_load_buffers(&options, d.data, path);
        r != cgltf_result_success)
        throw AssetError(std::string("glTF buffers: ") +
                         (!path && r == cgltf_result_unknown_format
                              ? "it needs files beside it (import it from its file)"
                              : explain(r)));
    if (const cgltf_result r = cgltf_validate(d.data); r != cgltf_result_success)
        throw AssetError(std::string("glTF: ") + explain(r));
    return Importer(d.data, std::move(dir), settings).run();
}

}  // namespace

Model load_model(const std::filesystem::path& file, const ImportSettings& settings) {
    const std::vector<uint8_t> bytes = read_file(file);
    if (bytes.empty()) throw AssetError("can't read " + file.string());
    FileAccess access{file.parent_path()};
    cgltf_options options{};
    options.file.read = &read_inside;
    options.file.release = &release;
    options.file.user_data = &access;
    Data d;
    if (const cgltf_result r = cgltf_parse(&options, bytes.data(), bytes.size(), &d.data);
        r != cgltf_result_success)
        throw AssetError(file.filename().string() + ": " + explain(r));
    const std::u8string u8 = file.u8string();
    const std::string path(u8.begin(), u8.end());
    try {
        return finish(d, options, path.c_str(), file.parent_path(), settings);
    } catch (const AssetError& e) {
        throw AssetError(file.filename().string() + ": " + e.what());
    }
}

Model load_model(std::span<const uint8_t> bytes, const ImportSettings& settings) {
    cgltf_options options{};
    Data d;
    if (const cgltf_result r = cgltf_parse(&options, bytes.data(), bytes.size(), &d.data);
        r != cgltf_result_success)
        throw AssetError(std::string("glTF: ") + explain(r));
    return finish(d, options, nullptr, std::nullopt, settings);
}

}  // namespace midgard
