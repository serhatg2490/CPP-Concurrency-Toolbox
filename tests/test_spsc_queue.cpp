#include <gtest/gtest.h>
#include <SPSCLockFreeQueue.h>

#include <atomic>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>

// ===== Test types =====

// Copy-only: user-declared copy ctor suppresses implicit move generation.
// Rvalue arguments fall back to copy ctor via const T& binding.
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
    MoveOnly(MoveOnly&&) = default;
    MoveOnly& operator=(MoveOnly&&) = default;
};

// SPSCQueue concept: requires copy_constructible || move_constructible.
// This type satisfies neither and must be rejected by the concept.
struct NonCopyNonMove {
    NonCopyNonMove() = default;
    NonCopyNonMove(const NonCopyNonMove&) = delete;
    NonCopyNonMove& operator=(const NonCopyNonMove&) = delete;
    NonCopyNonMove(NonCopyNonMove&&) = delete;
    NonCopyNonMove& operator=(NonCopyNonMove&&) = delete;
};

static_assert(
    !(std::is_copy_constructible_v<NonCopyNonMove> || std::is_move_constructible_v<NonCopyNonMove>),
    "NonCopyNonMove must not satisfy SPSCQueue's concept requirement"
);

// ===== Correctness Tests =====

TEST(SPSCQueueTest, InvalidCapacityThrows) {
    EXPECT_THROW(SPSCQueue<int>(0),   std::invalid_argument);
    EXPECT_THROW(SPSCQueue<int>(3),   std::invalid_argument);
    EXPECT_THROW(SPSCQueue<int>(5),   std::invalid_argument);
    EXPECT_THROW(SPSCQueue<int>(100), std::invalid_argument);
    EXPECT_NO_THROW(SPSCQueue<int>(2));
    EXPECT_NO_THROW(SPSCQueue<int>(4));
    EXPECT_NO_THROW(SPSCQueue<int>(1024));
}

TEST(SPSCQueueTest, EmptyQueueReturnsNullopt) {
    SPSCQueue<int> queue(4);
    EXPECT_FALSE(queue.try_pop().has_value());
}

// With capacity=4 the ring buffer holds capacity-1 = 3 elements.
TEST(SPSCQueueTest, FullQueueReturnsFalse) {
    SPSCQueue<int> queue(4);
    EXPECT_TRUE(queue.try_emplace(1));
    EXPECT_TRUE(queue.try_emplace(2));
    EXPECT_TRUE(queue.try_emplace(3));
    EXPECT_FALSE(queue.try_emplace(4));
}

TEST(SPSCQueueTest, FIFOOrdering) {
    SPSCQueue<int> queue(8);
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(queue.try_emplace(i));
    }
    for (int i = 0; i < 5; ++i) {
        auto val = queue.try_pop();
        ASSERT_TRUE(val.has_value());
        EXPECT_EQ(*val, i);
    }
    EXPECT_FALSE(queue.try_pop().has_value());
}

// Fill and drain multiple times to exercise index wrap-around.
TEST(SPSCQueueTest, IndexWrapAround) {
    SPSCQueue<int> queue(4);
    for (int round = 0; round < 8; ++round) {
        for (int i = 1; i <= 3; ++i) {
            EXPECT_TRUE(queue.try_emplace(round * 10 + i));
        }
        for (int i = 1; i <= 3; ++i) {
            auto v = queue.try_pop();
            ASSERT_TRUE(v.has_value());
            EXPECT_EQ(*v, round * 10 + i);
        }
        EXPECT_FALSE(queue.try_pop().has_value());
    }
}

TEST(SPSCQueueTest, StandardIntType_LValue) {
    SPSCQueue<int> queue(4);
    int x = 42;
    EXPECT_TRUE(queue.try_emplace(x));
    auto v = queue.try_pop();
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 42);
}

TEST(SPSCQueueTest, StandardIntType_RValue) {
    SPSCQueue<int> queue(4);
    EXPECT_TRUE(queue.try_emplace(99));
    auto v = queue.try_pop();
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 99);
}

TEST(SPSCQueueTest, StandardStringType) {
    SPSCQueue<std::string> queue(4);

    std::string lval = "hello";
    EXPECT_TRUE(queue.try_emplace(lval));           // lvalue copy
    EXPECT_TRUE(queue.try_emplace(std::string{"world"}));  // rvalue move
    EXPECT_TRUE(queue.try_emplace("literal"));      // forwarded to string ctor

    EXPECT_EQ(*queue.try_pop(), "hello");
    EXPECT_EQ(*queue.try_pop(), "world");
    EXPECT_EQ(*queue.try_pop(), "literal");
}

TEST(SPSCQueueTest, CopyOnlyType_LValue) {
    SPSCQueue<CopyOnly> queue(4);
    CopyOnly obj{42};
    EXPECT_TRUE(queue.try_emplace(obj));  // lvalue → copy
    auto v = queue.try_pop();
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->value, 42);
    EXPECT_EQ(obj.value, 42);  // original untouched
}

TEST(SPSCQueueTest, CopyOnlyType_RValue) {
    SPSCQueue<CopyOnly> queue(4);
    // No move ctor: rvalue binds to const T&, copy ctor is used.
    EXPECT_TRUE(queue.try_emplace(CopyOnly{77}));
    auto v = queue.try_pop();
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->value, 77);
}

TEST(SPSCQueueTest, MoveOnlyType_RValue) {
    SPSCQueue<MoveOnly> queue(4);
    MoveOnly m{55};
    EXPECT_TRUE(queue.try_emplace(std::move(m)));  // explicit rvalue
    EXPECT_TRUE(queue.try_emplace(MoveOnly{88}));  // temporary rvalue
    EXPECT_EQ(queue.try_pop()->value, 55);
    EXPECT_EQ(queue.try_pop()->value, 88);
}

// try_emplace forwards multiple args directly to T's constructor (in-place construction).
TEST(SPSCQueueTest, TryEmplaceInPlace) {
    struct Point {
        int x{0}, y{0};
        Point() = default;
        Point(int x, int y) : x(x), y(y) {}
        Point(const Point&) = default;
        Point& operator=(const Point&) = default;
    };

    SPSCQueue<Point> queue(4);
    EXPECT_TRUE(queue.try_emplace(3, 7));
    auto v = queue.try_pop();
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->x, 3);
    EXPECT_EQ(v->y, 7);
}

// Runtime verification that NonCopyNonMove does not satisfy the concept.
// The commented line below is a compile error — intentionally left for documentation.
TEST(SPSCQueueTest, ConceptNotSatisfiedForNonConstructible) {
    constexpr bool satisfies =
        std::is_copy_constructible_v<NonCopyNonMove> ||
        std::is_move_constructible_v<NonCopyNonMove>;

    EXPECT_FALSE(satisfies);

    // SPSCQueue<NonCopyNonMove> q(4);  // concept constraint not satisfied → compile error
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

// Single-threaded baseline: measures raw push+pop throughput without thread overhead.
TEST(SPSCQueuePerfTest, SingleThreaded_FillAndDrain) {
    constexpr size_t CAPACITY = 1 << 12;           // 4096 slots
    constexpr size_t SLOTS    = CAPACITY - 1;       // usable slots per cycle
    constexpr size_t ROUNDS   = 2000;

    SPSCQueue<int> queue(CAPACITY);

    double elapsed = measure_seconds([&]() {
        for (size_t r = 0; r < ROUNDS; ++r) {
            for (size_t i = 0; i < SLOTS; ++i) {
                queue.try_emplace(static_cast<int>(i));
            }
            for (size_t i = 0; i < SLOTS; ++i) {
                (void)queue.try_pop();
            }
        }
    });

    double total_ops = static_cast<double>(ROUNDS) * SLOTS * 2;  // push + pop counted separately
    std::cout << "[Perf] Single-threaded fill+drain: "
              << std::fixed << std::setprecision(1)
              << total_ops / elapsed / 1e6 << " M ops/sec\n";
}

// SPSC throughput with integers: producer and consumer in separate threads, spinning.
TEST(SPSCQueuePerfTest, MultiThreaded_Int_Throughput) {
    constexpr size_t CAPACITY = 1 << 14;   // 16384 slots
    constexpr size_t TOTAL    = 2'000'000;

    SPSCQueue<int> queue(CAPACITY);
    std::atomic<bool> go{false};
    size_t consumed = 0;

    std::thread producer([&]() {
        while (!go.load(std::memory_order_acquire));
        for (size_t i = 0; i < TOTAL; ++i) {
            while (!queue.try_emplace(static_cast<int>(i)));
        }
    });

    auto t0 = Clock::now();
    go.store(true, std::memory_order_release);

    while (consumed < TOTAL) {
        if (queue.try_pop().has_value()) {
            ++consumed;
        }
    }

    double elapsed = std::chrono::duration<double>(Clock::now() - t0).count();
    producer.join();

    std::cout << "[Perf] SPSC int throughput:    "
              << std::fixed << std::setprecision(1)
              << TOTAL / elapsed / 1e6 << " M ops/sec\n";
}

// SPSC throughput with strings: exercises heap allocation and move semantics.
TEST(SPSCQueuePerfTest, MultiThreaded_String_Throughput) {
    constexpr size_t CAPACITY = 1 << 12;   // 4096 slots
    constexpr size_t TOTAL    = 300'000;

    SPSCQueue<std::string> queue(CAPACITY);
    std::atomic<bool> go{false};
    size_t consumed = 0;

    std::thread producer([&]() {
        while (!go.load(std::memory_order_acquire));
        for (size_t i = 0; i < TOTAL; ++i) {
            while (!queue.try_emplace(std::to_string(i)));
        }
    });

    auto t0 = Clock::now();
    go.store(true, std::memory_order_release);

    while (consumed < TOTAL) {
        if (queue.try_pop().has_value()) {
            ++consumed;
        }
    }

    double elapsed = std::chrono::duration<double>(Clock::now() - t0).count();
    producer.join();

    std::cout << "[Perf] SPSC string throughput: "
              << std::fixed << std::setprecision(1)
              << TOTAL / elapsed / 1e3 << " K ops/sec\n";
}
