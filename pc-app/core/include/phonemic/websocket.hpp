// Minimal HTTP/1.1 + WebSocket (RFC 6455) server-side protocol pieces.
// Pure functions over bytes (no sockets) so they are unit-tested directly.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace phonemic::ws {

std::array<uint8_t, 20> sha1(const uint8_t* data, size_t n);
std::string base64(const uint8_t* data, size_t n);
// Sec-WebSocket-Accept for a client's Sec-WebSocket-Key.
std::string accept_key(const std::string& client_key);

struct HttpRequest {
    std::string method, target, path, query, version;
    std::map<std::string, std::string> headers;  // keys lower-cased
    std::string header(const std::string& lower_name) const;
    std::string query_param(const std::string& name) const;
    bool is_websocket_upgrade() const;
};

// Parse "GET /path?q HTTP/1.1\r\nHeader: v\r\n...\r\n\r\n". Returns false on
// malformed input. `end` receives the length of the header block.
bool parse_request(const std::string& buf, HttpRequest* out, size_t* end);

enum Opcode : uint8_t { kCont = 0x0, kText = 0x1, kBinary = 0x2, kClose = 0x8, kPing = 0x9, kPong = 0xA };

struct Frame {
    uint8_t opcode = 0;
    bool fin = true;
    std::vector<uint8_t> payload;
};

// Incremental client-frame parser: feed() bytes as they arrive, then pull
// complete frames with next(). Enforces masking (required for client frames)
// and a maximum payload size; error() is set on any violation.
class FrameParser {
public:
    explicit FrameParser(size_t max_payload = 64 * 1024) : max_(max_payload) {}
    void feed(const uint8_t* data, size_t n);
    bool next(Frame* out);
    bool error() const { return error_; }
    const std::string& error_text() const { return err_; }

private:
    std::vector<uint8_t> buf_;
    size_t max_;
    bool error_ = false;
    std::string err_;
};

// Server -> client frame (servers never mask).
std::vector<uint8_t> encode_frame(uint8_t opcode, const uint8_t* payload, size_t n, bool fin = true);

}  // namespace phonemic::ws
