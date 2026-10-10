#include "phonemic/websocket.hpp"

#include "phonemic/util.hpp"
#include "test.hpp"

using namespace phonemic;

namespace {
std::string hex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) { s.push_back(d[p[i] >> 4]); s.push_back(d[p[i] & 15]); }
    return s;
}
std::vector<uint8_t> client_frame(uint8_t opcode, const std::string& payload, bool fin = true) {
    std::vector<uint8_t> f;
    f.push_back(static_cast<uint8_t>((fin ? 0x80 : 0) | opcode));
    const uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
    if (payload.size() < 126) {
        f.push_back(static_cast<uint8_t>(0x80 | payload.size()));
    } else {
        f.push_back(0x80 | 126);
        f.push_back(static_cast<uint8_t>(payload.size() >> 8));
        f.push_back(static_cast<uint8_t>(payload.size()));
    }
    f.insert(f.end(), mask, mask + 4);
    for (size_t i = 0; i < payload.size(); ++i) f.push_back(static_cast<uint8_t>(payload[i]) ^ mask[i & 3]);
    return f;
}
}  // namespace

TEST(ws_sha1_known_vectors) {
    const auto a = ws::sha1(reinterpret_cast<const uint8_t*>("abc"), 3);
    CHECK(hex(a.data(), 20) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    const auto e = ws::sha1(nullptr, 0);
    CHECK(hex(e.data(), 20) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    const std::string m(1000, 'a');
    const auto l = ws::sha1(reinterpret_cast<const uint8_t*>(m.data()), m.size());
    CHECK(hex(l.data(), 20) == "291e9a6c66994949b57ba5e650361e98fc36b1ba");
}

TEST(ws_base64_and_rfc6455_accept_key) {
    CHECK(ws::base64(reinterpret_cast<const uint8_t*>("f"), 1) == "Zg==");
    CHECK(ws::base64(reinterpret_cast<const uint8_t*>("fo"), 2) == "Zm8=");
    CHECK(ws::base64(reinterpret_cast<const uint8_t*>("foo"), 3) == "Zm9v");
    // The example from RFC 6455 section 1.3.
    CHECK(ws::accept_key("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

TEST(ws_parse_http_request) {
    ws::HttpRequest r;
    size_t end = 0;
    const std::string req =
        "GET /ws?x=1&t=abc123 HTTP/1.1\r\nHost: 192.168.1.5:8443\r\nUpgrade: websocket\r\n"
        "Connection: keep-alive, Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\nEXTRA";
    CHECK(ws::parse_request(req, &r, &end));
    CHECK(r.path == "/ws");
    CHECK(r.query_param("t") == "abc123");
    CHECK(r.query_param("x") == "1");
    CHECK(r.query_param("nope").empty());
    CHECK(r.header("host") == "192.168.1.5:8443");
    CHECK(r.is_websocket_upgrade());
    CHECK(req.substr(end) == "EXTRA");
    CHECK(!ws::parse_request("GET / HTTP/1.1\r\n", &r, &end));           // incomplete
    CHECK(!ws::parse_request("garbage\r\n\r\n", &r, &end));              // malformed
    CHECK(!ws::parse_request("GET relative HTTP/1.1\r\n\r\n", &r, &end));
}

TEST(ws_frame_parser_handles_split_and_multiple_frames) {
    ws::FrameParser p;
    auto a = client_frame(ws::kBinary, "hello");
    auto b = client_frame(ws::kText, std::string(300, 'x'));
    std::vector<uint8_t> all = a;
    all.insert(all.end(), b.begin(), b.end());
    ws::Frame f;
    p.feed(all.data(), 3);  // partial header
    CHECK(!p.next(&f));
    p.feed(all.data() + 3, all.size() - 3);
    CHECK(p.next(&f));
    CHECK_EQ(f.opcode, ws::kBinary);
    CHECK(std::string(f.payload.begin(), f.payload.end()) == "hello");
    CHECK(p.next(&f));
    CHECK_EQ(f.payload.size(), 300u);
    CHECK(!p.next(&f));
    CHECK(!p.error());
}

TEST(ws_frame_parser_rejects_unmasked_and_oversized) {
    ws::FrameParser p(100);
    const uint8_t unmasked[] = {0x82, 0x01, 0x00};
    p.feed(unmasked, sizeof unmasked);
    ws::Frame f;
    CHECK(!p.next(&f));
    CHECK(p.error());
    ws::FrameParser q(100);
    const auto big = client_frame(ws::kBinary, std::string(200, 'z'));
    q.feed(big.data(), big.size());
    CHECK(!q.next(&f));
    CHECK(q.error());
}

TEST(ws_encode_server_frames) {
    const uint8_t p[] = {1, 2, 3};
    auto f = ws::encode_frame(ws::kBinary, p, 3);
    CHECK_EQ(f.size(), 5u);
    CHECK_EQ(f[0], 0x82);
    CHECK_EQ(f[1], 3);  // unmasked
    std::vector<uint8_t> mid(1000, 9);
    f = ws::encode_frame(ws::kText, mid.data(), mid.size());
    CHECK_EQ(f[1], 126);
    CHECK_EQ(f.size(), 4u + 1000);
}

TEST(util_tokens_and_constant_time_compare) {
    const auto a = random_token(10), b = random_token(10);
    CHECK_EQ(a.size(), 10u);
    CHECK(a != b);
    for (char c : a) CHECK((c >= 'a' && c <= 'z') || (c >= '2' && c <= '9'));
    CHECK(ct_equal("abc", "abc"));
    CHECK(!ct_equal("abc", "abd"));
    CHECK(!ct_equal("abc", "abcd"));
    CHECK(random_u32_nonzero() != 0);
}
