// SPDX-License-Identifier: Apache-2.0
// On-screen touch controls: the fingers' work (which finger does what), and the screen that
// shows it (resources/touch.rml).
#include "midgard/ui/touch.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <optional>

#include <RmlUi/Core.h>

#include "midgard/ui/ui.h"
#include "ui_touch_rml.h"  // generated: resources/touch.rml

namespace midgard::ui {
namespace {

// Sizes and places in dp. The stick rests this far in from the safe area's bottom left; the
// buttons sit this far in from its bottom right (x left, y up).
constexpr float kStickRestX = 110.0f, kStickRestY = 110.0f, kStickRestNarrow = 96.0f;
constexpr float kStickRadius = 60.0f, kKnobRadius = 26.0f;
constexpr float kButtonSlop = 8.0f;  // a finger this near a button's rim still presses it

struct ButtonDef {
    const char* id;
    const char* label;
    float x, y, r;  // from the safe area's bottom right
    bool toggles;   // tap on, tap off (Run)
    bool flying_only;
    bool menu_only = false;  // shown when the game has a pause menu
};
constexpr ButtonDef kButtons[] = {
    {"jump", "Jump", 84.0f, 96.0f, 40.0f, false, false},
    {"use", "Use", 186.0f, 62.0f, 30.0f, false, false},
    {"run", "Run", 180.0f, 170.0f, 30.0f, true, false},
    {"view", "View", 78.0f, 206.0f, 26.0f, false, false},
    {"descend", "Down", 84.0f, 290.0f, 30.0f, false, true},
    {"menu", "Menu", 186.0f, 252.0f, 26.0f, false, false, true},
};
// On a narrow screen (a phone held upright) the buttons draw in closer to the right edge, so
// they keep clear of the stick: the same buttons, in the same order.
constexpr float kNarrow = 560.0f;  // dp across the safe area
constexpr ButtonDef kButtonsNarrow[] = {
    {"jump", "Jump", 74.0f, 92.0f, 38.0f, false, false},
    {"use", "Use", 166.0f, 56.0f, 28.0f, false, false},
    {"run", "Run", 162.0f, 152.0f, 28.0f, true, false},
    {"view", "View", 70.0f, 196.0f, 26.0f, false, false},
    {"descend", "Down", 152.0f, 240.0f, 28.0f, false, true},
    {"menu", "Menu", 70.0f, 290.0f, 26.0f, false, false, true},
};
static_assert(std::size(kButtons) == std::size(kButtonsNarrow));

struct ButtonRow {
    Rml::String id, label;
    float left = 0.0f, top = 0.0f, size = 0.0f;
    bool down = false, shown = true;
    bool operator==(const ButtonRow&) const = default;
};

const std::string& touch_rml() {
    static const std::string text = [] {
        std::string out;
        for (const char* piece : kUiTouchRml) out += piece;
        return out;
    }();
    return text;
}

}  // namespace

struct TouchControls::Impl {
    Ui& ui;
    Rml::DataModelHandle handle;
    Rml::ElementDocument* document = nullptr;
    bool shown = false, flying = false, menu = false;

    // Each finger's job, from where it went down.
    enum class Job { Stick, Look, Button };
    struct Finger {
        Job job = Job::Look;
        int button = -1;
        float x = 0.0f, y = 0.0f;  // where it was last
    };
    std::map<int64_t, Finger> fingers;
    std::optional<int64_t> stick_finger, look_finger;
    float stick_x = 0.0f, stick_y = 0.0f;  // the stick's middle while a finger holds it (px)
    float knob_x = 0.0f, knob_y = 0.0f;
    int presses[std::size(kButtons)] = {};  // fingers on each button
    bool toggled[std::size(kButtons)] = {};
    Output out;

    // The screen's data.
    bool m_shown = false, m_active = false;
    float stick_left = 0, stick_top = 0, stick_size = 0, knob_left = 0, knob_top = 0, knob_size = 0;
    Rml::Vector<ButtonRow> rows;

    explicit Impl(Ui& u) : ui(u) {}

    float dp() const { return ui.dp_ratio(); }
    bool narrow() const {
        const Insets& s = ui.safe_area();
        return (float(ui.width()) - s.left - s.right) / dp() < kNarrow;
    }
    Circle rest() const {
        const Insets& s = ui.safe_area();
        const float in = narrow() ? kStickRestNarrow : kStickRestX;
        return {s.left + in * dp(), float(ui.height()) - s.bottom - kStickRestY * dp(),
                kStickRadius * dp()};
    }
    bool button_shown(size_t i) const {
        return (!kButtons[i].flying_only || flying) && (!kButtons[i].menu_only || menu);
    }
    Circle button(size_t i) const {
        const Insets& s = ui.safe_area();
        const ButtonDef& b = narrow() ? kButtonsNarrow[i] : kButtons[i];
        if (!button_shown(i)) return {};
        return {float(ui.width()) - s.right - b.x * dp(),
                float(ui.height()) - s.bottom - b.y * dp(), b.r * dp()};
    }
    int button_at(float x, float y) const {
        for (size_t i = 0; i < std::size(kButtons); ++i) {
            const Circle c = button(i);
            if (c.r > 0.0f && std::hypot(x - c.x, y - c.y) <= c.r + kButtonSlop * dp())
                return int(i);
        }
        return -1;
    }

    // The stick's middle kept so all of it is on the screen.
    void place_stick(float x, float y) {
        const Insets& s = ui.safe_area();
        const float r = kStickRadius * dp();
        stick_x = std::clamp(x, s.left + r, std::max(s.left + r, float(ui.width()) - s.right - r));
        stick_y = std::clamp(y, s.top + r, std::max(s.top + r, float(ui.height()) - s.bottom - r));
    }
    void move_knob(float x, float y) {
        const float r = kStickRadius * dp();
        float dx = x - stick_x, dy = y - stick_y;
        const float d = std::hypot(dx, dy);
        if (d > r) {  // past the rim: the stick follows the finger
            stick_x += dx * (1.0f - r / d);
            stick_y += dy * (1.0f - r / d);
            dx *= r / d;
            dy *= r / d;
        }
        knob_x = stick_x + dx;
        knob_y = stick_y + dy;
        float vx = dx / r, vy = -dy / r;  // (up the screen: forward)
        const float amount = std::hypot(vx, vy);
        if (amount < kStickDeadZone) {
            vx = vy = 0.0f;
        } else {  // (from the dead zone's edge to the rim: 0 to 1)
            const float eased = (amount - kStickDeadZone) / (1.0f - kStickDeadZone) / amount;
            vx *= eased;
            vy *= eased;
        }
        out.stick_x = std::clamp(vx, -1.0f, 1.0f);
        out.stick_y = std::clamp(vy, -1.0f, 1.0f);
    }
    void release_stick() {
        stick_finger.reset();
        out.stick_x = out.stick_y = 0.0f;
    }
    void release_all() {
        fingers.clear();
        stick_finger.reset();
        look_finger.reset();
        out.stick_x = out.stick_y = out.drag_x = out.drag_y = 0.0f;
        for (int& p : presses) p = 0;
        refresh_buttons();
    }
    void refresh_buttons() {
        for (size_t i = 0; i < std::size(kButtons); ++i) {
            const bool on = kButtons[i].toggles ? toggled[i] : presses[i] > 0;
            out.buttons[kButtons[i].id] = on ? 1.0f : 0.0f;
        }
    }

    template <typename T>
    void set(T& now, T next, const char* name) {
        if (now == next) return;
        now = std::move(next);
        handle.DirtyVariable(name);
    }
};

TouchControls::TouchControls(Ui& ui) : impl_(std::make_unique<Impl>(ui)) {
    Impl& t = *impl_;
    Rml::DataModelConstructor model = ui.context()->CreateDataModel("touch");
    if (!model) throw UiError("The touch controls' data model couldn't be made");
    if (auto s = model.RegisterStruct<ButtonRow>()) {
        s.RegisterMember("id", &ButtonRow::id);
        s.RegisterMember("label", &ButtonRow::label);
        s.RegisterMember("left", &ButtonRow::left);
        s.RegisterMember("top", &ButtonRow::top);
        s.RegisterMember("size", &ButtonRow::size);
        s.RegisterMember("down", &ButtonRow::down);
        s.RegisterMember("shown", &ButtonRow::shown);
    }
    model.RegisterArray<Rml::Vector<ButtonRow>>();
    model.Bind("shown", &t.m_shown);
    model.Bind("stick_active", &t.m_active);
    model.Bind("stick_left", &t.stick_left);
    model.Bind("stick_top", &t.stick_top);
    model.Bind("stick_size", &t.stick_size);
    model.Bind("knob_left", &t.knob_left);
    model.Bind("knob_top", &t.knob_top);
    model.Bind("knob_size", &t.knob_size);
    model.Bind("buttons", &t.rows);
    t.handle = model.GetModelHandle();
    t.refresh_buttons();
    t.document = ui.open_text(touch_rml(), "midgard://touch.rml", false);
    if (!t.document) {
        ui.context()->RemoveDataModel("touch");
        throw UiError("Midgard's touch controls screen couldn't be read");
    }
    update();
}

TouchControls::~TouchControls() {
    Impl& t = *impl_;
    if (t.document) {
        t.ui.close(t.document);
        t.ui.context()->Update();  // (the screen goes before the data it shows)
    }
    t.ui.context()->RemoveDataModel("touch");
}

void TouchControls::set_shown(bool shown) {
    Impl& t = *impl_;
    if (t.shown == shown) return;
    t.shown = shown;
    t.release_all();
    if (!shown)
        for (bool& on : t.toggled) on = false;  // (back to walking)
    t.refresh_buttons();
}

bool TouchControls::shown() const {
    return impl_->shown;
}

void TouchControls::set_flying(bool flying) {
    Impl& t = *impl_;
    if (t.flying == flying) return;
    t.flying = flying;
    // A finger on the Down button as it goes lets go of it.
    for (auto& [id, f] : t.fingers)
        if (f.job == Impl::Job::Button && !t.button_shown(size_t(f.button))) {
            t.presses[f.button] = 0;
            f.button = -1;
        }
    t.refresh_buttons();
}

void TouchControls::set_menu(bool menu) {
    impl_->menu = menu;
}

bool TouchControls::handle(const Input& in) {
    Impl& t = *impl_;
    using T = Input::Type;
    if (in.type != T::TouchDown && in.type != T::TouchMove && in.type != T::TouchUp &&
        in.type != T::TouchCancel)
        return false;
    if (!t.shown) return false;
    if (in.type == T::TouchDown) {
        Impl::Finger f;
        f.x = in.x;
        f.y = in.y;
        if (const int b = t.button_at(in.x, in.y); b >= 0) {
            f.job = Impl::Job::Button;
            f.button = b;
            ++t.presses[b];
            if (kButtons[b].toggles) t.toggled[b] = !t.toggled[b];
            t.refresh_buttons();
        } else if (in.x < 0.5f * float(t.ui.width()) && !t.stick_finger) {
            f.job = Impl::Job::Stick;
            t.stick_finger = in.touch;
            t.place_stick(in.x, in.y);
            t.move_knob(in.x, in.y);
        } else if (!t.look_finger) {
            f.job = Impl::Job::Look;
            t.look_finger = in.touch;
        } else {
            f.job = Impl::Job::Button;  // (a third finger on the game: nothing)
            f.button = -1;
        }
        t.fingers[in.touch] = f;
        return true;
    }
    const auto it = t.fingers.find(in.touch);
    if (it == t.fingers.end()) return true;  // (one that went down before they showed)
    Impl::Finger& f = it->second;
    if (in.type == T::TouchMove) {
        if (f.job == Impl::Job::Stick) {
            t.move_knob(in.x, in.y);
        } else if (f.job == Impl::Job::Look) {
            t.out.drag_x += (in.x - f.x) / t.dp();
            t.out.drag_y += (in.y - f.y) / t.dp();
        }
        f.x = in.x;
        f.y = in.y;
        return true;
    }
    // Up, or cancelled.
    if (f.job == Impl::Job::Stick) t.release_stick();
    if (f.job == Impl::Job::Look) t.look_finger.reset();
    if (f.job == Impl::Job::Button && f.button >= 0) {
        t.presses[f.button] = std::max(0, t.presses[f.button] - 1);
        t.refresh_buttons();
    }
    t.fingers.erase(it);
    return true;
}

TouchControls::Output TouchControls::take() {
    Output now = impl_->out;
    impl_->out.drag_x = impl_->out.drag_y = 0.0f;
    return now;
}

const TouchControls::Output& TouchControls::peek() const {
    return impl_->out;
}

TouchControls::Circle TouchControls::stick() const {
    const Impl& t = *impl_;
    if (!t.stick_finger) return t.rest();
    return {t.stick_x, t.stick_y, kStickRadius * t.dp()};
}

TouchControls::Circle TouchControls::knob() const {
    const Impl& t = *impl_;
    if (!t.stick_finger) {
        const Circle r = t.rest();
        return {r.x, r.y, kKnobRadius * t.dp()};
    }
    return {t.knob_x, t.knob_y, kKnobRadius * t.dp()};
}

TouchControls::Circle TouchControls::button(const std::string& id) const {
    for (size_t i = 0; i < std::size(kButtons); ++i)
        if (id == kButtons[i].id) return impl_->button(i);
    return {};
}

void TouchControls::update() {
    Impl& t = *impl_;
    const Insets& s = t.ui.safe_area();
    // (rounded to quarter pixels: small changes don't redo the layout)
    const auto px = [](float v) { return std::round(v * 4.0f) / 4.0f; };
    t.set(t.m_shown, t.shown, "shown");
    t.set(t.m_active, t.stick_finger.has_value(), "stick_active");
    const Circle st = stick(), kn = knob();
    t.set(t.stick_left, px(st.x - st.r - s.left), "stick_left");
    t.set(t.stick_top, px(st.y - st.r - s.top), "stick_top");
    t.set(t.stick_size, px(2.0f * st.r), "stick_size");
    t.set(t.knob_left, px(kn.x - kn.r - s.left), "knob_left");
    t.set(t.knob_top, px(kn.y - kn.r - s.top), "knob_top");
    t.set(t.knob_size, px(2.0f * kn.r), "knob_size");
    Rml::Vector<ButtonRow> rows;
    for (size_t i = 0; i < std::size(kButtons); ++i) {
        const ButtonDef& b = kButtons[i];
        const Circle c = t.button(i);
        const bool down = b.toggles ? t.toggled[i] : t.presses[i] > 0;
        rows.push_back({b.id, b.label, px(c.x - c.r - s.left), px(c.y - c.r - s.top),
                        px(2.0f * c.r), down, c.r > 0.0f});
    }
    t.set(t.rows, std::move(rows), "buttons");
}

Rml::ElementDocument* TouchControls::document() {
    return impl_->document;
}

}  // namespace midgard::ui
