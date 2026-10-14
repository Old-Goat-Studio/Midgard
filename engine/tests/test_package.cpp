// SPDX-License-Identifier: Apache-2.0
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include <sqlite3.h>

#include "midgard/package.h"

namespace fs = std::filesystem;

namespace {

// A throwaway package folder: manifest.json plus a database at the given schema version.
struct TempPackage {
    fs::path dir;

    TempPackage(int manifest_schema, int db_schema, int format_major = 1) {
        std::random_device rd;
        dir = fs::temp_directory_path() / ("midgard_test_" + std::to_string(rd()));
        fs::create_directories(dir);
        std::ofstream(dir / "manifest.json")
            << R"({"format":"midgard-package","format_version":")" << format_major
            << R"(.0","game":{"name":"T","version":"0.1.0"},)"
            << R"("built_with":{"editor":"0.1.0+1","engine_abi":0},)"
            << R"("target":{"os":"linux","arch":"x86_64","role":"game"},)"
            << R"("entry":{"scene":"scn_main"},"database":"T.db",)"
            << R"("schema_version":)" << manifest_schema << R"(,"content":[]})";
        sqlite3* db = nullptr;
        const std::u8string u8 = (dir / "T.db").u8string();
        sqlite3_open(std::string(u8.begin(), u8.end()).c_str(), &db);
        sqlite3_exec(db, ("PRAGMA user_version = " + std::to_string(db_schema)).c_str(), nullptr,
                     nullptr, nullptr);
        sqlite3_close(db);
    }
    ~TempPackage() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

}  // namespace

TEST_CASE("a matching package opens") {
    TempPackage pkg(1, 1);
    const midgard::PackageInfo info = midgard::open_package(pkg.dir);
    CHECK(info.database_schema_version == 1);
    CHECK(info.manifest.game_name == "T");
}

TEST_CASE("schema mismatch between manifest and database is refused") {
    TempPackage pkg(1, 2);
    CHECK_THROWS_AS(midgard::open_package(pkg.dir), midgard::PackageError);
}

TEST_CASE("unsupported schema version is refused") {
    TempPackage pkg(7, 7);
    CHECK_THROWS_WITH_AS(midgard::open_package(pkg.dir),
                         doctest::Contains("outside the supported range"), midgard::PackageError);
}

TEST_CASE("unsupported format major version is refused") {
    TempPackage pkg(1, 1, 2);
    CHECK_THROWS_WITH_AS(midgard::open_package(pkg.dir), doctest::Contains("not supported"),
                         midgard::PackageError);
}

TEST_CASE("a missing database is reported") {
    TempPackage pkg(1, 1);
    fs::remove(pkg.dir / "T.db");
    CHECK_THROWS_WITH_AS(midgard::open_package(pkg.dir), doctest::Contains("missing"),
                         midgard::PackageError);
}
