// SPDX-License-Identifier: Apache-2.0
// The live link (roadmap §2, channel 2): the running game talks to the editor over TCP. Each
// message is a 4-byte big-endian length followed by UTF-8 JSON (schema/live_link.schema.json).
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace midgard::link {

inline constexpr int kProtocol = 0;
inline constexpr uint32_t kMaxMessage = 16u * 1024u * 1024u;

class LinkError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// One message as bytes on the wire.
std::string encode(const nlohmann::json& message);

// Splits a byte stream into messages.
class FrameReader {
public:
    void feed(const char* data, size_t size);
    // The next complete message, if one has arrived. Throws LinkError on a malformed stream.
    std::optional<nlohmann::json> next();

private:
    std::string buffer_;
};

// "host:port" -> host and port. Returns false if it is not of that form.
bool parse_endpoint(std::string_view text, std::string& host, uint16_t& port);

// The runtime's end: connects to the editor and reads messages on a background thread.
class Client {
public:
    // Connects. Throws LinkError if the editor can't be reached.
    Client(const std::string& host, uint16_t port);
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // Thread-safe. Messages sent after the connection closed are dropped.
    void send(const nlohmann::json& message);
    // Messages received since the last call, oldest first.
    std::vector<nlohmann::json> receive();
    bool connected() const { return connected_; }

private:
    void read_loop();

    std::intptr_t socket_ = -1;
    std::atomic<bool> connected_{false};
    std::mutex send_mutex_, inbox_mutex_;
    std::vector<nlohmann::json> inbox_;
    std::thread reader_;
};

}  // namespace midgard::link
