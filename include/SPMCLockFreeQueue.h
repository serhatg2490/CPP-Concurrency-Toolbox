#ifndef SPMC_LOCKFREE_QUEUE_HPP
#define SPMC_LOCKFREE_QUEUE_HPP

// SPMCLockFreeQueue.hpp — Single-Producer, Multiple-Consumer lock-free queue
//
// Design : Vyukov sequence-per-slot (adapted from MPMC, restricted to SPSC producer path)
// Standard: C++23
//
// Guarantees:
//   - T must satisfy QueueElement (movable + noexcept move constructible)
//   - No default construction of T (placement-new + explicit destructor)
//   - Capacity is rounded up to the next power-of-two, minimum 2
//   - try_emplace is wait-free for the single producer
//   - try_pop   is lock-free for multiple consumers
//   - pop_wait / emplace_wait block via std::atomic::wait (futex on Linux,
//     WaitOnAddress on Windows) — zero syscall overhead when not actually waiting
//
// Memory layout (three cache lines to prevent false sharing):
//   Line 0  {capacity_, mask_, slots_}  — read-only after construction
//   Line 1  {head_}                     — written by consumers
//   Line 2  {tail_}                     — written by producer
//
// Slot sequence semantics (capacity == C):
//   sequence[i] == i         slot free, generation 0 (initial state)
//   sequence[i] == t + 1     slot Full: producer wrote at tail-index t, ready for consumer
//   sequence[i] == h + C     slot Empty: consumer read at head-index h, ready for next generation
//
// The sequence advances by C each round-trip, preventing ABA across SIZE_MAX operations.

#include <atomic>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <new>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <type_traits>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

// ---------------------------------------------------------------------------
// detail — internal helpers
// ---------------------------------------------------------------------------
namespace detail {

// Portable cache-line size.
// std::hardware_destructive_interference_size is 128 on Apple Silicon / POWER —
// using the constant avoids false sharing at the L2 coherence granule level.
#ifdef __cpp_lib_hardware_interference_size
    inline constexpr std::size_t cache_line =
        std::hardware_destructive_interference_size;
#else
    inline constexpr std::size_t cache_line = 64;
#endif

// Portable spin-wait hint.
// x86/x86-64 : PAUSE instruction (reduces pipeline contention, saves power in spin loops)
// AArch64/ARM : YIELD instruction
// fallback    : std::this_thread::yield() (OS scheduler hint)
[[gnu::always_inline]] inline void spin_hint() noexcept {
#if defined(_MSC_VER)
    #if defined(_M_X64) || defined(_M_IX86)
        _mm_pause();
    #elif defined(_M_ARM64) || defined(_M_ARM)
        __yield();
    #else
        std::this_thread::yield();
    #endif
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ volatile("yield" ::: "memory");
#else
    std::this_thread::yield();
#endif
}

// Validate and round up capacity. Called before any member initialisation to
// avoid the Security Auditor's Finding 2 (capacity=0 wrapping to SIZE_MAX).
inline std::size_t check_and_round_capacity(std::size_t n) {
    if (n < 2)
        throw std::invalid_argument("SPMCQueue: capacity must be >= 2");
    // Prevent overflow: bit_ceil(SIZE_MAX/2 + 1) wraps to 0.
    constexpr std::size_t max_cap = std::size_t{1} << (sizeof(std::size_t) * 8 - 1);
    if (n > max_cap)
        throw std::invalid_argument("SPMCQueue: capacity exceeds maximum");
    return std::bit_ceil(n); // C++20: next power-of-two >= n
}

} // namespace detail

// ---------------------------------------------------------------------------
// QueueElement concept
// ---------------------------------------------------------------------------
// noexcept move is a hard correctness requirement: once a consumer's CAS
// succeeds and the slot is claimed, there is no safe rollback if the move
// throws. Enforce this at compile time rather than silently corrupting state.
template <typename T>
concept QueueElement =
    std::movable<T> &&
    std::is_nothrow_move_constructible_v<T>;

// ---------------------------------------------------------------------------
// QueueError
// ---------------------------------------------------------------------------
enum class QueueError : std::uint8_t { Empty = 0, Full = 1 };

[[nodiscard]] constexpr std::string_view to_string(QueueError e) noexcept {
    switch (e) {
        case QueueError::Empty: return "Empty";
        case QueueError::Full:  return "Full";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------
// Slot<T> — one ring-buffer entry
// ---------------------------------------------------------------------------
namespace detail {

template <QueueElement T>
struct alignas(cache_line) Slot {
    // The sole atomic in this struct. Initialized to the slot's own array index
    // (done by SPMCQueue constructor, not here, because the index is not known
    // at struct construction time).
    std::atomic<std::size_t> sequence;

    // Uninitialized raw storage — T is constructed with placement-new only when
    // the producer writes, and destroyed explicitly by the consumer after reading.
    alignas(T) std::byte storage[sizeof(T)];

    [[nodiscard]] T*       ptr()       noexcept { return std::launder(reinterpret_cast<T*>(storage)); }
    [[nodiscard]] const T* ptr() const noexcept { return std::launder(reinterpret_cast<const T*>(storage)); }

    template <typename... Args>
    void construct(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args...>) {
        ::new (static_cast<void*>(storage)) T(std::forward<Args>(args)...);
    }

    void destroy() noexcept { ptr()->~T(); }

    // noexcept guaranteed by the QueueElement concept on T.
    [[nodiscard]] T move_out() noexcept {
        T tmp = std::move(*ptr());
        destroy();
        return tmp;
    }
};

} // namespace detail

// ---------------------------------------------------------------------------
// SPMCQueue<T>
// ---------------------------------------------------------------------------
template <QueueElement T>
class SPMCQueue {
public:
    // ------------------------------------------------------------------
    // Construction / Destruction
    // ------------------------------------------------------------------

    // Actual capacity is std::bit_ceil(capacity), minimum 2.
    explicit SPMCQueue(std::size_t capacity)
        : capacity_(detail::check_and_round_capacity(capacity))
        , mask_(capacity_ - 1)
        , slots_(allocate_slots(capacity_))
    {
        for (std::size_t i = 0; i < capacity_; ++i)
            slots_[i].sequence.store(i, std::memory_order_relaxed);
        // Publish all sequence initialisations before any concurrent access.
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }

    ~SPMCQueue() noexcept {
        // Walk [head, tail) and destroy any T still in a Full slot.
        // seq == pos + 1  ↔  Full state in Vyukov sequence scheme.
        const std::size_t h = head_.load(std::memory_order_relaxed);
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        for (std::size_t pos = h; pos != t; ++pos) {
            detail::Slot<T>& s = slots_[pos & mask_];
            if (s.sequence.load(std::memory_order_relaxed) == pos + 1)
                s.destroy();
        }
        destroy_slots(slots_, capacity_);
    }

    SPMCQueue(const SPMCQueue&)            = delete;
    SPMCQueue& operator=(const SPMCQueue&) = delete;
    SPMCQueue(SPMCQueue&&)                 = delete;
    SPMCQueue& operator=(SPMCQueue&&)      = delete;

    // ------------------------------------------------------------------
    // Static utility
    // ------------------------------------------------------------------
    [[nodiscard]] static std::size_t check_capacity(std::size_t n) {
        return detail::check_and_round_capacity(n);
    }

    // ------------------------------------------------------------------
    // Producer API  (SINGLE PRODUCER — do not call concurrently)
    // ------------------------------------------------------------------

    // Returns true on success, false if the queue is full.
    template <typename... Args>
        requires std::constructible_from<T, Args...>
    [[nodiscard]] bool try_emplace(Args&&... args)
        noexcept(std::is_nothrow_constructible_v<T, Args...>)
    {
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        detail::Slot<T>& slot = slots_[t & mask_];

        // acquire: synchronise with consumer's release store of (h + capacity_)
        // — ensures storage is fully reclaimed before we placement-new into it.
        const std::size_t seq = slot.sequence.load(std::memory_order_acquire);

        if (seq != t) [[unlikely]]
            return false; // seq > t: slot not yet consumed → queue full

        slot.construct(std::forward<Args>(args)...);

        // release: the constructed T must be visible to any consumer that
        // loads sequence with acquire and observes (t + 1).
        slot.sequence.store(t + 1, std::memory_order_release);
        // relaxed: only this (single) thread reads and writes tail_.
        tail_.store(t + 1, std::memory_order_relaxed);
        // notify_all: with multiple consumers, several may be parked in
        // pop_wait on this exact slot (all having observed the same stale
        // head_). notify_one would only guarantee waking one of them,
        // leaving the rest blocked until this slot is produced-to again.
        slot.sequence.notify_all();
        return true;
    }

    // Blocks until a free slot is available, then enqueues.
    // Uses std::atomic::wait (OS-assisted; no syscall when slot is free).
    template <typename... Args>
        requires std::constructible_from<T, Args...>
    void emplace_wait(Args&&... args)
        noexcept(std::is_nothrow_constructible_v<T, Args...>)
    {
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        detail::Slot<T>& slot = slots_[t & mask_];

        std::size_t seq = slot.sequence.load(std::memory_order_acquire);
        int backoff = 0;
        while (seq != t) {
            // Atomically: if sequence is still `seq`, suspend this thread.
            // A notify fired between our load and this call is not lost.
            slot.sequence.wait(seq, std::memory_order_acquire);
            seq = slot.sequence.load(std::memory_order_acquire);
            if (++backoff < 8)
                detail::spin_hint();
            else { std::this_thread::yield(); backoff = 0; }
        }

        slot.construct(std::forward<Args>(args)...);
        slot.sequence.store(t + 1, std::memory_order_release);
        tail_.store(t + 1, std::memory_order_relaxed);
        // See try_emplace: multiple consumers can be parked on this slot.
        slot.sequence.notify_all();
    }

    // ------------------------------------------------------------------
    // Consumer API  (MULTIPLE CONSUMERS — CAS on head_)
    // ------------------------------------------------------------------

    // Returns std::nullopt if the queue is empty.  Lock-free.
    [[nodiscard]] std::optional<T> try_pop() noexcept {
        std::size_t h = head_.load(std::memory_order_relaxed);

        for (;;) {
            detail::Slot<T>& slot = slots_[h & mask_];

            // acquire: synchronise with producer's release store of (t + 1)
            // so the constructed T is visible after a successful CAS.
            const std::size_t seq  = slot.sequence.load(std::memory_order_acquire);
            const std::size_t diff = seq - (h + 1); // unsigned subtraction

            if (diff == 0) [[likely]] {
                // seq == h+1: slot is Full.  Race to claim it.
                //   acq_rel on success : acquire slot data (belt-and-suspenders
                //                        with load above) + release head advance
                //                        so the producer sees the freed slot.
                //   relaxed on failure : h updated by CAS; we retry with fresh load.
                if (head_.compare_exchange_weak(
                        h, h + 1,
                        std::memory_order_acq_rel,
                        std::memory_order_relaxed)) [[likely]]
                {
                    T result = slot.move_out();
                    // release: destructor complete; producer may reclaim this slot.
                    slot.sequence.store(h + capacity_, std::memory_order_release);
                    // Wake producer blocked in emplace_wait on this slot.
                    slot.sequence.notify_one();
                    return result;
                }
                // Lost the CAS to another consumer. h updated; retry.
                detail::spin_hint();
                continue;
            }

            if (diff > std::size_t(-1) / 2) [[unlikely]] {
                // Unsigned wrap: seq < h+1 — producer hasn't written yet.
                return std::nullopt; // queue empty
            }

            // diff > 0, no wrap: seq > h+1 — h is stale (another consumer
            // already advanced head past h).  Reload and retry.
            h = head_.load(std::memory_order_relaxed);
            detail::spin_hint();
        }
    }

    // C++23 variant: returns std::expected<T, QueueError> for richer error handling.
    // try_pop never returns QueueError::Full; that variant exists for emplace paths.
    [[nodiscard]] std::expected<T, QueueError> try_pop_expected() noexcept {
        auto v = try_pop();
        if (v.has_value()) [[likely]] return std::move(*v);
        return std::unexpected(QueueError::Empty);
    }

    // Blocks until an item is available.
    // Uses std::atomic::wait — no syscall unless the queue is genuinely empty.
    [[nodiscard]] T pop_wait() noexcept {
        std::size_t h = head_.load(std::memory_order_relaxed);

        for (;;) {
            detail::Slot<T>& slot = slots_[h & mask_];
            std::size_t seq = slot.sequence.load(std::memory_order_acquire);
            int backoff = 0;

            while (true) {
                const std::size_t diff = seq - (h + 1);

                if (diff == 0) break; // slot is Full — proceed to CAS

                if (diff > std::size_t(-1) / 2) {
                    // seq < h+1: slot not yet written.  Block until it changes.
                    slot.sequence.wait(seq, std::memory_order_acquire);
                    seq = slot.sequence.load(std::memory_order_acquire);
                    if (++backoff < 8)
                        detail::spin_hint();
                    else { std::this_thread::yield(); backoff = 0; }
                } else {
                    // h is stale (another consumer moved past it). Reload outer.
                    h = head_.load(std::memory_order_relaxed);
                    goto retry_outer; // restart the outer loop with the new h
                }
            }

            if (head_.compare_exchange_weak(
                    h, h + 1,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)) [[likely]]
            {
                T result = slot.move_out();
                slot.sequence.store(h + capacity_, std::memory_order_release);
                slot.sequence.notify_one();
                return result;
            }
            // Lost CAS; h updated by compare_exchange_weak. Fall through to retry.
            detail::spin_hint();

            retry_outer:;
        }
    }

    // ------------------------------------------------------------------
    // Accessors
    // ------------------------------------------------------------------

    [[nodiscard]] std::size_t capacity()    const noexcept { return capacity_; }

    // O(1) non-linearisable hint.  Do NOT use for correctness decisions.
    [[nodiscard]] std::size_t size_approx() const noexcept {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        return t - h; // unsigned; correct because |t - h| <= capacity_ always holds
    }

    [[nodiscard]] bool empty() const noexcept { return size_approx() == 0; }

private:
    // ------------------------------------------------------------------
    // Cache-line-separated fields (see layout comment at top of file)
    // ------------------------------------------------------------------
    alignas(detail::cache_line) const std::size_t capacity_;
    const std::size_t                             mask_;
    detail::Slot<T>* const                        slots_;

    alignas(detail::cache_line) std::atomic<std::size_t> head_{0};
    alignas(detail::cache_line) std::atomic<std::size_t> tail_{0};

    // ------------------------------------------------------------------
    // Slot array lifecycle
    // ------------------------------------------------------------------

    // Allocate capacity slots with cache-line alignment.
    // std::make_unique<Slot<T>[]> would default-construct T in each slot —
    // we use aligned operator new + placement-new to avoid that.
    [[nodiscard]] static detail::Slot<T>* allocate_slots(std::size_t n) {
        void* raw = ::operator new(
            n * sizeof(detail::Slot<T>),
            std::align_val_t{detail::cache_line});
        auto* slots = static_cast<detail::Slot<T>*>(raw);
        for (std::size_t i = 0; i < n; ++i)
            ::new (&slots[i]) detail::Slot<T>; // constructs the std::atomic member
        return slots;
    }

    static void destroy_slots(detail::Slot<T>* slots, std::size_t n) noexcept {
        for (std::size_t i = 0; i < n; ++i)
            slots[i].~Slot<T>();
        ::operator delete(
            static_cast<void*>(slots),
            std::align_val_t{detail::cache_line});
    }
};

#endif // SPMC_LOCKFREE_QUEUE_HPP
