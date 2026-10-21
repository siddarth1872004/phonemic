// Bluetooth transport: frame splitting, the per-connection pump (serve_stream),
// the socket ByteStream, and the process runner the virtual mic uses.
#include "phonemic/bluetooth.hpp"

#include <phonemic/wire.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <thread>

#include "phonemic/process.hpp"
#include "test.hpp"

#ifndef _WIN32
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace phonemic;

namespace {
struct CollectSink : AudioSinkIface {
    std::mutex mu;
    size_t samples = 0;
    void write(const float*, size_t n) override {
        std::lock_guard<std::mutex> lk(mu);
        samples += n;
    }
    void flush() override {}
    size_t count() {
        std::lock_guard<std::mutex> lk(mu);
        return samples;
    }
};

std::vector<uint8_t> framed_audio(uint32_t session, uint32_t seq) {
    pm_audio_header h{};
    h.codec = PM_CODEC_PCM16;
    h.session_id = session;
    h.seq = seq;
    h.timestamp_us = 1000 + seq * 10000ull;
    h.sample_rate = 48000;
    std::vector<uint8_t> pcm(960, 0);
    std::vector<uint8_t> out(PM_FRAME_PREFIX_LEN + PM_AUDIO_HEADER_LEN + pcm.size());
    pm_write_frame_prefix(out.data(), PM_AUDIO_HEADER_LEN + pcm.size());
    pm_encode_audio(out.data() + PM_FRAME_PREFIX_LEN, out.size() - PM_FRAME_PREFIX_LEN, &h, pcm.data(), pcm.size());
    return out;
}

// An in-memory ByteStream: the test pushes the phone's bytes in, reads the PC's out.
struct FakeStream : ByteStream {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<uint8_t> in;
    std::vector<uint8_t> out;
    bool closed = false;

    void push(const std::vector<uint8_t>& b) {
        std::lock_guard<std::mutex> lk(mu);
        in.insert(in.end(), b.begin(), b.end());
        cv.notify_all();
    }
    int read(uint8_t* buf, size_t cap, int timeout_ms) override {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] { return !in.empty() || closed; });
        if (in.empty()) return closed ? -1 : 0;
        size_t n = 0;
        while (n < cap && !in.empty()) {
            buf[n++] = in.front();
            in.pop_front();
        }
        return static_cast<int>(n);
    }
    bool write(const uint8_t* p, size_t n) override {
        std::lock_guard<std::mutex> lk(mu);
        out.insert(out.end(), p, p + n);
        return !closed;
    }
    void close() override {
        std::lock_guard<std::mutex> lk(mu);
        closed = true;
        cv.notify_all();
    }
    std::vector<uint8_t> written() {
        std::lock_guard<std::mutex> lk(mu);
        return out;
    }
};
}  // namespace

TEST(bt_frame_splitter) {
    FrameSplitter f(100);
    const uint8_t a[] = {3, 0, 'a', 'b', 'c', 2, 0, 'x'};
    f.feed(a, sizeof a);
    std::vector<uint8_t> out;
    CHECK(f.next(&out));
    CHECK(std::string(out.begin(), out.end()) == "abc");
    CHECK(!f.next(&out));  // second frame incomplete
    const uint8_t b[] = {'y'};
    f.feed(b, 1);
    CHECK(f.next(&out));
    CHECK(std::string(out.begin(), out.end()) == "xy");
    const uint8_t bad[] = {0xFF, 0xFF};
    f.feed(bad, 2);
    CHECK(!f.next(&out));
    CHECK(f.error());
}

TEST(bt_serve_stream_plays_audio_and_answers_with_status) {
    Receiver rx;
    CollectSink mic;
    rx.set_voice_focus(false);
    rx.set_mic_sink(&mic);
    FakeStream s;
    SourceId src;
    src.conn_id = 1;
    src.name = "Pixel";
    std::atomic<bool> stop{false};
    std::thread t([&] { serve_stream(&rx, s, src, stop); });

    std::vector<uint8_t> bytes;
    for (uint32_t i = 0; i < 6; ++i) {
        const auto f = framed_audio(5, i);
        bytes.insert(bytes.end(), f.begin(), f.end());
    }
    // Deliver in awkward chunks: frames straddle reads.
    for (size_t off = 0; off < bytes.size(); off += 333)
        s.push(std::vector<uint8_t>(bytes.begin() + off, bytes.begin() + std::min(bytes.size(), off + 333)));
    for (int i = 0; i < 100 && mic.count() < 6 * 480; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK_EQ(mic.count(), 6u * 480u);
    CHECK(rx.snapshot().connected);
    CHECK(rx.snapshot().peer == "Pixel");

    s.close();  // the phone hangs up
    t.join();

    const auto w = s.written();
    CHECK(w.size() >= PM_FRAME_PREFIX_LEN + PM_STATUS_LEN);
    CHECK_EQ(pm_r16(w.data()), PM_STATUS_LEN);
    pm_status st{};
    CHECK_EQ(pm_decode_status(w.data() + PM_FRAME_PREFIX_LEN, PM_STATUS_LEN, &st), PM_OK);
    CHECK_EQ(st.state, PM_STATUS_OK);
    CHECK_EQ(st.session_id, 5u);
}

TEST(bt_serve_stream_drops_a_non_phonemic_stream) {
    Receiver rx;
    FakeStream s;
    SourceId src;
    src.conn_id = 1;
    std::atomic<bool> stop{false};
    s.push({0xFF, 0xFF, 'h', 'i'});  // a 65535-byte "frame": not ours
    const auto t0 = std::chrono::steady_clock::now();
    serve_stream(&rx, s, src, stop);  // returns on its own
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2));
}

#ifndef _WIN32
TEST(bt_socket_stream_reads_writes_and_closes) {
    int sv[2];
    CHECK_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    auto s = socket_stream(sv[0]);
    uint8_t buf[8];
    CHECK_EQ(s->read(buf, sizeof buf, 20), 0);  // nothing yet
    CHECK_EQ(::write(sv[1], "ab", 2), 2);
    CHECK_EQ(s->read(buf, sizeof buf, 200), 2);
    CHECK(s->write(reinterpret_cast<const uint8_t*>("xyz"), 3));
    CHECK_EQ(::read(sv[1], buf, sizeof buf), 3);
    std::thread closer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        s->close();
    });
    CHECK(s->read(buf, sizeof buf, 5000) < 0);  // close() wakes a blocked read
    closer.join();
    ::close(sv[1]);
}
#endif

TEST(process_runner_captures_output_and_reports_missing_programs) {
#ifdef _WIN32
    const auto r = run_process({"cmd", "/c", "echo hello"}, 10000);
#else
    const auto r = run_process({"sh", "-c", "echo hello; exit 3"}, 10000);
#endif
    CHECK(r.started);
    CHECK(r.output.find("hello") != std::string::npos);
#ifndef _WIN32
    CHECK_EQ(r.exit_code, 3);
    const auto t = run_process({"sleep", "5"}, 200);  // killed on timeout
    CHECK(t.started);
    CHECK_EQ(t.exit_code, -1);
#endif
    const auto m = run_process({"definitely-not-a-program-phonemic"}, 2000);
    CHECK(!m.started);
}
