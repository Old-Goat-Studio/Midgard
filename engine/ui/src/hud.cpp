// SPDX-License-Identifier: Apache-2.0
// The HUD: its data model ("hud") and Midgard's own HUD screen (resources/hud.rml).
#include "midgard/ui/hud.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <RmlUi/Core.h>

#include "midgard/ui/ui.h"
#include "ui_hud_rml.h"  // generated: resources/hud.rml

namespace midgard::ui {
namespace {

struct BarRow {
    Rml::String id, name, text;
    float percent = 0.0f;
    bool operator==(const BarRow&) const = default;
};
struct NumberRow {
    Rml::String id, name, text;
    bool operator==(const NumberRow&) const = default;
};
struct TimerRow {
    Rml::String id, label, text;
    bool operator==(const TimerRow&) const = default;
};
struct NoteRow {
    Rml::String text;
    float opacity = 1.0f;
    bool operator==(const NoteRow&) const = default;
};

std::string whole(double v) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.0f", v);
    return text;
}

const std::string& hud_rml() {
    static const std::string text = [] {
        std::string out;
        for (const char* piece : kUiHudRml) out += piece;
        return out;
    }();
    return text;
}

}  // namespace

std::string clock_text(double seconds) {
    const long long s = static_cast<long long>(std::ceil(std::max(seconds, 0.0) - 1e-9));
    char text[32];
    if (s >= 3600)
        std::snprintf(text, sizeof(text), "%lld:%02lld:%02lld", s / 3600, s / 60 % 60, s % 60);
    else
        std::snprintf(text, sizeof(text), "%lld:%02lld", s / 60, s % 60);
    return text;
}

struct Hud::Impl {
    Ui& ui;
    Rml::DataModelHandle handle;
    Rml::ElementDocument* document = nullptr;
    bool visible = true, crosshair = false, has_prompt = false;
    Rml::String prompt;
    Rml::Vector<BarRow> bars;
    Rml::Vector<NumberRow> numbers;
    Rml::Vector<TimerRow> timers;
    Rml::Vector<NoteRow> notes;

    explicit Impl(Ui& u) : ui(u) {}

    template <typename T>
    void set(T& now, T next, const char* name) {
        if (now == next) return;
        now = std::move(next);
        handle.DirtyVariable(name);
    }
};

Hud::Hud(Ui& ui) : impl_(std::make_unique<Impl>(ui)) {
    Impl& h = *impl_;
    Rml::DataModelConstructor model = ui.context()->CreateDataModel("hud");
    if (!model) throw UiError("The HUD's data model couldn't be made");
    if (auto s = model.RegisterStruct<BarRow>()) {
        s.RegisterMember("id", &BarRow::id);
        s.RegisterMember("name", &BarRow::name);
        s.RegisterMember("text", &BarRow::text);
        s.RegisterMember("percent", &BarRow::percent);
    }
    if (auto s = model.RegisterStruct<NumberRow>()) {
        s.RegisterMember("id", &NumberRow::id);
        s.RegisterMember("name", &NumberRow::name);
        s.RegisterMember("text", &NumberRow::text);
    }
    if (auto s = model.RegisterStruct<TimerRow>()) {
        s.RegisterMember("id", &TimerRow::id);
        s.RegisterMember("label", &TimerRow::label);
        s.RegisterMember("text", &TimerRow::text);
    }
    if (auto s = model.RegisterStruct<NoteRow>()) {
        s.RegisterMember("text", &NoteRow::text);
        s.RegisterMember("opacity", &NoteRow::opacity);
    }
    model.RegisterArray<Rml::Vector<BarRow>>();
    model.RegisterArray<Rml::Vector<NumberRow>>();
    model.RegisterArray<Rml::Vector<TimerRow>>();
    model.RegisterArray<Rml::Vector<NoteRow>>();
    model.Bind("visible", &h.visible);
    model.Bind("crosshair", &h.crosshair);
    model.Bind("has_prompt", &h.has_prompt);
    model.Bind("prompt", &h.prompt);
    model.Bind("bars", &h.bars);
    model.Bind("numbers", &h.numbers);
    model.Bind("timers", &h.timers);
    model.Bind("notes", &h.notes);
    h.handle = model.GetModelHandle();
    h.document = ui.open_text(hud_rml(), "midgard://hud.rml", false);
    if (!h.document) {
        ui.context()->RemoveDataModel("hud");
        throw UiError("Midgard's HUD screen couldn't be read");
    }
}

Hud::~Hud() {
    Impl& h = *impl_;
    if (h.document) {
        h.ui.close(h.document);
        h.ui.context()->Update();  // (the screen goes before the data it shows)
    }
    h.ui.context()->RemoveDataModel("hud");
}

Rml::ElementDocument* Hud::document() {
    return impl_->document;
}

void Hud::show(const HudData& data) {
    Impl& h = *impl_;
    Rml::Vector<BarRow> bars;
    Rml::Vector<NumberRow> numbers;
    for (const HudData::Stat& s : data.stats) {
        if (s.bar) {
            const double max = s.max > 0.0 ? s.max : 1.0;
            const float percent = float(std::clamp(s.value / max * 100.0, 0.0, 100.0));
            bars.push_back({s.id, s.name, whole(s.value) + "/" + whole(s.max),
                            std::round(percent * 10.0f) / 10.0f});
        } else {
            numbers.push_back(
                {s.id, s.name, s.max > 0.0 ? whole(s.value) + "/" + whole(s.max) : whole(s.value)});
        }
    }
    Rml::Vector<TimerRow> timers;
    for (const HudData::Timer& t : data.timers)
        timers.push_back({t.id, t.label, clock_text(t.seconds)});
    Rml::Vector<NoteRow> notes;
    for (const HudData::Note& n : data.notes)
        notes.push_back({n.text, std::round(n.opacity * 20.0f) / 20.0f});
    h.set(h.visible, data.visible, "visible");
    h.set(h.crosshair, data.crosshair, "crosshair");
    h.set(h.has_prompt, !data.prompt.empty(), "has_prompt");
    h.set(h.prompt, Rml::String(data.prompt), "prompt");
    h.set(h.bars, std::move(bars), "bars");
    h.set(h.numbers, std::move(numbers), "numbers");
    h.set(h.timers, std::move(timers), "timers");
    h.set(h.notes, std::move(notes), "notes");
}

}  // namespace midgard::ui
