// SPDX-License-Identifier: Apache-2.0
#include <doctest/doctest.h>

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "midgard/manifest.h"

using nlohmann::json;

namespace {

const std::filesystem::path kExample =
    std::filesystem::path(MIDGARD_SCHEMA_DIR) / "examples" / "manifest.example.json";

json example() {
    return json::parse(R"({
      "format": "midgard-package", "format_version": "1.0",
      "game": {"name": "HelloCube", "version": "0.1.0"},
      "built_with": {"editor": "0.1.0+1", "engine_abi": 0},
      "target": {"os": "windows", "arch": "x86_64", "role": "game"},
      "entry": {"scene": "scn_main"},
      "database": "HelloCube.db", "schema_version": 1, "content": []
    })");
}

std::string error_for(const json& doc) {
    try {
        midgard::parse_manifest(doc.dump());
    } catch (const midgard::ManifestError& e) {
        return e.what();
    }
    return {};
}

}  // namespace

TEST_CASE("the shared example manifest loads") {
    const midgard::Manifest m = midgard::load_manifest(kExample);
    CHECK(m.game_name == "HelloCube");
    CHECK(m.format_major == 1);
    CHECK(m.format_minor == 0);
    CHECK(m.entry_scene == "scn_main");
    CHECK(m.world_mode == "bounded_surface_map");
    CHECK(m.schema_version == 1);
    REQUIRE(m.content.size() == 1);
    CHECK(m.content[0].id == "mesh_cube");
}

TEST_CASE("a minimal manifest parses") {
    CHECK(error_for(example()).empty());
}

TEST_CASE("problems are reported clearly") {
    json doc = example();

    SUBCASE("missing game") {
        doc.erase("game");
        CHECK(error_for(doc).find("missing 'game'") != std::string::npos);
    }
    SUBCASE("unknown target os") {
        doc["target"]["os"] = "macos";
        CHECK(error_for(doc).find("target/os") != std::string::npos);
    }
    SUBCASE("database must be a plain file name") {
        doc["database"] = "../escape.db";
        CHECK(error_for(doc).find("plain file name") != std::string::npos);
    }
    SUBCASE("not a Midgard package") {
        doc["format"] = "zip";
        CHECK(error_for(doc).find("not a Midgard package") != std::string::npos);
    }
    SUBCASE("format version shape") {
        doc["format_version"] = "1";
        CHECK(error_for(doc).find("major.minor") != std::string::npos);
    }
}

TEST_CASE("broken JSON is a ManifestError, not a crash") {
    CHECK_THROWS_AS(midgard::parse_manifest("{not json"), midgard::ManifestError);
}
