// SPDX-License-Identifier: Apache-2.0
// _midgard: the engine as a Python extension module for the editor (roadmap §2, channel 3).
// Anything that must match the game exactly is done here rather than re-implemented in Python.
//
//   import _midgard
//   _midgard.version()            -> {"engine": "0.1.0", "engine_abi": 0, ...}
//   _midgard.validate(package)    -> {"ok": bool, "errors": [...], "warnings": [...], ...}
//   _midgard.Viewport()           the editor's 3D view (see viewport.h)
//   _midgard.Body(settings_json)  a world body's shape: positions, up and gravity, exactly as
//                                 the game computes them (see midgard/shape.h)
//   _midgard.import_model(path, settings_json)
//                                 a glTF model cooked for a package: its parts as .glb bytes,
//                                 its textures as KTX2 bytes, its materials (midgard/model.h)
//   _midgard.encode_texture(path, srgb)
//                                 an image file (PNG, JPEG) as KTX2 bytes with mipmaps
#include <array>
#include <fstream>
#include <iterator>
#include <span>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/stl/filesystem.h>

#include <nlohmann/json.hpp>

#include "midgard/body.h"
#include "midgard/gltf.h"
#include "midgard/link.h"
#include "midgard/model.h"
#include "midgard/pak.h"
#include "midgard/render/ktx.h"
#include "midgard/render/mesh_data.h"
#include "midgard/validate.h"
#include "midgard/version.h"
#include "viewport.h"

namespace py = pybind11;
using midgard::editor::Viewport;

namespace {

glm::vec3 vec3(const std::array<float, 3>& v) {
    return {v[0], v[1], v[2]};
}

}  // namespace

PYBIND11_MODULE(_midgard, m) {
    m.doc() = "The Midgard engine, for the Midgard editor.";

    m.def(
        "version",
        [] {
            py::dict v;
            v["engine"] = midgard::kVersion;
            v["engine_abi"] = midgard::kEngineAbi;
            v["format_major"] = midgard::kFormatMajor;
            v["schema_min"] = midgard::kSchemaMin;
            v["schema_max"] = midgard::kSchemaMax;
            v["live_link_protocol"] = midgard::link::kProtocol;
            return v;
        },
        "The engine's version and the package formats it reads.");

    m.def(
        "encode_texture",
        [](const std::filesystem::path& path, bool srgb) {
            std::vector<uint8_t> ktx2;
            {
                py::gil_scoped_release release;
                std::ifstream in(path, std::ios::binary);
                if (!in) throw midgard::AssetError("can't open " + path.filename().string());
                const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                                 std::istreambuf_iterator<char>());
                ktx2 = midgard::render::encode_ktx2(midgard::load_image(bytes, srgb));
            }
            return py::bytes(reinterpret_cast<const char*>(ktx2.data()), ktx2.size());
        },
        py::arg("path"), py::arg("srgb") = true,
        "An image file (PNG or JPEG) as KTX2 bytes with mipmaps, sRGB for colours. Raises "
        "RuntimeError for one that can't be read.");

    m.def(
        "import_model",
        [](const std::filesystem::path& path, const std::string& settings_json) {
            const nlohmann::json s = nlohmann::json::parse(settings_json);
            midgard::ImportSettings settings;
            settings.scale = s.value("scale", 1.0f);
            settings.z_up = s.value("z_up", false);
            midgard::Model model;
            std::vector<std::vector<uint8_t>> glbs, ktx2s;
            {
                py::gil_scoped_release release;  // importing and encoding take a while
                model = midgard::load_model(path, settings);
                for (const auto& part : model.parts) glbs.push_back(midgard::save_glb(part.mesh));
                for (const auto& texture : model.textures)
                    ktx2s.push_back(midgard::render::encode_ktx2(texture.data));
            }
            const auto bytes = [](const std::vector<uint8_t>& b) {
                return py::bytes(reinterpret_cast<const char*>(b.data()), b.size());
            };
            py::list parts, materials, textures;
            for (size_t i = 0; i < model.parts.size(); ++i) {
                py::dict p;
                p["name"] = model.parts[i].name;
                p["glb"] = bytes(glbs[i]);
                p["material"] = model.parts[i].material;
                parts.append(p);
            }
            for (const auto& mat : model.materials) {
                py::dict d;
                d["name"] = mat.name;
                d["color"] = std::vector<float>{mat.base_color.r, mat.base_color.g,
                                                mat.base_color.b, mat.base_color.a};
                d["metallic"] = mat.metallic;
                d["roughness"] = mat.roughness;
                d["emissive"] = std::vector<float>{mat.emissive.r, mat.emissive.g, mat.emissive.b};
                d["emissive_strength"] = mat.emissive_strength;
                d["normal_scale"] = mat.normal_scale;
                d["occlusion_strength"] = mat.occlusion_strength;
                py::dict slots;
                const std::pair<const char*, int> texture_slots[] = {
                    {"base_color", mat.base_color_texture},
                    {"metallic_roughness", mat.metallic_roughness_texture},
                    {"normal", mat.normal_texture},
                    {"occlusion", mat.occlusion_texture},
                    {"emissive", mat.emissive_texture}};
                for (const auto& [slot, index] : texture_slots)
                    if (index >= 0) slots[slot] = index;
                d["textures"] = slots;
                materials.append(d);
            }
            for (size_t i = 0; i < model.textures.size(); ++i) {
                py::dict t;
                t["name"] = model.textures[i].name;
                t["ktx2"] = bytes(ktx2s[i]);
                textures.append(t);
            }
            py::dict out;
            out["parts"] = parts;
            out["materials"] = materials;
            out["textures"] = textures;
            out["bounds"] =
                std::vector<float>{model.bounds_min.x, model.bounds_min.y, model.bounds_min.z,
                                   model.bounds_max.x, model.bounds_max.y, model.bounds_max.z};
            out["warnings"] = model.warnings;
            return out;
        },
        py::arg("path"), py::arg("settings_json") = "{}",
        "A glTF model cooked for a package: {parts: [{name, glb, material}], materials: [...], "
        "textures: [{name, ktx2}], bounds: [min xyz, max xyz], warnings}. Raises RuntimeError "
        "for a file that can't be imported.");

    m.def(
        "validate",
        [](const std::filesystem::path& package_dir) {
            midgard::ValidationReport report;
            {
                py::gil_scoped_release release;  // reading meshes can take a while
                report = midgard::validate_package(package_dir);
            }
            py::dict out;
            out["ok"] = report.ok();
            out["errors"] = report.errors;
            out["warnings"] = report.warnings;
            out["entities"] = report.entities;
            out["assets"] = report.assets;
            return out;
        },
        py::arg("package_dir"),
        "Checks a cooked package the way the runtime loads it (no window or GPU needed).");

    py::register_exception<midgard::BodyError>(m, "BodyError", PyExc_ValueError);
    const auto xyz = [](const glm::dvec3& v) { return py::make_tuple(v.x, v.y, v.z); };
    py::class_<midgard::Body>(m, "Body",
                              "A world body: its shape, size, seed and gravity (midgard/body.h).")
        .def(
            py::init([](const std::string& settings) {
                nlohmann::json j;
                try {
                    j = nlohmann::json::parse(settings);
                } catch (const nlohmann::json::exception& e) {
                    throw midgard::BodyError(std::string("body settings aren't JSON: ") + e.what());
                }
                return midgard::body_from_json(j);
            }),
            py::arg("settings_json"))
        .def_readonly("id", &midgard::Body::id)
        .def_readonly("seed", &midgard::Body::seed)
        .def_property_readonly(
            "shape",
            [](const midgard::Body& b) { return std::string(midgard::shape_name(b.shape.kind())); })
        .def_property_readonly("faces", [](const midgard::Body& b) { return b.shape.faces(); })
        .def_property_readonly("radius", [](const midgard::Body& b) { return b.shape.radius(); })
        .def_property_readonly("face_size",
                               [](const midgard::Body& b) {
                                   const glm::dvec2 s = b.shape.face_size();
                                   return py::make_tuple(s.x, s.y);
                               })
        .def(
            "position",
            [xyz](const midgard::Body& b, int face, double u, double v, double height) {
                if (face < 0 || face >= b.shape.faces())
                    throw py::value_error("no face " + std::to_string(face));
                return xyz(b.position({face, u, v, height}));
            },
            py::arg("face"), py::arg("u"), py::arg("v"), py::arg("height") = 0.0,
            "The 3D position of a surface point.")
        .def(
            "surface",
            [](const midgard::Body& b, double x, double y, double z) {
                const midgard::SurfacePoint s = b.surface({x, y, z});
                return py::make_tuple(s.face, s.u, s.v, s.height);
            },
            py::arg("x"), py::arg("y"), py::arg("z"), "The surface point (face, u, v, height).")
        .def(
            "up",
            [xyz](const midgard::Body& b, double x, double y, double z) {
                return xyz(b.up({x, y, z}));
            },
            py::arg("x"), py::arg("y"), py::arg("z"))
        .def(
            "gravity",
            [xyz](const midgard::Body& b, double x, double y, double z) {
                return xyz(b.gravity({x, y, z}));
            },
            py::arg("x"), py::arg("y"), py::arg("z"))
        .def(
            "frame",
            [xyz](const midgard::Body& b, double x, double y, double z) {
                const midgard::SurfaceFrame f = b.frame({x, y, z});
                return py::make_tuple(xyz(f.up), xyz(f.east), xyz(f.north));
            },
            py::arg("x"), py::arg("y"), py::arg("z"), "(up, east, north) at a point.");
    m.def("shape_fingerprint", &midgard::shape_fingerprint,
          "The shapes' fingerprint (equal to schema/tests/shape_fingerprint.json everywhere).");

    py::class_<Viewport>(m, "Viewport", "The editor's 3D view of the scene being edited.")
        .def(py::init<>())
        .def("put_entity", &Viewport::put_entity, py::arg("entity_json"),
             "Adds or replaces an entity: JSON {id, name, parent, components}.")
        .def("remove_entity", &Viewport::remove_entity, py::arg("entity_id"))
        .def("clear", &Viewport::clear, "Removes every entity.")
        .def("set_settings", &Viewport::set_settings, py::arg("settings_json"),
             "The scene settings: JSON {sky, ambient, body}.")
        .def_property("show_colliders", &Viewport::show_colliders, &Viewport::set_show_colliders,
                      "Outlines of colliders, triggers and the player over the scene.")
        .def(
            "set_mesh",
            [](Viewport& v, const std::string& asset, const py::bytes& glb) {
                const std::string_view data = glb;
                v.set_mesh(asset,
                           std::span(reinterpret_cast<const uint8_t*>(data.data()), data.size()));
            },
            py::arg("asset"), py::arg("glb"), "A mesh (glTF binary) for a mesh asset id.")
        .def(
            "set_texture",
            [](Viewport& v, const std::string& texture, const py::bytes& ktx2) {
                const std::string_view data = ktx2;
                v.set_texture(
                    texture, std::span(reinterpret_cast<const uint8_t*>(data.data()), data.size()));
            },
            py::arg("texture"), py::arg("ktx2"), "A KTX2 texture for a texture content id.")
        .def("set_object", &Viewport::set_object, py::arg("object_id"), py::arg("parts_json"),
             "An object's template (JSON list of {name, components}); its instances follow.")
        .def(
            "set_camera",
            [](Viewport& v, const std::array<float, 3>& eye, const std::array<float, 3>& target,
               float fov) { v.set_camera(vec3(eye), vec3(target), fov); },
            py::arg("eye"), py::arg("target"), py::arg("fov") = 55.0f)
        .def("attach_window", &Viewport::attach_window, py::arg("platform"), py::arg("handle"),
             py::arg("width"), py::arg("height"),
             "Draws into a native window: platform 'windows' (HWND) or 'xcb' (X11 window id).")
        .def("attach_offscreen", &Viewport::attach_offscreen, py::arg("width"), py::arg("height"))
        .def("detach", &Viewport::detach, py::call_guard<py::gil_scoped_release>(),
             "Lets go of the window (before the window system destroys it).")
        .def_property_readonly("attached", &Viewport::attached)
        .def("resize", &Viewport::resize, py::arg("width"), py::arg("height"))
        .def("render", &Viewport::render, py::call_guard<py::gil_scoped_release>(),
             "Draws one frame. False if there is nothing to draw into.")
        .def(
            "read_pixels",
            [](Viewport& v) {
                const std::vector<uint8_t> pixels = v.read_pixels();
                return py::bytes(reinterpret_cast<const char*>(pixels.data()), pixels.size());
            },
            "Offscreen only: the last frame as RGBA8, top row first.")
        .def_property_readonly("device_name", &Viewport::device_name)
        .def_property_readonly("entity_count", &Viewport::entity_count);
}
