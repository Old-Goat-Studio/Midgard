// SPDX-License-Identifier: Apache-2.0
#include "midgard/shader_compiler.h"

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>

#include <algorithm>
#include <cctype>
#include <mutex>
#include <optional>
#include <regex>
#include <stdexcept>

#include "surface_template.h"

namespace midgard::shader {
namespace {

// glslang must be set up once per process, before any compile (and is thread safe after).
void initialise() {
    static std::once_flag once;
    std::call_once(once, [] { glslang::InitializeProcess(); });
}

EShLanguage language(Stage stage) {
    return stage == Stage::Vertex ? EShLangVertex : EShLangFragment;
}

}  // namespace

std::vector<Message> parse_log(std::string_view log, const Options& options) {
    // "ERROR: <name>:<line>: <text>", "WARNING: ...", or a line glslang gives no place for
    // ("ERROR: 1 compilation errors.  No code generated." is a summary: left out).
    static const std::regex placed(R"(^(ERROR|WARNING):\s*(?:[^:]*:)?(\d+):\s*(.*)$)");
    static const std::regex unplaced(R"(^(ERROR|WARNING):\s*(.*)$)");
    std::vector<Message> out;
    size_t start = 0;
    while (start < log.size()) {
        size_t end = log.find('\n', start);
        if (end == std::string_view::npos) end = log.size();
        std::string line(log.substr(start, end - start));
        start = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::smatch m;
        // ("'' : compilation terminated" only follows another mistake: left out too.)
        if (line.find("compilation terminated") != std::string::npos) continue;
        if (std::regex_match(line, m, placed)) {
            Message msg;
            msg.error = m[1] == "ERROR";
            msg.line = std::stoi(m[2].str()) - (options.first_line - 1);
            if (msg.line < 1) msg.line = 0;  // (the template's own line)
            msg.text = m[3].str();
            out.push_back(std::move(msg));
        } else if (std::regex_match(line, m, unplaced)) {
            const std::string text = m[2].str();
            if (text.find("compilation errors") != std::string::npos ||
                text.find("No code generated") != std::string::npos)
                continue;
            out.push_back({0, m[1] == "ERROR", text});
        }
    }
    return out;
}

Compiled compile_glsl(std::string_view source, Stage stage, const Options& options) {
    initialise();
    const EShLanguage lang = language(stage);
    glslang::TShader shader(lang);
    const std::string text(source);
    const char* texts[] = {text.c_str()};
    const char* names[] = {options.name.c_str()};
    shader.setStringsWithLengthsAndNames(texts, nullptr, names, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
    const auto messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
    Compiled out;
    if (!shader.parse(GetDefaultResources(), 450, false, messages)) {
        out.messages = parse_log(shader.getInfoLog(), options);
        if (out.messages.empty())
            out.messages.push_back({0, true, "the shader didn't compile (no reason given)"});
        return out;
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages)) {
        out.messages = parse_log(program.getInfoLog(), options);
        if (out.messages.empty())
            out.messages.push_back({0, true, "the shader didn't link (no reason given)"});
        return out;
    }
    out.messages = parse_log(shader.getInfoLog(), options);  // (its warnings)
    glslang::SpvOptions spv;
    spv.generateDebugInfo = false;
    spv.disableOptimizer = true;
    glslang::GlslangToSpv(*program.getIntermediate(lang), out.spirv, &spv);
    return out;
}

namespace {

std::string trim(std::string text) {
    const auto space = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!text.empty() && space(text.back())) text.pop_back();
    size_t start = 0;
    while (start < text.size() && space(text[start])) ++start;
    return text.substr(start);
}

// How many numbers a setting of a type holds (0: a texture; -1: no such type).
int numbers_of(const std::string& type) {
    if (type == "float" || type == "int" || type == "bool") return 1;
    if (type == "vec2") return 2;
    if (type == "vec3" || type == "color") return 3;
    if (type == "vec4") return 4;
    if (type == "texture" || type == "data_texture") return 0;
    return -1;
}

// Names a setting can't have: what the code reads and changes, Midgard's own names, GLSL's.
bool reserved(const std::string& name) {
    static const char* const names[] = {
        "i",           "s",         "surface",  "color",    "alpha",
        "metallic",    "roughness", "normal",   "emission", "occlusion",
        "cutout",      "uv",        "position", "view",     "geometry_normal",
        "up",          "time",      "pc",       "frame",    "main",
        "hash",        "noise",     "fbm",      "srgb",     "sun",
        "bend_normal", "texture",   "float",    "int",      "bool",
        "vec2",        "vec3",      "vec4",     "mat4",     "true",
        "false",       "if",        "else",     "for",      "while",
        "return",      "discard",   "in",       "out",      "inout",
        "uniform",     "const",     "struct",   "void",     "out_color"};
    for (const char* n : names)
        if (name == n) return true;
    return name.starts_with("midgard_") || name.starts_with("gl_") || name.ends_with("_map");
}

std::string label_from(const std::string& name) {
    std::string out;
    for (char c : name) out += c == '_' ? ' ' : c;
    if (!out.empty()) out[0] = char(std::toupper(static_cast<unsigned char>(out[0])));
    return out;
}

// "vec3(1.0, 0.4, 0.1)", "(1, 0, 0)", "0.5", "true": its numbers, or a reason it can't be.
std::optional<std::vector<float>> numbers_in(std::string text, std::string& why) {
    text = trim(text);
    if (text == "true") return std::vector<float>{1.0f};
    if (text == "false") return std::vector<float>{0.0f};
    static const std::regex wrapped(R"(^(?:[A-Za-z_]\w*)?\s*\((.*)\)$)");
    std::smatch m;
    if (std::regex_match(text, m, wrapped)) text = m[1].str();
    std::vector<float> out;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(',', start);
        if (end == std::string::npos) end = text.size();
        std::string item = trim(text.substr(start, end - start));
        if (!item.empty() && (item.back() == 'f' || item.back() == 'F')) item.pop_back();
        try {
            size_t used = 0;
            out.push_back(std::stof(item, &used));
            if (used != item.size()) throw std::invalid_argument(item);
        } catch (const std::exception&) {
            why = "'" + item + "' isn't a number";
            return std::nullopt;
        }
        start = end + 1;
    }
    return out;
}

}  // namespace

SurfaceSource surface_shader_source(std::string_view code) {
    static const std::string base = [] {
        std::string text = "#version 450\n";
        for (const char* piece : kSurfaceTemplate) text += piece;
        return text;
    }();
    SurfaceSource out;
    // The settings: read from their lines, which are left blank (so lines keep their numbers).
    static const std::regex declared(
        R"(^\s*setting\s+([A-Za-z_]\w*)\s+([A-Za-z_]\w*)\s*(?:=\s*([^;]*?))?\s*;\s*(?://\s*(.*))?$)");
    static const std::regex range(
        R"((-?\d+(?:\.\d*)?|-?\.\d+)\s*\.\.\s*(-?\d+(?:\.\d*)?|-?\.\d+))");
    std::string body;
    int slots = 0, maps = 0, line_no = 0;
    size_t start = 0;
    while (start < code.size()) {
        size_t end = code.find('\n', start);
        const bool last = end == std::string_view::npos;
        if (last) end = code.size();
        std::string line(code.substr(start, end - start));
        start = end + 1;
        ++line_no;
        const std::string stripped = trim(line);
        if (!stripped.starts_with("setting ") && !stripped.starts_with("setting\t")) {
            body += line + (last ? "" : "\n");
            continue;
        }
        body += last ? "" : "\n";
        const auto problem = [&](const std::string& text) {
            out.problems.push_back({line_no, true, text});
        };
        std::smatch m;
        if (!std::regex_match(stripped, m, declared)) {
            problem(
                "a setting is written 'setting <type> <name> = <value>;' (e.g. 'setting "
                "float speed = 1.0;')");
            continue;
        }
        Setting st;
        st.type = m[1].str();
        st.name = m[2].str();
        st.line = line_no;
        const int numbers = numbers_of(st.type);
        if (numbers < 0) {
            problem("'" + st.type +
                    "' isn't a type of setting (float, int, bool, vec2, vec3, "
                    "vec4, color, texture, data_texture)");
            continue;
        }
        if (reserved(st.name)) {
            problem("a setting can't be called '" + st.name + "' (the shader uses that name)");
            continue;
        }
        bool taken = false;
        for (const Setting& other : out.settings) taken = taken || other.name == st.name;
        if (taken) {
            problem("there's already a setting called '" + st.name + "'");
            continue;
        }
        if (numbers == 0) {
            if (m[3].matched && !trim(m[3].str()).empty()) {
                problem("a texture setting has no value in the code (each material picks one)");
                continue;
            }
            if (maps >= kMaxMaps) {
                problem("a shader can have at most " + std::to_string(kMaxMaps) +
                        " texture settings");
                continue;
            }
            st.slot = maps++;
        } else {
            if (slots >= kMaxSettings) {
                problem("a shader can have at most " + std::to_string(kMaxSettings) +
                        " number settings");
                continue;
            }
            std::vector<float> value(size_t(numbers), st.type == "color" ? 1.0f : 0.0f);
            if (m[3].matched && !trim(m[3].str()).empty()) {
                std::string why;
                const auto given = numbers_in(m[3].str(), why);
                if (!given) {
                    problem("setting '" + st.name + "': " + why);
                    continue;
                }
                if (given->size() == 1) {
                    std::fill(value.begin(), value.end(), (*given)[0]);
                } else if (int(given->size()) == numbers) {
                    value = *given;
                } else {
                    problem("setting '" + st.name + "': a " + st.type + " has " +
                            std::to_string(numbers) + " numbers");
                    continue;
                }
            }
            st.value = value;
            st.slot = slots++;
            if (st.type == "int") st.max = 10.0f;
        }
        // Its comment: a range ("0 .. 10") and a label.
        std::string comment = m[4].matched ? m[4].str() : "";
        std::smatch r;
        if (std::regex_search(comment, r, range)) {
            st.min = std::stof(r[1].str());
            st.max = std::stof(r[2].str());
            if (st.max < st.min) std::swap(st.min, st.max);
            comment = r.prefix().str() + r.suffix().str();
        }
        for (float v : st.value) {
            if (st.type == "bool") break;
            st.min = std::min(st.min, v);
            st.max = std::max(st.max, v);
        }
        comment = trim(comment);
        while (!comment.empty() && std::string(",;:-").find(comment.back()) != std::string::npos)
            comment = trim(comment.substr(0, comment.size() - 1));
        st.label = comment.empty() ? label_from(st.name) : comment;
        out.settings.push_back(std::move(st));
    }

    std::string prefix = base;
    prefix += "\n// ---- the shader's settings (each material's own) ----\n";
    prefix +=
        "layout(std430, set = 0, binding = 2) readonly buffer MidgardSettings {\n"
        "    vec4 midgard_values[];\n"
        "};\n"
        "vec4 midgard_setting(int k) { return midgard_values[uint(pc.extra.x) + uint(k)]; }\n";
    for (int k = 0; k < kMaxMaps; ++k)
        prefix += "layout(set = 1, binding = " + std::to_string(5 + k) +
                  ") uniform sampler2D midgard_map_" + std::to_string(k) + ";\n";
    for (const Setting& st : out.settings) {
        const std::string at = "midgard_setting(" + std::to_string(st.slot) + ")";
        std::string value;
        if (st.texture())
            value = "midgard_map_" + std::to_string(st.slot);
        else if (st.type == "float")
            value = "(" + at + ".x)";
        else if (st.type == "int")
            value = "int(round(" + at + ".x))";
        else if (st.type == "bool")
            value = "(" + at + ".x > 0.5)";
        else if (st.type == "vec2")
            value = "(" + at + ".xy)";
        else if (st.type == "vec3")
            value = "(" + at + ".xyz)";
        else if (st.type == "color")
            value = "srgb(" + at + ".xyz)";  // (picked: made linear)
        else
            value = at;
        prefix += "#define " + st.name + " " + value + "\n";
    }
    prefix += "// ---- the surface shader ----\n";

    out.first_line = int(std::count(prefix.begin(), prefix.end(), '\n')) + 1;
    out.lines = line_no;
    out.source = prefix + body;
    if (!body.empty() && body.back() != '\n') out.source += '\n';
    out.source +=
        "// ---- Midgard's part ----\n"
        "void main() {\n"
        "    const SurfaceInput i = surface_input();\n"
        "    Surface s = material_surface(i);\n"
        "    surface(i, s);\n"
        "    if (s.alpha < s.cutout) discard;\n"
        "    out_color = light_surface(i, s);\n"
        "}\n";
    return out;
}

CompiledSurface compile_surface(std::string_view code, std::string_view name) {
    const SurfaceSource source = surface_shader_source(code);
    CompiledSurface out;
    out.settings = source.settings;
    if (!source.problems.empty()) {  // (its settings first: the code reads them)
        out.messages = source.problems;
        return out;
    }
    Options options;
    options.name = std::string(name);
    options.first_line = source.first_line;
    Compiled compiled = compile_glsl(source.source, Stage::Fragment, options);
    out.spirv = std::move(compiled.spirv);
    out.messages = std::move(compiled.messages);
    for (Message& m : out.messages) {
        if (m.line <= source.lines) continue;
        m.line = 0;  // (in Midgard's part, after the code)
        if (m.text.find("surface") != std::string::npos ||
            m.text.find("no matching overloaded function") != std::string::npos)
            m.text =
                "the shader needs a function 'void surface(SurfaceInput i, inout Surface s)'"
                " (" +
                m.text + ")";
    }
    return out;
}

}  // namespace midgard::shader
