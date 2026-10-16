// SPDX-License-Identifier: Apache-2.0
// FNV-1a: a small, fast, fully specified hash. The same bytes give the same value on every
// target, so it fingerprints data that must be identical everywhere (generated chunks, test
// meshes). Not for security.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace midgard {

class Fnv1a64 {
public:
    Fnv1a64& add(const void* data, std::size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            value_ ^= bytes[i];
            value_ *= 0x100000001b3ULL;
        }
        return *this;
    }
    Fnv1a64& add(std::string_view text) { return add(text.data(), text.size()); }
    std::uint64_t value() const { return value_; }

private:
    std::uint64_t value_ = 0xcbf29ce484222325ULL;
};

inline std::uint64_t fnv1a64(const void* data, std::size_t size) {
    return Fnv1a64().add(data, size).value();
}

}  // namespace midgard
