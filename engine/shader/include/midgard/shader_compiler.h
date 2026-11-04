// SPDX-License-Identifier: Apache-2.0
// The shader compiler (milestone 5.17): GLSL to SPIR-V inside Midgard, through glslang, with
// its mistakes reported line by line, for the editor (the Shader editor, the cook). Games carry
// shaders already compiled (SPIR-V), so the runtime never needs this; it is built only where the
// editor runs (Windows and Linux).
//
// Shaders are compiled for Vulkan 1.1, as Midgard's own are (glslangValidator -V --target-env
// vulkan1.1). A shader made by wrapping someone's code in a template (5.17's surface shaders)
// reports its lines as theirs: `first_line` is the template line their code starts on, and
// messages on lines before it are the template's own.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace midgard::shader {

enum class Stage { Vertex, Fragment };

struct Message {
    int line = 0;  // in the source as written (1 = its first line); 0: not tied to a line
    bool error = true;
    std::string text;  // "'colr' : undeclared identifier"
};

struct Compiled {
    std::vector<uint32_t> spirv;  // empty when it didn't compile
    std::vector<Message> messages;
    bool ok() const { return !spirv.empty(); }
};

struct Options {
    std::string name = "shader";  // how glslang names the source in its log
    int first_line = 1;           // the line the user's code starts on (see above)
};

// The source compiled to SPIR-V, or its mistakes. Thread safe.
Compiled compile_glsl(std::string_view source, Stage stage, const Options& options = {});

// ---- surface shaders (5.17) ------------------------------------------------------------------
// Someone's own code for a mesh's surface: a function
//
//     void surface(SurfaceInput i, inout Surface s) { ... }
//
// that changes the surface (its colour, alpha and cutout, metallic, roughness, normal, emission,
// occlusion; each starts as the material's own) from what it reads (uv, position, view,
// geometry_normal, up, time), with the material's maps (base_color_map ...) and helpers (hash,
// noise, fbm, srgb) to hand: render/shaders/surface.glsl. Midgard lights it as its own
// materials (sun and shadows, lamps, ambient light, snow and wetness, fog).
//
// Settings (5.17 step 3): lines of the code such as
//
//     setting float speed = 2.0;              // How fast it moves, 0 .. 10
//     setting color tint = vec3(1.0, 0.4, 0.1);
//     setting bool sparkle = true;
//     setting texture pattern;                // a picture (sRGB)
//
// are values each material using the shader chooses for itself (the Material editor shows
// them), read in the code by name. Types: float, int, bool, vec2, vec3, vec4, color (a vec3
// picked as a colour, sRGB as colours are picked; the code reads it made linear, as surfaces
// use colours), texture (a picture) and data_texture (numbers: masks, noise, normals).
// The comment after one, if any, is its label; a "min .. max" in it, its range. Each number
// setting takes one of kMaxSettings vec4 slots (in order), each texture one of kMaxMaps.
inline constexpr int kMaxSettings = 16;
inline constexpr int kMaxMaps = 4;

struct Setting {
    std::string name;   // as the code reads it
    std::string type;   // "float", "int", "bool", "vec2", "vec3", "vec4", "color", "texture",
                        // "data_texture"
    std::string label;  // from its comment, or made from its name ("light_speed": "Light speed")
    std::vector<float> value;      // its default (1 to 4 numbers; none for a texture)
    float min = 0.0f, max = 1.0f;  // the range of a number (from its comment, or 0 .. 1)
    int slot = 0;                  // its vec4 slot, or (a texture) its map
    int line = 0;                  // in the code
    bool texture() const { return type == "texture" || type == "data_texture"; }
};

struct SurfaceSource {
    std::string source;  // the whole fragment shader
    int first_line = 1;  // the line the code starts on in it
    int lines = 0;       // how many lines the code has
    std::vector<Setting> settings;
    std::vector<Message> problems;  // settings that can't be (by their lines)
};
SurfaceSource surface_shader_source(std::string_view code);

// A surface shader compiled: the code wrapped as above, its mistakes on its own lines (a mistake
// in Midgard's part, after the code, has line 0).
struct CompiledSurface : Compiled {
    std::vector<Setting> settings;
};
CompiledSurface compile_surface(std::string_view code, std::string_view name = "surface");

// glslang's log ("ERROR: shader:12: 'x' : undeclared identifier ...") as messages, each line
// moved by first_line (exposed for tests).
std::vector<Message> parse_log(std::string_view log, const Options& options);

}  // namespace midgard::shader
