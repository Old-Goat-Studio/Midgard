// SPDX-License-Identifier: Apache-2.0
// The engine's reading of manifest.json (schema/manifest.schema.json). The editor validates
// manifests fully against the JSON Schema; the engine checks what it needs to run safely.
#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace midgard {

class ManifestError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct ContentEntry {
    std::string id;
    std::string path;
    std::string sha256;
};

struct Manifest {
    int format_major = 0;
    int format_minor = 0;
    std::string game_name;
    std::string game_version;
    std::string editor_build;
    int engine_abi = 0;
    std::string os;    // windows | linux | android
    std::string arch;  // x86_64 | arm64
    std::string role;  // game | server
    std::string world_mode;
    std::string entry_scene;
    std::string entry_script;
    std::string database;
    int schema_version = 0;
    std::vector<ContentEntry> content;
};

// Parses and checks a manifest. Throws ManifestError with a readable message.
Manifest parse_manifest(std::string_view json_text);
Manifest load_manifest(const std::filesystem::path& file);

}  // namespace midgard
