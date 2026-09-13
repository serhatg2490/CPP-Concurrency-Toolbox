#ifndef SPMC_OVERWRITE_QUEUE_HPP
#define SPMC_OVERWRITE_QUEUE_HPP

// SPMCOverwriteQueue.h — Single-Producer, Multi-Consumer LOSSY broadcast queue
//
// Design : Seqlock-per-slot ring buffer; the producer never waits
// Standard: C++23
//
// How this differs from SPMCBroadcastQueue:
//   SPMCBroadcastQueue is lossless -- try_publish() refuses to overwrite a
//   slot the SLOWEST consumer hasn't read yet. That guarantees delivery but
//   means one stalled consumer stalls the producer, and therefore every
//   other consumer with it (head-of-line blocking).
//
//   This queue makes the opposite trade: publish() ALWAYS succeeds and never
//   waits for anyone. A consumer that falls more than `capacity` items behind
//   gets its data overwritten -- but it detects that precisely and is told
//   exactly how many items it missed, rather than silently reading garbage.
//
//   Use this when the newest data matters and stale data is worthless
//   (market-data feeds, telemetry, sensor streams). Use SPMCBroadcastQueue
//   when every item must be delivered (journaling, order flow).
//
// Consequences of not gating on consumers:
//   - The producer does not need to know how many consumers exist, so N is
//     NOT a template parameter here. Consumers can be created at runtime and
//     simply stop being read from when they go away.
//   - The write position is producer-private and need not be atomic at all.
//   - Reads CANNOT be zero-copy. The slot can be overwritten at any instant,
//     so the item must be copied out before it can be safely handed to the
//     caller. This is why T is required to be trivially copyable.
//
// Slot protocol (a standard seqlock):
//   seq == (position << 1)       -> slot stably holds that position's item
//   seq == (position << 1) | 1   -> producer is writing that position now
//   Positions are 1-based, so the initial seq of 0 reads as "holds position
//   0", which never occurs -- a consumer waiting for position 1 correctly
//   sees Empty rather than mistaking a zeroed slot for real data.
//
// FORMAL CAVEAT: like every seqlock, the reader memcpy's the payload while
// the writer may concurrently be writing it, and only afterwards checks
// whether the read was torn. That concurrent access is a data race by the
// letter of the C++ memory model (the torn value is discarded, never used,
// but the race itself is formally UB). This is the same trade every
// production seqlock makes -- the Linux kernel's seqlock and Folly's among
// them -- and it is why T is restricted to trivially copyable types.
//
// ============================================================================

#include <atomic>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstring>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// detail_overwrite — internal helpers
// ---------------------------------------------------------------------------
namespace detail_overwrite {

#ifdef __cpp_lib_hardware_interference_size
    inline constexpr std::size_t cache_line =
        std::hardware_destructive_interference_size;
#else
    inline constexpr std::size_t cache_line = 64;
#endif

inline std::size_t check_and_round_capacity(std::size_t n) {
    if (n < 2)
        throw std::invalid_argument("SPMCOverwriteQueue: capacity must be >= 2");
    // One bit of every position is spent on the seqlock's in-progress flag,
    // so the usable position space is half of size_t.
    constexpr std::size_t max_cap = std::size_t{1} << (sizeof(std::size_t) * 8 - 2);
    if (n > max_cap)
        throw std::invalid_argument("SPMCOverwriteQueue: capacity exceeds maximum");
    return std::bit_ceil(n);
}

} // namespace detail_overwrite

// ---------------------------------------------------------------------------
// OverwriteElement concept
// ---------------------------------------------------------------------------
// trivially_copyable: the payload is moved in and out with std::memcpy under
// the seqlock -- a type with a non-trivial copy constructor could not survive
// being read while concurrently written, and a non-trivial destructor could
// not be run on a slot that gets overwritten without notice.
// default_initializable: the ring is pre-populated once at construction.
template <typename T>
concept OverwriteElement =
    std::is_trivially_copyable_v<T> &&
    std::default_initializable<T>;

// ---------------------------------------------------------------------------
// SPMCOverwriteQueue<T>
// ---------------------------------------------------------------------------
template <OverwriteElement T>
class SPMCOverwriteQueue {
private:
    // Padded to a cache line: the producer stores to `seq` twice per publish,
    // so without padding a consumer reading slot i would keep getting slot
    // i+1's line invalidated underneath it. Costs memory for small T; that is
    // the right default for a latency-oriented fan-out buffer.
    struct alignas(detail_overwrite::cache_line) Slot {
        std::atomic<std::size_t> seq{0};
        T                        data{};
    };

public:
    enum class Status {
        Ok,      // an item was written to `out`
        Empty,   // nothing new published at this consumer's position yet
        Lapped   // the producer overwrote items this consumer hadn't read;
                 // the cursor has been repositioned and missed() updated --
                 // nothing was written to `out`, call again to resume
    };

    // ------------------------------------------------------------------
    // Consumer — an independent read cursor. Obtained from
    // SPMCOverwriteQueue::make_consumer(); must be driven by one thread at
    // a time. The producer neither knows nor cares that it exists.
    // ------------------------------------------------------------------
    class Consumer {
    public:
        [[nodiscard]] Status try_read(T& out) noexcept {
            const Slot& s = queue_->slots_[pos_ & queue_->mask_];

            // acquire: pairs with the producer's release fence, so a stable
            // seq read here means the payload writes are visible below.
            const std::size_t s1 = s.seq.load(std::memory_order_acquire);

            if (s1 & 1)
                return Status::Empty;          // producer is mid-write here

            const std::size_t have = s1 >> 1;
            if (have < pos_)
                return Status::Empty;          // this position not published yet
            if (have > pos_)
                return lapped_to(have);        // overwritten before we got to it

            std::memcpy(&out, &s.data, sizeof(T));

            // Ensure the payload read above completes before re-checking seq;
            // if the producer touched this slot meanwhile, `out` is torn and
            // must be discarded.
            std::atomic_thread_fence(std::memory_order_acquire);
            const std::size_t s2 = s.seq.load(std::memory_order_relaxed);
            if (s1 != s2)
                return lapped_to(s2 >> 1);     // lapped while we were reading

            ++pos_;
            return Status::Ok;
        }

        // Cumulative number of items this consumer never saw because the
        // producer overwrote them first. Monotonically increasing.
        [[nodiscard]] std::size_t missed() const noexcept { return missed_; }

        // Next position this consumer will attempt to read (1-based).
        [[nodiscard]] std::size_t position() const noexcept { return pos_; }

    private:
        friend class SPMCOverwriteQueue;
        Consumer(const SPMCOverwriteQueue* q, std::size_t start) noexcept
            : queue_(q), pos_(start) {}

        // Resume as close to where we were as the ring still allows.
        //
        // The obvious choice -- jump to `newest`, the position the contended
        // slot holds now -- throws away far too much: that position is a
        // whole number of laps ahead, so on a capacity-8 ring a consumer one
        // item behind would skip eight. Instead resume from the OLDEST
        // position the ring can still hold, which loses only what was
        // genuinely overwritten.
        //
        // `published_` is read relaxed and may lag reality, which only makes
        // `target` conservative (too old). If that position turns out to have
        // been overwritten too, the next read simply detects the lap again
        // and re-adjusts -- the scheme is self-correcting.
        Status lapped_to(std::size_t newest) noexcept {
            const std::size_t pub = queue_->published_.load(std::memory_order_acquire);
            const std::size_t cap = queue_->capacity_;

            std::size_t target = (pub > cap) ? (pub - cap + 1) : 1;
            // `newest` is always ahead of pos_ on every path that lands here,
            // so falling back to it guarantees forward progress even if the
            // stale `published_` produced a target that wouldn't advance.
            if (target <= pos_)
                target = newest;

            if (target > pos_) {
                missed_ += target - pos_;
                pos_     = target;
            }
            return Status::Lapped;
        }

        const SPMCOverwriteQueue* queue_;
        std::size_t               pos_;
        std::size_t               missed_{0};
    };

    // ------------------------------------------------------------------
    // Construction / Destruction
    // ------------------------------------------------------------------

    // Actual capacity is std::bit_ceil(capacity), minimum 2.
    explicit SPMCOverwriteQueue(std::size_t capacity)
        : capacity_(detail_overwrite::check_and_round_capacity(capacity))
        , mask_(capacity_ - 1)
        , slots_(capacity_)
    {}

    ~SPMCOverwriteQueue() = default;

    SPMCOverwriteQueue(const SPMCOverwriteQueue&)            = delete;
    SPMCOverwriteQueue& operator=(const SPMCOverwriteQueue&) = delete;
    SPMCOverwriteQueue(SPMCOverwriteQueue&&)                 = delete;
    SPMCOverwriteQueue& operator=(SPMCOverwriteQueue&&)      = delete;

    // ------------------------------------------------------------------
    // Producer API (SINGLE PRODUCER — do not call concurrently)
    // ------------------------------------------------------------------

    // Always succeeds. Never blocks, never waits on a consumer, and has no
    // failure mode to handle -- that is the entire point of this queue.
    // Whatever it overwrites is lost, and whichever consumers hadn't read it
    // will find out via Status::Lapped.
    template <typename... Args>
        requires std::constructible_from<T, Args...>
    void publish(Args&&... args) {
        const std::size_t pos = ++write_pos_;   // positions are 1-based
        Slot& s = slots_[pos & mask_];

        // Mark the slot in-progress, then fence so that flag is visible
        // before any payload byte changes.
        s.seq.store((pos << 1) | 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);

        const T tmp(std::forward<Args>(args)...);
        std::memcpy(&s.data, &tmp, sizeof(T));

        // Fence so the payload is visible before the slot is marked stable.
        std::atomic_thread_fence(std::memory_order_release);
        s.seq.store(pos << 1, std::memory_order_relaxed);

        published_.store(pos, std::memory_order_relaxed);
    }

    // ------------------------------------------------------------------
    // Accessors
    // ------------------------------------------------------------------

    // A new consumer starts live -- it sees only items published from now
    // on, not the backlog still sitting in the ring. That matches the use
    // case this queue exists for: a late subscriber to a market-data feed
    // wants the current picture, not a replay of stale ticks.
    [[nodiscard]] Consumer make_consumer() const noexcept {
        return Consumer(this, published_.load(std::memory_order_relaxed) + 1);
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    // Total items published so far (non-linearisable hint for observability).
    [[nodiscard]] std::size_t published_count() const noexcept {
        return published_.load(std::memory_order_relaxed);
    }

private:
    alignas(detail_overwrite::cache_line) const std::size_t capacity_;
    const std::size_t                                       mask_;
    std::vector<Slot>                                       slots_;

    // Producer-private: no consumer reads it, so it need not be atomic.
    alignas(detail_overwrite::cache_line) std::size_t write_pos_{0};

    // Observability only, and the start point for a newly created consumer.
    std::atomic<std::size_t> published_{0};
};

#endif // SPMC_OVERWRITE_QUEUE_HPP
