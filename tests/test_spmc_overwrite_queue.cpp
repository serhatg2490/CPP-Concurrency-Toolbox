#include <gtest/gtest.h>
#include <SPMCOverwriteQueue.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

using IntQueue  = SPMCOverwriteQueue<int>;
using IntStatus = IntQueue::Status;

// ===== Test types =====

// Not trivially copyable -- the seqlock cannot memcpy it, so the concept
// must reject it.
struct NonTrivial {
    std::string s;
};
static_assert(!OverwriteElement<NonTrivial>,
              "NonTrivial must not satisfy OverwriteElement");

// Trivially copyable but not default constructible -- the ring is
// pre-populated, so this must be rejected too.
struct NoDefault {
    int v;
    explicit NoDefault(int x) : v(x) {}
};
static_assert(!OverwriteElement<NoDefault>,
              "NoDefault must not satisfy OverwriteElement");

static_assert(OverwriteElement<int>, "int must satisfy OverwriteElement");

// Wide payload whose every word must agree with `id`. A torn seqlock read
// would show up here as a mismatch.
struct WidePayload {
    static constexpr std::size_t WORDS = 64;   // 512 bytes

    std::uint64_t                    id{0};
    std::array<std::uint64_t, WORDS> body{};

    WidePayload() = default;
    explicit WidePayload(std::uint64_t v) : id(v) { body.fill(v); }

    [[nodiscard]] bool consistent() const noexcept {
        for (auto w : body)
            if (w != id) return false;
        return true;
    }
};
static_assert(OverwriteElement<WidePayload>,
              "WidePayload must satisfy OverwriteElement");

// ===== Construction =====

TEST(SPMCOverwriteQueueTest, InvalidCapacityThrows) {
    EXPECT_THROW(IntQueue(0), std::invalid_argument);
    EXPECT_THROW(IntQueue(1), std::invalid_argument);
}

TEST(SPMCOverwriteQueueTest, CapacityRoundsUpToPowerOfTwo) {
    IntQueue q(100);
    EXPECT_EQ(q.capacity(), 128u);
}

// ===== Basic delivery =====

TEST(SPMCOverwriteQueueTest, EmptyQueueReportsEmpty) {
    IntQueue q(16);
    auto c = q.make_consumer();

    int out = -1;
    EXPECT_EQ(c.try_read(out), IntStatus::Empty);
    EXPECT_EQ(c.missed(), 0u);
}

TEST(SPMCOverwriteQueueTest, FIFOOrdering) {
    IntQueue q(16);
    auto c = q.make_consumer();

    for (int i = 0; i < 10; ++i) q.publish(i);

    for (int i = 0; i < 10; ++i) {
        int out = -1;
        ASSERT_EQ(c.try_read(out), IntStatus::Ok) << "item " << i;
        EXPECT_EQ(out, i);
    }

    int out = -1;
    EXPECT_EQ(c.try_read(out), IntStatus::Empty);
    EXPECT_EQ(c.missed(), 0u);
}

TEST(SPMCOverwriteQueueTest, IndexWrapAround) {
    IntQueue q(4);                       // tiny ring, many laps
    auto c = q.make_consumer();

    for (int i = 0; i < 50; ++i) {
        q.publish(i);
        int out = -1;
        ASSERT_EQ(c.try_read(out), IntStatus::Ok) << "item " << i;
        EXPECT_EQ(out, i);
    }
    EXPECT_EQ(c.missed(), 0u);
}

TEST(SPMCOverwriteQueueTest, AllConsumersReceiveAllItems_WhenKeepingUp) {
    constexpr int N     = 4;
    constexpr int TOTAL = 1000;

    IntQueue q(64);
    std::vector<IntQueue::Consumer> cs;
    for (int i = 0; i < N; ++i) cs.push_back(q.make_consumer());

    // Lockstep, so nobody ever falls a lap behind: every consumer must see
    // every item, exactly as SPMCBroadcastQueue would deliver it.
    for (int i = 0; i < TOTAL; ++i) {
        q.publish(i);
        for (int ci = 0; ci < N; ++ci) {
            int out = -1;
            ASSERT_EQ(cs[ci].try_read(out), IntStatus::Ok) << "consumer " << ci;
            EXPECT_EQ(out, i) << "consumer " << ci;
        }
    }
    for (int ci = 0; ci < N; ++ci)
        EXPECT_EQ(cs[ci].missed(), 0u) << "consumer " << ci;
}

// ===== The defining behavior: producer never blocks, loss is never silent =====

TEST(SPMCOverwriteQueueTest, ProducerNeverBlocksOnStalledConsumer) {
    IntQueue q(8);

    auto stalled = q.make_consumer();    // created, then never read from

    // Thousands of times the ring's capacity. Unlike
    // SPMCBroadcastQueue::try_publish, publish() has no failure mode at all --
    // that is the entire point of this queue.
    for (int i = 0; i < 10'000; ++i) q.publish(i);

    EXPECT_EQ(q.published_count(), 10'000u);

    // Loss is only *discovered* on read, so an idle consumer still reports
    // nothing missed and has not moved.
    EXPECT_EQ(stalled.position(), 1u);
    EXPECT_EQ(stalled.missed(), 0u);
}

TEST(SPMCOverwriteQueueTest, LappedConsumerReportsExactMissCount) {
    constexpr std::size_t CAPACITY = 8;
    IntQueue q(CAPACITY);
    auto c = q.make_consumer();          // waiting for position 1

    // Exactly one more than the ring holds, so precisely one item
    // (position 1, value 1) is overwritten.
    for (int i = 1; i <= 9; ++i) q.publish(i);

    int out = -1;
    ASSERT_EQ(c.try_read(out), IntStatus::Lapped);
    EXPECT_EQ(c.missed(), 1u) << "only position 1 was overwritten";

    // Resumes at the OLDEST surviving item, not a whole lap ahead.
    ASSERT_EQ(c.try_read(out), IntStatus::Ok);
    EXPECT_EQ(out, 2);

    for (int expected = 3; expected <= 9; ++expected) {
        ASSERT_EQ(c.try_read(out), IntStatus::Ok) << "expected " << expected;
        EXPECT_EQ(out, expected);
    }
    EXPECT_EQ(c.try_read(out), IntStatus::Empty);
    EXPECT_EQ(c.missed(), 1u) << "no further loss once caught up";
}

TEST(SPMCOverwriteQueueTest, FastConsumerUnaffectedByStalledOne) {
    constexpr int TOTAL = 1000;
    IntQueue q(8);

    auto fast    = q.make_consumer();
    auto stalled = q.make_consumer();

    // In SPMCBroadcastQueue the stalled consumer would gate the producer and
    // starve this one. Here the fast consumer loses nothing.
    for (int i = 0; i < TOTAL; ++i) {
        q.publish(i);
        int out = -1;
        ASSERT_EQ(fast.try_read(out), IntStatus::Ok) << "item " << i;
        ASSERT_EQ(out, i);
    }
    EXPECT_EQ(fast.missed(), 0u);

    // The stalled one now finds out how much it lost, in one shot.
    int out = -1;
    ASSERT_EQ(stalled.try_read(out), IntStatus::Lapped);
    EXPECT_EQ(stalled.missed(), TOTAL - static_cast<int>(q.capacity()));
    ASSERT_EQ(stalled.try_read(out), IntStatus::Ok);
    EXPECT_EQ(out, TOTAL - static_cast<int>(q.capacity()));   // oldest survivor
}

TEST(SPMCOverwriteQueueTest, NewConsumerStartsLive_NotFromBacklog) {
    IntQueue q(16);
    for (int i = 0; i < 5; ++i) q.publish(i);

    auto late = q.make_consumer();       // joins after those 5

    int out = -1;
    EXPECT_EQ(late.try_read(out), IntStatus::Empty) << "must not replay backlog";

    q.publish(42);
    ASSERT_EQ(late.try_read(out), IntStatus::Ok);
    EXPECT_EQ(out, 42);
    EXPECT_EQ(late.missed(), 0u);
}

// Consumers are independent cursors, so one can be created while the
// producer is already far along without disturbing anyone else.
TEST(SPMCOverwriteQueueTest, ConsumersAreIndependent) {
    IntQueue q(64);
    auto early = q.make_consumer();

    for (int i = 0; i < 10; ++i) q.publish(i);

    auto late = q.make_consumer();
    q.publish(99);

    int out = -1;
    for (int i = 0; i < 10; ++i) {                 // early still sees the backlog
        ASSERT_EQ(early.try_read(out), IntStatus::Ok);
        EXPECT_EQ(out, i);
    }
    ASSERT_EQ(early.try_read(out), IntStatus::Ok);
    EXPECT_EQ(out, 99);

    ASSERT_EQ(late.try_read(out), IntStatus::Ok);  // late only sees the new one
    EXPECT_EQ(out, 99);
    EXPECT_EQ(late.try_read(out), IntStatus::Empty);
}

// ===== Concurrency =====

// The seqlock must never hand out a torn payload, and every item a consumer
// did not receive must be accounted for in missed() -- the queue is lossy,
// but never silently lossy.
TEST(SPMCOverwriteQueueTest, MultiThreaded_NoTornReads_AndLossIsAccounted) {
    using WideQueue  = SPMCOverwriteQueue<WidePayload>;
    using WideStatus = WideQueue::Status;

    constexpr int N     = 3;
    constexpr int TOTAL = 200'000;

    WideQueue q(1024);

    // Created up front so every consumer provably starts at position 1.
    std::vector<WideQueue::Consumer> cs;
    for (int i = 0; i < N; ++i) cs.push_back(q.make_consumer());

    std::atomic<bool> start{false};
    std::atomic<bool> done{false};
    std::atomic<int>  torn{0};

    std::array<std::size_t, N> received{};

    std::vector<std::thread> consumers;
    for (int ci = 0; ci < N; ++ci) {
        consumers.emplace_back([&, ci] {
            while (!start.load(std::memory_order_acquire)) {}

            WidePayload item;
            std::size_t got      = 0;
            bool        draining = false;

            for (;;) {
                const auto st = cs[ci].try_read(item);
                if (st == WideStatus::Ok) {
                    if (!item.consistent())
                        torn.fetch_add(1, std::memory_order_relaxed);
                    ++got;
                    continue;
                }
                if (st == WideStatus::Lapped) continue;   // cursor already moved

                // Empty. Once the producer is finished, an Empty is final.
                if (draining) break;
                if (done.load(std::memory_order_acquire)) draining = true;
            }
            received[ci] = got;
        });
    }

    std::thread producer([&] {
        while (!start.load(std::memory_order_acquire)) {}
        for (int i = 1; i <= TOTAL; ++i)
            q.publish(static_cast<std::uint64_t>(i));
        done.store(true, std::memory_order_release);
    });

    start.store(true, std::memory_order_release);
    producer.join();
    for (auto& t : consumers) t.join();

    EXPECT_EQ(torn.load(), 0) << "seqlock handed out a torn payload";
    EXPECT_EQ(q.published_count(), static_cast<std::size_t>(TOTAL));

    for (int ci = 0; ci < N; ++ci) {
        // Every consumer started at position 1 and ends at `position()`, so
        // what it saw plus what it lost must exactly cover that span.
        EXPECT_EQ(received[ci] + cs[ci].missed(), cs[ci].position() - 1)
            << "consumer " << ci << " accounting does not balance";
        EXPECT_LE(cs[ci].position() - 1, static_cast<std::size_t>(TOTAL))
            << "consumer " << ci << " ran past the producer";
        EXPECT_GT(received[ci], 0u)
            << "consumer " << ci << " received nothing at all";
    }
}
