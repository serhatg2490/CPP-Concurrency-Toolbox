// ============================================================================
//  BenchmarkMain.cpp — SPMC lock-free queue latency measurement
// ============================================================================
//
//  Two scenarios:
//
//  [1] SATURATED   — queue always full
//  [2] MECHANICAL  — 500 ns rate-limited producer, queue almost always empty
//
//  Platform optimizations (CPU pinning + priority elevation):
//
//  Windows:
//    • HIGH_PRIORITY_CLASS      — raises process scheduling priority
//    • THREAD_PRIORITY_HIGHEST  — puts each benchmark thread in the top class
//    • timeBeginPeriod(1)       — reduces Windows timer interrupt period from
//                                 15.6 ms to 1 ms; lowers scheduler jitter
//                                 (large effect on Max/P99.999, smaller on P50/P99)
//    • SetThreadAffinityMask    — pins each thread to a fixed logical core
//
//  Linux (for bare-metal runs):
//    • pthread_setaffinity_np   — core pinning
//    • SCHED_FIFO prio=99       — real-time scheduler (requires root/CAP_SYS_NICE)
//    • isolcpus= boot parameter provides full OS interrupt isolation
//      (this file does not trigger isolation; set it in the kernel command line)
//
//  Core assignment — Intel Core Ultra 5 125H (Meteor Lake, hybrid):
//
//    Physical -> logical mapping:
//      P-core 0 : logical  0 and  1   (HT pair, ~3 GHz, fastest)
//      P-core 1 : logical  2 and  3   (HT pair)
//      P-core 2 : logical  4 and  5   (HT pair)
//      P-core 3 : logical  6 and  7   (HT pair)
//      E-core 0 : logical  8          (no HT, ~2x slower, low power)
//      E-core 1-7: logical 9-15       (same)
//      LP-E 0-1 : logical 16-17       (very low power, unsuitable for latency)
//
//    Benchmark assignment (CONS_STEP=2 -> different physical P-cores):
//      main=0   : P-core 0, HT-A — idle during benchmark; absorbs OS interrupts
//      producer=2: P-core 1, HT-A — far fewer interrupts than core 0
//      1C: consumer=4           -> P-core 2
//      2C: consumers=4,6        -> P-core 2, P-core 3
//      4C: consumers=4,6,8,10   -> P-core 2, P-core 3, E-core 0, E-core 1 (!)
//
//    NOTE: The 4th consumer in the 4C test runs on an E-core (~2x slower than P-core).
//    For a pure P-core 4C test, HT siblings (4,5,6,7) must be used, which places
//    two consumers on the same physical P-core — a different tradeoff.
//
// ============================================================================

#include <SPMCLockFreeQueue.h>
#include <SPSCLockFreeQueue.h>
#include "LatencyRecorder.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

// ── Platform: Windows ────────────────────────────────────────────────────────
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <timeapi.h>   // timeBeginPeriod / timeEndPeriod  (-> winmm)

static void pin_thread(int core) {
    if (core >= 0)
        SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR(1) << core);
}

static void elevate_thread() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
}

static void setup_process(int main_core) {
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    pin_thread(main_core);
    elevate_thread();
    // Reduce timer granularity from 15.6 ms to 1 ms.
    // Affects Sleep() resolution and scheduler interrupt period;
    // reduces jitter (large impact on Max and P99.999).
    timeBeginPeriod(1);
}

static void teardown_process() {
    timeEndPeriod(1);
    SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);
}

// ── Platform: Linux / macOS ──────────────────────────────────────────────────
#else
#  include <pthread.h>
#  include <sched.h>

static void pin_thread(int core) {
    if (core < 0) return;
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(core, &s);
    pthread_setaffinity_np(pthread_self(), sizeof(s), &s);
}

static void elevate_thread() {
    // SCHED_FIFO: real-time scheduler — requires root or CAP_SYS_NICE.
    // Falls through silently if permission is denied (benchmark runs at normal priority).
    struct sched_param p{};
    p.sched_priority = 90;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &p) != 0)
        std::fprintf(stderr, "  [!] SCHED_FIFO unavailable — run as root\n");
}

static void setup_process(int main_core) {
    pin_thread(main_core);
    elevate_thread();
}

static void teardown_process() {}
#endif

// ── Core assignment constants — Intel Core Ultra 5 125H ──────────────────────
// P-core HT pairs: (0,1) (2,3) (4,5) (6,7); E-cores: 8-15 (no HT)
//
// Core 0 (P-core 0) is the primary target for Windows timer interrupts and DPCs.
// The main thread (idle during benchmark) is pinned here to absorb OS noise
// and shield the producer and consumer cores.
//
// Producer on core 2 (P-core 1): receives far fewer interrupts than core 0.
static constexpr int MAIN_CORE  = 0;   // P-core 0, HT-A — idle; absorbs OS interrupts
static constexpr int PROD_CORE  = 2;   // P-core 1, HT-A — shielded from interrupts
static constexpr int CONS0_CORE = 4;   // P-core 2, HT-A — first consumer core
static constexpr int CONS_STEP  = 2;   // skip HT siblings -> each consumer on a different P-core
// Result: 1C->{4}  2C->{4,6}  4C->{4,6,8,10}  (8,10 = E-core; ~2x slower than P-core)

// ── Clock ─────────────────────────────────────────────────────────────────────
static std::int64_t now_ns() noexcept {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

struct Item {
    std::int64_t enqueue_ns = 0;
    std::int64_t payload    = 0;
};

// ── Generic benchmark function ────────────────────────────────────────────────
template <typename Queue>
void run_benchmark(const char*  label,
                   std::size_t  n_consumers,
                   std::size_t  warmup,
                   std::size_t  measure,
                   std::size_t  capacity,
                   std::int64_t rate_limit_ns   = 0,
                   int          producer_core   = PROD_CORE,
                   int          consumer0_core  = CONS0_CORE,
                   int          consumer_stride = CONS_STEP)
{
    const std::size_t total = warmup + measure;
    Queue q(capacity);

    std::vector<LatencyRecorder> recs(n_consumers,
                                      LatencyRecorder{measure / n_consumers + 10'000});
    std::atomic<bool>        go{false};
    std::atomic<std::size_t> consumed{0};

    // ── Consumer threads ──────────────────────────────────────────────────────
    std::vector<std::thread> cthr;
    cthr.reserve(n_consumers);
    for (std::size_t ci = 0; ci < n_consumers; ++ci) {
        cthr.emplace_back([&, ci, warmup, consumer0_core, consumer_stride]() {
            pin_thread(consumer0_core >= 0 ? consumer0_core + (int)ci * consumer_stride : -1);
            elevate_thread();
            while (!go.load(std::memory_order_acquire)) {}

            while (consumed.load(std::memory_order_relaxed) < total) {
                auto item = q.try_pop();
                if (!item) continue;

                std::size_t n = consumed.fetch_add(1, std::memory_order_relaxed);
                if (n >= warmup) {
                    std::int64_t lat = now_ns() - item->enqueue_ns;
                    if (lat >= 0)
                        recs[ci].record(static_cast<std::uint64_t>(lat));
                }
            }
        });
    }

    // ── Producer thread ───────────────────────────────────────────────────────
    std::thread pthr([&, rate_limit_ns, total]() {
        pin_thread(producer_core);
        elevate_thread();
        while (!go.load(std::memory_order_acquire)) {}

        if (rate_limit_ns > 0) {
            std::int64_t next_ns = now_ns();
            for (std::size_t i = 0; i < total; ++i) {
                std::int64_t ts;
                do { ts = now_ns(); } while (ts < next_ns);
                next_ns += rate_limit_ns;
                const Item item{ts, static_cast<std::int64_t>(i)};
                while (!q.try_emplace(item)) {}
            }
        } else {
            for (std::size_t i = 0; i < total; ++i) {
                const Item item{now_ns(), static_cast<std::int64_t>(i)};
                while (!q.try_emplace(item)) {}
            }
        }
    });

    go.store(true, std::memory_order_release);
    pthr.join();
    for (auto& t : cthr) t.join();

    LatencyRecorder merged{measure + 10'000};
    for (auto& r : recs) merged.merge_from(r);
    merged.print_report(label);
}

// ── main ─────────────────────────────────────────────────────────────────────
int main() {
    setup_process(MAIN_CORE);

    std::printf("  Core assignment : main=%d  producer=%d  consumer_base=%d  stride=%d\n",
                MAIN_CORE, PROD_CORE, CONS0_CORE, CONS_STEP);
    std::printf("  Topology        : P{0,1}=core0  P{2,3}=core1  P{4,5}=core2  P{6,7}=core3  E:{8-15}\n");
    std::printf("  NOTE            : core 0 absorbs OS interrupts (idle main); producer shielded on core 2\n");
#ifdef _WIN32
    std::printf("  Priority        : HIGH_PRIORITY_CLASS + THREAD_PRIORITY_HIGHEST\n");
    std::printf("  Timer           : timeBeginPeriod(1) -> 1 ms granularity\n");
#else
    std::printf("  Priority        : SCHED_FIFO prio=90  (error shown above if root is required)\n");
#endif

    // =========================================================================
    // SCENARIO 1 — Saturated
    // =========================================================================
    {
        constexpr std::size_t W = 200'000;
        constexpr std::size_t M = 2'000'000;
        constexpr std::size_t C = 4096;

        std::printf("\n");
        std::printf("╔══════════════════════════════════════════════════════════════╗\n");
        std::printf("║  SCENARIO 1 — Saturated  (queue always full)                ║\n");
        std::printf("║  Warmup %6zu  │  Measure %7zu  │  Capacity %4zu            ║\n", W, M, C);
        std::printf("╚══════════════════════════════════════════════════════════════╝\n");

        // SPSC is single-consumer only (non-atomic head store) -- 1C is the
        // only valid registration; never call with n_consumers > 1.
        run_benchmark<SPSCQueue<Item>>      ("SPSC        1C", 1, W, M, C);
        run_benchmark<SPMCQueue<Item>>      ("VyukovSPMC 1C", 1, W, M, C);
        run_benchmark<SPMCQueue<Item>>      ("VyukovSPMC 2C", 2, W, M, C);
        run_benchmark<SPMCQueue<Item>>      ("VyukovSPMC 4C", 4, W, M, C);
    }

    // =========================================================================
    // SCENARIO 2 — Mechanical latency
    // =========================================================================
    {
        constexpr std::size_t  W       =   100'000;
        constexpr std::size_t  M       = 1'000'000;
        constexpr std::size_t  C       = 4096;
        constexpr std::int64_t RATE_NS = 500;

        LatencyRecorder::reset_header();
        std::printf("\n");
        std::printf("╔══════════════════════════════════════════════════════════════╗\n");
        std::printf("║  SCENARIO 2 — Mechanical latency  (queue almost always empty)║\n");
        std::printf("║  Warmup %6zu  │  Measure %7zu  │  Rate 1/%lldns            ║\n",
                    W, M, (long long)RATE_NS);
        std::printf("╚══════════════════════════════════════════════════════════════╝\n");

        // SPSC 1C only -- see Scenario 1 note above.
        run_benchmark<SPSCQueue<Item>>      ("SPSC        1C", 1, W, M, C, RATE_NS);
        run_benchmark<SPMCQueue<Item>>      ("VyukovSPMC 1C", 1, W, M, C, RATE_NS);
        run_benchmark<SPMCQueue<Item>>      ("VyukovSPMC 2C", 2, W, M, C, RATE_NS);
        run_benchmark<SPMCQueue<Item>>      ("VyukovSPMC 4C", 4, W, M, C, RATE_NS);
    }

    std::printf("\n  All values are in nanoseconds (ns).\n");
    std::printf("  Clock overhead (~50-100 ns) is added equally to all columns.\n\n");

    teardown_process();
    return 0;
}
