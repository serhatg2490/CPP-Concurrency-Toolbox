// ============================================================================
//  TscBenchmarkMain.cpp — RDTSCP-based SPMC queue latency measurement
// ============================================================================
//
//  Same scenarios and optimizations as BenchmarkMain.cpp — clock layer is TSC.
//
//  Fence strategy:
//    Producer: now_raw() = RDTSC (no fence)
//      Data dependency provides ordering: item.enqueue_ns = ts -> try_emplace(item)
//      No LFENCE -> consumer CAS storms do not stall the producer.
//    Consumer: now() = RDTSCP
//      Does not read the TSC until all preceding loads have retired.
//
// ============================================================================

#include <SPMCLockFreeQueue.h>
#include <SPSCLockFreeQueue.h>
#include "LatencyRecorder.hpp"
#include "TscClock.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

// ── Platform: Windows ────────────────────────────────────────────────────────
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <timeapi.h>

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
    struct sched_param p{}; p.sched_priority = 90;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &p) != 0)
        std::fprintf(stderr, "  [!] SCHED_FIFO unavailable — run as root\n");
}
static void setup_process(int main_core) { pin_thread(main_core); elevate_thread(); }
static void teardown_process() {}
#endif

// ── Core assignment constants — Intel Core Ultra 5 125H ──────────────────────
// P-core HT pairs: (0,1) (2,3) (4,5) (6,7); E-cores: 8-15 (no HT)
// Core 0: primary OS interrupt / DPC target — idle main pinned here to absorb noise
static constexpr int MAIN_CORE  = 0;   // P-core 0, HT-A — idle; absorbs OS interrupts
static constexpr int PROD_CORE  = 2;   // P-core 1, HT-A — shielded from interrupts
static constexpr int CONS0_CORE = 4;   // P-core 2, HT-A
static constexpr int CONS_STEP  = 2;   // skip HT siblings: 1C->{4}  2C->{4,6}  4C->{4,6,8,10}

// ── TSC-based clock ───────────────────────────────────────────────────────────
static std::int64_t produce_ts() noexcept {
    return static_cast<std::int64_t>(tsc::epoch_ns(tsc::now_raw()));
}
static std::int64_t consume_ts() noexcept {
    return static_cast<std::int64_t>(tsc::epoch_ns(tsc::now()));
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
                const std::int64_t now = consume_ts();
                std::size_t n = consumed.fetch_add(1, std::memory_order_relaxed);
                if (n >= warmup) {
                    std::int64_t lat = now - item->enqueue_ns;
                    if (lat >= 0)
                        recs[ci].record(static_cast<std::uint64_t>(lat));
                }
            }
        });
    }

    std::thread pthr([&, rate_limit_ns, total]() {
        pin_thread(producer_core);
        elevate_thread();
        while (!go.load(std::memory_order_acquire)) {}

        if (rate_limit_ns > 0) {
            std::int64_t next_ns = static_cast<std::int64_t>(tsc::epoch_ns(tsc::now()));
            for (std::size_t i = 0; i < total; ++i) {
                std::int64_t ts;
                do { ts = produce_ts(); } while (ts < next_ns);
                next_ns += rate_limit_ns;
                const Item item{ts, static_cast<std::int64_t>(i)};
                while (!q.try_emplace(item)) {}
            }
        } else {
            for (std::size_t i = 0; i < total; ++i) {
                const Item item{produce_ts(), static_cast<std::int64_t>(i)};
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
    tsc::calibrate(std::chrono::milliseconds{20});
    tsc::set_epoch();

    setup_process(MAIN_CORE);

    std::printf("  TSC calibration : %.3f GHz\n", tsc::g_tsc_hz / 1e9);
    std::printf("  Core assignment : main=%d  producer=%d  consumer_base=%d  stride=%d\n",
                MAIN_CORE, PROD_CORE, CONS0_CORE, CONS_STEP);
    std::printf("  Topology        : P{0,1}=core0  P{2,3}=core1  P{4,5}=core2  P{6,7}=core3  E:{8-15}\n");
    std::printf("  NOTE            : core 0 absorbs OS interrupts (idle main); producer shielded on core 2\n");
#ifdef _WIN32
    std::printf("  Priority        : HIGH_PRIORITY_CLASS + THREAD_PRIORITY_HIGHEST\n");
    std::printf("  Timer           : timeBeginPeriod(1) -> 1 ms granularity\n");
#endif

    {
        constexpr std::size_t W = 200'000;
        constexpr std::size_t M = 2'000'000;
        constexpr std::size_t C = 4096;

        std::printf("\n");
        std::printf("╔══════════════════════════════════════════════════════════════╗\n");
        std::printf("║  SCENARIO 1 — Saturated  [TSC + pinned]                     ║\n");
        std::printf("║  Warmup %6zu  │  Measure %7zu  │  Capacity %4zu            ║\n", W, M, C);
        std::printf("╚══════════════════════════════════════════════════════════════╝\n");

        // SPSC is single-consumer only (non-atomic head store) -- 1C is the
        // only valid registration; never call with n_consumers > 1.
        run_benchmark<SPSCQueue<Item>>      ("SPSC        1C", 1, W, M, C);
        run_benchmark<SPMCQueue<Item>>      ("VyukovSPMC 1C", 1, W, M, C);
        run_benchmark<SPMCQueue<Item>>      ("VyukovSPMC 2C", 2, W, M, C);
        run_benchmark<SPMCQueue<Item>>      ("VyukovSPMC 4C", 4, W, M, C);
    }

    {
        constexpr std::size_t  W       =   100'000;
        constexpr std::size_t  M       = 1'000'000;
        constexpr std::size_t  C       = 4096;
        constexpr std::int64_t RATE_NS = 500;

        LatencyRecorder::reset_header();
        std::printf("\n");
        std::printf("╔══════════════════════════════════════════════════════════════╗\n");
        std::printf("║  SCENARIO 2 — Mechanical latency  [TSC + pinned]            ║\n");
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
    std::printf("  TSC overhead (~20-40 ns) is added equally to all columns.\n\n");

    teardown_process();
    return 0;
}
