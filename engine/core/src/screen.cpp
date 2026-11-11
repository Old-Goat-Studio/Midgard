// SPDX-License-Identifier: Apache-2.0
#include "midgard/screen.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <set>

#include <nlohmann/json.hpp>

namespace midgard {
namespace {

using json = nlohmann::json;

bool known(const std::vector<std::string>& names, const std::string& name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

std::string num(double v) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.3g", std::abs(v) < 1e-9 ? 0.0 : v);
    return text;
}

std::string dp(double v) {
    return num(v) + "dp";
}

std::string colour(const Rgba& c) {
    const auto byte = [](double v) { return int(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)); };
    char text[16];
    std::snprintf(text, sizeof(text), "#%02x%02x%02x%02x", byte(c[0]), byte(c[1]), byte(c[2]),
                  byte(c[3]));
    return text;
}

}  // namespace

const std::vector<std::string>& screen_kinds() {
    static const std::vector<std::string> kinds = {"panel", "text",   "button", "slider",
                                                   "check", "option", "image"};
    return kinds;
}

const std::vector<std::string>& screen_image_states(const std::string& kind) {
    static const std::vector<std::string> button = {"hover", "pressed", "disabled"};
    static const std::vector<std::string> tick = {"unchecked", "checked"};
    static const std::vector<std::string> slider = {"track", "pointer"};
    static const std::vector<std::string> none;
    if (kind == "button") return button;
    if (kind == "check" || kind == "option") return tick;
    if (kind == "slider") return slider;
    return none;
}

const std::vector<std::string>& screen_anchors() {
    static const std::vector<std::string> anchors = {
        "top_left", "top",         "top_right", "left",         "center",
        "right",    "bottom_left", "bottom",    "bottom_right", "fill"};
    return anchors;
}

// A button's control to rebind (midgard/input.h's control_slot): key:<action>, pad:<action>,
// or one way of an axis, key:<action>:+ or key:<action>:-.
bool rebinds(const std::string& binds) {
    if (!binds.starts_with("key:") && !binds.starts_with("pad:")) return false;
    std::string rest = binds.substr(4);
    if (rest.ends_with(":+") || rest.ends_with(":-")) rest.resize(rest.size() - 2);
    return !rest.empty() && rest.find(':') == std::string::npos;
}

const std::vector<std::string>& screen_actions() {
    static const std::vector<std::string> actions = {"none",    "close", "open",          "quit",
                                                     "respawn", "event", "reset_controls"};
    return actions;
}

const std::vector<std::string>& screen_roles() {
    static const std::vector<std::string> roles = {"",      "splash",   "title",
                                                   "pause", "settings", "game_over"};
    return roles;
}

const std::vector<std::string>& screen_buses() {
    static const std::vector<std::string> buses = {"master", "music",    "effects",
                                                   "voice",  "ambience", "interface"};
    return buses;
}

const ScreenElement* ScreenDef::find(const std::string& element) const {
    for (const ScreenElement& e : elements)
        if (e.id == element) return &e;
    return nullptr;
}

ScreenDef screen_from_json(const std::string& id, const std::string& name,
                           const std::string& text) {
    const std::string who = "Screen '" + (name.empty() ? id : name) + "'";
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object()) throw ScreenError(who + " isn't a JSON object");
    ScreenDef s;
    s.id = id;
    s.name = name.empty() ? id : name;
    const auto flag = [&](const json& o, const char* key, bool fallback, const std::string& in) {
        if (!o.contains(key)) return fallback;
        if (!o.at(key).is_boolean()) throw ScreenError(in + ": '" + key + "' isn't true or false");
        return o.at(key).get<bool>();
    };
    const auto number = [&](const json& o, const char* key, double fallback,
                            const std::string& in) {
        if (!o.contains(key)) return fallback;
        const json& v = o.at(key);
        if (!v.is_number() || !std::isfinite(v.get<double>()))
            throw ScreenError(in + ": '" + key + "' isn't a number");
        return v.get<double>();
    };
    const auto rgba = [&](const json& o, const char* key,
                          const std::string& in) -> std::optional<Rgba> {
        if (!o.contains(key) || o.at(key).is_null()) return std::nullopt;
        const json& v = o.at(key);
        if (!v.is_array() || (v.size() != 3 && v.size() != 4))
            throw ScreenError(in + ": '" + key + "' isn't a colour [r, g, b] or [r, g, b, a]");
        Rgba c{0.0, 0.0, 0.0, 1.0};
        for (size_t i = 0; i < v.size(); ++i) {
            if (!v[i].is_number()) throw ScreenError(in + ": '" + key + "' isn't a colour");
            c[i] = v[i].get<double>();
        }
        return c;
    };
    const auto string = [&](const json& o, const char* key, const std::string& fallback,
                            const std::string& in) {
        if (!o.contains(key)) return fallback;
        if (!o.at(key).is_string()) throw ScreenError(in + ": '" + key + "' isn't text");
        return o.at(key).get<std::string>();
    };
    s.role = string(j, "role", "", who);
    if (!known(screen_roles(), s.role))
        throw ScreenError(who + ": no role '" + s.role +
                          "' (splash, title, pause, settings or game_over)");
    s.stat = string(j, "stat", "health", who);
    s.modal = flag(j, "modal", true, who);
    s.pauses = flag(j, "pauses", false, who);
    s.music = flag(j, "music", true, who);
    s.seconds = std::clamp(number(j, "seconds", 0.0, who), 0.0, 60.0);
    s.background = rgba(j, "background", who);
    s.image = string(j, "image", "", who);
    if (!s.image.empty() && !s.image.starts_with("ui:"))
        throw ScreenError(who + ": its image isn't one from the Asset Library's GUI section");
    const json elements = j.value("elements", json::array());
    if (!elements.is_array()) throw ScreenError(who + ": 'elements' isn't a list");
    std::set<std::string> ids, panels;
    for (const json& e : elements) {
        if (!e.is_object()) throw ScreenError(who + " has an element that isn't a JSON object");
        ScreenElement el;
        el.id = string(e, "id", "", who);
        if (el.id.empty()) throw ScreenError(who + " has an element without an id");
        const std::string in = who + ", element '" + el.id + "'";
        if (!ids.insert(el.id).second) throw ScreenError(in + ": its id is used twice");
        el.kind = string(e, "kind", "text", in);
        if (!known(screen_kinds(), el.kind))
            throw ScreenError(in + ": no kind '" + el.kind +
                              "' (panel, text, button, slider, check, option or image)");
        el.parent = string(e, "parent", "", in);
        if (!el.parent.empty() && !panels.contains(el.parent))
            throw ScreenError(in + ": its parent '" + el.parent + "' isn't a panel before it");
        el.anchor = string(e, "anchor", "top_left", in);
        if (!known(screen_anchors(), el.anchor))
            throw ScreenError(in + ": no anchor '" + el.anchor + "'");
        el.x = number(e, "x", 0.0, in);
        el.y = number(e, "y", 0.0, in);
        el.w = std::max(0.0, number(e, "w", 100.0, in));
        el.h = std::max(0.0, number(e, "h", 40.0, in));
        el.text = string(e, "text", "", in);
        el.image = string(e, "image", "", in);
        if (!el.image.empty() && !el.image.starts_with("ui:"))
            throw ScreenError(in + ": its image '" + el.image +
                              "' isn't one from the Asset Library's GUI section");
        if (e.contains("images")) {
            const json& im = e.at("images");
            if (!im.is_object()) throw ScreenError(in + ": 'images' isn't a set of pictures");
            const auto& states = screen_image_states(el.kind);
            for (const auto& [state, v] : im.items()) {
                if (!known(states, state))
                    throw ScreenError(in + ": a " + el.kind + " has no '" + state + "' picture");
                if (!v.is_string() || !v.get<std::string>().starts_with("ui:"))
                    throw ScreenError(in + ": its '" + state +
                                      "' picture isn't one from the Asset Library's GUI section");
                el.images[state] = v.get<std::string>();
            }
        }
        el.enabled = flag(e, "enabled", true, in);
        el.binds = string(e, "binds", "", in);
        if (!el.binds.empty()) {
            const bool volume = el.binds.starts_with("volume:") && el.kind == "slider" &&
                                known(screen_buses(), el.binds.substr(7));
            const bool key = el.kind == "button" && rebinds(el.binds);
            if (!volume && !key)
                throw ScreenError(in + ": it can't be bound to '" + el.binds +
                                  "' (a slider to volume:<bus>, a button to key:<action> or "
                                  "pad:<action>, an axis's way to key:<action>:+ or :-)");
        }
        el.group = string(e, "group", "", in);
        if (e.contains("style")) {
            const json& st = e.at("style");
            if (!st.is_object()) throw ScreenError(in + ": 'style' isn't a set of settings");
            if (st.contains("font_size")) el.style.font_size = number(st, "font_size", 18.0, in);
            el.style.color = rgba(st, "color", in);
            el.style.background = rgba(st, "background", in);
            el.style.border = rgba(st, "border", in);
            if (st.contains("border_width"))
                el.style.border_width = number(st, "border_width", 0.0, in);
            if (st.contains("radius")) el.style.radius = number(st, "radius", 0.0, in);
            el.style.align = string(st, "align", "", in);
            if (!el.style.align.empty() && el.style.align != "left" && el.style.align != "center" &&
                el.style.align != "right")
                throw ScreenError(in + ": no alignment '" + el.style.align + "'");
            el.style.bold = flag(st, "bold", false, in);
            el.style.italic = flag(st, "italic", false, in);
            el.style.font = string(st, "font", "", in);
            if (!el.style.font.empty() && el.style.font != "Inconsolata" &&
                el.style.font != "OpenDyslexic" && !el.style.font.starts_with("ui:"))
                throw ScreenError(in + ": no font '" + el.style.font +
                                  "' (Inconsolata, OpenDyslexic, or one from the Asset "
                                  "Library's Fonts)");
        }
        if (e.contains("action")) {
            const json& a = e.at("action");
            if (!a.is_object()) throw ScreenError(in + ": 'action' isn't a set of settings");
            el.action = string(a, "do", "none", in);
            if (!known(screen_actions(), el.action))
                throw ScreenError(in + ": no action '" + el.action +
                                  "' (none, close, open, quit, respawn or event)");
            el.target = string(a, "screen", "", in);
            if (el.action == "open" && el.target.empty())
                throw ScreenError(in + ": it opens a screen but doesn't say which");
        }
        el.min = number(e, "min", 0.0, in);
        el.max = number(e, "max", 1.0, in);
        el.step = std::max(0.0, number(e, "step", 0.0, in));
        if (el.kind == "check" || el.kind == "option")
            el.value = flag(e, "value", false, in) ? 1.0 : 0.0;
        else
            el.value = number(e, "value", el.min, in);
        if (el.kind == "slider" && el.max <= el.min)
            throw ScreenError(in + ": its most isn't above its least");
        if (el.kind == "panel") panels.insert(el.id);
        s.elements.push_back(std::move(el));
    }
    return s;
}

std::vector<std::string> screen_images(const ScreenDef& screen) {
    std::vector<std::string> out;
    const auto add = [&](const std::string& image) {
        if (!image.empty() && std::find(out.begin(), out.end(), image) == out.end())
            out.push_back(image);
    };
    add(screen.image);
    for (const ScreenElement& e : screen.elements) {
        add(e.image);
        for (const auto& [state, image] : e.images) add(image);
    }
    return out;
}

std::vector<std::string> screen_fonts(const ScreenDef& screen) {
    std::vector<std::string> out;
    for (const ScreenElement& e : screen.elements)
        if (e.style.font.starts_with("ui:") &&
            std::find(out.begin(), out.end(), e.style.font) == out.end())
            out.push_back(e.style.font);
    return out;
}

std::string screen_font_family(const std::string& font) {
    std::string out = "font_";  // (letters, digits and underscores: an RCSS name)
    for (const char c : font) out += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
    return out;
}

std::string screen_image_name(const std::string& content) {
    // (letters, digits and a few marks: it goes in a URL)
    std::string out = "screen_";
    for (const char c : content)
        out += (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-') ? c : '_';
    return out;
}

std::string rml_escape(const std::string& text) {
    std::string out;
    for (const char c : text) {
        switch (c) {
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '&':
                out += "&amp;";
                break;
            case '"':
                out += "&quot;";
                break;
            default:
                out += c;
        }
    }
    return out;
}

namespace {

// Where an element sits on its parent: left/top as a share of the parent, then a margin.
std::string placement(const ScreenElement& e) {
    if (e.anchor == "fill")
        return "left: " + dp(e.x) + "; top: " + dp(e.y) + "; right: " + dp(e.x) +
               "; bottom: " + dp(e.y) + ";";
    const std::string& a = e.anchor;
    const double across = a.ends_with("left") ? 0.0 : a.ends_with("right") ? 1.0 : 0.5;
    const double down = a.starts_with("top") ? 0.0 : a.starts_with("bottom") ? 1.0 : 0.5;
    return "left: " + num(across * 100.0) + "%; top: " + num(down * 100.0) +
           "%; margin-left: " + dp(e.x - across * e.w) + "; margin-top: " + dp(e.y - down * e.h) +
           "; width: " + dp(e.w) + "; height: " + dp(e.h) + ";";
}

std::string look(const ScreenElement& e) {
    const ScreenStyle& s = e.style;
    std::string out;
    if (!e.image.empty() && e.kind != "button")  // (a button's: in the screen's rules)
        out += " decorator: image(midgard://image/" + screen_image_name(e.image) + " fill);";
    if (s.font_size) out += " font-size: " + dp(*s.font_size) + ";";
    if (s.color) out += " color: " + colour(*s.color) + ";";
    if (s.background) out += " background-color: " + colour(*s.background) + ";";
    if (s.border || s.border_width)
        out += " border: " + dp(s.border_width.value_or(2.0)) + " " +
               colour(s.border.value_or(Rgba{0.27, 0.33, 0.43, 1.0})) + ";";
    if (s.radius) out += " border-radius: " + dp(*s.radius) + ";";
    if (!s.align.empty()) out += " text-align: " + s.align + ";";
    if (s.bold) out += " font-weight: bold;";
    if (s.italic) out += " font-style: italic;";
    if (!s.font.empty())
        out +=
            " font-family: " + (s.font.starts_with("ui:") ? screen_font_family(s.font) : s.font) +
            ";";
    return out;
}

void write(const ScreenDef& screen, const std::string& parent, std::string& out,
           const std::string& indent) {
    for (const ScreenElement& e : screen.elements) {
        if (e.parent != parent) continue;
        const std::string id = "el-" + rml_escape(e.id);
        const std::string style =
            "position: absolute; box-sizing: border-box; " + placement(e) + look(e);
        if (e.kind == "panel") {
            out += indent + "<div id=\"" + id + "\" class=\"panel\" style=\"" + style + "\">\n";
            write(screen, e.id, out, indent + "    ");
            out += indent + "</div>\n";
        } else if (e.kind == "image") {
            out +=
                indent + "<div id=\"" + id + "\" class=\"image\" style=\"" + style + "\"></div>\n";
        } else if (e.kind == "text") {
            out += indent + "<div id=\"" + id + "\" class=\"text\" style=\"" + style + "\">" +
                   rml_escape(e.text) + "</div>\n";
        } else if (e.kind == "button") {
            // (the text in the button's middle: a flex row, centred)
            out += indent + "<button id=\"" + id + "\"" +
                   (e.enabled ? "" : " class=\"disabled\" disabled=\"\"") + " style=\"" + style +
                   " display: flex; align-items: center; justify-content: center;"
                   " padding: 0dp 8dp;\"><span>" +
                   rml_escape(e.text) + "</span></button>\n";
        } else if (e.kind == "slider") {
            out += indent + "<input type=\"range\" id=\"" + id + "\"" +
                   (e.enabled ? "" : " disabled=\"\"") + " min=\"" + num(e.min) + "\" max=\"" +
                   num(e.max) + "\" step=\"" +
                   num(e.step > 0.0 ? e.step : (e.max - e.min) / 100.0) + "\" value=\"" +
                   num(std::clamp(e.value, e.min, e.max)) + "\" style=\"" + style + "\"/>\n";
        } else if (e.kind == "check" || e.kind == "option") {
            // (the box is "el-<id>-box" inside it: the element's own id is the whole row's; an
            // option's box is a radio button, one of its group's)
            const std::string box = e.kind == "check"
                                        ? std::string("checkbox\"")
                                        : "radio\" name=\"" +
                                              rml_escape(e.group.empty() ? "options" : e.group) +
                                              "\" value=\"" + rml_escape(e.id) + "\"";
            out += indent + "<div id=\"" + id + "\" class=\"" + e.kind + "\" style=\"" + style +
                   " display: flex; align-items: center;\">"
                   "<input type=\"" +
                   box + " id=\"" + id + "-box\"" + (e.value != 0.0 ? " checked=\"\"" : "") +
                   (e.enabled ? "" : " disabled=\"\"") +
                   " style=\"flex: 0 0 auto;\"/><span style=\"margin-left: 10dp;\">" +
                   rml_escape(e.text) + "</span></div>\n";
        }
    }
}

// The pictures that change with an element's state (a button hovered or pressed, a box
// ticked), and the screen's own picture: style rules, by the elements' ids.
std::string rules(const ScreenDef& screen) {
    const auto picture = [](const std::string& image) {
        return "decorator: image(midgard://image/" + screen_image_name(image) + " fill);";
    };
    const auto first = [](std::initializer_list<std::string> images) {
        for (const std::string& i : images)
            if (!i.empty()) return i;
        return std::string();
    };
    std::string out;
    if (!screen.image.empty()) out += "        body { " + picture(screen.image) + " }\n";
    for (const ScreenElement& e : screen.elements) {
        const std::string id = "#el-" + e.id;
        const auto state = [&](const char* s) {
            const auto it = e.images.find(s);
            return it == e.images.end() ? std::string() : it->second;
        };
        if (e.kind == "button") {
            // (each picture with no fill or border of its own, unless its style gives them
            // back: inline, first)
            const std::string bare = "background-color: #00000000; border-width: 0dp; ";
            if (!e.image.empty()) out += "        " + id + " { " + bare + picture(e.image) + " }\n";
            const std::string hover = first({state("hover"), e.image});
            if (!hover.empty())
                out += "        " + id + ":hover { " + bare + picture(hover) + " }\n";
            const std::string pressed = first({state("pressed"), state("hover"), e.image});
            if (!pressed.empty())
                out += "        " + id + ":active { " + bare + picture(pressed) + " }\n";
            const std::string off = first({state("disabled"), e.image});
            // (RmlUi's buttons don't take :disabled: the class says so, as the attribute)
            if (!off.empty())
                out += "        " + id + ".disabled { " + bare + picture(off) + " }\n";
        }
        if ((e.kind == "check" || e.kind == "option") && !e.images.empty()) {
            const std::string off = first({state("unchecked"), state("checked")});
            const std::string on = first({state("checked"), state("unchecked")});
            out += "        " + id + "-box { background-color: #00000000; border-width: 0dp; " +
                   picture(off) + " }\n";
            out += "        " + id + "-box:checked { background-color: #00000000; " + picture(on) +
                   " }\n";
        }
        if (e.kind == "slider") {
            if (!state("track").empty())
                out += "        " + id +
                       " slidertrack { margin-top: 0dp; height: 100%; border-radius: 0dp;"
                       " background-color: #00000000; " +
                       picture(state("track")) + " }\n";
            if (!state("pointer").empty())
                out += "        " + id +
                       " sliderbar { background-color: #00000000; border-width: 0dp; " +
                       picture(state("pointer")) + " }\n";
        }
    }
    return out;
}

}  // namespace

std::string screen_rml(const ScreenDef& screen) {
    std::string out =
        "<rml>\n<head>\n    <title>" + rml_escape(screen.name) +
        "</title>\n"
        "    <link type=\"text/rcss\" href=\"midgard://base.rcss\"/>\n"
        "    <style>\n"
        "        body { box-sizing: border-box;" +
        (screen.background ? " background-color: " + colour(*screen.background) + ";" : "") +
        " }\n"
        "        #frame { position: relative; width: 100%; height: 100%; }\n"
        "        .panel { background-color: #1c2230f0; border: 1dp #3a4660;"
        " border-radius: 8dp; }\n"
        "        .text { white-space: pre-wrap; }\n"
        "        button.disabled { focus: none; tab-index: none; opacity: 0.6; }\n" +
        rules(screen) + "    </style>\n</head>\n<body>\n    <form id=\"frame\">\n";
    write(screen, "", out, "        ");
    out += "    </form>\n</body>\n</rml>\n";
    return out;
}

}  // namespace midgard
