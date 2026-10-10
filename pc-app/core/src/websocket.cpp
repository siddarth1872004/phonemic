#include "phonemic/websocket.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace phonemic::ws {

std::array<uint8_t, 20> sha1(const uint8_t* data, size_t n) {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    std::vector<uint8_t> m(data, data + n);
    const uint64_t bits = static_cast<uint64_t>(n) * 8;
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    for (int i = 7; i >= 0; --i) m.push_back(static_cast<uint8_t>(bits >> (8 * i)));
    auto rol = [](uint32_t v, int s) { return (v << s) | (v >> (32 - s)); };
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t)m[off + 4 * i] << 24 | (uint32_t)m[off + 4 * i + 1] << 16 |
                   (uint32_t)m[off + 4 * i + 2] << 8 | (uint32_t)m[off + 4 * i + 3];
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
            const uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    std::array<uint8_t, 20> out{};
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 4; ++j) out[4 * i + j] = static_cast<uint8_t>(h[i] >> (24 - 8 * j));
    return out;
}

std::string base64(const uint8_t* d, size_t n) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = (uint32_t)d[i] << 16 | (i + 1 < n ? (uint32_t)d[i + 1] << 8 : 0) |
                           (i + 2 < n ? d[i + 2] : 0);
        o.push_back(t[(v >> 18) & 63]);
        o.push_back(t[(v >> 12) & 63]);
        o.push_back(i + 1 < n ? t[(v >> 6) & 63] : '=');
        o.push_back(i + 2 < n ? t[v & 63] : '=');
    }
    return o;
}

std::string accept_key(const std::string& client_key) {
    const std::string s = client_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    const auto h = sha1(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    return base64(h.data(), h.size());
}

static std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

std::string HttpRequest::header(const std::string& k) const {
    auto it = headers.find(k);
    return it == headers.end() ? std::string() : it->second;
}

std::string HttpRequest::query_param(const std::string& name) const {
    size_t pos = 0;
    while (pos <= query.size()) {
        size_t amp = query.find('&', pos);
        if (amp == std::string::npos) amp = query.size();
        const std::string kv = query.substr(pos, amp - pos);
        const size_t eq = kv.find('=');
        if (kv.substr(0, eq) == name) return eq == std::string::npos ? std::string() : kv.substr(eq + 1);
        pos = amp + 1;
    }
    return {};
}

bool HttpRequest::is_websocket_upgrade() const {
    return method == "GET" && lower(header("upgrade")) == "websocket" &&
           lower(header("connection")).find("upgrade") != std::string::npos &&
           !header("sec-websocket-key").empty();
}

bool parse_request(const std::string& buf, HttpRequest* out, size_t* end) {
    const size_t hdr_end = buf.find("\r\n\r\n");
    if (hdr_end == std::string::npos) return false;
    if (end) *end = hdr_end + 4;
    size_t line_end = buf.find("\r\n");
    const std::string line = buf.substr(0, line_end);
    const size_t s1 = line.find(' ');
    const size_t s2 = line.find(' ', s1 == std::string::npos ? 0 : s1 + 1);
    if (s1 == std::string::npos || s2 == std::string::npos) return false;
    out->method = line.substr(0, s1);
    out->target = line.substr(s1 + 1, s2 - s1 - 1);
    out->version = line.substr(s2 + 1);
    if (out->target.empty() || out->target[0] != '/' || out->version.rfind("HTTP/1.", 0) != 0) return false;
    const size_t q = out->target.find('?');
    out->path = out->target.substr(0, q);
    out->query = q == std::string::npos ? std::string() : out->target.substr(q + 1);
    out->headers.clear();
    size_t pos = line_end + 2;
    while (pos < hdr_end) {
        size_t e = buf.find("\r\n", pos);
        if (e == std::string::npos || e > hdr_end) e = hdr_end;
        const std::string h = buf.substr(pos, e - pos);
        const size_t colon = h.find(':');
        if (colon == std::string::npos || colon == 0) return false;
        out->headers[lower(trim(h.substr(0, colon)))] = trim(h.substr(colon + 1));
        pos = e + 2;
    }
    return true;
}

void FrameParser::feed(const uint8_t* data, size_t n) {
    if (!error_) buf_.insert(buf_.end(), data, data + n);
}

bool FrameParser::next(Frame* out) {
    if (error_ || buf_.size() < 2) return false;
    const uint8_t b0 = buf_[0], b1 = buf_[1];
    if (b0 & 0x70) {
        error_ = true;
        err_ = "reserved bits set";
        return false;
    }
    if (!(b1 & 0x80)) {
        error_ = true;
        err_ = "client frame not masked";
        return false;
    }
    uint64_t len = b1 & 0x7F;
    size_t off = 2;
    if (len == 126) {
        if (buf_.size() < 4) return false;
        len = (uint64_t)buf_[2] << 8 | buf_[3];
        off = 4;
    } else if (len == 127) {
        if (buf_.size() < 10) return false;
        len = 0;
        for (int i = 0; i < 8; ++i) len = len << 8 | buf_[2 + i];
        off = 10;
    }
    if (len > max_) {
        error_ = true;
        err_ = "frame too large";
        return false;
    }
    const uint8_t opcode = b0 & 0x0F;
    if ((opcode & 0x08) && (len > 125 || !(b0 & 0x80))) {
        error_ = true;
        err_ = "bad control frame";
        return false;
    }
    if (buf_.size() < off + 4 + len) return false;
    const uint8_t* mask = &buf_[off];
    out->opcode = opcode;
    out->fin = (b0 & 0x80) != 0;
    out->payload.resize(static_cast<size_t>(len));
    for (size_t i = 0; i < len; ++i) out->payload[i] = buf_[off + 4 + i] ^ mask[i & 3];
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(off + 4 + len));
    return true;
}

std::vector<uint8_t> encode_frame(uint8_t opcode, const uint8_t* p, size_t n, bool fin) {
    std::vector<uint8_t> f;
    f.push_back(static_cast<uint8_t>((fin ? 0x80 : 0) | (opcode & 0x0F)));
    if (n < 126) {
        f.push_back(static_cast<uint8_t>(n));
    } else if (n <= 0xFFFF) {
        f.push_back(126);
        f.push_back(static_cast<uint8_t>(n >> 8));
        f.push_back(static_cast<uint8_t>(n));
    } else {
        f.push_back(127);
        for (int i = 7; i >= 0; --i) f.push_back(static_cast<uint8_t>(static_cast<uint64_t>(n) >> (8 * i)));
    }
    f.insert(f.end(), p, p + n);
    return f;
}

}  // namespace phonemic::ws
