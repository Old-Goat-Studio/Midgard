// SPDX-License-Identifier: Apache-2.0
// The engine against the shared tables in schema/tests/, which the Python reference
// implementations (tools/reference/) are checked against too.
#include <doctest/doctest.h>

#include <cinttypes>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "midgard/hash.h"

namespace {

nlohmann::json table(const std::string& name) {
    std::ifstream in(std::filesystem::path(MIDGARD_SCHEMA_DIR) / "tests" / (name + ".json"));
    REQUIRE(in);
    return nlohmann::json::parse(in).at("cases");
}

std::vector<unsigned char> case_bytes(const nlohmann::json& c) {
    if (c.contains("text")) {
        const std::string text = c.at("text").get<std::string>();
        return {text.begin(), text.end()};
    }
    const std::string hex = c.at("hex").get<std::string>();
    std::vector<unsigned char> out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2)
        out.push_back(static_cast<unsigned char>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    return out;
}

}  // namespace

TEST_CASE("FNV-1a matches its shared table") {
    const nlohmann::json cases = table("fnv1a64");
    REQUIRE(cases.size() >= 3);
    for (const auto& c : cases) {
        const std::vector<unsigned char> bytes = case_bytes(c);
        char got[17];
        std::snprintf(got, sizeof(got), "%016" PRIx64,
                      midgard::fnv1a64(bytes.data(), bytes.size()));
        CHECK_MESSAGE(std::string(got) == c.at("hash").get<std::string>(), c.dump());
    }
}
