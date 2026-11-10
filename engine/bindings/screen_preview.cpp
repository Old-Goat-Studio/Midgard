// SPDX-License-Identifier: Apache-2.0
// The Menu editor's canvas (milestone 6.0): a screen drawn as the game draws it (the same RmlUi
// documents, fonts and base style sheet, offscreen), with where each element landed, so what
// the designer sees is what the player gets.
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include <RmlUi/Core.h>

#include "midgard/render/renderer.h"
#include "midgard/screen.h"
#include "midgard/ui/ui.h"

namespace py = pybind11;

namespace {

using namespace midgard;

class ScreenPreview {
public:
    // The screen (its JSON, as the screen table holds it) at a size in pixels, `scale` pixels a
    // dp, kept clear of `safe` (left, top, right, bottom, pixels). Returns the picture (RGBA8,
    // top row first), each element's box in pixels by id ([x, y, w, h]) and the screen checks'
    // problems (with touch: a finger's too). Throws ValueError for a screen that can't be read.
    std::tuple<py::bytes, py::dict, std::vector<std::string>> render(
        const std::string& json, uint32_t width, uint32_t height, float scale,
        std::array<float, 4> safe, bool touch, const std::map<std::string, std::string>& images) {
        ScreenDef def;
        try {
            def = screen_from_json("preview", "Preview", json);
        } catch (const ScreenError& e) {
            throw py::value_error(e.what());
        }
        width = std::max(width, 16u);
        height = std::max(height, 16u);
        if (!renderer_ || width != width_ || height != height_) {
            ui_.reset();  // (before its renderer)
            renderer_.reset();
            render::RendererDesc desc;
            desc.offscreen = true;
            desc.width = width;
            desc.height = height;
            renderer_ = std::make_unique<render::Renderer>(desc);
            width_ = width;
            height_ = height;
        }
        if (!ui_) {
            ui::Options o;
            o.width = width;
            o.height = height;
            ui_ = std::make_unique<ui::Ui>(*renderer_, o);
        }
        ui_->set_scale(scale);
        // Its pictures, from their files (read again each time: the designer may change them).
        for (const std::string& image : screen_images(def)) {
            const auto it = images.find(image);
            std::vector<uint8_t> bytes;
            if (it != images.end()) {
                std::ifstream in(std::filesystem::u8path(it->second), std::ios::binary);
                bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            }
            ui_->set_image_file(screen_image_name(image), bytes);
        }
        ui_->set_safe_area({safe[0], safe[1], safe[2], safe[3]});
        Rml::ElementDocument* doc =
            ui_->open_text(screen_rml(def), "midgard://screen/preview", false);
        if (!doc) throw py::value_error("the screen couldn't be drawn");
        ui_->update(0.0);
        std::vector<std::string> problems;
        if (def.modal)
            for (const ui::Problem& p : ui_->check(doc, touch))
                problems.push_back(p.kind + ": " + p.element + ": " + p.message);
        ui_->close(doc);  // (the checks pressed its controls: draw it as it opens)
        ui_->update(0.0);
        doc = ui_->open_text(screen_rml(def), "midgard://screen/preview", false);
        ui_->update(0.0);
        py::dict boxes;
        for (const ScreenElement& e : def.elements) {
            Rml::Element* el = doc ? doc->GetElementById("el-" + e.id) : nullptr;
            if (!el) continue;
            const Rml::Vector2f at = el->GetAbsoluteOffset(Rml::BoxArea::Border);
            const Rml::Vector2f size = el->GetBox().GetSize(Rml::BoxArea::Border);
            boxes[py::str(e.id)] = py::make_tuple(at.x, at.y, size.x, size.y);
        }
        ui_->render();
        render::Camera camera;
        // (a dim blue-grey stands for the game behind it)
        renderer_->render_frame(camera, render::Light{}, {}, glm::vec4(0.16f, 0.19f, 0.24f, 1.0f));
        std::vector<uint8_t> pixels = renderer_->read_pixels();
        if (doc) ui_->close(doc);
        ui_->update(0.0);
        return {py::bytes(reinterpret_cast<const char*>(pixels.data()), pixels.size()), boxes,
                problems};
    }

private:
    std::unique_ptr<render::Renderer> renderer_;
    std::unique_ptr<ui::Ui> ui_;
    uint32_t width_ = 0, height_ = 0;
};

}  // namespace

void bind_screen_preview(py::module_& m) {
    py::class_<ScreenPreview>(m, "ScreenPreview",
                              "The Menu editor's canvas: a screen drawn as the game draws it.")
        .def(py::init<>())
        .def("render", &ScreenPreview::render, py::arg("screen"), py::arg("width"),
             py::arg("height"), py::arg("scale") = 1.0f,
             py::arg("safe") = std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f},
             py::arg("touch") = false, py::arg("images") = std::map<std::string, std::string>{},
             "Draws a screen (its JSON) at width x height pixels, scale pixels a dp, clear of a "
             "safe area (left, top, right, bottom): (RGBA8 bytes, {element id: (x, y, w, h)}, "
             "[the screen checks' problems]). ValueError for a screen that can't be read.");
}
