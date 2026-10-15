// SPDX-License-Identifier: Apache-2.0
// game.pak: a zip archive of cooked assets, read with miniz.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace midgard {

class AssetError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Pak {
public:
    explicit Pak(const std::filesystem::path& file);  // throws AssetError
    ~Pak();
    Pak(const Pak&) = delete;
    Pak& operator=(const Pak&) = delete;

    bool contains(const std::string& path) const;
    std::vector<uint8_t> read(const std::string& path) const;  // throws AssetError
    std::vector<std::string> files() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace midgard
