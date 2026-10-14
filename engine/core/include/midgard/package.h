// SPDX-License-Identifier: Apache-2.0
// Opening a cooked Midgard package: manifest.json plus its read-only SQLite database.
#pragma once

#include <filesystem>
#include <stdexcept>

#include "midgard/manifest.h"

namespace midgard {

class PackageError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct PackageInfo {
    std::filesystem::path root;
    Manifest manifest;
    int database_schema_version = 0;
};

// Loads <dir>/manifest.json, checks it is a format and schema this engine supports, and opens
// the database read-only to confirm its schema version. Throws PackageError or ManifestError.
PackageInfo open_package(const std::filesystem::path& dir);

}  // namespace midgard
