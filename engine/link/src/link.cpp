// SPDX-License-Identifier: Apache-2.0
#include "midgard/link.h"

#include <charconv>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
namespace {
using socket_t = SOCKET;
constexpr socket_t kNoSocket = INVALID_SOCKET;
constexpr int kSendFlags = 0;
void close_socket(socket_t s) {
    closesocket(s);
}
}  // namespace
#else
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
namespace {
using socket_t = int;
constexpr socket_t kNoSocket = -1;
constexpr int kSendFlags = MSG_NOSIGNAL;  // a closed editor must not kill the game (SIGPIPE)
void close_socket(socket_t s) {
    ::close(s);
}
}  // namespace
#endif

namespace midgard::link {

using nlohmann::json;

std::string encode(const json& message) {
    const std::string body = message.dump();
    const auto n = static_cast<uint32_t>(body.size());
    std::string out(4, '\0');
    out[0] = char((n >> 24) & 0xFF);
    out[1] = char((n >> 16) & 0xFF);
    out[2] = char((n >> 8) & 0xFF);
    out[3] = char(n & 0xFF);
    return out + body;
}

void FrameReader::feed(const char* data, size_t size) {
    buffer_.append(data, size);
}

std::optional<json> FrameReader::next() {
    if (buffer_.size() < 4) return std::nullopt;
    const auto* b = reinterpret_cast<const unsigned char*>(buffer_.data());
    const uint32_t n =
        (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | uint32_t(b[3]);
    if (n > kMaxMessage) throw LinkError("live-link message too large");
    if (buffer_.size() < 4 + size_t(n)) return std::nullopt;
    json message = json::parse(buffer_.begin() + 4, buffer_.begin() + 4 + n, nullptr, false);
    buffer_.erase(0, 4 + size_t(n));
    if (message.is_discarded() || !message.is_object() || !message.contains("type"))
        throw LinkError("malformed live-link message");
    return message;
}

bool parse_endpoint(std::string_view text, std::string& host, uint16_t& port) {
    const size_t colon = text.rfind(':');
    if (colon == std::string_view::npos || colon == 0) return false;
    unsigned value = 0;
    const char* first = text.data() + colon + 1;
    const char* last = text.data() + text.size();
    const auto [end, ec] = std::from_chars(first, last, value);
    if (ec != std::errc() || end != last || value == 0 || value > 65535) return false;
    host = std::string(text.substr(0, colon));
    port = uint16_t(value);
    return true;
}

Client::Client(const std::string& host, uint16_t port) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) throw LinkError("networking is unavailable");
#endif
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &found) != 0)
        throw LinkError("cannot resolve " + host);
    socket_t s = kNoSocket;
    for (addrinfo* a = found; a && s == kNoSocket; a = a->ai_next) {
        s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == kNoSocket) continue;
        if (::connect(s, a->ai_addr, int(a->ai_addrlen)) != 0) {
            close_socket(s);
            s = kNoSocket;
        }
    }
    freeaddrinfo(found);
    if (s == kNoSocket)
        throw LinkError("cannot connect to the editor at " + host + ":" + std::to_string(port));
    int on = 1;  // small messages go out at once
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));
    socket_ = std::intptr_t(s);
    connected_ = true;
    reader_ = std::thread([this] { read_loop(); });
}

Client::~Client() {
    const auto s = socket_t(socket_);
#ifdef _WIN32
    shutdown(s, SD_BOTH);
#else
    shutdown(s, SHUT_RDWR);
#endif
    if (reader_.joinable()) reader_.join();
    close_socket(s);
#ifdef _WIN32
    WSACleanup();
#endif
}

void Client::send(const json& message) {
    if (!connected_) return;
    const std::string bytes = encode(message);
    std::lock_guard lock(send_mutex_);
    size_t sent = 0;
    while (sent < bytes.size()) {
        const auto n =
            ::send(socket_t(socket_), bytes.data() + sent, int(bytes.size() - sent), kSendFlags);
        if (n <= 0) {
            connected_ = false;
            return;
        }
        sent += size_t(n);
    }
}

std::vector<json> Client::receive() {
    std::lock_guard lock(inbox_mutex_);
    std::vector<json> out;
    out.swap(inbox_);
    return out;
}

void Client::read_loop() {
    FrameReader reader;
    char buffer[16384];
    try {
        for (;;) {
            const auto n = ::recv(socket_t(socket_), buffer, int(sizeof(buffer)), 0);
            if (n <= 0) break;
            reader.feed(buffer, size_t(n));
            while (auto message = reader.next()) {
                std::lock_guard lock(inbox_mutex_);
                inbox_.push_back(std::move(*message));
            }
        }
    } catch (const LinkError&) {  // a broken stream ends the link
    }
    connected_ = false;
}

}  // namespace midgard::link
