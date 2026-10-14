// SPDX-License-Identifier: Apache-2.0
#include "midgard/package.h"

#include <memory>
#include <string>

#include <sqlite3.h>

#include "midgard/version.h"

namespace midgard {
namespace {

struct SqliteCloser {
    void operator()(sqlite3* db) const { sqlite3_close(db); }
};
using SqliteHandle = std::unique_ptr<sqlite3, SqliteCloser>;

int read_user_version(const std::filesystem::path& file) {
    sqlite3* raw = nullptr;
    // Packages are read-only: the shipped database is never written (player saves live elsewhere).
    const std::u8string u8 = file.u8string();  // SQLite expects UTF-8 file names
    const std::string utf8(u8.begin(), u8.end());
    int rc = sqlite3_open_v2(utf8.c_str(), &raw, SQLITE_OPEN_READONLY, nullptr);
    SqliteHandle db(raw);
    if (rc != SQLITE_OK)
        throw PackageError("cannot open database " + file.string() + ": " +
                           (raw ? sqlite3_errmsg(raw) : "out of memory"));

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db.get(), "PRAGMA user_version", -1, &stmt, nullptr) != SQLITE_OK)
        throw PackageError("cannot read schema version: " + std::string(sqlite3_errmsg(db.get())));
    int version = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) version = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    if (version < 0) throw PackageError("cannot read schema version of " + file.string());
    return version;
}

}  // namespace

PackageInfo open_package(const std::filesystem::path& dir) {
    PackageInfo info;
    info.root = dir;
    info.manifest = load_manifest(dir / "manifest.json");
    const Manifest& m = info.manifest;

    if (m.format_major != kFormatMajor)
        throw PackageError("package format " + std::to_string(m.format_major) + "." +
                           std::to_string(m.format_minor) + " is not supported (engine reads " +
                           std::to_string(kFormatMajor) + ".x)");
    if (m.engine_abi > kEngineAbi)
        throw PackageError("package needs engine ABI " + std::to_string(m.engine_abi) +
                           ", this engine is ABI " + std::to_string(kEngineAbi));
    if (m.schema_version < kSchemaMin || m.schema_version > kSchemaMax)
        throw PackageError("package database schema " + std::to_string(m.schema_version) +
                           " is outside the supported range " + std::to_string(kSchemaMin) + "-" +
                           std::to_string(kSchemaMax));

    const std::filesystem::path db_file = dir / m.database;
    if (!std::filesystem::is_regular_file(db_file))
        throw PackageError("database " + m.database + " is missing from the package");
    info.database_schema_version = read_user_version(db_file);
    if (info.database_schema_version != m.schema_version)
        throw PackageError(
            "database schema version " + std::to_string(info.database_schema_version) +
            " does not match the manifest (" + std::to_string(m.schema_version) + ")");
    return info;
}

}  // namespace midgard
