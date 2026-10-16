// SPDX-License-Identifier: Apache-2.0
#include <doctest/doctest.h>

#include <cstdint>

#include "midgard/hash.h"
#include "midgard/render/mesh_data.h"

TEST_CASE("FNV-1a gives the published values") {
    // Test vectors from the FNV reference (http://www.isthe.com/chongo/tech/comp/fnv/).
    CHECK(midgard::Fnv1a64().value() == 0xcbf29ce484222325ULL);
    CHECK(midgard::Fnv1a64().add("a").value() == 0xaf63dc4c8601ec8cULL);
    CHECK(midgard::Fnv1a64().add("foobar").value() == 0x85944171f73967e8ULL);
    CHECK(midgard::Fnv1a64().add("foo").add("bar").value() == 0x85944171f73967e8ULL);
}

TEST_CASE("the test cube hashes the same on every target") {
    // The runtime's self-test (engine/runtime/selftest.cpp) checks the same value on devices.
    const auto cube = midgard::render::make_cube();
    midgard::Fnv1a64 hash;
    hash.add(cube.vertices.data(), cube.vertices.size() * sizeof(midgard::render::Vertex));
    hash.add(cube.indices.data(), cube.indices.size() * sizeof(std::uint32_t));
    CHECK(hash.value() == 0x48cc279e660d43e5ULL);
}
