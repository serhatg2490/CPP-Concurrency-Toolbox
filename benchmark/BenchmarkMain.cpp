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
//    Physical -> logical mapping (verified via `lscpu --extended`; the HT
//    pairs are NOT sequential (0,1)(2,3)... on this chip — core_id groups
//    logical CPUs irregularly):
//      P-core 0 : logical  0 and  5   (HT pair)
//      P-core 1 : logical  1 and  2   (HT pair)
//      P-core 2 : logical  3 and  4   (HT pair)
//      P-core 3 : logical  6 and  7   (HT pair)
//      E-core 0-7: logical  8-15      (no HT, ~2x slower, low power)
//      LP-E 0-1 : logical 16-17       (very low power, unsuitable for latency)
//
//    Only 4 physical P-cores exist in total. The benchmark needs a producer
//    plus up to 4 consumers (5 threads), so something has to give: the main
//    thread blocks in join() for the entire measured run (it never spins),
//    so it is safe to park it on the HT sibling of the producer's P-core
//    instead of burning a whole dedicated P-core on an idle thread.
//
//    Benchmark assignment:
//      main=0     : P-core 0, HT-A — idle (blocked in join); absorbs OS interrupts
//      producer=5 : P-core 0, HT-B — shares main's physical core, safe since main is idle
//      1C: consumer=1           -> P-core 1
//      2C: consumers=1,3        -> P-core 1, P-core 2
//      4C: consumers=1,3,6,8    -> P-core 1, P-core 2, P-core 3, E-core 0 (!)
//
//    NOTE: With only 4 physical P-cores and one spent on main+producer, the
//    4th consumer in the 4C test necessarily spills onto an E-core (~2x
//    slower, different microarchitecture) — this is a hardware limit, not a
//    tuning choice. Treat 4C results as "3 P-core + 1 E-core", not a clean
//    scale-up from 2C.
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
// Real HT pairs (from `lscpu --extended`, core_id column): (0,5) (1,2) (3,4) (6,7)
// E-cores: 8-15 (no HT). See topology block above for the full derivation.
//
// Main is idle for the whole measured run (blocked in join()), so it shares
// P-core 0 with the producer instead of consuming a full dedicated P-core.
static constexpr int MAIN_CORE = 0;   // P-core 0, HT-A — idle; absorbs OS interrupts
static constexpr int PROD_CORE = 5;   // P-core 0, HT-B — shares main's core (main is idle, not spinning)
static constexpr int CONSUMER_CORES[] = { 1, 3, 6, 8 }; // P-core 1, P-core 2, P-core 3, E-core 0
// Result: 1C->{1}  2C->{1,3}  4C->{1,3,6,8}  (8 = E-core; only 4 P-cores exist total)

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
                   const int*   consumer_cores  = CONSUMER_CORES)
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
        cthr.emplace_back([&, ci, warmup, consumer_cores]() {
            pin_thread(consumer_cores ? consumer_cores[ci] : -1);
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

    std::printf("  Core assignment : main=%d  producer=%d  consumers=%d,%d,%d,%d\n",
                MAIN_CORE, PROD_CORE,
                CONSUMER_CORES[0], CONSUMER_CORES[1], CONSUMER_CORES[2], CONSUMER_CORES[3]);
    std::printf("  Topology        : P{0,5}=core0  P{1,2}=core1  P{3,4}=core2  P{6,7}=core3  E:{8-15}\n");
    std::printf("  NOTE            : main+producer share core0 (main idle, blocked in join); 4th consumer (%d) is an E-core\n",
                CONSUMER_CORES[3]);
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
