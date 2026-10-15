// SPDX-License-Identifier: Apache-2.0
#include <doctest/doctest.h>

#include <string>

#include <nlohmann/json.hpp>

#include "midgard/link.h"

using nlohmann::json;
namespace link = midgard::link;

TEST_CASE("live-link messages are length-prefixed JSON") {
    const std::string bytes = link::encode({{"type", "log"}, {"text", "hi"}});
    const std::string body = R"({"text":"hi","type":"log"})";
    CHECK(bytes.size() == 4 + body.size());
    CHECK(bytes[0] == 0);
    CHECK(bytes[3] == char(body.size()));
    CHECK(bytes.substr(4) == body);
}

TEST_CASE("the frame reader handles split and joined messages") {
    const std::string stream = link::encode({{"type", "play"}}) +
                               link::encode({{"type", "entity.set"}, {"entity", "e000004"}});
    link::FrameReader reader;
    for (char c : stream.substr(0, 7)) reader.feed(&c, 1);  // a message in pieces
    CHECK_FALSE(reader.next());
    reader.feed(stream.data() + 7, stream.size() - 7);
    const auto first = reader.next();
    REQUIRE(first);
    CHECK((*first)["type"] == "play");
    const auto second = reader.next();
    REQUIRE(second);
    CHECK((*second)["entity"] == "e000004");
    CHECK_FALSE(reader.next());
}

TEST_CASE("malformed streams are refused") {
    link::FrameReader huge;
    const char too_long[4] = {char(0x7F), 0, 0, 0};
    huge.feed(too_long, 4);
    CHECK_THROWS_AS(huge.next(), link::LinkError);

    link::FrameReader not_json;
    const std::string bad = std::string("\0\0\0\3abc", 7);
    not_json.feed(bad.data(), bad.size());
    CHECK_THROWS_AS(not_json.next(), link::LinkError);
}

TEST_CASE("endpoints") {
    std::string host;
    uint16_t port = 0;
    CHECK(link::parse_endpoint("127.0.0.1:47011", host, port));
    CHECK(host == "127.0.0.1");
    CHECK(port == 47011);
    CHECK_FALSE(link::parse_endpoint("127.0.0.1", host, port));
    CHECK_FALSE(link::parse_endpoint(":80", host, port));
    CHECK_FALSE(link::parse_endpoint("localhost:99999", host, port));
    CHECK_FALSE(link::parse_endpoint("localhost:8x", host, port));
}

TEST_CASE("connecting to nothing fails cleanly") {
    // Port 1 on the loopback address is essentially never listening.
    CHECK_THROWS_AS(link::Client("127.0.0.1", 1), link::LinkError);
}
