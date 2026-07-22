#pragma once

// ============================================================================
//  TscClock.hpp — RDTSCP-based high-resolution clock
// ============================================================================
//
//  Why use this instead of steady_clock?
//    steady_clock -> QPC -> ~50-100 ns overhead, ~100 ns granularity
//    TscClock     -> RDTSCP -> ~20-40 ns overhead, ~0.33 ns granularity (3 GHz TSC)
//
//  Constraints:
//    • Correct only on single-socket systems (TSC may drift on multi-socket)
//    • calibrate() must be called once at startup — takes ~10 ms
//    • Processor migration detection is possible via rdtscp_raw().processor_id
//
//  Supported: x86/x86-64 with MSVC/GCC/Clang.
//  Falls back to steady_clock on non-x86 architectures.
// ============================================================================

#include <chrono>
#include <cstdint>
#include <thread>

#if defined(_MSC_VER)
#  include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#  include <x86intrin.h>
#endif

namespace tsc {

// ── TSC read primitives ───────────────────────────────────────────────────────

#if defined(_MSC_VER) || defined(__x86_64__) || defined(__i386__)

// now_raw(): Lightest option — plain RDTSC, no fence.
// Use when compiler/data-flow dependency provides ordering.
// Recommended for cross-thread timestamps (producer -> consumer):
//   item.enqueue_ns = epoch_ns(now_raw()) -> try_emplace(item)
//   The compiler guarantees ordering because item is passed to try_emplace.
//   The CPU cannot move RDTSC forward due to the data dependency on item.enqueue_ns.
//   No LFENCE -> multi-consumer CAS storms do not stall the producer.
inline std::uint64_t now_raw() noexcept {
    return __rdtsc();
}

// start(): LFENCE + RDTSC — use to measure a narrow code block on the same thread.
// WARNING: Do NOT use as a producer timestamp in multi-consumer scenarios.
//   LFENCE waits for all pending loads (including CAS) to complete,
//   pulling consumer cache-line invalidations into the measurement.
inline std::uint64_t start() noexcept {
    _mm_lfence();
    return __rdtsc();
}

// end(): RDTSCP — waits for all prior instructions to retire; returns processor ID.
// Ideal as the end of a same-thread, same-block measurement.
inline std::uint64_t end(unsigned* processor_id = nullptr) noexcept {
    unsigned aux;
    std::uint64_t cycles = __rdtscp(&aux);
    if (processor_id) *processor_id = aux;
    return cycles;
}

// now(): RDTSCP without processor ID — general-purpose read.
inline std::uint64_t now() noexcept {
    unsigned aux;
    return __rdtscp(&aux);
}

constexpr bool available = true;

#else
// Non-x86 architectures: fall back to steady_clock

inline std::uint64_t start() noexcept {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}
inline std::uint64_t end(unsigned* = nullptr) noexcept { return start(); }
inline std::uint64_t now() noexcept { return start(); }

constexpr bool available = false;

#endif

// ── Calibration ───────────────────────────────────────────────────────────────
//
// The TSC frequency must be measured to convert cycles to nanoseconds.
// Computed by sleeping ~20 ms and comparing TSC delta to steady_clock delta.
//
// Call once from main() before any benchmarks. Not thread-safe.

inline std::uint64_t g_tsc_hz = 0;   // cycles per second — filled by calibrate()

inline void calibrate(std::chrono::milliseconds duration = std::chrono::milliseconds{20}) {
    using namespace std::chrono;

    const std::uint64_t tsc0 = now();
    const auto          ns0  = steady_clock::now();

    std::this_thread::sleep_for(duration);

    const std::uint64_t tsc1 = now();
    const auto          ns1  = steady_clock::now();

    const std::uint64_t elapsed_ns =
        static_cast<std::uint64_t>(duration_cast<nanoseconds>(ns1 - ns0).count());
    const std::uint64_t elapsed_tsc = tsc1 - tsc0;

    g_tsc_hz = elapsed_tsc * 1'000'000'000ULL / elapsed_ns;
}

// ── Cycle -> nanosecond conversion ────────────────────────────────────────────
//
// calibrate() must have been called first.
//
// IMPORTANT: Use only for DELTA (difference) cycle values.
//   Calling to_ns() on an absolute TSC value (cycles since boot) will overflow:
//     absolute_cycles * 1_000_000_000 -> uint64_t overflow!
//   Use epoch_ns() or compute a delta instead.

inline std::uint64_t to_ns(std::uint64_t delta_cycles) noexcept {
    // delta_cycles: difference between two tsc::start()/end() calls.
    // Reasonable measurement < 18 s -> delta < 54e9 cycles -> 54e9 * 1e9 = 5.4e19 < 2^63 ok
    return delta_cycles * 1'000'000'000ULL / g_tsc_hz;
}

// ── Epoch-based absolute timestamp ────────────────────────────────────────────
//
// Same semantics as now_ns() in BenchmarkMain.cpp:
//   - Usable after calibrate() + set_epoch()
//   - Returns nanoseconds elapsed since the epoch
//   - Difference of two calls gives the true elapsed time
//
// Why an epoch?
//   absolute_TSC * 1e9 -> overflow (billions of cycles since boot)
//   (absolute_TSC - epoch_cycles) * 1e9 -> small delta, no overflow

inline std::uint64_t g_epoch_cycles = 0;  // filled by set_epoch()

inline void set_epoch() noexcept { g_epoch_cycles = now(); }

// Nanoseconds since epoch; because producer and consumer share the same constant,
// latency = epoch_ns(t1) - epoch_ns(t0) = to_ns(t1_cycles - t0_cycles) ok
inline std::uint64_t epoch_ns(std::uint64_t absolute_cycles) noexcept {
    return to_ns(absolute_cycles - g_epoch_cycles);
}

// ── Measurement guide ─────────────────────────────────────────────────────────
//
// Same-thread delta:
//   auto t0 = tsc::start();
//   // ... code under measurement ...
//   auto t1 = tsc::end();
//   uint64_t lat_ns = tsc::to_ns(t1 - t0);
//
// Cross-thread timestamp (producer -> consumer):
//   // Once in main():  tsc::calibrate(); tsc::set_epoch();
//   // Producer:   Item{tsc::epoch_ns(tsc::now_raw()), ...}
//   // Consumer:   lat = tsc::epoch_ns(tsc::now()) - item.enqueue_ns

} // namespace tsc
