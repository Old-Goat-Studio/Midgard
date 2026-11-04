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

// glslang's log ("ERROR: shader:12: 'x' : undeclared identifier ...") as messages, each line
// moved by first_line (exposed for tests).
std::vector<Message> parse_log(std::string_view log, const Options& options);

}  // namespace midgard::shader
