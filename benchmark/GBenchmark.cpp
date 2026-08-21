// ============================================================================
//  GBenchmark.cpp — SPMC queue measurements with Google Benchmark
// ============================================================================
//
//  Three measurement types:
//
//  [RT]   Roundtrip (single thread) — GB's natural ns/op metric
//         Same thread produces and consumes; no scheduling effect.
//         Shows the raw data-structure cost: slot write + slot read.
//
//  [Sat]  Saturated latency — N consumers in background, producer in GB loop
//         Same semantics as BenchmarkMain.cpp Scenario 1.
//         GB's ns/op -> producer throughput; the meaningful numbers are the
//         custom counters: Mean_ns, P50_ns, P99_ns, P99.9_ns, P99.99_ns
//
//  [Mech] Mechanical latency — 500 ns rate-limited producer
//         Same semantics as BenchmarkMain.cpp Scenario 2.
//         Queue is almost always empty; measures pure transit time.
//
//  Running:
//    spmc_gbenchmark.exe
//    spmc_gbenchmark.exe --benchmark_counters_tabular=true
//    spmc_gbenchmark.exe --benchmark_out=results.json --benchmark_out_format=json
//    spmc_gbenchmark.exe --benchmark_filter="Mech"
//
// ============================================================================

#include <benchmark/benchmark.h>

#include <SPMCBroadcastQueue.h>
#include <SPMCLockFreeQueue.h>
#include <SPSCLockFreeQueue.h>
#include "LatencyRecorder.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

// ── Platform: Windows ────────────────────────────────────────────────────────
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>

static void pin_thread(int core) {
    if (core >= 0)
        SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR(1) << core);
}
static void elevate_thread() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
}
static void reset_thread_affinity() {
    SetThreadAffinityMask(GetCurrentThread(), ~DWORD_PTR(0));
}

// ── Platform: Linux / macOS ──────────────────────────────────────────────────
#else
#  include <pthread.h>
#  include <sched.h>
#  include <unistd.h>

static void pin_thread(int core) {
    if (core < 0) return;
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(core, &s);
    pthread_setaffinity_np(pthread_self(), sizeof(s), &s);
}
static void elevate_thread() {
    // SCHED_FIFO: real-time scheduler -- requires root or CAP_SYS_NICE.
    // Falls through silently if permission is denied.
    struct sched_param p{}; p.sched_priority = 99;
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &p);
}
// A new thread inherits its creator's CPU affinity mask at creation time.
// The GB driver thread runs every registered benchmark sequentially and
// pins itself to PROD_CORE for each one -- so by the time a LATER benchmark
// spawns its consumer threads, the driver (and thus each new consumer) can
// already be born restricted to PROD_CORE alone. Under real SCHED_FIFO 99
// with RT-throttling relaxed, a consumer stuck sharing PROD_CORE with a
// producer that never yields can never even reach its own pin_thread() call
// -- a permanent livelock. Reset to "all cores" before spawning so children
// are always free to migrate to their own target core.
static void reset_thread_affinity() {
    cpu_set_t s; CPU_ZERO(&s);
    const long n = sysconf(_SC_NPROCESSORS_ONLN);
    for (long c = 0; c < n; ++c) CPU_SET(static_cast<int>(c), &s);
    pthread_setaffinity_np(pthread_self(), sizeof(s), &s);
}
#endif

// ── Core assignment constants — Intel Core Ultra 5 125H ──────────────────────
// Same topology as BenchmarkMain.cpp / TscBenchmarkMain.cpp, so results are
// comparable across all three harnesses. GB itself drives each benchmark
// sequentially on the calling thread -- there is no separate idle "main"
// thread here, so that thread takes the producer role and is pinned to
// PROD_CORE inside each BM_* function instead.
static constexpr int PROD_CORE = 5;   // P-core 0, HT-B
static constexpr int CONSUMER_CORES[] = { 1, 3, 6, 8 }; // P-core1, P-core2, P-core3, E-core0

// ── Helpers ──────────────────────────────────────────────────────────────────

struct Item {
    std::int64_t enqueue_ns = 0;
    std::int64_t payload    = 0;
};

static std::int64_t now_ns() noexcept {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

// ── Benchmark 1: Roundtrip (single thread) ───────────────────────────────────
//
// Same thread produces + consumes -> no thread synchronization cost.
// Shows two things:
//   • Slot write + read latency (cache line round trip)
//   • Sequence/counter update cost
//
// GB reports this automatically as ns/op.
// Expected: ~10-50 ns/op (L1 cache access + atomic store/load)

template <typename Queue>
static void BM_Roundtrip(benchmark::State& state) {
    pin_thread(PROD_CORE);
    elevate_thread();
    Queue q(4096);
    std::int64_t i = 0;
    for (auto _ : state) {
        Item item{0LL, i++};
        (void)q.try_emplace(item);  // queue never fills (1 emplace + 1 pop)
        auto r = q.try_pop();
        benchmark::DoNotOptimize(r);
    }
    state.SetItemsProcessed(state.iterations());
}

BENCHMARK_TEMPLATE(BM_Roundtrip, SPSCQueue<Item>)      ->Name("RT/SPSC");
BENCHMARK_TEMPLATE(BM_Roundtrip, SPMCQueue<Item>)      ->Name("RT/Vyukov");

// Broadcast queue has a different API (try_publish + per-index consumer
// handles, N fixed at compile time) so it gets its own roundtrip function
// rather than reusing BM_Roundtrip<Queue>.
template <typename T, std::size_t N>
static void BM_BroadcastRoundtrip(benchmark::State& state) {
    pin_thread(PROD_CORE);
    elevate_thread();
    SPMCBroadcastQueue<T, N> q(4096);
    auto& c = q.consumer(0);
    std::int64_t i = 0;
    for (auto _ : state) {
        T item{0LL, i++};
        (void)q.try_publish(item);
        bool got = c.try_consume([](const T&) {});
        benchmark::DoNotOptimize(got);
    }
    state.SetItemsProcessed(state.iterations());
}

BENCHMARK_TEMPLATE(BM_BroadcastRoundtrip, Item, 1) ->Name("RT/Broadcast");

// ── Benchmarks 2 & 3: Multi-thread latency ───────────────────────────────────
//
// Design:
//   • N consumer threads run in the background, sampling with LatencyRecorder.
//   • Producer runs inside GB's iteration loop.
//   • When the GB loop finishes, consumers drain remaining items and stop.
//   • Warmup samples are not recorded by LatencyRecorder.
//   • Results are attached to GB output via state.counters.
//
// Parameter layout:  state.range(0) = n_consumers
//                    state.range(1) = rate_ns  (0 -> saturated, >0 -> mechanical)
//
// Warmup:  rate_ns == 0 -> 200 000 items (saturated)
//          rate_ns >  0 -> 100 000 items (mechanical)
//
// Iterations: GB receives Iterations(warmup + measure); consumers only record
//             samples after the warmup count.

template <typename Queue>
static void BM_Latency(benchmark::State& state) {
    // See reset_thread_affinity()'s comment: must run before consumer threads
    // are spawned below, and PROD_CORE must not be applied until after.
    reset_thread_affinity();
    const auto   n_consumers = static_cast<std::size_t>(state.range(0));
    const auto   rate_ns     = static_cast<std::int64_t>(state.range(1));
    const std::size_t warmup = (rate_ns > 0) ? 100'000UZ : 200'000UZ;

    Queue q(4096);

    // Separate recorder per consumer -> no mutex on hot path
    const std::size_t max_iters = static_cast<std::size_t>(state.max_iterations);
    std::vector<LatencyRecorder> crecs(
        n_consumers,
        LatencyRecorder{max_iters / n_consumers + 10'000});

    std::atomic<bool>        stop{false};
    std::atomic<std::size_t> produced{0};
    std::atomic<std::size_t> consumed{0};

    // Launch consumer threads
    std::vector<std::thread> cthr;
    cthr.reserve(n_consumers);
    for (std::size_t ci = 0; ci < n_consumers; ++ci) {
        cthr.emplace_back([&, ci, warmup]() {
            pin_thread(CONSUMER_CORES[ci]);
            elevate_thread();
            while (!stop.load(std::memory_order_acquire)) {
                auto item = q.try_pop();
                if (!item) continue;

                const std::size_t n = consumed.fetch_add(1, std::memory_order_relaxed);
                if (n >= warmup) {
                    const std::int64_t lat = now_ns() - item->enqueue_ns;
                    if (lat >= 0)
                        crecs[ci].record(static_cast<std::uint64_t>(lat));
                }
            }
        });
    }

    // Only now does the calling (producer) thread restrict itself to
    // PROD_CORE -- consumer threads above have already been spawned with an
    // unrestricted inherited mask, so they're free to migrate to their own
    // core regardless of what this thread does to itself from here on.
    pin_thread(PROD_CORE);
    elevate_thread();

    // Producer: GB iteration loop
    std::int64_t payload = 0;
    if (rate_ns > 0) {
        // Mechanical mode: spin-wait between emissions
        std::int64_t next_ns = now_ns();
        for (auto _ : state) {
            std::int64_t ts;
            do { ts = now_ns(); } while (ts < next_ns);
            next_ns += rate_ns;

            Item item{ts, payload++};
            while (!q.try_emplace(item)) {}
            produced.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        // Saturated mode: timestamp before spin (CO-safe)
        for (auto _ : state) {
            Item item{now_ns(), payload++};
            while (!q.try_emplace(item)) {}
            produced.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Wait for consumers to drain all produced items
    while (consumed.load(std::memory_order_acquire)
           < produced.load(std::memory_order_acquire))
        std::this_thread::yield();

    // Stop consumers
    stop.store(true, std::memory_order_release);
    for (auto& t : cthr) t.join();

    // Merge all recorders and compute percentiles
    LatencyRecorder merged{max_iters + 1'000};
    for (auto& r : crecs) merged.merge_from(r);

    if (!merged.empty()) {
        state.counters["Min_ns"]      = static_cast<double>(merged.min_ns());
        state.counters["Mean_ns"]     = merged.mean_ns();
        state.counters["P50_ns"]      = static_cast<double>(merged.percentile(50.0));
        state.counters["P90_ns"]      = static_cast<double>(merged.percentile(90.0));
        state.counters["P99_ns"]      = static_cast<double>(merged.percentile(99.0));
        state.counters["P99.9_ns"]    = static_cast<double>(merged.percentile(99.9));
        state.counters["P99.99_ns"]   = static_cast<double>(merged.percentile(99.99));
        state.counters["P99.999_ns"]  = static_cast<double>(merged.percentile(99.999));
        state.counters["Max_ns"]      = static_cast<double>(merged.max_ns());
        state.counters["Samples"]     = static_cast<double>(merged.count());
    }
    state.SetItemsProcessed(state.iterations());
}

// ── Benchmark 4: Broadcast (fan-out) multi-thread latency ───────────────────
//
// Same producer-side design as BM_Latency, but every one of the N consumers
// receives EVERY published item (fan-out, not competing pop) -- so progress
// is tracked per-consumer, not with a single shared counter. N is fixed at
// compile time (SPMCBroadcastQueue<T, N>), so only rate_ns is a runtime Arg.
//
// Parameter layout:  state.range(0) = rate_ns  (0 -> saturated, >0 -> mechanical)

template <typename T, std::size_t N>
static void BM_BroadcastLatency(benchmark::State& state) {
    // See reset_thread_affinity()'s comment: must run before consumer threads
    // are spawned below, and PROD_CORE must not be applied until after.
    reset_thread_affinity();
    const auto        rate_ns = static_cast<std::int64_t>(state.range(0));
    const std::size_t warmup  = (rate_ns > 0) ? 100'000UZ : 200'000UZ;

    SPMCBroadcastQueue<T, N> q(4096);

    const std::size_t max_iters = static_cast<std::size_t>(state.max_iterations);
    std::vector<LatencyRecorder> crecs(N, LatencyRecorder{max_iters + 10'000});

    std::atomic<bool>                       stop{false};
    std::atomic<std::size_t>                produced{0};
    std::array<std::atomic<std::size_t>, N> consumed{};

    std::vector<std::thread> cthr;
    cthr.reserve(N);
    for (std::size_t ci = 0; ci < N; ++ci) {
        cthr.emplace_back([&, ci, warmup]() {
            pin_thread(CONSUMER_CORES[ci]);
            elevate_thread();
            auto& c = q.consumer(ci);
            while (!stop.load(std::memory_order_acquire) ||
                   consumed[ci].load(std::memory_order_relaxed)
                       < produced.load(std::memory_order_acquire)) {
                const bool got = c.try_consume([&](const T& item) {
                    const std::size_t n = consumed[ci].load(std::memory_order_relaxed);
                    if (n >= warmup) {
                        const std::int64_t lat = now_ns() - item.enqueue_ns;
                        if (lat >= 0)
                            crecs[ci].record(static_cast<std::uint64_t>(lat));
                    }
                });
                if (got) consumed[ci].fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // Only now does the calling (producer) thread restrict itself to
    // PROD_CORE -- consumer threads above have already been spawned with an
    // unrestricted inherited mask, so they're free to migrate to their own
    // core regardless of what this thread does to itself from here on.
    pin_thread(PROD_CORE);
    elevate_thread();

    std::int64_t payload = 0;
    if (rate_ns > 0) {
        std::int64_t next_ns = now_ns();
        for (auto _ : state) {
            std::int64_t ts;
            do { ts = now_ns(); } while (ts < next_ns);
            next_ns += rate_ns;

            T item{ts, payload++};
            while (!q.try_publish(item)) {}
            produced.fetch_add(1, std::memory_order_release);
        }
    } else {
        for (auto _ : state) {
            T item{now_ns(), payload++};
            while (!q.try_publish(item)) {}
            produced.fetch_add(1, std::memory_order_release);
        }
    }

    stop.store(true, std::memory_order_release);
    for (auto& t : cthr) t.join();

    LatencyRecorder merged{max_iters * N + 1'000};
    for (auto& r : crecs) merged.merge_from(r);

    if (!merged.empty()) {
        state.counters["Min_ns"]      = static_cast<double>(merged.min_ns());
        state.counters["Mean_ns"]     = merged.mean_ns();
        state.counters["P50_ns"]      = static_cast<double>(merged.percentile(50.0));
        state.counters["P90_ns"]      = static_cast<double>(merged.percentile(90.0));
        state.counters["P99_ns"]      = static_cast<double>(merged.percentile(99.0));
        state.counters["P99.9_ns"]    = static_cast<double>(merged.percentile(99.9));
        state.counters["P99.99_ns"]   = static_cast<double>(merged.percentile(99.99));
        state.counters["P99.999_ns"]  = static_cast<double>(merged.percentile(99.999));
        state.counters["Max_ns"]      = static_cast<double>(merged.max_ns());
        state.counters["Samples"]     = static_cast<double>(merged.count());
    }
    state.SetItemsProcessed(state.iterations());
}

// ── Saturated registrations (rate_ns = 0) ────────────────────────────────────
//  Total iterations = 200K warmup + 2M measure = 2.2M
//  GB's ns/op -> producer throughput (not latency!)
//  Latency in custom counters: Mean_ns, P99_ns, P99.9_ns, P99.99_ns

static constexpr std::int64_t SAT_ITERS  = 2'200'000;
static constexpr std::int64_t MECH_ITERS = 1'100'000;
static constexpr std::int64_t RATE_NS    = 500;

// clang-format off
// SPSC is single-consumer only (non-atomic head store) -- register 1C ONLY.
// Never add an ->Args({N, ...}) with N > 1 for SPSCQueue; it would be a real
// data race, not just an unsupported configuration.
BENCHMARK_TEMPLATE(BM_Latency, SPSCQueue<Item>)
    ->Name("Sat/SPSC/1C")  ->Args({1, 0})->Iterations(SAT_ITERS);

BENCHMARK_TEMPLATE(BM_Latency, SPMCQueue<Item>)
    ->Name("Sat/Vyukov/1C")->Args({1, 0})->Iterations(SAT_ITERS);
BENCHMARK_TEMPLATE(BM_Latency, SPMCQueue<Item>)
    ->Name("Sat/Vyukov/2C")->Args({2, 0})->Iterations(SAT_ITERS);
BENCHMARK_TEMPLATE(BM_Latency, SPMCQueue<Item>)
    ->Name("Sat/Vyukov/4C")->Args({4, 0})->Iterations(SAT_ITERS);

BENCHMARK_TEMPLATE(BM_BroadcastLatency, Item, 1)
    ->Name("Sat/Broadcast/1C")->Args({0})->Iterations(SAT_ITERS);
BENCHMARK_TEMPLATE(BM_BroadcastLatency, Item, 2)
    ->Name("Sat/Broadcast/2C")->Args({0})->Iterations(SAT_ITERS);
BENCHMARK_TEMPLATE(BM_BroadcastLatency, Item, 4)
    ->Name("Sat/Broadcast/4C")->Args({0})->Iterations(SAT_ITERS);

// ── Mechanical latency registrations (rate_ns = 500) ─────────────────────────
//  Total iterations = 100K warmup + 1M measure = 1.1M
//  GB's ns/op ~= 500 ns (rate limit) — expected, not a latency number
//  Latency in custom counters: Mean_ns ~200-400 ns expected

// SPSC 1C only -- see note above.
BENCHMARK_TEMPLATE(BM_Latency, SPSCQueue<Item>)
    ->Name("Mech/SPSC/1C")  ->Args({1, RATE_NS})->Iterations(MECH_ITERS);

BENCHMARK_TEMPLATE(BM_Latency, SPMCQueue<Item>)
    ->Name("Mech/Vyukov/1C")->Args({1, RATE_NS})->Iterations(MECH_ITERS);
BENCHMARK_TEMPLATE(BM_Latency, SPMCQueue<Item>)
    ->Name("Mech/Vyukov/2C")->Args({2, RATE_NS})->Iterations(MECH_ITERS);
BENCHMARK_TEMPLATE(BM_Latency, SPMCQueue<Item>)
    ->Name("Mech/Vyukov/4C")->Args({4, RATE_NS})->Iterations(MECH_ITERS);

BENCHMARK_TEMPLATE(BM_BroadcastLatency, Item, 1)
    ->Name("Mech/Broadcast/1C")->Args({RATE_NS})->Iterations(MECH_ITERS);
BENCHMARK_TEMPLATE(BM_BroadcastLatency, Item, 2)
    ->Name("Mech/Broadcast/2C")->Args({RATE_NS})->Iterations(MECH_ITERS);
BENCHMARK_TEMPLATE(BM_BroadcastLatency, Item, 4)
    ->Name("Mech/Broadcast/4C")->Args({RATE_NS})->Iterations(MECH_ITERS);
// clang-format on
