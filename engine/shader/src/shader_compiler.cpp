// SPDX-License-Identifier: Apache-2.0
#include "midgard/shader_compiler.h"

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>

#include <algorithm>
#include <mutex>
#include <regex>

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

SurfaceSource surface_shader_source(std::string_view code) {
    static const std::string prefix = [] {
        std::string text = "#version 450\n";
        for (const char* piece : kSurfaceTemplate) text += piece;
        text += "\n// ---- the surface shader ----\n";
        return text;
    }();
    SurfaceSource out;
    out.first_line = int(std::count(prefix.begin(), prefix.end(), '\n')) + 1;
    out.lines = int(std::count(code.begin(), code.end(), '\n')) +
                (code.empty() || code.back() == '\n' ? 0 : 1);
    out.source = prefix + std::string(code);
    if (!code.empty() && code.back() != '\n') out.source += '\n';
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

Compiled compile_surface(std::string_view code, std::string_view name) {
    const SurfaceSource source = surface_shader_source(code);
    Options options;
    options.name = std::string(name);
    options.first_line = source.first_line;
    Compiled out = compile_glsl(source.source, Stage::Fragment, options);
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
