// SPDX-License-Identifier: Apache-2.0
// The in-game UI engine: RmlUi's render, system and file interfaces for Midgard, and its input.
#include "midgard/ui/ui.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <unordered_map>

#include <RmlUi/Core.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "midgard/render/renderer.h"
#include "midgard/ui/fonts.h"
#include "ui_base_rcss.h"  // generated: the base style sheet (resources/base.rcss)

#define STB_IMAGE_STATIC  // (this file's own copy: the engine's others are built differently)
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_MAX_DIMENSIONS 8192
#define STBI_NO_STDIO
#include <stb_image.h>

namespace midgard::ui {
namespace {

constexpr const char* kBaseSheet = "midgard://base.rcss";

std::string joined(const char* const* pieces, size_t count) {
    std::string out;
    for (size_t i = 0; i < count; ++i) out += pieces[i];
    return out;
}

const std::string& base_sheet() {
    static const std::string text =
        joined(kUiBaseRcss, sizeof(kUiBaseRcss) / sizeof(kUiBaseRcss[0]));
    return text;
}

// Pictures the game makes (Ui::set_image), by name: what midgard://image/NAME shows.
struct Picture {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;
};
constexpr const char* kImagePrefix = "midgard://image/";

// ---- drawing: RmlUi's geometry into the renderer's UI frame ---------------------------------
class RenderBridge final : public Rml::RenderInterface {
public:
    explicit RenderBridge(render::Renderer& renderer) : renderer_(renderer) {}
    std::map<std::string, Picture> pictures;

    struct Geometry {
        std::vector<render::UiVertex> vertices;
        std::vector<uint32_t> indices;
    };

    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                                Rml::Span<const int> indices) override {
        auto* g = new Geometry;
        g->vertices.resize(vertices.size());
        static_assert(sizeof(Rml::Vertex) == sizeof(render::UiVertex));
        if (!vertices.empty())
            std::memcpy(g->vertices.data(), vertices.data(), vertices.size() * sizeof(Rml::Vertex));
        g->indices.reserve(indices.size());
        for (int i : indices) g->indices.push_back(uint32_t(std::max(i, 0)));
        return reinterpret_cast<Rml::CompiledGeometryHandle>(g);
    }

    void RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation,
                        Rml::TextureHandle texture) override {
        const auto& g = *reinterpret_cast<const Geometry*>(handle);
        if (g.indices.empty()) return;
        render::UiDraw d;
        d.first_index = uint32_t(frame_.indices.size());
        d.index_count = uint32_t(g.indices.size());
        d.first_vertex = uint32_t(frame_.vertices.size());
        d.texture = render::UiTextureId(texture);
        d.transform = transform_ *
                      glm::translate(glm::mat4(1.0f), glm::vec3(translation.x, translation.y, 0));
        d.clip = scissor_on_;
        d.clip_rect = scissor_;
        frame_.vertices.insert(frame_.vertices.end(), g.vertices.begin(), g.vertices.end());
        frame_.indices.insert(frame_.indices.end(), g.indices.begin(), g.indices.end());
        frame_.draws.push_back(d);
    }

    void ReleaseGeometry(Rml::CompiledGeometryHandle handle) override {
        delete reinterpret_cast<Geometry*>(handle);
    }

    Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) override {
        if (source.rfind(kImagePrefix, 0) == 0) {
            const auto it = pictures.find(source.substr(std::strlen(kImagePrefix)));
            if (it == pictures.end() || it->second.rgba.empty()) return 0;
            std::vector<uint8_t> rgba = it->second.rgba;
            premultiply(rgba);
            dimensions = {int(it->second.width), int(it->second.height)};
            return upload(it->second.width, it->second.height, rgba);
        }
        Rml::String data;
        if (!Rml::GetFileInterface()->LoadFile(source, data) || data.empty()) return 0;
        int w = 0, h = 0, channels = 0;
        stbi_uc* pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(data.data()),
                                                int(data.size()), &w, &h, &channels, 4);
        if (!pixels) {
            Rml::Log::Message(Rml::Log::LT_WARNING, "Can't read the picture %s: %s", source.c_str(),
                              stbi_failure_reason());
            return 0;
        }
        std::vector<uint8_t> rgba(pixels, pixels + size_t(w) * h * 4);
        stbi_image_free(pixels);
        premultiply(rgba);
        dimensions = {w, h};
        return upload(uint32_t(w), uint32_t(h), rgba);
    }

    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source,
                                       Rml::Vector2i dimensions) override {
        return upload(uint32_t(dimensions.x), uint32_t(dimensions.y),
                      {reinterpret_cast<const uint8_t*>(source.data()), source.size()});
    }

    void ReleaseTexture(Rml::TextureHandle texture) override {
        renderer_.free_ui_texture(render::UiTextureId(texture));
    }

    void EnableScissorRegion(bool enable) override { scissor_on_ = enable; }
    void SetScissorRegion(Rml::Rectanglei region) override {
        scissor_ = {region.Left(), region.Top(), region.Width(), region.Height()};
    }
    void SetTransform(const Rml::Matrix4f* transform) override {
        // (RmlUi's matrices are column-major, as glm's are)
        transform_ = transform ? glm::make_mat4(transform->data()) : glm::mat4(1.0f);
    }

    void begin() {
        frame_ = renderer_.take_ui();  // (its vectors kept: no allocations each frame)
        frame_.clear();
        scissor_on_ = false;
        transform_ = glm::mat4(1.0f);
    }
    void end() { renderer_.set_ui(std::move(frame_)); }

private:
    static void premultiply(std::vector<uint8_t>& rgba) {  // (RmlUi draws premultiplied alpha)
        for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
            const unsigned a = rgba[i + 3];
            for (int c = 0; c < 3; ++c) rgba[i + c] = uint8_t((rgba[i + c] * a + 127) / 255);
        }
    }

    Rml::TextureHandle upload(uint32_t w, uint32_t h, std::span<const uint8_t> rgba) {
        try {
            return Rml::TextureHandle(renderer_.upload_ui_texture(w, h, rgba));
        } catch (const std::exception& e) {
            Rml::Log::Message(Rml::Log::LT_ERROR, "A UI texture: %s", e.what());
            return 0;
        }
    }

    render::Renderer& renderer_;
    render::UiFrame frame_;
    bool scissor_on_ = false;
    glm::ivec4 scissor_{0};
    glm::mat4 transform_{1.0f};
};

// ---- the system: the clock, messages, the cursor, the clipboard, phones' keyboards ----------
class SystemBridge final : public Rml::SystemInterface {
public:
    double time = 0.0;
    const Platform* platform = nullptr;

    double GetElapsedTime() override { return time; }

    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override {
        const int level = type == Rml::Log::LT_ERROR || type == Rml::Log::LT_ASSERT ? 2
                          : type == Rml::Log::LT_WARNING                            ? 1
                                                                                    : 0;
        if (type == Rml::Log::LT_DEBUG) return true;
        if (platform && platform->log) platform->log(level, message);
        return true;  // (carry on: an assertion in a game mustn't stop it)
    }

    void SetMouseCursor(const Rml::String& name) override {
        if (platform && platform->set_cursor) platform->set_cursor(name);
    }
    void SetClipboardText(const Rml::String& text) override {
        if (platform && platform->set_clipboard) platform->set_clipboard(text);
    }
    void GetClipboardText(Rml::String& text) override {
        text = platform && platform->get_clipboard ? platform->get_clipboard() : std::string();
    }
    void ActivateKeyboard(Rml::Vector2f caret, float /*line_height*/) override {
        if (platform && platform->keyboard) platform->keyboard(true, caret.x, caret.y);
    }
    void DeactivateKeyboard() override {
        if (platform && platform->keyboard) platform->keyboard(false, 0.0f, 0.0f);
    }

    // Midgard's own files ("midgard://base.rcss") are never relative to a document.
    void JoinPath(Rml::String& out, const Rml::String& document, const Rml::String& path) override {
        if (path.rfind("midgard://", 0) == 0) {
            out = path;
            return;
        }
        Rml::SystemInterface::JoinPath(out, document, path);
    }
};

// ---- files: Midgard's built-in ones, and the game's (read whole: screens are small) ---------
class FileBridge final : public Rml::FileInterface {
public:
    struct File {
        std::string data;
        size_t at = 0;
    };

    Rml::FileHandle Open(const Rml::String& path) override {
        auto file = std::make_unique<File>();
        if (path == kBaseSheet) {
            file->data = base_sheet();
        } else if (path.rfind("midgard://", 0) == 0) {
            Rml::Log::Message(Rml::Log::LT_WARNING, "Midgard has no built-in file %s",
                              path.c_str());
            return 0;
        } else {
            std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
            if (!in) {
                Rml::Log::Message(Rml::Log::LT_WARNING, "Can't read %s", path.c_str());
                return 0;
            }
            file->data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        return reinterpret_cast<Rml::FileHandle>(file.release());
    }
    void Close(Rml::FileHandle handle) override { delete reinterpret_cast<File*>(handle); }
    size_t Read(void* buffer, size_t size, Rml::FileHandle handle) override {
        auto& f = *reinterpret_cast<File*>(handle);
        const size_t n = std::min(size, f.data.size() - f.at);
        std::memcpy(buffer, f.data.data() + f.at, n);
        f.at += n;
        return n;
    }
    bool Seek(Rml::FileHandle handle, long offset, int origin) override {
        auto& f = *reinterpret_cast<File*>(handle);
        const long base = origin == SEEK_SET   ? 0
                          : origin == SEEK_CUR ? long(f.at)
                                               : long(f.data.size());
        const long to = base + offset;
        if (to < 0 || size_t(to) > f.data.size()) return false;
        f.at = size_t(to);
        return true;
    }
    size_t Tell(Rml::FileHandle handle) override { return reinterpret_cast<File*>(handle)->at; }
    size_t Length(Rml::FileHandle handle) override {
        return reinterpret_cast<File*>(handle)->data.size();
    }
};

// RmlUi's interfaces are global: one UI at a time.
struct Global {
    SystemBridge system;
    FileBridge files;
    bool in_use = false;
};
Global& global() {
    static Global g;
    return g;
}

// ---- keys --------------------------------------------------------------------------------
Rml::Input::KeyIdentifier key_of(const std::string& name) {
    using K = Rml::Input::KeyIdentifier;
    static const std::unordered_map<std::string, K> named = [] {
        std::unordered_map<std::string, K> m = {
            {"Return", Rml::Input::KI_RETURN}, {"Keypad Enter", Rml::Input::KI_NUMPADENTER},
            {"Escape", Rml::Input::KI_ESCAPE}, {"Backspace", Rml::Input::KI_BACK},
            {"Tab", Rml::Input::KI_TAB},       {"Space", Rml::Input::KI_SPACE},
            {"Delete", Rml::Input::KI_DELETE}, {"Insert", Rml::Input::KI_INSERT},
            {"Home", Rml::Input::KI_HOME},     {"End", Rml::Input::KI_END},
            {"PageUp", Rml::Input::KI_PRIOR},  {"PageDown", Rml::Input::KI_NEXT},
            {"Left", Rml::Input::KI_LEFT},     {"Right", Rml::Input::KI_RIGHT},
            {"Up", Rml::Input::KI_UP},         {"Down", Rml::Input::KI_DOWN},
        };
        for (int i = 0; i < 26; ++i) m[std::string(1, char('A' + i))] = K(Rml::Input::KI_A + i);
        for (int i = 0; i < 10; ++i) m[std::string(1, char('0' + i))] = K(Rml::Input::KI_0 + i);
        for (int i = 0; i < 12; ++i) m["F" + std::to_string(i + 1)] = K(Rml::Input::KI_F1 + i);
        return m;
    }();
    const auto it = named.find(name);
    return it == named.end() ? Rml::Input::KI_UNKNOWN : it->second;
}

int modifiers(const Input& in) {
    return (in.ctrl ? Rml::Input::KM_CTRL : 0) | (in.shift ? Rml::Input::KM_SHIFT : 0) |
           (in.alt ? Rml::Input::KM_ALT : 0) | (in.meta ? Rml::Input::KM_META : 0);
}

// A gamepad button as a key: the d-pad moves between controls, A presses, B goes back.
Rml::Input::KeyIdentifier pad_key(const std::string& button) {
    if (button == "dpup") return Rml::Input::KI_UP;
    if (button == "dpdown") return Rml::Input::KI_DOWN;
    if (button == "dpleft") return Rml::Input::KI_LEFT;
    if (button == "dpright") return Rml::Input::KI_RIGHT;
    if (button == "a") return Rml::Input::KI_RETURN;
    if (button == "b") return Rml::Input::KI_ESCAPE;
    return Rml::Input::KI_UNKNOWN;
}

constexpr double kRepeatDelay = 0.4;  // seconds a held direction waits before repeating
constexpr double kRepeatInterval = 0.12;
constexpr float kStickOn = 0.5f, kStickOff = 0.3f;

bool is_direction(Rml::Input::KeyIdentifier key) {
    return key == Rml::Input::KI_UP || key == Rml::Input::KI_DOWN || key == Rml::Input::KI_LEFT ||
           key == Rml::Input::KI_RIGHT;
}

// How far `to` is from `from` going in a direction, as RmlUi's own navigation measures it (the
// distance along it, plus a large penalty for being off to the side); INT_MAX if behind.
int nav_distance(const Rml::Rectanglef& from, const Rml::Rectanglef& to,
                 Rml::Input::KeyIdentifier key) {
    const auto along = [](int axis, const Rml::Rectanglef& a, const Rml::Rectanglef& b) {
        const float main = a.p0[axis] - b.p1[axis];
        if (main < 0.0f) return INT_MAX;
        const int cross = (axis + 1) % 2;
        const float off =
            std::max(0.0f, b.p0[cross] - a.p1[cross]) + std::max(0.0f, a.p0[cross] - b.p1[cross]);
        return int(main) + 10000 * int(off);
    };
    switch (key) {
        case Rml::Input::KI_UP:
            return along(1, from, to);
        case Rml::Input::KI_DOWN:
            return along(1, to, from);
        case Rml::Input::KI_RIGHT:
            return along(0, to, from);
        case Rml::Input::KI_LEFT:
            return along(0, from, to);
        default:
            return INT_MAX;
    }
}

Rml::Rectanglef border_box(Rml::Element* e) {
    return Rml::Rectanglef::FromPositionSize(e->GetAbsoluteOffset(Rml::BoxArea::Border),
                                             e->GetBox().GetSize(Rml::BoxArea::Border));
}

// The nearest control in a direction anywhere in the document, into and out of scrolling lists
// too (RmlUi's own search stays on its side of a list's edge).
void nearest_control(Rml::Element* e, Rml::Element* from, const Rml::Rectanglef& box,
                     Rml::Input::KeyIdentifier key, Rml::Element*& best, int& best_distance) {
    for (int i = 0; i < e->GetNumChildren(); ++i) {
        Rml::Element* child = e->GetChild(i);
        if (child == from || !child->IsVisible()) continue;
        const auto& v = child->GetComputedValues();
        if (v.focus() == Rml::Style::Focus::None) continue;
        if (v.tab_index() == Rml::Style::TabIndex::Auto && !child->HasAttribute("disabled")) {
            const int d = nav_distance(box, border_box(child), key);
            if (d < best_distance) {
                best = child;
                best_distance = d;
            }
        }
        nearest_control(child, from, box, key, best, best_distance);
    }
}

bool is_text_field(Rml::Element* e) {
    if (!e) return false;
    if (e->GetTagName() == "textarea") return true;
    if (e->GetTagName() != "input") return false;
    const std::string type = e->GetAttribute<Rml::String>("type", "text");
    return type == "text" || type == "password";
}

}  // namespace

// ---- the Ui ------------------------------------------------------------------------------
struct Ui::Impl {
    render::Renderer& renderer;
    Options options;
    RenderBridge bridge;
    Rml::Context* context = nullptr;
    std::vector<Rml::ElementDocument*> modal_documents;
    // Held directions (d-pad or stick), repeating: the key, and when it next repeats.
    std::map<Rml::Input::KeyIdentifier, double> held;
    Rml::Input::KeyIdentifier stick_x = Rml::Input::KI_UNKNOWN, stick_y = Rml::Input::KI_UNKNOWN;
    int64_t primary_touch = -1;
    std::deque<std::vector<uint8_t>> fonts;  // (font faces read from memory: kept while used)

    Impl(render::Renderer& r, Options o) : renderer(r), options(std::move(o)), bridge(r) {}

    void apply_safe_area(Rml::ElementDocument* doc) {
        const Insets& s = options.safe_area;
        const auto px = [](float v) { return std::to_string(std::max(0.0f, v)) + "px"; };
        doc->SetProperty("padding-left", px(s.left));
        doc->SetProperty("padding-top", px(s.top));
        doc->SetProperty("padding-right", px(s.right));
        doc->SetProperty("padding-bottom", px(s.bottom));
    }

    void apply_all_safe_areas() {
        for (int i = 0; i < context->GetNumDocuments(); ++i)
            apply_safe_area(context->GetDocument(i));
    }

    Rml::ElementDocument* shown(Rml::ElementDocument* doc, bool modal) {
        if (!doc) return nullptr;
        apply_safe_area(doc);
        if (modal) {
            modal_documents.push_back(doc);
            doc->Show(Rml::ModalFlag::Modal, Rml::FocusFlag::Auto);
        } else {
            doc->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        }
        return doc;
    }

    bool modal() const {
        return std::any_of(modal_documents.begin(), modal_documents.end(),
                           [](Rml::ElementDocument* d) { return d->IsVisible(); });
    }

    // Over one of a screen's elements (not just a screen's empty background).
    bool over_ui() {
        Rml::Element* hover = context->GetHoverElement();
        return hover && hover != hover->GetOwnerDocument() && hover != context->GetRootElement();
    }

    // A key to the focused screen. A direction no control on its side of a list's edge answers
    // goes to the nearest control that way anywhere on the screen (into or out of the list).
    void press(Rml::Input::KeyIdentifier key, int mods) {
        const bool unused = context->ProcessKeyDown(key, mods);
        context->ProcessKeyUp(key, mods);
        if (!unused || !is_direction(key)) return;
        Rml::Element* from = context->GetFocusElement();
        for (; from; from = from->GetParentNode())
            if (from->GetComputedValues().tab_index() == Rml::Style::TabIndex::Auto) break;
        if (!from) return;
        Rml::ElementDocument* doc = from->GetOwnerDocument();
        if (!doc || from == doc) return;
        Rml::Element* best = nullptr;
        int best_distance = INT_MAX;
        nearest_control(doc, from, border_box(from), key, best, best_distance);
        if (best && best->Focus(true)) best->ScrollIntoView(Rml::ScrollAlignment::Adaptive);
    }

    // A direction or button from a gamepad: with nothing focused yet, the first control is.
    void pad_press(Rml::Input::KeyIdentifier key) {
        Rml::Element* focus = context->GetFocusElement();
        const bool nothing =
            !focus || focus == focus->GetOwnerDocument() || focus == context->GetRootElement();
        if (nothing && key != Rml::Input::KI_ESCAPE) {
            context->ProcessKeyDown(Rml::Input::KI_TAB, 0);
            context->ProcessKeyUp(Rml::Input::KI_TAB, 0);
            if (key != Rml::Input::KI_RETURN) return;  // (the first move only finds a control)
        }
        press(key, 0);
    }

    void hold(Rml::Input::KeyIdentifier key, bool down) {
        if (key == Rml::Input::KI_UNKNOWN) return;
        const bool direction = key == Rml::Input::KI_UP || key == Rml::Input::KI_DOWN ||
                               key == Rml::Input::KI_LEFT || key == Rml::Input::KI_RIGHT;
        if (down) {
            pad_press(key);
            if (direction) held[key] = global().system.time + kRepeatDelay;
        } else {
            held.erase(key);
        }
    }

    void stick(Rml::Input::KeyIdentifier& state, float v, Rml::Input::KeyIdentifier negative,
               Rml::Input::KeyIdentifier positive) {
        Rml::Input::KeyIdentifier now = state;
        if (std::abs(v) >= kStickOn)
            now = v < 0 ? negative : positive;
        else if (std::abs(v) <= kStickOff)
            now = Rml::Input::KI_UNKNOWN;
        if (now == state) return;
        hold(state, false);
        state = now;
        hold(state, true);
    }
};

Ui::Ui(render::Renderer& renderer, Options options) {
    Global& g = global();
    if (g.in_use) throw UiError("Only one UI can run at a time");
    impl_ = std::make_unique<Impl>(renderer, std::move(options));
    g.system.platform = &impl_->options.platform;
    g.system.time = 0.0;
    Rml::SetSystemInterface(&g.system);
    Rml::SetFileInterface(&g.files);
    if (!Rml::Initialise()) throw UiError("RmlUi didn't start");
    g.in_use = true;
    using FS = Rml::Style::FontStyle;
    using FW = Rml::Style::FontWeight;
    const auto font = [](std::span<const uint8_t> data, const char* family, FW weight,
                         bool fallback) {
        if (!Rml::LoadFontFace({reinterpret_cast<const Rml::byte*>(data.data()), data.size()},
                               family, FS::Normal, weight, fallback))
            throw UiError(std::string("Can't read the built-in font ") + family);
    };
    try {
        font(fonts::inconsolata(), "Inconsolata", FW::Normal, true);
        font(fonts::opendyslexic_regular(), "OpenDyslexic", FW::Normal, false);
        font(fonts::opendyslexic_bold(), "OpenDyslexic", FW::Bold, false);
        impl_->context = Rml::CreateContext(
            "midgard", {int(impl_->options.width), int(impl_->options.height)}, &impl_->bridge);
        if (!impl_->context) throw UiError("RmlUi couldn't make its context");
    } catch (...) {
        Rml::Shutdown();
        g.in_use = false;
        g.system.platform = nullptr;
        throw;
    }
    impl_->context->SetDensityIndependentPixelRatio(dp_ratio());
}

Ui::~Ui() {
    Global& g = global();
    Rml::RemoveContext("midgard");
    Rml::Shutdown();  // (its textures go back to the renderer through the bridge, still here)
    g.system.platform = nullptr;
    g.in_use = false;
}

Rml::ElementDocument* Ui::open(const std::filesystem::path& file, bool modal) {
    const std::u8string path = file.generic_u8string();
    return impl_->shown(impl_->context->LoadDocument(std::string(path.begin(), path.end())), modal);
}

Rml::ElementDocument* Ui::open_text(const std::string& rml, const std::string& name, bool modal) {
    return impl_->shown(impl_->context->LoadDocumentFromMemory(rml, name), modal);
}

void Ui::close(Rml::ElementDocument* document) {
    if (!document) return;
    std::erase(impl_->modal_documents, document);
    document->Close();
}

bool Ui::modal() const {
    return impl_->modal();
}

void Ui::resize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;
    impl_->options.width = width;
    impl_->options.height = height;
    impl_->context->SetDimensions({int(width), int(height)});
}

void Ui::set_density(float density) {
    impl_->options.density = std::max(density, 0.1f);
    impl_->context->SetDensityIndependentPixelRatio(dp_ratio());
}

void Ui::set_scale(float scale) {
    impl_->options.scale = std::clamp(scale, 0.25f, 4.0f);
    impl_->context->SetDensityIndependentPixelRatio(dp_ratio());
}

void Ui::set_safe_area(const Insets& insets) {
    impl_->options.safe_area = insets;
    impl_->apply_all_safe_areas();
}

float Ui::dp_ratio() const {
    return impl_->options.density * impl_->options.scale;
}

const Insets& Ui::safe_area() const {
    return impl_->options.safe_area;
}

uint32_t Ui::width() const {
    return impl_->options.width;
}

uint32_t Ui::height() const {
    return impl_->options.height;
}

bool Ui::handle(const Input& in) {
    Impl& u = *impl_;
    Rml::Context& c = *u.context;
    const int mods = modifiers(in);
    using T = Input::Type;
    switch (in.type) {
        case T::PointerMove:
            c.ProcessMouseMove(int(in.x), int(in.y), mods);
            return u.over_ui() || u.modal();
        case T::PointerDown:
            c.ProcessMouseMove(int(in.x), int(in.y), mods);
            c.ProcessMouseButtonDown(in.button, mods);
            return u.over_ui() || u.modal();
        case T::PointerUp:
            c.ProcessMouseButtonUp(in.button, mods);
            return u.over_ui() || u.modal();
        case T::PointerLeave:
            c.ProcessMouseLeave();
            return false;
        case T::Wheel:
            c.ProcessMouseWheel(Rml::Vector2f(-in.x, -in.y), mods);
            return u.over_ui() || u.modal();
        case T::KeyDown:
        case T::KeyUp: {
            const Rml::Input::KeyIdentifier key = key_of(in.key);
            const bool typing = is_text_field(c.GetFocusElement());
            if (key == Rml::Input::KI_UNKNOWN) return typing || u.modal();
            // Only while a menu is up, or a field is being typed in, do keys reach screens (the
            // HUD never takes the game's keys).
            if (!typing && !u.modal()) return false;
            if (in.type == T::KeyDown) {
                if (is_direction(key)) {
                    u.press(key, mods);  // (key up included)
                    return true;
                }
                c.ProcessKeyDown(key, mods);
            } else if (!is_direction(key)) {
                c.ProcessKeyUp(key, mods);
            }
            return typing || u.modal();
        }
        case T::Text:
            if (!is_text_field(c.GetFocusElement())) return u.modal();
            c.ProcessTextInput(Rml::String(in.text));
            return true;
        case T::TouchDown:
            if (u.primary_touch >= 0) return u.modal();  // (one finger works the UI)
            u.primary_touch = in.touch;
            c.ProcessTouchStart({{Rml::TouchId(in.touch), {in.x, in.y}}}, mods);
            return u.over_ui() || u.modal();
        case T::TouchMove:
            if (in.touch != u.primary_touch) return u.modal();
            c.ProcessTouchMove({{Rml::TouchId(in.touch), {in.x, in.y}}}, mods);
            return u.over_ui() || u.modal();
        case T::TouchUp:
        case T::TouchCancel: {
            if (in.touch != u.primary_touch) return u.modal();
            const bool used = u.over_ui() || u.modal();
            const Rml::TouchList touch = {{Rml::TouchId(in.touch), {in.x, in.y}}};
            if (in.type == T::TouchUp)
                c.ProcessTouchEnd(touch, mods);
            else
                c.ProcessTouchCancel(touch);
            c.ProcessMouseLeave();  // (no finger: nothing stays under the "pointer")
            u.primary_touch = -1;
            return used;
        }
        case T::PadDown:
        case T::PadUp:
            if (!u.modal()) return false;
            u.hold(pad_key(in.key), in.type == T::PadDown);
            return true;
        case T::PadAxis:
            if (!u.modal()) {
                u.stick_x = u.stick_y = Rml::Input::KI_UNKNOWN;
                return false;
            }
            if (in.key == "leftx")
                u.stick(u.stick_x, in.x, Rml::Input::KI_LEFT, Rml::Input::KI_RIGHT);
            else if (in.key == "lefty")
                u.stick(u.stick_y, in.x, Rml::Input::KI_UP, Rml::Input::KI_DOWN);
            return true;
    }
    return false;
}

void Ui::update(double seconds) {
    Impl& u = *impl_;
    global().system.time = seconds;
    for (auto& [key, due] : u.held) {
        if (seconds < due) continue;
        u.pad_press(key);
        due = seconds + kRepeatInterval;
    }
    std::erase_if(u.modal_documents, [&](Rml::ElementDocument* d) {
        for (int i = 0; i < u.context->GetNumDocuments(); ++i)
            if (u.context->GetDocument(i) == d) return false;
        return true;  // (closed)
    });
    u.context->Update();
}

void Ui::render() {
    impl_->bridge.begin();
    impl_->context->Render();
    impl_->bridge.end();
}

Rml::Context* Ui::context() {
    return impl_->context;
}

bool Ui::set_image(const std::string& name, uint32_t width, uint32_t height,
                   const std::vector<uint8_t>& rgba) {
    if (width == 0 || height == 0 || rgba.size() != size_t(width) * height * 4) return false;
    impl_->bridge.pictures[name] = {width, height, rgba};
    // (the screens showing it load it again when next drawn)
    Rml::ReleaseTexture(kImagePrefix + name, &impl_->bridge);
    return true;
}

bool Ui::set_image_file(const std::string& name, const std::vector<uint8_t>& encoded) {
    if (encoded.empty()) return false;
    int w = 0, h = 0, channels = 0;
    stbi_uc* pixels =
        stbi_load_from_memory(encoded.data(), int(encoded.size()), &w, &h, &channels, 4);
    if (!pixels) return false;
    std::vector<uint8_t> rgba(pixels, pixels + size_t(w) * size_t(h) * 4);
    stbi_image_free(pixels);
    return set_image(name, uint32_t(w), uint32_t(h), rgba);
}

bool Ui::add_font(const std::string& family, std::vector<uint8_t> bytes, bool italic) {
    if (bytes.empty()) return false;
    impl_->fonts.push_back(std::move(bytes));
    const std::vector<uint8_t>& kept = impl_->fonts.back();
    return Rml::LoadFontFace({reinterpret_cast<const Rml::byte*>(kept.data()), kept.size()}, family,
                             italic ? Rml::Style::FontStyle::Italic : Rml::Style::FontStyle::Normal,
                             Rml::Style::FontWeight::Auto, false);
}

bool Ui::has_image(const std::string& name) const {
    return impl_->bridge.pictures.contains(name);
}

void Ui::navigate(int rml_key) {
    impl_->press(Rml::Input::KeyIdentifier(rml_key), 0);
}

std::string Ui::describe(Rml::Element* element) {
    if (!element) return "(none)";
    if (!element->GetId().empty()) return "#" + element->GetId();
    std::string out = element->GetTagName();
    const std::string classes = element->GetClassNames();
    if (!classes.empty()) {
        std::string dotted = classes;
        std::replace(dotted.begin(), dotted.end(), ' ', '.');
        out += "." + dotted;
    }
    const std::string text = element->GetInnerRML();
    if (!text.empty() && text.size() < 40 && text.find('<') == std::string::npos)
        out += " \"" + text + "\"";
    return out;
}

}  // namespace midgard::ui
