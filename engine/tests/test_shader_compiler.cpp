// SPDX-License-Identifier: Apache-2.0
// The shader compiler (milestone 5.17 step 1): GLSL to SPIR-V inside Midgard, mistakes by line.
#include <doctest/doctest.h>

#include <string>

#include "midgard/shader_compiler.h"

namespace sc = midgard::shader;

namespace {

constexpr uint32_t kSpirvMagic = 0x07230203u;

const char* kFragment = R"(#version 450
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 colour;
layout(set = 0, binding = 0) uniform Settings { vec4 tint; float time; } settings;
void main() {
    float wave = 0.5 + 0.5 * sin(uv.x * 10.0 + settings.time);
    colour = vec4(settings.tint.rgb * wave, 1.0);
}
)";

const char* kVertex = R"(#version 450
layout(location = 0) in vec3 position;
layout(push_constant) uniform Push { mat4 mvp; } push;
void main() { gl_Position = push.mvp * vec4(position, 1.0); }
)";

}  // namespace

TEST_CASE("shader compiler: a fragment and a vertex shader become SPIR-V") {
    const sc::Compiled frag = sc::compile_glsl(kFragment, sc::Stage::Fragment);
    REQUIRE(frag.ok());
    CHECK(frag.spirv[0] == kSpirvMagic);
    CHECK(frag.messages.empty());
    const sc::Compiled vert = sc::compile_glsl(kVertex, sc::Stage::Vertex);
    REQUIRE(vert.ok());
    CHECK(vert.spirv[0] == kSpirvMagic);
    // The same source, the same SPIR-V (the cook's output must not change by itself).
    CHECK(sc::compile_glsl(kFragment, sc::Stage::Fragment).spirv == frag.spirv);
}

TEST_CASE("shader compiler: a mistake is reported on its line") {
    std::string broken = kFragment;
    broken.replace(broken.find("settings.tint.rgb"), 17, "settings.tnit.rgb");  // (line 7)
    const sc::Compiled out = sc::compile_glsl(broken, sc::Stage::Fragment);
    CHECK_FALSE(out.ok());
    REQUIRE_FALSE(out.messages.empty());
    CHECK(out.messages[0].error);
    CHECK(out.messages[0].line == 7);
    CHECK(out.messages[0].text.find("tnit") != std::string::npos);
}

TEST_CASE("shader compiler: every mistake is reported, each on its own line") {
    const char* source = R"(#version 450
layout(location = 0) out vec4 colour;
void main() {
    float a = undefined_one;
    float b = 1.0
    colour = vec4(a, b, 0.0, 1.0);
}
)";
    const sc::Compiled out = sc::compile_glsl(source, sc::Stage::Fragment);
    CHECK_FALSE(out.ok());
    REQUIRE(out.messages.size() >= 1);
    CHECK(out.messages[0].line == 4);
    CHECK(out.messages[0].text.find("undefined_one") != std::string::npos);
}

TEST_CASE("shader compiler: lines in a template are counted from the user's first line") {
    // Six template lines, then the user's code: their mistake on their own line 2.
    std::string source =
        "#version 450\nlayout(location = 0) out vec4 colour;\n"
        "// template\n// template\n// template\nvoid main() {\n";
    source += "    colour = vec4(1.0);\n    colour.w = oops;\n}\n";
    sc::Options options;
    options.name = "glow";
    options.first_line = 7;
    const sc::Compiled out = sc::compile_glsl(source, sc::Stage::Fragment, options);
    REQUIRE_FALSE(out.ok());
    CHECK(out.messages[0].line == 2);
}

TEST_CASE("shader compiler: glslang's log is read as messages") {
    sc::Options options;
    options.first_line = 11;
    const auto messages = sc::parse_log(
        "WARNING: s:12: 'x' : unused\nERROR: s:15: 'y' : undeclared identifier\n"
        "ERROR: s:3: template trouble\nERROR: 1 compilation errors.  No code generated.\n"
        "ERROR: Linking fragment stage: Missing entry point\n",
        options);
    REQUIRE(messages.size() == 4);
    CHECK_FALSE(messages[0].error);
    CHECK(messages[0].line == 2);
    CHECK(messages[1].line == 5);
    CHECK(messages[1].text == "'y' : undeclared identifier");
    CHECK(messages[2].line == 0);  // (before the user's code: the template's own line)
    CHECK(messages[3].line == 0);
    CHECK(messages[3].text == "Linking fragment stage: Missing entry point");
}

TEST_CASE("shader compiler: an empty or wrong-stage shader says why") {
    const sc::Compiled empty = sc::compile_glsl("", sc::Stage::Fragment);
    CHECK_FALSE(empty.ok());
    CHECK_FALSE(empty.messages.empty());
    // A vertex shader's built-in in a fragment shader.
    const sc::Compiled wrong = sc::compile_glsl(kVertex, sc::Stage::Fragment);
    CHECK_FALSE(wrong.ok());
    CHECK_FALSE(wrong.messages.empty());
}

// ---- surface shaders (5.17 step 2) --------------------------------------------------------------

TEST_CASE("surface shaders: someone's surface code becomes a whole fragment shader") {
    const char* stripes = R"(void surface(SurfaceInput i, inout Surface s) {
    float band = step(0.5, fract(i.uv.x * 8.0));
    s.color = mix(srgb(vec3(0.9, 0.2, 0.1)), s.color, band);
    s.roughness = 0.3;
}
)";
    const sc::Compiled out = sc::compile_surface(stripes, "stripes");
    REQUIRE(out.ok());
    CHECK(out.spirv[0] == kSpirvMagic);
    const sc::SurfaceSource source = sc::surface_shader_source(stripes);
    CHECK(source.lines == 5);
    CHECK(source.first_line > 100);  // (after Midgard's own surface code)
    CHECK(source.source.find("void main()") > source.source.find("void surface("));
}

TEST_CASE("surface shaders: the time, noise and cutouts are there to use") {
    const char* dissolve = R"(void surface(SurfaceInput i, inout Surface s) {
    float edge = fbm(i.uv * 6.0, 4) - fract(i.time * 0.1);
    s.alpha = edge;
    s.cutout = 0.0;
    s.emission = vec3(1.0, 0.4, 0.1) * smoothstep(0.08, 0.0, abs(edge)) * 4.0;
})";  // (no last newline: still whole)
    CHECK(sc::compile_surface(dissolve).ok());
}

TEST_CASE("surface shaders: a mistake is reported on the line of the surface code") {
    const char* broken = R"(void surface(SurfaceInput i, inout Surface s) {
    s.color = vec3(1.0);
    s.roughnes = 0.4;
}
)";
    const sc::Compiled out = sc::compile_surface(broken, "broken");
    REQUIRE_FALSE(out.ok());
    CHECK(out.messages[0].line == 3);
    CHECK(out.messages[0].text.find("roughnes") != std::string::npos);
    for (const sc::Message& m : out.messages)  // (glslang's follow-on line is left out)
        CHECK(m.text.find("compilation terminated") == std::string::npos);
}

TEST_CASE("surface shaders: code without its surface function says what it needs") {
    const sc::Compiled out = sc::compile_surface("float helper() { return 1.0; }\n");
    REQUIRE_FALSE(out.ok());
    REQUIRE_FALSE(out.messages.empty());
    CHECK(out.messages[0].line == 0);
    CHECK(out.messages[0].text.find("void surface(SurfaceInput i, inout Surface s)") !=
          std::string::npos);
}

// ---- shader settings (5.17 step 3)
// ---------------------------------------------------------------

TEST_CASE("shader settings: each declared setting is read, and the code compiles with it") {
    const char* glow = R"(setting float speed = 2.0;            // How fast it pulses, 0 .. 10
setting color tint = vec3(1.0, 0.4, 0.1);
setting bool sparkle = true;
setting int rings = 3;
setting vec2 scale = 2.0;
setting texture pattern;                 // The pattern
setting data_texture mask;
void surface(SurfaceInput i, inout Surface s) {
    float pulse = 0.5 + 0.5 * sin(i.time * speed);
    s.emission = tint * pulse * texture(pattern, i.uv * scale).rgb * texture(mask, i.uv).r;
    if (sparkle) s.roughness = float(rings) * 0.1;
    s.colr = vec3(1.0);
}
)";
    const sc::CompiledSurface out = sc::compile_surface(glow, "glow");
    REQUIRE_FALSE(out.ok());  // (the mistake on line 12 is the code's own)
    REQUIRE(out.messages.size() >= 1);
    CHECK(out.messages[0].line == 12);
    REQUIRE(out.settings.size() == 7);
    const sc::Setting& speed = out.settings[0];
    CHECK(speed.name == "speed");
    CHECK(speed.type == "float");
    CHECK(speed.label == "How fast it pulses");
    CHECK(speed.value == std::vector<float>{2.0f});
    CHECK(speed.min == 0.0f);
    CHECK(speed.max == 10.0f);
    CHECK(speed.slot == 0);
    CHECK(speed.line == 1);
    CHECK(out.settings[1].label == "Tint");
    CHECK(out.settings[1].value == std::vector<float>{1.0f, 0.4f, 0.1f});
    CHECK(out.settings[1].slot == 1);
    CHECK(out.settings[2].value == std::vector<float>{1.0f});
    CHECK(out.settings[3].max == 10.0f);                             // (an int's range)
    CHECK(out.settings[4].value == std::vector<float>{2.0f, 2.0f});  // (one number for both)
    CHECK(out.settings[4].max == 2.0f);  // (its range takes in its value)
    CHECK(out.settings[5].texture());
    CHECK(out.settings[5].slot == 0);
    CHECK(out.settings[5].label == "The pattern");
    CHECK(out.settings[6].slot == 1);

    std::string fixed = glow;
    fixed.replace(fixed.find("    s.colr = vec3(1.0);\n"), 24, "");
    CHECK(sc::compile_surface(fixed).ok());
}

TEST_CASE("shader settings: a setting that can't be is reported on its line") {
    const auto first = [](const char* code) {
        const sc::CompiledSurface out = sc::compile_surface(code);
        REQUIRE_FALSE(out.ok());
        REQUIRE_FALSE(out.messages.empty());
        return out.messages[0];
    };
    const char* tail = "void surface(SurfaceInput i, inout Surface s) {}\n";
    CHECK(first((std::string("setting float speed = fast;\n") + tail).c_str())
              .text.find("'fast' isn't a number") != std::string::npos);
    const sc::Message colour = first((std::string("\nsetting colour tint;\n") + tail).c_str());
    CHECK(colour.line == 2);
    CHECK(colour.text.find("isn't a type of setting") != std::string::npos);
    CHECK(first((std::string("setting float roughness = 1.0;\n") + tail).c_str())
              .text.find("can't be called 'roughness'") != std::string::npos);
    CHECK(first((std::string("setting float a;\nsetting vec2 a;\n") + tail).c_str()).line == 2);
    CHECK(first((std::string("setting vec3 v = vec3(1, 2);\n") + tail).c_str())
              .text.find("has 3 numbers") != std::string::npos);
    CHECK(first((std::string("setting float speed 2.0;\n") + tail).c_str())
              .text.find("a setting is written") != std::string::npos);
    std::string many;
    for (int k = 0; k < 5; ++k) many += "setting texture t" + std::to_string(k) + ";\n";
    CHECK(first((many + tail).c_str()).line == 5);
}
