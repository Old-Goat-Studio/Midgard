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
    float dy = 0.0f;                // the label moved down (dp), clear of the others' (full map)
    bool flip = false;              // its label on its left (near the full map's right edge)
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
    bool visible = true, crosshair = false, has_prompt = false, crosshair_picture = false;
    float crosshair_size = 32.0f, crosshair_offset = -16.0f;  // (half of it, back)
    Rml::String prompt;
    Rml::Vector<BarRow> bars;
    Rml::Vector<NumberRow> numbers;
    Rml::Vector<TimerRow> timers;
    Rml::Vector<NoteRow> notes;
    bool compass = false, minimap = false, has_ground = false, touch = false;
    float compass_left = 0.0f, arrow = 0.0f, ground_left = 0.0f, ground_top = 0.0f,
          ground_width = 100.0f;
    Rml::Vector<CompassMark> marks = compass_marks();
    Rml::Vector<MapMarkRow> map_marks;
    std::uint64_t ground_version = ~std::uint64_t(0);
    // The full map.
    bool bigmap = false, bigmap_ready = false, bigmap_ground = false;
    float bigmap_left = 0.0f, bigmap_top = 0.0f, bigmap_width = 100.0f, bigmap_height = 100.0f,
          bigmap_player_left = 50.0f, bigmap_player_top = 50.0f, bigmap_arrow = 0.0f;
    Rml::Vector<MapMarkRow> bigmap_marks;
    Rml::String bigmap_hint;
    std::uint64_t worldmap_version = ~std::uint64_t(0);

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
        s.RegisterMember("dy", &MapMarkRow::dy);
        s.RegisterMember("flip", &MapMarkRow::flip);
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
    model.Bind("bigmap", &h.bigmap);
    model.Bind("bigmap_ready", &h.bigmap_ready);
    model.Bind("bigmap_ground", &h.bigmap_ground);
    model.Bind("bigmap_left", &h.bigmap_left);
    model.Bind("bigmap_top", &h.bigmap_top);
    model.Bind("bigmap_width", &h.bigmap_width);
    model.Bind("bigmap_height", &h.bigmap_height);
    model.Bind("bigmap_marks", &h.bigmap_marks);
    model.Bind("bigmap_player_left", &h.bigmap_player_left);
    model.Bind("bigmap_player_top", &h.bigmap_player_top);
    model.Bind("bigmap_arrow", &h.bigmap_arrow);
    model.Bind("bigmap_hint", &h.bigmap_hint);
    model.Bind("visible", &h.visible);
    model.Bind("touch", &h.touch);
    model.Bind("crosshair", &h.crosshair);
    model.Bind("crosshair_picture", &h.crosshair_picture);
    model.Bind("crosshair_size", &h.crosshair_size);
    model.Bind("crosshair_offset", &h.crosshair_offset);
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

void Hud::set_touch(bool touch) {
    impl_->set(impl_->touch, touch, "touch");
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
        } else if (!s.text.empty()) {
            numbers.push_back({s.id, s.name, s.text});
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
    h.set(h.crosshair_picture, data.crosshair_picture, "crosshair_picture");
    h.set(h.crosshair_size, std::round(data.crosshair_size), "crosshair_size");
    h.set(h.crosshair_offset, -std::round(data.crosshair_size) / 2.0f, "crosshair_offset");
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

    // The full map: its picture (loaded again only when the game made it again), fitted in
    // its panel in its own proportions, and the marks and the player on it.
    const HudData::WorldMap& w = data.world_map;
    h.set(h.bigmap, w.shown, "bigmap");
    if (!w.shown) return;
    const bool picture = w.ground && w.width > 0 && w.height > 0 && !w.ground->empty();
    if (picture && w.version != h.worldmap_version) {
        h.ui.set_image("worldmap", uint32_t(w.width), uint32_t(w.height), *w.ground);
        h.worldmap_version = w.version;
    }
    h.set(h.bigmap_ready, w.ready, "bigmap_ready");
    h.set(h.bigmap_ground, picture, "bigmap_ground");
    // The panel's room for it (as laid out; before then, 3/4 of the window less its title).
    float room_w = 0.0f, room_h = 0.0f;
    if (h.document)
        if (Rml::Element* area = h.document->GetElementById("bigmap-area")) {
            const Rml::Vector2f size = area->GetBox().GetSize(Rml::BoxArea::Content);
            room_w = size.x;
            room_h = size.y;
        }
    if (room_w <= 1.0f || room_h <= 1.0f) {
        const Rml::Vector2i window = h.ui.context()->GetDimensions();
        const float dp = h.ui.context()->GetDensityIndependentPixelRatio();
        room_w = std::max(1.0f, float(window.x) * 0.75f - 24.0f * dp);
        room_h = std::max(1.0f, float(window.y) * 0.75f - 48.0f * dp);
    }
    const float room = room_w / room_h;
    const float aspect = float(w.aspect > 0.0 ? w.aspect : 1.0);
    float width = 100.0f, height = 100.0f;
    if (aspect > room)
        height = room / aspect * 100.0f;  // wider than the room: full width
    else
        width = aspect / room * 100.0f;  // taller: full height
    const auto tenth = [](float v) { return std::round(v * 10.0f) / 10.0f; };
    h.set(h.bigmap_width, tenth(width), "bigmap_width");
    h.set(h.bigmap_height, tenth(height), "bigmap_height");
    h.set(h.bigmap_left, tenth((100.0f - width) / 2.0f), "bigmap_left");
    h.set(h.bigmap_top, tenth((100.0f - height) / 2.0f), "bigmap_top");
    h.set(h.bigmap_player_left, tenth(float(w.player_x * 100.0)), "bigmap_player_left");
    h.set(h.bigmap_player_top, tenth(float(w.player_y * 100.0)), "bigmap_player_top");
    h.set(h.bigmap_arrow, std::round(float(w.heading) * 2.0f) / 2.0f, "bigmap_arrow");
    // The marks, their labels moved a line up or down where they would overlap (doors side
    // by side): each label's box guessed from its length, in dp.
    const float dp = std::max(h.ui.context()->GetDensityIndependentPixelRatio(), 0.01f);
    const float box_w = room_w * width / 100.0f / dp, box_h = room_h * height / 100.0f / dp;
    struct Box {
        float x0, y0, x1, y1;
    };
    std::vector<Box> taken;
    Rml::Vector<MapMarkRow> rows;
    for (const HudData::WorldMap::Mark& k : w.marks) {
        MapMarkRow row{k.kind, k.label, tenth(float(k.x * 100.0)), tenth(float(k.y * 100.0))};
        if (!k.label.empty()) {
            const float wide = 7.5f * float(k.label.size()) + 4.0f;
            row.flip = float(k.x) * box_w + 16.0f + wide > box_w;
            const float x = row.flip ? float(k.x) * box_w - 9.0f - wide : float(k.x) * box_w + 9.0f;
            const float y = float(k.y) * box_h - 10.0f;
            for (const int step : {0, 1, -1, 2, -2, 3, -3, 4, -4}) {
                const Box b{x, y + 17.0f * float(step), x + wide, y + 17.0f * float(step) + 16.0f};
                const bool clear = std::none_of(taken.begin(), taken.end(), [&](const Box& o) {
                    return b.x0 < o.x1 && o.x0 < b.x1 && b.y0 < o.y1 && o.y0 < b.y1;
                });
                if (clear || step == -4) {
                    row.dy = 17.0f * float(step);
                    taken.push_back(b);
                    break;
                }
            }
        }
        rows.push_back(std::move(row));
    }
    h.set(h.bigmap_marks, std::move(rows), "bigmap_marks");
    h.set(h.bigmap_hint, Rml::String(w.hint), "bigmap_hint");
}

}  // namespace midgard::ui
