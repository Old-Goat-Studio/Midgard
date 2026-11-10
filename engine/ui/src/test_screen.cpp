// SPDX-License-Identifier: Apache-2.0
// The built-in test screen (midgard_runtime --ui-test): every kind of control, each saying what
// it did, to try the mouse, keys, a gamepad and touch on.
#include <cstdio>
#include <string>

#include <RmlUi/Core.h>

#include "midgard/ui/ui.h"
#include "ui_test_screen.h"  // generated: resources/test_screen.rml

namespace midgard::ui {
namespace {

class TestScreen final : public Rml::EventListener {
public:
    explicit TestScreen(Ui& ui) : ui_(ui) {}

    void attach(Rml::ElementDocument* doc) {
        doc_ = doc;
        for (const char* type : {"click", "change", "keydown", "resize"})
            doc->AddEventListener(type, this);
        show_info();
    }

    void ProcessEvent(Rml::Event& event) override {
        const std::string type = event.GetType();
        if (type == "resize") return show_info();
        if (type == "keydown") {
            if (event.GetParameter<int>("key_identifier", 0) == Rml::Input::KI_ESCAPE)
                ui_.close(doc_);
            return;
        }
        Rml::Element* target = event.GetTargetElement();
        std::string id;
        for (Rml::Element* e = target; e && id.empty(); e = e->GetParentNode()) id = e->GetId();
        if (type == "click") {
            if (id == "close") return ui_.close(doc_);
            if (id == "one" || id == "two") {
                int& n = id == "one" ? ones_ : twos_;
                ++n;
                say("Pressed " + std::string(id == "one" ? "One" : "Two") + " (" +
                    std::to_string(n) + (n == 1 ? " time)" : " times)"));
            } else if (id == "font") {
                dyslexic_ = !dyslexic_;
                doc_->SetProperty("font-family", dyslexic_ ? "OpenDyslexic" : "Inconsolata");
                say(dyslexic_ ? "The font is OpenDyslexic" : "The font is Inconsolata");
            } else if (id.rfind("item", 0) == 0) {
                say("Chose item " + id.substr(4));
            }
        } else if (type == "change") {
            if (id == "subtitles") {
                const bool on = target->HasAttribute("checked");
                if (Rml::Element* state = doc_->GetElementById("subtitles_state"))
                    state->SetInnerRML(on ? "on" : "off");
                say(on ? "Subtitles on" : "Subtitles off");
            } else if (id == "volume") {
                say("Volume " + event.GetParameter<Rml::String>("value", ""));
            } else if (id == "name") {
                say("Name: " + event.GetParameter<Rml::String>("value", ""));
            }
        }
    }

    void OnDetach(Rml::Element* element) override {
        if (element == doc_) delete this;  // (the screen has gone)
    }

private:
    void say(const std::string& text) {
        if (Rml::Element* status = doc_->GetElementById("status"))
            status->SetInnerRML(Rml::StringUtilities::EncodeRml(text));
    }

    void show_info() {
        const Insets& s = ui_.safe_area();
        char text[160];
        std::snprintf(text, sizeof(text), "%u x %u px, %.2f px a dp; safe area %.0f %.0f %.0f %.0f",
                      ui_.width(), ui_.height(), double(ui_.dp_ratio()), double(s.left),
                      double(s.top), double(s.right), double(s.bottom));
        if (Rml::Element* info = doc_->GetElementById("info")) info->SetInnerRML(text);
    }

    Ui& ui_;
    Rml::ElementDocument* doc_ = nullptr;
    int ones_ = 0, twos_ = 0;
    bool dyslexic_ = false;
};

}  // namespace

const std::string& test_screen_rml() {
    static const std::string text = [] {
        std::string out;
        for (const char* piece : kUiTestScreen) out += piece;
        return out;
    }();
    return text;
}

Rml::ElementDocument* open_test_screen(Ui& ui) {
    Rml::ElementDocument* doc = ui.open_text(test_screen_rml(), "midgard://test_screen.rml", true);
    if (doc) (new TestScreen(ui))->attach(doc);
    return doc;
}

}  // namespace midgard::ui
