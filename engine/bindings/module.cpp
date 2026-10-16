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
#include <array>
#include <span>

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/stl/filesystem.h>

#include <nlohmann/json.hpp>

#include "midgard/body.h"
#include "midgard/link.h"
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
             "The scene settings: JSON {sky, ambient}.")
        .def(
            "set_mesh",
            [](Viewport& v, const std::string& asset, const py::bytes& glb) {
                const std::string_view data = glb;
                v.set_mesh(asset,
                           std::span(reinterpret_cast<const uint8_t*>(data.data()), data.size()));
            },
            py::arg("asset"), py::arg("glb"), "A mesh (glTF binary) for a mesh asset id.")
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
