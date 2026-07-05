#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "engine/spsc_ring.h"
#include "tt/events.h"

#include <cstdint>
#include <thread>

TEST_CASE("push/pop preserves FIFO order") {
    tt::SpscRing<int, 8> ring;
    for (int i = 0; i < 5; ++i) CHECK(ring.try_push(i));
    int v = -1;
    for (int i = 0; i < 5; ++i) {
        CHECK(ring.try_pop(v));
        CHECK(v == i);
    }
    CHECK_FALSE(ring.try_pop(v));  // empty again
}

TEST_CASE("full ring rejects push, drained ring rejects pop") {
    tt::SpscRing<int, 4> ring;
    for (int i = 0; i < 4; ++i) CHECK(ring.try_push(i));
    CHECK_FALSE(ring.try_push(99));  // full
    int v;
    for (int i = 0; i < 4; ++i) CHECK(ring.try_pop(v));
    CHECK_FALSE(ring.try_pop(v));    // empty
}

TEST_CASE("indices wrap correctly across many refills") {
    tt::SpscRing<uint64_t, 4> ring;
    uint64_t v;
    for (uint64_t i = 0; i < 1000; ++i) {
        CHECK(ring.try_push(i));
        CHECK(ring.try_pop(v));
        CHECK(v == i);
    }
}

TEST_CASE("two-thread stress: 4M items, checksum and order intact") {
    constexpr uint64_t kCount = 4'000'000;
    static tt::SpscRing<uint64_t, 1 << 12> ring;  // static: 32 KiB buffer off the stack

    std::thread producer([&] {
        for (uint64_t i = 0; i < kCount; ++i)
            while (!ring.try_push(i)) { /* spin */ }
    });

    uint64_t sum = 0, expected_next = 0;
    bool ordered = true;
    for (uint64_t received = 0; received < kCount;) {
        uint64_t v;
        if (ring.try_pop(v)) {
            ordered = ordered && (v == expected_next++);
            sum += v;
            ++received;
        }
    }
    producer.join();

    CHECK(ordered);
    CHECK(sum == kCount * (kCount - 1) / 2);
}

TEST_CASE("SDK event PODs keep their ABI size") {
    CHECK(sizeof(tt::Bar) == 48);
    CHECK(sizeof(tt::Tick) == 40);
    CHECK(sizeof(tt::OrderRequest) == 48);   // SDK v2: stop + bracket fields
    CHECK(sizeof(tt::Fill) == 48);
}
