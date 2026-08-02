#include <gtest/gtest.h>
#include <SPMCBroadcastQueue.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

// ===== Test types =====

// Does not satisfy BroadcastElement: no default constructor.
struct NonDefaultConstructible {
    int value;
    explicit NonDefaultConstructible(int v) : value(v) {}
    NonDefaultConstructible(const NonDefaultConstructible&)            = default;
    NonDefaultConstructible& operator=(const NonDefaultConstructible&) = default;
};

// Does not satisfy BroadcastElement: move-assignment is not noexcept.
struct ThrowingMoveAssign {
    int value{0};
    ThrowingMoveAssign() = default;
    ThrowingMoveAssign(const ThrowingMoveAssign&)            = default;
    ThrowingMoveAssign& operator=(const ThrowingMoveAssign&) = default;
    ThrowingMoveAssign(ThrowingMoveAssign&&)                 = default;
    // Deliberately not noexcept -- is_nothrow_move_assignable_v must be false.
    ThrowingMoveAssign& operator=(ThrowingMoveAssign&& other) {
        value = other.value;
        return *this;
    }
};

static_assert(!BroadcastElement<NonDefaultConstructible>,
              "NonDefaultConstructible must not satisfy BroadcastElement");
static_assert(!BroadcastElement<ThrowingMoveAssign>,
              "ThrowingMoveAssign must not satisfy BroadcastElement");
static_assert(BroadcastElement<int>, "int must satisfy BroadcastElement");

// Heavy payload to exercise non-trivial move-assignment cost and detect any
// torn/partial overwrite while a consumer might be reading (id/payload
// consistency check).
struct HeavyObject {
    static constexpr std::size_t PAYLOAD_WORDS = 256; // ~2KB payload

    int id{-1};
    std::array<std::uint64_t, PAYLOAD_WORDS> payload{};

    HeavyObject() = default;
    explicit HeavyObject(int id_) : id(id_) {
        payload.fill(static_cast<std::uint64_t>(id_));
    }
    HeavyObject(HeavyObject&&) noexcept            = default;
    HeavyObject& operator=(HeavyObject&&) noexcept = default;
    HeavyObject(const HeavyObject&)                = default;
    HeavyObject& operator=(const HeavyObject&)     = default;

    [[nodiscard]] bool payload_matches_id() const noexcept {
        for (auto w : payload)
            if (w != static_cast<std::uint64_t>(id))
                return false;
        return true;
    }
};
static_assert(BroadcastElement<HeavyObject>, "HeavyObject must satisfy BroadcastElement");

// ===== Correctness Tests =====

TEST(SPMCBroadcastQueueTest, InvalidCapacityThrows) {
    EXPECT_THROW((SPMCBroadcastQueue<int, 2>(0)), std::invalid_argument);
    EXPECT_THROW((SPMCBroadcastQueue<int, 2>(1)), std::invalid_argument);
}

TEST(SPMCBroadcastQueueTest, CapacityRoundsUpToPowerOfTwo) {
    SPMCBroadcastQueue<int, 2> q(100);
    EXPECT_EQ(q.capacity(), 128u);
}

TEST(SPMCBroadcastQueueTest, SingleConsumer_FIFOOrdering) {
    SPMCBroadcastQueue<int, 1> q(16);
    for (int i = 0; i < 10; ++i) EXPECT_TRUE(q.try_publish(i));

    auto& c = q.consumer(0);
    for (int i = 0; i < 10; ++i) {
        int got = -1;
        ASSERT_TRUE(c.try_consume([&](const int& v) { got = v; }));
        EXPECT_EQ(got, i);
    }
    // Nothing left to read.
    EXPECT_FALSE(c.try_consume([](const int&) {}));
}

TEST(SPMCBroadcastQueueTest, AllConsumersReceiveAllItemsInOrder_SingleThreaded) {
    constexpr std::size_t N     = 4;
    constexpr std::size_t TOTAL = 500;

    // Capacity must cover the whole run: nothing drains concurrently here,
    // so a smaller ring would make try_publish spin forever once full.
    SPMCBroadcastQueue<int, N> q(TOTAL);
    for (int i = 0; i < static_cast<int>(TOTAL); ++i) {
        ASSERT_TRUE(q.try_publish(i));
    }

    // Every consumer independently sees every item, in the exact same order.
    for (std::size_t ci = 0; ci < N; ++ci) {
        auto& c = q.consumer(ci);
        for (int i = 0; i < static_cast<int>(TOTAL); ++i) {
            int got = -1;
            ASSERT_TRUE(c.try_consume([&](const int& v) { got = v; }))
                << "consumer " << ci << " missing item " << i;
            EXPECT_EQ(got, i) << "consumer " << ci << " out of order at item " << i;
        }
        EXPECT_FALSE(c.try_consume([](const int&) {}));
    }
}

TEST(SPMCBroadcastQueueTest, TryCopy_MatchesTryConsume) {
    SPMCBroadcastQueue<int, 2> q(16);
    for (int i = 0; i < 5; ++i) EXPECT_TRUE(q.try_publish(i * 10));

    auto& c0 = q.consumer(0);
    for (int i = 0; i < 5; ++i) {
        auto v = c0.try_copy();
        ASSERT_TRUE(v.has_value());
        EXPECT_EQ(*v, i * 10);
    }
    EXPECT_FALSE(c0.try_copy().has_value());
}

TEST(SPMCBroadcastQueueTest, FullQueueRejectsPublish_UntilSlowestConsumerCatchesUp) {
    constexpr std::size_t CAPACITY = 8;
    SPMCBroadcastQueue<int, 2> q(CAPACITY);

    for (std::size_t i = 0; i < CAPACITY; ++i)
        ASSERT_TRUE(q.try_publish(static_cast<int>(i)));

    // Ring is exactly full relative to both (idle) consumers.
    EXPECT_FALSE(q.try_publish(999));

    // Only consumer 0 reads -- consumer 1 (the slowest) still gates publish.
    int got = -1;
    ASSERT_TRUE(q.consumer(0).try_consume([&](const int& v) { got = v; }));
    EXPECT_EQ(got, 0);
    EXPECT_FALSE(q.try_publish(999));

    // Once the slowest consumer (1) also reads, one slot frees up.
    ASSERT_TRUE(q.consumer(1).try_consume([&](const int& v) { got = v; }));
    EXPECT_EQ(got, 0);
    EXPECT_TRUE(q.try_publish(999));
}

TEST(SPMCBroadcastQueueTest, IndexWrapAround) {
    constexpr std::size_t CAPACITY = 4;
    constexpr std::size_t TOTAL    = 50; // several full laps around the ring
    SPMCBroadcastQueue<int, 1> q(CAPACITY);
    auto& c = q.consumer(0);

    for (int i = 0; i < static_cast<int>(TOTAL); ++i) {
        ASSERT_TRUE(q.try_publish(i));
        int got = -1;
        ASSERT_TRUE(c.try_consume([&](const int& v) { got = v; }));
        EXPECT_EQ(got, i);
    }
}

TEST(SPMCBroadcastQueueTest, HeavyObjectType_AllConsumersSeeUncorruptedPayload) {
    constexpr std::size_t N     = 3;
    constexpr std::size_t TOTAL = 200;

    SPMCBroadcastQueue<HeavyObject, N> q(32);

    std::thread producer([&] {
        for (int i = 0; i < static_cast<int>(TOTAL); ++i)
            while (!q.try_publish(i)) std::this_thread::yield();
    });

    std::vector<std::thread> consumers;
    std::atomic<int> failures{0};
    for (std::size_t ci = 0; ci < N; ++ci) {
        consumers.emplace_back([&, ci] {
            auto& c = q.consumer(ci);
            int received = 0;
            while (received < static_cast<int>(TOTAL)) {
                bool ok = c.try_consume([&](const HeavyObject& obj) {
                    if (!obj.payload_matches_id()) failures.fetch_add(1);
                });
                if (ok) ++received;
                else std::this_thread::yield();
            }
        });
    }

    producer.join();
    for (auto& t : consumers) t.join();

    EXPECT_EQ(failures.load(), 0);
}

TEST(SPMCBroadcastQueueTest, BacklogApprox_ReflectsSlowestConsumer) {
    SPMCBroadcastQueue<int, 2> q(64);
    EXPECT_EQ(q.backlog_approx(), 0u);

    for (int i = 0; i < 5; ++i) ASSERT_TRUE(q.try_publish(i));
    EXPECT_EQ(q.backlog_approx(), 5u); // neither consumer has read anything yet

    int got = -1;
    q.consumer(0).try_consume([&](const int& v) { got = v; });
    EXPECT_EQ(q.backlog_approx(), 5u); // consumer 1 (idle) is still the slowest

    q.consumer(1).try_consume([&](const int& v) { got = v; });
    EXPECT_EQ(q.backlog_approx(), 4u); // both have now read exactly one item
    (void)got;
}

// Concurrent producer + N consumer threads: every consumer must independently
// receive every item, in the same order the producer published them.
TEST(SPMCBroadcastQueueTest, MultiThreaded_AllConsumersReceiveAllItemsInOrder) {
    constexpr std::size_t N     = 4;
    constexpr std::size_t TOTAL = 100'000;

    SPMCBroadcastQueue<int, N> q(4096);

    std::thread producer([&] {
        for (int i = 0; i < static_cast<int>(TOTAL); ++i)
            while (!q.try_publish(i)) std::this_thread::yield();
    });

    std::array<std::vector<int>, N> received;
    std::vector<std::thread> consumers;
    for (std::size_t ci = 0; ci < N; ++ci) {
        received[ci].reserve(TOTAL);
        consumers.emplace_back([&, ci] {
            auto& c = q.consumer(ci);
            while (received[ci].size() < TOTAL) {
                int v = -1;
                if (c.try_consume([&](const int& item) { v = item; }))
                    received[ci].push_back(v);
            }
        });
    }

    producer.join();
    for (auto& t : consumers) t.join();

    for (std::size_t ci = 0; ci < N; ++ci) {
        ASSERT_EQ(received[ci].size(), TOTAL) << "consumer " << ci;
        for (std::size_t i = 0; i < TOTAL; ++i)
            ASSERT_EQ(received[ci][i], static_cast<int>(i))
                << "consumer " << ci << " out of order at index " << i;
    }
}
