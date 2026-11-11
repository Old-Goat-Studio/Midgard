// SPDX-License-Identifier: Apache-2.0
#include "midgard/ui/screens.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <set>

#include <RmlUi/Core.h>

#include "midgard/ui/ui.h"

namespace midgard::ui {
namespace {

std::string key(const std::string& screen, const std::string& element) {
    return screen + "/" + element;
}

}  // namespace

struct Screens::Impl : Rml::EventListener {
    Ui& ui;
    ImageLoader images;
    FontLoader fonts;
    std::set<std::string> tried;  // pictures and fonts loaded, or found missing
    struct Open {
        Rml::ElementDocument* document = nullptr;
        std::string rml;  // what it was made from (made again when that changes)
        ScreenDef def;    // its design
        bool pauses = false;
        std::map<std::string, std::string> texts;  // as set on it, by element
        std::map<std::string, double> values;
        std::map<std::string, bool> enabled;
        // Just opened, or the game just set a value: RmlUi's sliders say "change" as they lay
        // out (rounding to their step), which isn't the player. Quiet until the next sync.
        bool quiet = true;
    };
    std::map<std::string, Open> open;  // by screen id
    std::vector<std::string> order;    // bottom to top, as last synced
    std::vector<Event> events;

    explicit Impl(Ui& u) : ui(u) {}

    // Which screen, and which of its elements, an RmlUi element is (or is inside).
    bool locate(Rml::Element* target, std::string& screen, const ScreenElement*& element) {
        Rml::ElementDocument* document = target ? target->GetOwnerDocument() : nullptr;
        const auto it = std::find_if(open.begin(), open.end(),
                                     [&](const auto& o) { return o.second.document == document; });
        if (it == open.end()) return false;
        for (Rml::Element* e = target; e; e = e->GetParentNode()) {
            std::string id = e->GetId();
            if (!id.starts_with("el-")) continue;
            id = id.substr(3);
            if (id.ends_with("-box") && !it->second.def.find(id)) id.resize(id.size() - 4);
            if (const ScreenElement* found = it->second.def.find(id)) {
                screen = it->first;
                element = found;
                return true;
            }
        }
        return false;
    }

    void ProcessEvent(Rml::Event& event) override {
        std::string screen;
        const ScreenElement* element = nullptr;
        if (!locate(event.GetTargetElement(), screen, element)) return;
        if (event.GetId() == Rml::EventId::Click) {
            // (a disabled button: nothing)
            Rml::Element* button = open[screen].document->GetElementById("el-" + element->id);
            if (element->kind == "button" && !(button && button->HasAttribute("disabled")))
                events.push_back({screen, element->id, "click", 0.0});
            return;
        }
        if (event.GetId() != Rml::EventId::Change) return;
        double value = 0.0;
        if (element->kind == "slider") {
            value = std::strtod(event.GetParameter<Rml::String>("value", "0").c_str(), nullptr);
        } else if (element->kind == "check" || element->kind == "option") {
            value = event.GetTargetElement()->HasAttribute("checked") ? 1.0 : 0.0;
        } else {
            return;
        }
        Open& o = open[screen];
        if (o.quiet) return;
        if (const auto v = o.values.find(element->id); v != o.values.end() && v->second == value)
            return;  // (a value the game set)
        o.values[element->id] = value;
        events.push_back({screen, element->id, "change", value});
    }

    void close(Open& o) {
        if (!o.document) return;
        o.document->RemoveEventListener(Rml::EventId::Click, this);
        o.document->RemoveEventListener(Rml::EventId::Change, this);
        ui.close(o.document);
        o.document = nullptr;
    }

    // Scripts' text and values onto a shown screen (only what changed).
    void apply(const std::string& id, Open& o, const std::map<std::string, std::string>& texts,
               const std::map<std::string, double>& values,
               const std::map<std::string, bool>& enabled) {
        for (const ScreenElement& e : o.def.elements) {
            Rml::Element* el = o.document->GetElementById("el-" + e.id);
            if (!el) continue;
            // (a check's or an option's box is what's enabled, and ticked)
            Rml::Element* control = el;
            if (e.kind == "check" || e.kind == "option")
                control = o.document->GetElementById("el-" + e.id + "-box");
            if (const auto on = enabled.find(key(id, e.id)); on != enabled.end() && control) {
                const auto now = o.enabled.find(e.id);
                if (now == o.enabled.end() || now->second != on->second) {
                    o.enabled[e.id] = on->second;
                    if (on->second)
                        control->RemoveAttribute("disabled");
                    else
                        control->SetAttribute("disabled", "");
                    control->SetClass("disabled", !on->second);  // (a button's look: the class)
                }
            }
            if (const auto t = texts.find(key(id, e.id)); t != texts.end()) {
                auto& shown = o.texts[e.id];
                if (shown != t->second) {
                    shown = t->second;
                    Rml::Element* into = el;
                    // (a check's text is its second part; a button's, its span)
                    if (e.kind == "check" && el->GetNumChildren() > 1) into = el->GetChild(1);
                    if (e.kind == "button" && el->GetNumChildren() > 0) into = el->GetChild(0);
                    if (e.kind != "slider" && e.kind != "panel")
                        into->SetInnerRML(rml_escape(t->second));
                }
            }
            if (const auto v = values.find(key(id, e.id)); v != values.end()) {
                const auto now = o.values.find(e.id);
                if (now != o.values.end() && now->second == v->second) continue;
                o.values[e.id] = v->second;
                o.quiet = true;
                if (e.kind == "slider") {
                    el->SetAttribute("value", std::to_string(v->second));
                } else if (e.kind == "check" || e.kind == "option") {
                    if (Rml::Element* box = o.document->GetElementById("el-" + e.id + "-box")) {
                        if (v->second != 0.0)
                            box->SetAttribute("checked", "");
                        else
                            box->RemoveAttribute("checked");
                    }
                }
            }
        }
    }
};

Screens::Screens(Ui& ui, ImageLoader images, FontLoader fonts) : impl_(std::make_unique<Impl>(ui)) {
    impl_->images = std::move(images);
    impl_->fonts = std::move(fonts);
}

void load_fonts(Ui& ui, const ScreenDef& screen, const Screens::FontLoader& fonts,
                std::set<std::string>& tried) {
    for (const std::string& font : screen_fonts(screen)) {
        if (!fonts || !tried.insert(font).second) continue;
        for (auto& [name, bytes] : fonts(font)) {
            std::string lower = name;
            for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
            const bool italic = lower.find("italic") != std::string::npos ||
                                lower.find("oblique") != std::string::npos;
            ui.add_font(screen_font_family(font), std::move(bytes), italic);
        }
    }
}

Screens::~Screens() {
    for (auto& [id, o] : impl_->open) impl_->close(o);
}

void Screens::sync(const std::vector<ScreenDef>& defs, const std::vector<std::string>& shown,
                   const std::map<std::string, std::string>& texts,
                   const std::map<std::string, double>& values,
                   const std::map<std::string, bool>& enabled) {
    Impl& s = *impl_;
    const auto def_of = [&](const std::string& id) -> const ScreenDef* {
        for (const ScreenDef& d : defs)
            if (d.id == id) return &d;
        return nullptr;
    };
    for (auto& [id, o] : s.open) o.quiet = false;
    std::vector<std::string> order;
    for (const std::string& id : shown)
        if (def_of(id)) order.push_back(id);
    // Close those no longer shown, or whose design changed.
    for (auto it = s.open.begin(); it != s.open.end();) {
        const ScreenDef* d = def_of(it->first);
        const bool wanted = std::find(order.begin(), order.end(), it->first) != order.end();
        if (!wanted || !d || screen_rml(*d) != it->second.rml) {
            s.close(it->second);
            it = s.open.erase(it);
        } else {
            ++it;
        }
    }
    bool reorder = order != s.order;
    for (const std::string& id : order) {
        if (s.open.contains(id)) continue;
        const ScreenDef& d = *def_of(id);
        // Its pictures first (once each: a missing one is said once).
        for (const std::string& image : screen_images(d)) {
            if (!s.images || !s.tried.insert(image).second) continue;
            if (!s.ui.set_image_file(screen_image_name(image), s.images(image)))
                Rml::Log::Message(Rml::Log::LT_WARNING, "Screen '%s': no picture %s",
                                  d.name.c_str(), image.c_str());
        }
        load_fonts(s.ui, d, s.fonts, s.tried);
        Impl::Open o;
        o.def = d;
        o.rml = screen_rml(d);
        o.pauses = d.pauses;
        o.document = s.ui.open_text(o.rml, "midgard://screen/" + d.id, d.modal);
        if (!o.document) continue;  // (the reason is logged)
        o.document->AddEventListener(Rml::EventId::Click, &s);
        o.document->AddEventListener(Rml::EventId::Change, &s);
        for (const ScreenElement& e : d.elements)
            if (e.kind == "slider" || e.kind == "check" || e.kind == "option")
                o.values[e.id] = e.value;
        s.open.emplace(id, std::move(o));
        reorder = true;
    }
    if (reorder)
        for (const std::string& id : order)
            if (const auto it = s.open.find(id); it != s.open.end())
                it->second.document->PullToFront();
    s.order = order;
    for (auto& [id, o] : s.open) s.apply(id, o, texts, values, enabled);
}

std::vector<Screens::Event> Screens::take_events() {
    return std::exchange(impl_->events, {});
}

bool Screens::pauses() const {
    return std::any_of(impl_->open.begin(), impl_->open.end(),
                       [](const auto& o) { return o.second.pauses; });
}

Rml::ElementDocument* Screens::document(const std::string& screen) const {
    const auto it = impl_->open.find(screen);
    return it == impl_->open.end() ? nullptr : it->second.document;
}

}  // namespace midgard::ui
