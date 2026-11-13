// SPDX-License-Identifier: Apache-2.0
// The portal menu: its screen (resources/portal.rml) and what the player chooses on it.
#include "midgard/ui/portal_menu.h"

#include <RmlUi/Core.h>

#include "midgard/ui/ui.h"
#include "ui_portal_rml.h"  // generated: resources/portal.rml

namespace midgard::ui {
namespace {

const std::string& portal_rml() {
    static const std::string text = [] {
        std::string out;
        for (const char* piece : kUiPortalRml) out += piece;
        return out;
    }();
    return text;
}

std::string encode(const std::string& text) {
    return Rml::StringUtilities::EncodeRml(text);
}

}  // namespace

struct PortalMenu::Impl : Rml::EventListener {
    Ui& ui;
    Rml::ElementDocument* document = nullptr;
    std::optional<Choice> chosen;

    explicit Impl(Ui& u) : ui(u) {}

    std::string field() const {
        Rml::Element* name = document ? document->GetElementById("name") : nullptr;
        return name ? name->GetAttribute<Rml::String>("value", "") : std::string();
    }

    void ProcessEvent(Rml::Event& event) override {
        const std::string type = event.GetType();
        Rml::Element* target = event.GetTargetElement();
        if (type == "keydown") {
            const int key = event.GetParameter<int>("key_identifier", 0);
            if (key == Rml::Input::KI_ESCAPE) {
                chosen = Choice{Choice::Kind::Close, {}};
            } else if (key == Rml::Input::KI_RETURN && target && target->GetId() == "name") {
                chosen = Choice{Choice::Kind::Rename, field()};
            }
            return;
        }
        if (type != "click") return;
        for (Rml::Element* e = target; e; e = e->GetParentNode()) {
            const std::string id = e->GetId();
            if (id == "rename") {
                chosen = Choice{Choice::Kind::Rename, field()};
                return;
            }
            if (id == "close") {
                chosen = Choice{Choice::Kind::Close, {}};
                return;
            }
            if (e->HasAttribute("data-portal")) {
                chosen =
                    Choice{Choice::Kind::Travel, e->GetAttribute<Rml::String>("data-portal", "")};
                return;
            }
        }
    }
};

PortalMenu::PortalMenu(Ui& ui, const std::string& name,
                       const std::vector<Destination>& destinations)
    : impl_(std::make_unique<Impl>(ui)) {
    Impl& m = *impl_;
    m.document = ui.open_text(portal_rml(), "midgard://portal.rml", true);
    if (!m.document) throw UiError("Midgard's portal menu couldn't be read");
    set_name(name);
    std::string rows;
    for (const Destination& d : destinations)
        rows += "<button data-portal=\"" + encode(d.id) + "\">" + encode(d.name) +
                (d.place.empty() ? std::string()
                                 : " <span class=\"place\">(" + encode(d.place) + ")</span>") +
                "</button>";
    if (rows.empty()) rows = "<p id=\"none\">There are no other portals yet.</p>";
    if (Rml::Element* list = m.document->GetElementById("list")) list->SetInnerRML(rows);
    m.document->AddEventListener(Rml::EventId::Click, &m);
    m.document->AddEventListener(Rml::EventId::Keydown, &m);
}

PortalMenu::~PortalMenu() {
    Impl& m = *impl_;
    if (!m.document) return;
    m.document->RemoveEventListener(Rml::EventId::Click, &m);
    m.document->RemoveEventListener(Rml::EventId::Keydown, &m);
    m.ui.close(m.document);
    m.ui.context()->Update();  // (the screen goes before its listener does)
}

std::optional<PortalMenu::Choice> PortalMenu::take() {
    return std::exchange(impl_->chosen, std::nullopt);
}

void PortalMenu::say(const std::string& text) {
    if (Rml::Element* said = impl_->document->GetElementById("said"))
        said->SetInnerRML(encode(text));
}

void PortalMenu::set_name(const std::string& name) {
    if (Rml::Element* field = impl_->document->GetElementById("name"))
        field->SetAttribute("value", name);
}

Rml::ElementDocument* PortalMenu::document() {
    return impl_->document;
}

}  // namespace midgard::ui
