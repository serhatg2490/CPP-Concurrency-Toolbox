#ifndef SPMC_BROADCAST_QUEUE_HPP
#define SPMC_BROADCAST_QUEUE_HPP

// SPMCBroadcastQueue.h — Single-Producer, Multi-Consumer FAN-OUT queue
//
// Design : Disruptor-style ring buffer with N independent read cursors
// Standard: C++23
//
// How this differs from SPMCQueue (SPMCLockFreeQueue.h):
//   SPMCQueue delivers each item to exactly ONE consumer (they race via
//   CAS for who gets it). This queue delivers each item to EVERY
//   consumer -- there is no race, because there is no competition: each
//   consumer reads at its own pace, gated only by the producer not being
//   allowed to overwrite a slot the slowest consumer hasn't read yet.
//
//   Consequently there is no per-slot sequence/CAS on the consumer side.
//   A single producer-published cursor (tail_) plus N independent
//   consumer cursors are enough. Items are never moved out of the ring --
//   the producer overwrites each slot in place (move-assignment) once
//   every consumer has passed it, and consumers read in place via a
//   callback (zero-copy) or, for convenience, via a copying try_copy().
//
// Guarantees:
//   - T must be default_initializable (to pre-populate the ring once at
//     construction) and movable with a non-throwing move-assignment
//     (used to overwrite a slot in place on every publish).
//   - Capacity is rounded up to the next power-of-two, minimum 2.
//   - Consumer count N is fixed at compile time.
//   - try_publish is wait-free with respect to consumer progress, as long
//     as the slowest consumer is less than `capacity` items behind; if it
//     is exactly `capacity` behind, try_publish returns false (mirrors
//     the other queues' "full" semantics, gated by the slowest reader
//     instead of a single head cursor).
//   - try_consume is lock-free; each Consumer handle must be used by
//     exactly one thread (it is not itself thread-safe).
//
// Memory layout:
//   {capacity_, mask_, slots_}          — read-only after construction
//   {tail_}                             — written by the producer
//   {cursors_[i]}                       — each written only by consumer i,
//                                          each on its own cache line
//
// ============================================================================

#include <array>
#include <atomic>
#include <bit>
#include <concepts>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// detail_broadcast — internal helpers (kept separate from SPMCLockFreeQueue.h's
// own `detail` namespace so this header has no dependency on it)
// ---------------------------------------------------------------------------
namespace detail_broadcast {

#ifdef __cpp_lib_hardware_interference_size
    inline constexpr std::size_t cache_line =
        std::hardware_destructive_interference_size;
#else
    inline constexpr std::size_t cache_line = 64;
#endif

inline std::size_t check_and_round_capacity(std::size_t n) {
    if (n < 2)
        throw std::invalid_argument("SPMCBroadcastQueue: capacity must be >= 2");
    constexpr std::size_t max_cap = std::size_t{1} << (sizeof(std::size_t) * 8 - 1);
    if (n > max_cap)
        throw std::invalid_argument("SPMCBroadcastQueue: capacity exceeds maximum");
    return std::bit_ceil(n);
}

} // namespace detail_broadcast

// ---------------------------------------------------------------------------
// BroadcastElement concept
// ---------------------------------------------------------------------------
// default_initializable: the ring is pre-populated once at construction.
// movable + nothrow move-assignable: the producer overwrites a live slot
// in place (`slots_[i] = std::move(tmp)`) on every publish -- a throwing
// move-assignment here would leave the slot in an unknown state while a
// consumer might already be reading it.
template <typename T>
concept BroadcastElement =
    std::default_initializable<T> &&
    std::movable<T> &&
    std::is_nothrow_move_assignable_v<T>;

// ---------------------------------------------------------------------------
// SPMCBroadcastQueue<T, N>
// ---------------------------------------------------------------------------
template <BroadcastElement T, std::size_t N>
    requires (N >= 1)
class SPMCBroadcastQueue {
private:
    struct alignas(detail_broadcast::cache_line) Cursor {
        std::atomic<std::size_t> pos{0};
    };

public:
    // ------------------------------------------------------------------
    // Consumer — a handle into one of the N read cursors. Copyable (it's
    // just two pointers) but must only ever be driven from one thread at
    // a time; obtained via SPMCBroadcastQueue::consumer(index).
    // ------------------------------------------------------------------
    class Consumer {
    public:
        // Zero-copy read: fn is invoked with `const T&` referencing the
        // slot in place. Returns false if nothing new has been published
        // since this consumer's last read. The reference passed to fn is
        // valid only for the duration of the call -- the cursor (and thus
        // the producer's permission to overwrite this slot) only advances
        // after fn returns, so do not store the reference beyond fn.
        template <typename F>
            requires std::invocable<F&, const T&>
        bool try_consume(F&& fn) {
            const std::size_t pos = cursor_->pos.load(std::memory_order_relaxed);

            // acquire: synchronizes with the producer's release store of
            // tail_, so the slot write below is guaranteed visible.
            if (pos >= queue_->tail_.load(std::memory_order_acquire))
                return false; // nothing published at this position yet

            std::forward<F>(fn)(queue_->slots_[pos & queue_->mask_]);

            // release: only after fn has finished reading the slot may the
            // producer's gating check (an acquire load of this cursor)
            // observe that this slot is free to be overwritten.
            cursor_->pos.store(pos + 1, std::memory_order_release);
            return true;
        }

        // Convenience wrapper: copies the item out instead of taking a
        // callback. Prefer try_consume() when T is non-trivial to copy --
        // that's the whole reason this queue type exists.
        [[nodiscard]] std::optional<T> try_copy()
            requires std::copy_constructible<T>
        {
            std::optional<T> out;
            try_consume([&out](const T& item) { out = item; });
            return out;
        }

        // How many items this consumer has read so far (a monotonically
        // increasing, non-linearisable hint -- do not use for correctness).
        [[nodiscard]] std::size_t position() const noexcept {
            return cursor_->pos.load(std::memory_order_relaxed);
        }

    private:
        friend class SPMCBroadcastQueue;
        Consumer(SPMCBroadcastQueue* q, Cursor* c) noexcept
            : queue_(q), cursor_(c) {}

        SPMCBroadcastQueue* queue_;
        Cursor*             cursor_;
    };

    // ------------------------------------------------------------------
    // Construction / Destruction
    // ------------------------------------------------------------------

    // Actual capacity is std::bit_ceil(capacity), minimum 2.
    explicit SPMCBroadcastQueue(std::size_t capacity)
        : capacity_(detail_broadcast::check_and_round_capacity(capacity))
        , mask_(capacity_ - 1)
        , slots_(capacity_)
        , cursors_{}
        , consumer_handles_(make_consumers(std::make_index_sequence<N>{}))
    {}

    // Trivial: slots_ holds N live T objects for the queue's entire
    // lifetime (overwritten in place, never placement-new'd/destroyed per
    // item), so std::vector<T>'s own destructor is all that's needed --
    // unlike SPMCQueue there is no manual walk-and-destroy step.
    ~SPMCBroadcastQueue() = default;

    SPMCBroadcastQueue(const SPMCBroadcastQueue&)            = delete;
    SPMCBroadcastQueue& operator=(const SPMCBroadcastQueue&) = delete;
    SPMCBroadcastQueue(SPMCBroadcastQueue&&)                 = delete;
    SPMCBroadcastQueue& operator=(SPMCBroadcastQueue&&)      = delete;

    // ------------------------------------------------------------------
    // Producer API (SINGLE PRODUCER — do not call concurrently)
    // ------------------------------------------------------------------

    // Returns false if the slowest consumer is a full lap (capacity items)
    // behind -- i.e. publishing would overwrite a slot that consumer
    // hasn't read yet.
    template <typename... Args>
        requires std::constructible_from<T, Args...>
    [[nodiscard]] bool try_publish(Args&&... args) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);

        // Gating: find the slowest consumer's position. N is small and
        // fixed at compile time, so this is a short, unrollable loop --
        // no allocation, no locking, just N acquire loads.
        std::size_t min_cursor = cursors_[0].pos.load(std::memory_order_acquire);
        for (std::size_t i = 1; i < N; ++i) {
            const std::size_t c = cursors_[i].pos.load(std::memory_order_acquire);
            if (c < min_cursor) min_cursor = c;
        }
        if (tail - min_cursor >= capacity_)
            return false; // full: slowest consumer hasn't vacated this slot

        slots_[tail & mask_] = T(std::forward<Args>(args)...);

        // release: publishes both the slot write above and makes this
        // item visible to every consumer's acquire load of tail_.
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // ------------------------------------------------------------------
    // Accessors
    // ------------------------------------------------------------------

    [[nodiscard]] Consumer& consumer(std::size_t index) noexcept {
        return consumer_handles_[index];
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    // O(1) non-linearisable hint: how far ahead the producer is of its
    // single slowest consumer. Do NOT use for correctness decisions.
    [[nodiscard]] std::size_t backlog_approx() const noexcept {
        std::size_t min_cursor = cursors_[0].pos.load(std::memory_order_relaxed);
        for (std::size_t i = 1; i < N; ++i) {
            const std::size_t c = cursors_[i].pos.load(std::memory_order_relaxed);
            if (c < min_cursor) min_cursor = c;
        }
        return tail_.load(std::memory_order_relaxed) - min_cursor;
    }

private:
    template <std::size_t... Is>
    std::array<Consumer, N> make_consumers(std::index_sequence<Is...>) {
        return { Consumer(this, &cursors_[Is])... };
    }

    // ------------------------------------------------------------------
    // Cache-line-separated fields
    // ------------------------------------------------------------------
    alignas(detail_broadcast::cache_line) const std::size_t capacity_;
    const std::size_t                                       mask_;
    std::vector<T>                                          slots_;

    alignas(detail_broadcast::cache_line) std::atomic<std::size_t> tail_{0};
    std::array<Cursor, N>   cursors_;          // each Cursor is independently padded
    std::array<Consumer, N> consumer_handles_; // constructed after cursors_ (declaration order)
};

#endif // SPMC_BROADCAST_QUEUE_HPP
