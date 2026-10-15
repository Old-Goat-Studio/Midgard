// SPDX-License-Identifier: Apache-2.0
#include "midgard/script.h"

#include <pybind11/embed.h>

namespace py = pybind11;

namespace midgard::script {
namespace {

bool g_running = false;

void append(PyWideStringList& list, const std::filesystem::path& path) {
    const PyStatus status = PyWideStringList_Append(&list, path.wstring().c_str());
    if (PyStatus_Exception(status)) throw ScriptError("Python: cannot add " + path.string());
}

}  // namespace

std::filesystem::path python_home_beside(const std::filesystem::path& executable_dir) {
    return executable_dir / "python";
}

Interpreter::Interpreter(const std::filesystem::path& python_home,
                         const std::vector<std::filesystem::path>& script_paths) {
    if (g_running) throw ScriptError("the Python interpreter is already running");
    const std::filesystem::path stdlib = python_home / "python313.zip";
    if (!std::filesystem::is_regular_file(stdlib))
        throw ScriptError("Python is missing: " + stdlib.string() + " not found");
    PyConfig config;
    PyConfig_InitIsolatedConfig(&config);  // ignores PYTHON* variables and the user's site
    config.site_import = 0;
    config.write_bytecode = 0;
    config.install_signal_handlers = 0;
    config.parse_argv = 0;
    config.module_search_paths_set = 1;
    PyStatus status = PyConfig_SetString(&config, &config.home, python_home.wstring().c_str());
    if (PyStatus_Exception(status)) {
        PyConfig_Clear(&config);
        throw ScriptError("Python: cannot set its home folder");
    }
    try {
        for (const auto& path : script_paths) append(config.module_search_paths, path);
        append(config.module_search_paths, stdlib);
#ifdef _WIN32
        append(config.module_search_paths, python_home / "DLLs");
#else
        append(config.module_search_paths, python_home / "lib" / "python3.13" / "lib-dynload");
#endif
    } catch (...) {
        PyConfig_Clear(&config);
        throw;
    }
    try {
        py::initialize_interpreter(&config, 0, nullptr, false);  // clears config
    } catch (const std::exception& e) {
        throw ScriptError(std::string("Python did not start: ") + e.what());
    }
    g_running = true;
}

Interpreter::~Interpreter() {
    py::finalize_interpreter();
    g_running = false;
}

std::string Interpreter::version() const {
    py::gil_scoped_acquire gil;
    return py::module_::import("platform").attr("python_version")().cast<std::string>();
}

void Interpreter::exec(const std::string& code) {
    py::gil_scoped_acquire gil;
    try {
        py::dict scope;
        scope["__builtins__"] = py::module_::import("builtins");
        py::exec(code, scope);
    } catch (const py::error_already_set& e) {
        throw ScriptError(e.what());
    }
}

}  // namespace midgard::script
