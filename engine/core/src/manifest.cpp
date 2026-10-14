// SPDX-License-Identifier: Apache-2.0
#include "midgard/manifest.h"

#include <fstream>
#include <regex>
#include <sstream>

#include <nlohmann/json.hpp>

namespace midgard {
namespace {

using nlohmann::json;

const json& field(const json& obj, const char* name, const std::string& where) {
    auto it = obj.find(name);
    if (it == obj.end()) throw ManifestError(where + ": missing '" + name + "'");
    return *it;
}

std::string text(const json& obj, const char* name, const std::string& where) {
    const json& v = field(obj, name, where);
    if (!v.is_string() || v.get_ref<const std::string&>().empty())
        throw ManifestError(where + "/" + name + ": must be a non-empty string");
    return v.get<std::string>();
}

std::string optional_text(const json& obj, const char* name, const std::string& where) {
    return obj.contains(name) ? text(obj, name, where) : std::string{};
}

int integer(const json& obj, const char* name, const std::string& where, int min) {
    const json& v = field(obj, name, where);
    if (!v.is_number_integer() || v.get<long long>() < min)
        throw ManifestError(where + "/" + name + ": must be an integer >= " + std::to_string(min));
    return v.get<int>();
}

const json& object(const json& obj, const char* name, const std::string& where) {
    const json& v = field(obj, name, where);
    if (!v.is_object()) throw ManifestError(where + "/" + name + ": must be an object");
    return v;
}

void one_of(const std::string& value, std::initializer_list<const char*> allowed,
            const std::string& where) {
    for (const char* a : allowed)
        if (value == a) return;
    throw ManifestError(where + ": unexpected value '" + value + "'");
}

}  // namespace

Manifest parse_manifest(std::string_view json_text) {
    json doc;
    try {
        doc = json::parse(json_text);
    } catch (const json::parse_error& e) {
        throw ManifestError(std::string("manifest is not valid JSON: ") + e.what());
    }
    if (!doc.is_object()) throw ManifestError("manifest must be a JSON object");

    const std::string root = "manifest";
    if (text(doc, "format", root) != "midgard-package")
        throw ManifestError("manifest/format: not a Midgard package");

    Manifest m;
    static const std::regex major_minor(R"(^(\d+)\.(\d+)$)");
    std::smatch parts;
    const std::string format_version = text(doc, "format_version", root);
    if (!std::regex_match(format_version, parts, major_minor))
        throw ManifestError("manifest/format_version: expected major.minor");
    m.format_major = std::stoi(parts[1].str());
    m.format_minor = std::stoi(parts[2].str());

    const json& game = object(doc, "game", root);
    m.game_name = text(game, "name", "manifest/game");
    m.game_version = text(game, "version", "manifest/game");

    const json& built = object(doc, "built_with", root);
    m.editor_build = text(built, "editor", "manifest/built_with");
    m.engine_abi = integer(built, "engine_abi", "manifest/built_with", 0);

    const json& target = object(doc, "target", root);
    m.os = text(target, "os", "manifest/target");
    one_of(m.os, {"windows", "linux", "android"}, "manifest/target/os");
    m.arch = text(target, "arch", "manifest/target");
    one_of(m.arch, {"x86_64", "arm64"}, "manifest/target/arch");
    m.role = text(target, "role", "manifest/target");
    one_of(m.role, {"game", "server"}, "manifest/target/role");

    m.world_mode = optional_text(doc, "world_mode", root);
    if (!m.world_mode.empty())
        one_of(m.world_mode, {"system_map", "bounded_surface_map"}, "manifest/world_mode");

    const json& entry = object(doc, "entry", root);
    m.entry_scene = text(entry, "scene", "manifest/entry");
    m.entry_script = optional_text(entry, "script", "manifest/entry");

    m.database = text(doc, "database", root);
    if (m.database.find_first_of("/\\") != std::string::npos || m.database.size() < 4 ||
        m.database.compare(m.database.size() - 3, 3, ".db") != 0)
        throw ManifestError("manifest/database: must be a plain file name ending in .db");

    m.schema_version = integer(doc, "schema_version", root, 1);

    const json& content = field(doc, "content", root);
    if (!content.is_array()) throw ManifestError("manifest/content: must be an array");
    for (std::size_t i = 0; i < content.size(); ++i) {
        const std::string where = "manifest/content/" + std::to_string(i);
        if (!content[i].is_object()) throw ManifestError(where + ": must be an object");
        m.content.push_back({text(content[i], "id", where), text(content[i], "path", where),
                             text(content[i], "sha256", where)});
    }
    return m;
}

Manifest load_manifest(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw ManifestError("cannot open " + file.string());
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return parse_manifest(buffer.str());
}

}  // namespace midgard
