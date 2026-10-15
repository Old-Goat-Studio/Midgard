// SPDX-License-Identifier: Apache-2.0
// Game scripts: the CPython that every game embeds (roadmap §1 "Python in games"). The
// interpreter is the one tools/setup_python.py puts beside the runtime (python313.dll and the
// python/ folder), so a game never depends on a Python the player may or may not have installed.
#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace midgard::script {

class ScriptError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The interpreter. One per process, started by the runtime and stopped when it goes.
class Interpreter {
public:
    // python_home is the python/ folder (python313.zip and the compiled standard-library
    // modules); script_paths go first on sys.path. Throws ScriptError.
    explicit Interpreter(const std::filesystem::path& python_home,
                         const std::vector<std::filesystem::path>& script_paths = {});
    ~Interpreter();
    Interpreter(const Interpreter&) = delete;
    Interpreter& operator=(const Interpreter&) = delete;

    std::string version() const;  // "3.13.15"

    // Runs Python source in a fresh namespace. Throws ScriptError with the traceback.
    void exec(const std::string& code);
};

// The python/ folder beside the running executable.
std::filesystem::path python_home_beside(const std::filesystem::path& executable_dir);

}  // namespace midgard::script
