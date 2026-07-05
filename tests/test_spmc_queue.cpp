#include <gtest/gtest.h>
#include <SPMCLockFreeQueue.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

// ===== Test types =====

// Copy-only: no move ctor declared, so QueueElement's std::movable is
// satisfied via copy-as-move fallback (defaulted copy ctor is noexcept here).
struct CopyOnly {
    int value{0};
    CopyOnly() = default;
    explicit CopyOnly(int v) : value(v) {}
    CopyOnly(const CopyOnly&) = default;
    CopyOnly& operator=(const CopyOnly&) = default;
};

// Move-only: copy explicitly deleted.
struct MoveOnly {
    int value{0};
    MoveOnly() = default;
    explicit MoveOnly(int v) : value(v) {}
    MoveOnly(const MoveOnly&) = delete;
    MoveOnly& operator=(const MoveOnly&) = delete;
    MoveOnly(MoveOnly&&) noexcept = default;
    MoveOnly& operator=(MoveOnly&&) noexcept = default;
};

// QueueElement requires std::movable && is_nothrow_move_constructible_v.
// This type satisfies neither and must be rejected by the concept.
struct NonMovable {
    NonMovable() = default;
    NonMovable(const NonMovable&) = delete;
    NonMovable& operator=(const NonMovable&) = delete;
    NonMovable(NonMovable&&) = delete;
    NonMovable& operator=(NonMovable&&) = delete;
};

static_assert(
    !QueueElement<NonMovable>,
    "NonMovable must not satisfy SPMCQueue's QueueElement concept"
);

// Light object: small trivially-movable aggregate.
struct LightObject {
    int a{0};
    int b{0};
    LightObject() = default;
    LightObject(int a_, int b_) : a(a_), b(b_) {}
};

// Heavy object: large payload to exercise non-trivial move cost, with an
// id + payload-consistency check to detect any corruption during moves.
struct HeavyObject {
    static constexpr std::size_t PAYLOAD_WORDS = 256; // ~2KB payload

    int id{0};
    std::array<std::uint64_t, PAYLOAD_WORDS> payload{};

    HeavyObject() = default;
    explicit HeavyObject(int id_) : id(id_) {
        payload.fill(static_cast<std::uint64_t>(id_));
    }
    HeavyObject(HeavyObject&&) noexcept = default;
    HeavyObject& operator=(HeavyObject&&) noexcept = default;
    HeavyObject(const HeavyObject&) = delete;
    HeavyObject& operator=(const HeavyObject&) = delete;

    [[nodiscard]] bool payload_matches_id() const noexcept {
        for (auto w : payload)
            if (w != static_cast<std::uint64_t>(id))
                return false;
        return true;
    }
};

// Tracks construction/destruction counts to verify the destructor correctly
// drains any elements still resident in the queue (Full slots between head/tail).
struct LifecycleTracker {
    inline static std::atomic<int> alive_count{0};

    int value{0};

    explicit LifecycleTracker(int v) noexcept : value(v) {
        alive_count.fetch_add(1, std::memory_order_relaxed);
    }
    LifecycleTracker(LifecycleTracker&& other) noexcept : value(other.value) {
        alive_count.fetch_add(1, std::memory_order_relaxed);
    }
    LifecycleTracker& operator=(LifecycleTracker&& other) noexcept {
        value = other.value;
        return *this;
    }
    LifecycleTracker(const LifecycleTracker&) = delete;
    LifecycleTracker& operator=(const LifecycleTracker&) = delete;
    ~LifecycleTracker() { alive_count.fetch_add(-1, std::memory_order_relaxed); }
};

namespace {
    // Leaves one core for the single producer where possible.
    unsigned consumer_thread_count() {
        unsigned hc = std::thread::hardware_concurrency();
        if (hc == 0) return 3;
        return std::clamp(hc > 1 ? hc - 1 : hc, 2u, 6u);
    }
}

// ===== Correctness Tests =====

TEST(SPMCQueueTest, InvalidCapacityThrows) {
    EXPECT_THROW(SPMCQueue<int>(0), std::invalid_argument);
    EXPECT_THROW(SPMCQueue<int>(1), std::invalid_argument);
    EXPECT_NO_THROW(SPMCQueue<int>(2));
    EXPECT_NO_THROW(SPMCQueue<int>(1024));
}

TEST(SPMCQueueTest, CapacityRoundsUpToPowerOfTwo) {
    EXPECT_EQ(SPMCQueue<int>(2).capacity(), 2u);
    EXPECT_EQ(SPMCQueue<int>(3).capacity(), 4u);
    EXPECT_EQ(SPMCQueue<int>(5).capacity(), 8u);
    EXPECT_EQ(SPMCQueue<int>(1024).capacity(), 1024u);
}

TEST(SPMCQueueTest, CheckCapacityStaticUtility) {
    EXPECT_EQ(SPMCQueue<int>::check_capacity(2), 2u);
    EXPECT_EQ(SPMCQueue<int>::check_capacity(5), 8u);
    EXPECT_EQ(SPMCQueue<int>::check_capacity(1024), 1024u);
    EXPECT_THROW(SPMCQueue<int>::check_capacity(0), std::invalid_argument);
    EXPECT_THROW(SPMCQueue<int>::check_capacity(1), std::invalid_argument);
}

TEST(SPMCQueueTest, QueueErrorToString) {
    EXPECT_EQ(to_string(QueueError::Empty), "Empty");
    EXPECT_EQ(to_string(QueueError::Full), "Full");
}

TEST(SPMCQueueTest, EmptyQueueReturnsNullopt) {
    SPMCQueue<int> queue(4);
    EXPECT_FALSE(queue.try_pop().has_value());
}

// Unlike the SPSC queue (capacity-1 usable slots), SPMCQueue's Vyukov scheme
// allows all `capacity_` slots to hold an element simultaneously.
TEST(SPMCQueueTest, FullQueueRejectsEmplace) {
    SPMCQueue<int> queue(4);
    EXPECT_TRUE(queue.try_emplace(1));
    EXPECT_TRUE(queue.try_emplace(2));
    EXPECT_TRUE(queue.try_emplace(3));
    EXPECT_TRUE(queue.try_emplace(4));
    EXPECT_FALSE(queue.try_emplace(5));
}

TEST(SPMCQueueTest, FIFOOrdering_SingleConsumer) {
    SPMCQueue<int> queue(8);
    for (int i = 0; i < 6; ++i) {
        EXPECT_TRUE(queue.try_emplace(i));
    }
    for (int i = 0; i < 6; ++i) {
        auto v = queue.try_pop();
        ASSERT_TRUE(v.has_value());
        EXPECT_EQ(*v, i);
    }
    EXPECT_FALSE(queue.try_pop().has_value());
}

// Fill and drain multiple times to exercise index wrap-around and the
// slot-sequence generation advance (seq == h + capacity_ on reclaim).
TEST(SPMCQueueTest, IndexWrapAround) {
    SPMCQueue<int> queue(4);
    for (int round = 0; round < 8; ++round) {
        for (int i = 1; i <= 4; ++i) {
            EXPECT_TRUE(queue.try_emplace(round * 10 + i));
        }
        for (int i = 1; i <= 4; ++i) {
            auto v = queue.try_pop();
            ASSERT_TRUE(v.has_value());
            EXPECT_EQ(*v, round * 10 + i);
        }
        EXPECT_FALSE(queue.try_pop().has_value());
    }
}

TEST(SPMCQueueTest, SizeApproxAndEmpty) {
    SPMCQueue<int> queue(8);
    EXPECT_TRUE(queue.empty());
    EXPECT_EQ(queue.size_approx(), 0u);

    EXPECT_TRUE(queue.try_emplace(1));
    EXPECT_TRUE(queue.try_emplace(2));
    EXPECT_FALSE(queue.empty());
    EXPECT_EQ(queue.size_approx(), 2u);

    EXPECT_TRUE(queue.try_pop().has_value());
    EXPECT_EQ(queue.size_approx(), 1u);

    EXPECT_TRUE(queue.try_pop().has_value());
    EXPECT_TRUE(queue.empty());
    EXPECT_EQ(queue.size_approx(), 0u);
}

TEST(SPMCQueueTest, TryPopExpected_ReturnsUnexpectedWhenEmpty) {
    SPMCQueue<int> queue(4);
    auto result = queue.try_pop_expected();
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), QueueError::Empty);
}

TEST(SPMCQueueTest, TryPopExpected_ReturnsValueWhenPresent) {
    SPMCQueue<int> queue(4);
    EXPECT_TRUE(queue.try_emplace(7));
    auto result = queue.try_pop_expected();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 7);
}

// try_emplace forwards multiple args directly to T's constructor.
TEST(SPMCQueueTest, TryEmplaceInPlace) {
    SPMCQueue<LightObject> queue(4);
    EXPECT_TRUE(queue.try_emplace(3, 7));
    auto v = queue.try_pop();
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->a, 3);
    EXPECT_EQ(v->b, 7);
}

// Destructor must walk [head, tail) and destroy any element still resident
// in a Full slot — verified via construction/destruction bookkeeping.
TEST(SPMCQueueTest, DestructorDrainsRemainingElements) {
    LifecycleTracker::alive_count.store(0, std::memory_order_relaxed);
    {
        SPMCQueue<LifecycleTracker> queue(8);
        for (int i = 0; i < 5; ++i) {
            EXPECT_TRUE(queue.try_emplace(i));
        }
        EXPECT_EQ(LifecycleTracker::alive_count.load(), 5);

        EXPECT_TRUE(queue.try_pop().has_value());
        EXPECT_TRUE(queue.try_pop().has_value());
        EXPECT_EQ(LifecycleTracker::alive_count.load(), 3);
    } // ~SPMCQueue() must destroy the remaining 3 elements
    EXPECT_EQ(LifecycleTracker::alive_count.load(), 0);
}

TEST(SPMCQueueTest, PopWait_BlocksUntilItemAvailable) {
    SPMCQueue<int> queue(4);
    std::atomic<bool> popped{false};

    std::thread consumer([&]() {
        int v = queue.pop_wait();
        EXPECT_EQ(v, 42);
        popped.store(true, std::memory_order_release);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(popped.load(std::memory_order_acquire));

    EXPECT_TRUE(queue.try_emplace(42));
    consumer.join();
    EXPECT_TRUE(popped.load(std::memory_order_acquire));
}

TEST(SPMCQueueTest, EmplaceWait_BlocksUntilSlotFree) {
    SPMCQueue<int> queue(2); // capacity 2, both slots fillable
    EXPECT_TRUE(queue.try_emplace(1));
    EXPECT_TRUE(queue.try_emplace(2));

    std::atomic<bool> emplaced{false};
    std::thread producer([&]() {
        queue.emplace_wait(3);
        emplaced.store(true, std::memory_order_release);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(emplaced.load(std::memory_order_acquire));

    auto v1 = queue.try_pop(); // frees a slot, should wake the producer
    ASSERT_TRUE(v1.has_value());
    EXPECT_EQ(*v1, 1);

    producer.join();
    EXPECT_TRUE(emplaced.load(std::memory_order_acquire));

    auto v2 = queue.try_pop();
    ASSERT_TRUE(v2.has_value());
    EXPECT_EQ(*v2, 2);
    auto v3 = queue.try_pop();
    ASSERT_TRUE(v3.has_value());
    EXPECT_EQ(*v3, 3);
}

// ===== Different Types =====

TEST(SPMCQueueTest, StandardIntType) {
    SPMCQueue<int> queue(4);
    int x = 42;
    EXPECT_TRUE(queue.try_emplace(x));   // lvalue
    EXPECT_TRUE(queue.try_emplace(99));  // rvalue
    EXPECT_EQ(*queue.try_pop(), 42);
    EXPECT_EQ(*queue.try_pop(), 99);
}

TEST(SPMCQueueTest, StandardStringType) {
    SPMCQueue<std::string> queue(4);

    std::string lval = "hello";
    EXPECT_TRUE(queue.try_emplace(lval));                  // lvalue copy
    EXPECT_TRUE(queue.try_emplace(std::string{"world"}));  // rvalue move
    EXPECT_TRUE(queue.try_emplace("literal"));             // forwarded ctor

    EXPECT_EQ(*queue.try_pop(), "hello");
    EXPECT_EQ(*queue.try_pop(), "world");
    EXPECT_EQ(*queue.try_pop(), "literal");
}

TEST(SPMCQueueTest, CopyOnlyType) {
    SPMCQueue<CopyOnly> queue(4);
    CopyOnly obj{42};
    EXPECT_TRUE(queue.try_emplace(obj)); // lvalue -> copy
    auto v = queue.try_pop();
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->value, 42);
    EXPECT_EQ(obj.value, 42); // original untouched
}

TEST(SPMCQueueTest, MoveOnlyType) {
    SPMCQueue<MoveOnly> queue(4);
    MoveOnly m{55};
    EXPECT_TRUE(queue.try_emplace(std::move(m)));
    EXPECT_TRUE(queue.try_emplace(MoveOnly{88}));
    EXPECT_EQ(queue.try_pop()->value, 55);
    EXPECT_EQ(queue.try_pop()->value, 88);
}

TEST(SPMCQueueTest, LightObjectType) {
    SPMCQueue<LightObject> queue(4);
    EXPECT_TRUE(queue.try_emplace(1, 2));
    EXPECT_TRUE(queue.try_emplace(3, 4));
    auto a = queue.try_pop();
    auto b = queue.try_pop();
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(a->a, 1); EXPECT_EQ(a->b, 2);
    EXPECT_EQ(b->a, 3); EXPECT_EQ(b->b, 4);
}

TEST(SPMCQueueTest, HeavyObjectType) {
    SPMCQueue<HeavyObject> queue(4);
    EXPECT_TRUE(queue.try_emplace(7));
    EXPECT_TRUE(queue.try_emplace(99));

    auto a = queue.try_pop();
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->id, 7);
    EXPECT_TRUE(a->payload_matches_id());

    auto b = queue.try_pop();
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->id, 99);
    EXPECT_TRUE(b->payload_matches_id());
}

// ===== Multi-Consumer Correctness =====

// Single producer, several consumers racing on try_pop(): every item must be
// received by exactly one consumer, with none lost or duplicated.
TEST(SPMCQueueTest, MultipleConsumers_TryPop_AllItemsReceivedExactlyOnce) {
    constexpr std::size_t CAPACITY = 1 << 10;
    constexpr std::size_t TOTAL    = 200'000;

    SPMCQueue<int> queue(CAPACITY);
    std::vector<std::atomic<int>> seen(TOTAL);
    std::atomic<std::size_t> popped_count{0};

    const unsigned num_consumers = consumer_thread_count();
    std::vector<std::thread> consumers;
    consumers.reserve(num_consumers);
    for (unsigned c = 0; c < num_consumers; ++c) {
        consumers.emplace_back([&]() {
            while (popped_count.load(std::memory_order_relaxed) < TOTAL) {
                auto v = queue.try_pop();
                if (v.has_value()) {
                    seen[static_cast<std::size_t>(*v)].fetch_add(1, std::memory_order_relaxed);
                    popped_count.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    std::thread producer([&]() {
        for (std::size_t i = 0; i < TOTAL; ++i) {
            while (!queue.try_emplace(static_cast<int>(i)));
        }
    });

    producer.join();
    for (auto& t : consumers) t.join();

    EXPECT_EQ(popped_count.load(), TOTAL);
    for (std::size_t i = 0; i < TOTAL; ++i) {
        ASSERT_EQ(seen[i].load(), 1) << "index " << i << " received an unexpected number of times";
    }
}

// Same guarantee, but exercised through the blocking pop_wait()/notify path
// instead of the try_pop() spin path. A sentinel value (-1) per consumer
// tells each thread when to stop waiting.
TEST(SPMCQueueTest, MultipleConsumers_PopWait_AllItemsReceivedExactlyOnce) {
    constexpr std::size_t CAPACITY = 256;
    constexpr std::size_t TOTAL    = 50'000;
    constexpr int SENTINEL = -1;

    SPMCQueue<int> queue(CAPACITY);
    std::vector<std::atomic<int>> seen(TOTAL);
    std::atomic<std::size_t> popped_count{0};

    const unsigned num_consumers = consumer_thread_count();
    std::vector<std::thread> consumers;
    consumers.reserve(num_consumers);
    for (unsigned c = 0; c < num_consumers; ++c) {
        consumers.emplace_back([&]() {
            for (;;) {
                int v = queue.pop_wait();
                if (v == SENTINEL) return;
                seen[static_cast<std::size_t>(v)].fetch_add(1, std::memory_order_relaxed);
                popped_count.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    std::thread producer([&]() {
        for (std::size_t i = 0; i < TOTAL; ++i) {
            queue.emplace_wait(static_cast<int>(i));
        }
        for (unsigned c = 0; c < num_consumers; ++c) {
            queue.emplace_wait(SENTINEL);
        }
    });

    producer.join();
    for (auto& t : consumers) t.join();

    EXPECT_EQ(popped_count.load(), TOTAL);
    for (std::size_t i = 0; i < TOTAL; ++i) {
        ASSERT_EQ(seen[i].load(), 1) << "index " << i << " received an unexpected number of times";
    }
}

// Concurrent consumers racing on a heavy, non-trivial-to-move type: verifies
// no torn/partial moves occur under contention (payload must always match id).
TEST(SPMCQueueTest, MultipleConsumers_HeavyObject_NoCorruption) {
    constexpr std::size_t CAPACITY = 128;
    constexpr std::size_t TOTAL    = 20'000;

    SPMCQueue<HeavyObject> queue(CAPACITY);
    std::vector<std::atomic<int>> seen(TOTAL);
    std::atomic<std::size_t> popped_count{0};
    std::atomic<bool> corruption_detected{false};

    const unsigned num_consumers = consumer_thread_count();
    std::vector<std::thread> consumers;
    consumers.reserve(num_consumers);
    for (unsigned c = 0; c < num_consumers; ++c) {
        consumers.emplace_back([&]() {
            while (popped_count.load(std::memory_order_relaxed) < TOTAL) {
                auto v = queue.try_pop();
                if (v.has_value()) {
                    if (!v->payload_matches_id())
                        corruption_detected.store(true, std::memory_order_relaxed);
                    seen[static_cast<std::size_t>(v->id)].fetch_add(1, std::memory_order_relaxed);
                    popped_count.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    std::thread producer([&]() {
        for (std::size_t i = 0; i < TOTAL; ++i) {
            while (!queue.try_emplace(static_cast<int>(i)));
        }
    });

    producer.join();
    for (auto& t : consumers) t.join();

    EXPECT_FALSE(corruption_detected.load());
    EXPECT_EQ(popped_count.load(), TOTAL);
    for (std::size_t i = 0; i < TOTAL; ++i) {
        ASSERT_EQ(seen[i].load(), 1) << "index " << i << " received an unexpected number of times";
    }
}

// ===== Performance Tests =====

namespace {
    using Clock = std::chrono::high_resolution_clock;

    template <typename Fn>
    double measure_seconds(Fn&& fn) {
        auto t0 = Clock::now();
        fn();
        return std::chrono::duration<double>(Clock::now() - t0).count();
    }
}

// Single-threaded baseline: raw push+pop throughput without thread overhead.
TEST(SPMCQueuePerfTest, SingleThreaded_FillAndDrain) {
    constexpr std::size_t CAPACITY = 1 << 12; // 4096 slots
    constexpr std::size_t ROUNDS   = 2000;

    SPMCQueue<int> queue(CAPACITY);

    double elapsed = measure_seconds([&]() {
        for (std::size_t r = 0; r < ROUNDS; ++r) {
            for (std::size_t i = 0; i < CAPACITY; ++i) {
                (void)queue.try_emplace(static_cast<int>(i));
            }
            for (std::size_t i = 0; i < CAPACITY; ++i) {
                (void)queue.try_pop();
            }
        }
    });

    double total_ops = static_cast<double>(ROUNDS) * CAPACITY * 2;
    std::cout << "[Perf] Single-threaded fill+drain:      "
              << std::fixed << std::setprecision(1)
              << total_ops / elapsed / 1e6 << " M ops/sec\n";
}

// SPMC throughput: 1 producer + N consumer threads racing on try_pop().
TEST(SPMCQueuePerfTest, MultipleConsumers_Int_Throughput) {
    constexpr std::size_t CAPACITY = 1 << 14;
    constexpr std::size_t TOTAL    = 2'000'000;

    SPMCQueue<int> queue(CAPACITY);
    std::atomic<bool> go{false};
    std::atomic<std::size_t> popped_count{0};

    const unsigned num_consumers = consumer_thread_count();
    std::vector<std::thread> consumers;
    consumers.reserve(num_consumers);
    for (unsigned c = 0; c < num_consumers; ++c) {
        consumers.emplace_back([&]() {
            while (!go.load(std::memory_order_acquire));
            while (popped_count.load(std::memory_order_relaxed) < TOTAL) {
                if (queue.try_pop().has_value())
                    popped_count.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    std::thread producer([&]() {
        while (!go.load(std::memory_order_acquire));
        for (std::size_t i = 0; i < TOTAL; ++i) {
            while (!queue.try_emplace(static_cast<int>(i)));
        }
    });

    auto t0 = Clock::now();
    go.store(true, std::memory_order_release);

    producer.join();
    for (auto& t : consumers) t.join();
    double elapsed = std::chrono::duration<double>(Clock::now() - t0).count();

    std::cout << "[Perf] SPMC int throughput (" << num_consumers << " consumers): "
              << std::fixed << std::setprecision(1)
              << TOTAL / elapsed / 1e6 << " M ops/sec\n";
}

// SPMC throughput with strings: exercises heap allocation and move semantics.
TEST(SPMCQueuePerfTest, MultipleConsumers_String_Throughput) {
    constexpr std::size_t CAPACITY = 1 << 12;
    constexpr std::size_t TOTAL    = 300'000;

    SPMCQueue<std::string> queue(CAPACITY);
    std::atomic<bool> go{false};
    std::atomic<std::size_t> popped_count{0};

    const unsigned num_consumers = consumer_thread_count();
    std::vector<std::thread> consumers;
    consumers.reserve(num_consumers);
    for (unsigned c = 0; c < num_consumers; ++c) {
        consumers.emplace_back([&]() {
            while (!go.load(std::memory_order_acquire));
            while (popped_count.load(std::memory_order_relaxed) < TOTAL) {
                if (queue.try_pop().has_value())
                    popped_count.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    std::thread producer([&]() {
        while (!go.load(std::memory_order_acquire));
        for (std::size_t i = 0; i < TOTAL; ++i) {
            while (!queue.try_emplace(std::to_string(i)));
        }
    });

    auto t0 = Clock::now();
    go.store(true, std::memory_order_release);

    producer.join();
    for (auto& t : consumers) t.join();
    double elapsed = std::chrono::duration<double>(Clock::now() - t0).count();

    std::cout << "[Perf] SPMC string throughput (" << num_consumers << " consumers): "
              << std::fixed << std::setprecision(1)
              << TOTAL / elapsed / 1e3 << " K ops/sec\n";
}

// SPMC throughput with a heavy (~2KB) payload type.
TEST(SPMCQueuePerfTest, MultipleConsumers_HeavyObject_Throughput) {
    constexpr std::size_t CAPACITY = 1 << 10;
    constexpr std::size_t TOTAL    = 200'000;

    SPMCQueue<HeavyObject> queue(CAPACITY);
    std::atomic<bool> go{false};
    std::atomic<std::size_t> popped_count{0};

    const unsigned num_consumers = consumer_thread_count();
    std::vector<std::thread> consumers;
    consumers.reserve(num_consumers);
    for (unsigned c = 0; c < num_consumers; ++c) {
        consumers.emplace_back([&]() {
            while (!go.load(std::memory_order_acquire));
            while (popped_count.load(std::memory_order_relaxed) < TOTAL) {
                if (queue.try_pop().has_value())
                    popped_count.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    std::thread producer([&]() {
        while (!go.load(std::memory_order_acquire));
        for (std::size_t i = 0; i < TOTAL; ++i) {
            while (!queue.try_emplace(static_cast<int>(i)));
        }
    });

    auto t0 = Clock::now();
    go.store(true, std::memory_order_release);

    producer.join();
    for (auto& t : consumers) t.join();
    double elapsed = std::chrono::duration<double>(Clock::now() - t0).count();

    std::cout << "[Perf] SPMC heavy-object throughput (" << num_consumers << " consumers): "
              << std::fixed << std::setprecision(1)
              << TOTAL / elapsed / 1e6 << " M ops/sec\n";
}
