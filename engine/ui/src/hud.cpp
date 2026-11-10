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
struct CompassMark {
    Rml::String text;
    float left = 0.0f;  // dp along the strip
    bool major = false;
};
struct MapMarkRow {
    Rml::String kind, label;
    float left = 0.0f, top = 0.0f;  // % of the minimap
    bool operator==(const MapMarkRow&) const = default;
};

// The compass strip: 2.5 dp a degree, from -180 to 540 degrees, so any heading's 120 degrees
// (300 dp shown) are on it without wrapping.
constexpr float kDpPerDegree = 2.5f;
constexpr float kStripShown = 300.0f;

Rml::Vector<CompassMark> compass_marks() {
    Rml::Vector<CompassMark> out;
    static const char* const names[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    for (int d = -180; d <= 540; d += 15) {
        const int a = ((d % 360) + 360) % 360;
        CompassMark m;
        m.left = float(d + 180) * kDpPerDegree;
        m.major = a % 45 == 0;
        m.text = m.major ? names[a / 45] : (a % 15 == 0 ? Rml::String("|") : Rml::String());
        out.push_back(std::move(m));
    }
    return out;
}

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
    bool compass = false, minimap = false, has_ground = false;
    float compass_left = 0.0f, arrow = 0.0f, ground_left = 0.0f, ground_top = 0.0f,
          ground_width = 100.0f;
    Rml::Vector<CompassMark> marks = compass_marks();
    Rml::Vector<MapMarkRow> map_marks;
    std::uint64_t ground_version = ~std::uint64_t(0);

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
    if (auto s = model.RegisterStruct<CompassMark>()) {
        s.RegisterMember("text", &CompassMark::text);
        s.RegisterMember("left", &CompassMark::left);
        s.RegisterMember("major", &CompassMark::major);
    }
    if (auto s = model.RegisterStruct<MapMarkRow>()) {
        s.RegisterMember("kind", &MapMarkRow::kind);
        s.RegisterMember("label", &MapMarkRow::label);
        s.RegisterMember("left", &MapMarkRow::left);
        s.RegisterMember("top", &MapMarkRow::top);
    }
    model.RegisterArray<Rml::Vector<CompassMark>>();
    model.RegisterArray<Rml::Vector<MapMarkRow>>();
    model.Bind("compass", &h.compass);
    model.Bind("compass_left", &h.compass_left);
    model.Bind("compass_marks", &h.marks);
    model.Bind("minimap", &h.minimap);
    model.Bind("has_ground", &h.has_ground);
    model.Bind("arrow", &h.arrow);
    model.Bind("ground_left", &h.ground_left);
    model.Bind("ground_top", &h.ground_top);
    model.Bind("ground_width", &h.ground_width);
    model.Bind("map_marks", &h.map_marks);
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

    // The compass: the strip slides so the heading is in its middle.
    h.set(h.compass, data.heading.has_value(), "compass");
    if (data.heading) {
        const float left = kStripShown / 2.0f - (float(*data.heading) + 180.0f) * kDpPerDegree;
        h.set(h.compass_left, std::round(left * 4.0f) / 4.0f, "compass_left");
    }
    // The minimap: its ground (made again only when the game made it again), the player's
    // arrow, and the marks, as shares of its width from the middle.
    const HudData::Map& m = data.map;
    h.set(h.minimap, m.shown, "minimap");
    const bool ground = m.shown && m.ground && m.size > 0 && !m.ground->empty();
    if (ground && m.version != h.ground_version) {
        h.ui.set_image("minimap", uint32_t(m.size), uint32_t(m.size), *m.ground);
        h.ground_version = m.version;
    }
    h.set(h.has_ground, ground, "has_ground");
    if (m.shown) {
        const double r = m.radius > 0.0 ? m.radius : 1.0;
        h.set(h.arrow, std::round(float(data.heading.value_or(0.0)) * 2.0f) / 2.0f, "arrow");
        // (the ground may be wider than what shows: centred on its own middle)
        const double width = (m.ground_radius > 0.0 ? m.ground_radius : r) / r * 100.0;
        h.set(h.ground_width, float(width), "ground_width");
        h.set(h.ground_left, float(50.0 - width / 2.0 - m.east / r * 50.0), "ground_left");
        h.set(h.ground_top, float(50.0 - width / 2.0 + m.north / r * 50.0), "ground_top");
        Rml::Vector<MapMarkRow> rows;
        for (const HudData::Map::Mark& k : m.marks)
            rows.push_back({k.kind, k.label, float(50.0 + k.east / r * 50.0),
                            float(50.0 - k.north / r * 50.0)});
        h.set(h.map_marks, std::move(rows), "map_marks");
    }
}

}  // namespace midgard::ui
