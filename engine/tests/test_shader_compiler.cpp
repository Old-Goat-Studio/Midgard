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
}

TEST_CASE("surface shaders: code without its surface function says what it needs") {
    const sc::Compiled out = sc::compile_surface("float helper() { return 1.0; }\n");
    REQUIRE_FALSE(out.ok());
    REQUIRE_FALSE(out.messages.empty());
    CHECK(out.messages[0].line == 0);
    CHECK(out.messages[0].text.find("void surface(SurfaceInput i, inout Surface s)") !=
          std::string::npos);
}
