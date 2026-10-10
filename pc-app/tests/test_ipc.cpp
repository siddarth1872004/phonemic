// Shared-ring tests (ported from the Rust ipc crate), with a consumer that
// mirrors the driver's read loop.
#include "ring_producer.hpp"

#include <cstring>
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
