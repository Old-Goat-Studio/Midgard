// SPDX-License-Identifier: Apache-2.0
// Checks a cooked package the way the runtime will load it, without a window or a GPU. The
// editor runs it after every cook (through the _midgard Python module).
#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace midgard {

struct ValidationReport {
    std::vector<std::string> errors;    // the runtime would refuse the package
    std::vector<std::string> warnings;  // it would run, but something is missing
    size_t entities = 0;                // in the entry scene
    size_t assets = 0;                  // content entries that loaded
    bool ok() const { return errors.empty(); }
};

// Opens the manifest and database, loads the entry scene and the input bindings, and reads every
// mesh in the content list from game.pak. Never throws: problems go into the report.
ValidationReport validate_package(const std::filesystem::path& package_dir);

}  // namespace midgard
