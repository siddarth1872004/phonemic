// Shared-ring tests (ported from the Rust ipc crate), with a consumer that
// mirrors the driver's read loop.
#include "ring_producer.hpp"
#include "ring_feeder.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <memory>
#include <vector>

#include "test.hpp"

using phonemic::RingProducer;

namespace {
struct Consumer {
    PHONEMIC_RING* r;
    size_t pop(int16_t* out, size_t n) {
        const uint64_t w = r->write_index;
        std::atomic_thread_fence(std::memory_order_acquire);
        const uint64_t avail = w - r->read_index;
        if (n > avail) n = static_cast<size_t>(avail);
        for (size_t i = 0; i < n; ++i) out[i] = r->samples[(r->read_index + i) % PHONEMIC_RING_CAPACITY];
        std::atomic_thread_fence(std::memory_order_release);
        r->read_index = r->read_index + n;
        return n;
    }
};
std::unique_ptr<PHONEMIC_RING> fresh() {
    auto r = std::make_unique<PHONEMIC_RING>();
    std::memset(static_cast<void*>(r.get()), 0, sizeof(PHONEMIC_RING));
    phonemic::ring_init_header(r.get());
    return r;
}
}  // namespace

TEST(ipc_layout_matches_c_header) {
    CHECK_EQ(sizeof(PHONEMIC_RING), 192 + PHONEMIC_RING_CAPACITY * 2);
}

TEST(ipc_header_validation) {
    auto r = fresh();
    CHECK(phonemic::ring_header_ok(r.get()));
    r->abi_version = 999;
    CHECK(!phonemic::ring_header_ok(r.get()));
}

TEST(ipc_push_pop_roundtrip) {
    auto r = fresh();
    RingProducer p(r.get());
    Consumer c{r.get()};
    const int16_t in[] = {1, 2, 3, 4};
    CHECK_EQ(p.push(in, 4), 4u);
    CHECK_EQ(p.occupied(), 4u);
    int16_t out[4];
    CHECK_EQ(c.pop(out, 4), 4u);
    CHECK(std::memcmp(in, out, sizeof in) == 0);
    CHECK_EQ(p.occupied(), 0u);
}

TEST(ipc_backpressure_when_full) {
    auto r = fresh();
    RingProducer p(r.get());
    std::vector<int16_t> big(PHONEMIC_RING_CAPACITY + 100, 7);
    CHECK_EQ(p.push(big.data(), big.size()), PHONEMIC_RING_CAPACITY);
    CHECK_EQ(p.vacant(), 0u);
    CHECK_EQ(p.push(big.data(), 3), 0u);
}

TEST(ipc_wraps_around_correctly) {
    auto r = fresh();
    RingProducer p(r.get());
    Consumer c{r.get()};
    std::vector<int16_t> filler(PHONEMIC_RING_CAPACITY - 5, 0);
    p.push(filler.data(), filler.size());
    c.pop(filler.data(), filler.size());
    int16_t data[10], out[10];
    for (int i = 0; i < 10; ++i) data[i] = static_cast<int16_t>(i + 1);
    CHECK_EQ(p.push(data, 10), 10u);
    CHECK_EQ(c.pop(out, 10), 10u);
    CHECK(std::memcmp(data, out, sizeof data) == 0);
}

TEST(ipc_hostile_consumer_index_cannot_overfill) {
    auto r = fresh();
    RingProducer p(r.get());
    r->read_index = 1ull << 40;  // consumer claims to be far "ahead"
    CHECK(p.occupied() <= PHONEMIC_RING_CAPACITY);
    int16_t s[4] = {1, 2, 3, 4};
    p.push(s, 4);  // must not write out of bounds (slot is modulo capacity)
}

// The app side of the Windows driver path: the feeder keeps the ring topped up
// from the playout buffer, so the driver's clock (not the phone's) paces it.
// Three simulated minutes with the driver 300 ppm fast: it never starves, and
// the playout buffer holds its latency target by resampling.
TEST(ipc_feeder_follows_the_driver_clock) {
    auto r = fresh();
    phonemic::RingFeeder feeder(r.get(), 20);
    feeder.set_latency_ms(40);
    Consumer driver{r.get()};
    std::vector<float> phone(480);
    std::vector<int16_t> packet(1024);
    double phase = 0, owed = 0;
    size_t starved = 0;
    for (int step = 0; step < 18000; ++step) {  // 10 ms per step
        for (float& x : phone) {
            x = static_cast<float>(0.5 * std::sin(phase));
            phase += 2 * 3.14159265358979 * 440 / 48000;
        }
        feeder.write(phone.data(), phone.size());
        feeder.pump();
        owed += 480 * (1 + 300e-6);
        const size_t want = static_cast<size_t>(owed);
        owed -= static_cast<double>(want);
        const size_t got = driver.pop(packet.data(), want);
        if (step > 50 && got < want) ++starved;
        feeder.pump();
    }
    CHECK_EQ(starved, 0u);
    const auto st = feeder.stats();
    CHECK(st.playing);
    CHECK(std::fabs(st.fill_ms - 40.0f) < 15.0f);
    CHECK_EQ(st.resyncs, 0u);
    CHECK(st.ratio < 1.0f);  // the driver is fast: stretch the phone's audio slightly
}

// No app recording the mic: the driver doesn't consume, the ring just sits at
// its target and the playout buffer stays bounded. When recording starts, audio
// flows right away.
TEST(ipc_feeder_idle_driver_then_recording) {
    auto r = fresh();
    phonemic::RingFeeder feeder(r.get(), 20);
    std::vector<float> phone(480, 0.25f);
    for (int step = 0; step < 500; ++step) {
        feeder.write(phone.data(), phone.size());
        feeder.pump();
    }
    CHECK_EQ(r->write_index - r->read_index, 960u);  // 20 ms, and no further
    CHECK(feeder.buffered_ms() < 1200);
    Consumer driver{r.get()};
    std::vector<int16_t> packet(480);
    size_t loud = 0;
    for (int step = 0; step < 100; ++step) {
        feeder.write(phone.data(), phone.size());
        feeder.pump();
        driver.pop(packet.data(), packet.size());
        for (int16_t v : packet) loud += v > 4000;
        feeder.pump();
    }
    CHECK(loud > 100 * 480 / 2);
}

#ifdef _WIN32
#include <windows.h>

#include <chrono>
#include <thread>

#include "driver_feed.hpp"

// DriverFeed against a stand-in for the driver: a section named like the one
// the driver creates, with the header it publishes. Creating a Global\ name
// needs administrator rights (the real driver creates it); skip without them.
TEST(ipc_driver_feed_opens_the_driver_ring_and_feeds_it) {
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(PHONEMIC_RING),
                                  L"Global\\PhonemicRing");
    if (!h) {
        std::printf("        (skipped: can't create Global\\PhonemicRing without administrator rights)\n");
        return;
    }
    auto* ring = static_cast<PHONEMIC_RING*>(MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(PHONEMIC_RING)));
    CHECK(ring != nullptr);
    std::memset(static_cast<void*>(ring), 0, sizeof(PHONEMIC_RING));
    CHECK(!phonemic::DriverFeed::available());  // no valid header yet
    phonemic::ring_init_header(ring);
    CHECK(phonemic::DriverFeed::available());

    std::string err;
    auto feed = phonemic::DriverFeed::open(&err);
    CHECK(feed != nullptr);
    std::vector<float> tone(480, 0.5f);
    Consumer driver{ring};
    std::vector<int16_t> packet(480);
    size_t loud = 0;
    for (int i = 0; i < 60; ++i) {  // ~600 ms; the feeder's own thread tops the ring up
        feed->write(tone.data(), tone.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const size_t got = driver.pop(packet.data(), packet.size());
        for (size_t k = 0; k < got; ++k) loud += packet[k] > 8000;
    }
    CHECK(loud > 480 * 10);
    CHECK(feed->consumed() > 0);
    feed.reset();

    ring->abi_version = 99;  // a driver from another release
    CHECK(!phonemic::DriverFeed::available());
    CHECK(phonemic::DriverFeed::open(&err) == nullptr);
    CHECK(err.find("versions") != std::string::npos);
    UnmapViewOfFile(ring);
    CloseHandle(h);
}
#endif
