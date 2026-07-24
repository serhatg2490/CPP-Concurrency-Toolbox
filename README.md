# CPP-Concurrency-Toolbox

A small, header-only C++23 toolbox of lock-free SPSC/SPMC ring-buffer queues and a general-purpose thread pool, built with a specific goal: **know exactly what your tail latency looks like, and why.** The repository ships not just the data structures but the full measurement apparatus (three independent benchmark harnesses, an OS-tuning script, and a from-first-principles investigation of a real tail-latency anomaly) needed to answer that question on your own hardware.

---

## 1. Overview

The toolbox is three header-only components, each usable independently:

### `SPSCQueue<T>` — Single-Producer, Single-Consumer queue

**File:** [`include/SPSCLockFreeQueue.h`](include/SPSCLockFreeQueue.h)

A fixed-capacity ring buffer with the classic two-cache-line SPSC design: `head`/`tail` atomics live on separate cache lines (`alignas(64)`) so the producer and consumer cores never invalidate each other's cache line, and index wraparound uses a bitmask (`capacity - 1`) instead of modulo.

- **How it works:** producer writes into `buffer[tail]`, releases `tail`; consumer acquires `tail`, reads `buffer[head]`, releases `head`. Two atomics, two memory fences, no CAS.
- **Constraints:**
  - Capacity **must be a power of two** (constructor throws `std::invalid_argument` otherwise).
  - **Exactly one** producer thread and **exactly one** consumer thread — calling `try_emplace`/`try_pop` from more than one thread on either side is undefined behavior (the head/tail stores are not compare-and-swap based).
  - `T` must be copy- or move-constructible.

### `SPMCQueue<T>` — Single-Producer, Multi-Consumer queue

**File:** [`include/SPMCLockFreeQueue.h`](include/SPMCLockFreeQueue.h)

A Vyukov-style sequence-per-slot ring buffer (the single-producer half of Vyukov's bounded MPMC queue). Each slot carries its own `sequence` atomic, which doubles as the slot's state (free / full / being-reclaimed) and its ABA-safety generation counter.

- **How it works:** the producer is **wait-free** — it never retries, it either finds `sequence == tail` (slot free) or the queue is full. Consumers are **lock-free** — they race on `head` via `compare_exchange_weak`; the loser simply reloads and retries against the CAS-updated value, no backoff needed for correctness.
- **Blocking variants:** `pop_wait()` / `emplace_wait()` use `std::atomic::wait`/`notify` (a futex on Linux, `WaitOnAddress` on Windows) — no syscall unless the thread genuinely has to park.
- **Constraints:**
  - **Exactly one** producer thread (multiple producers are unsafe — this is *not* an MPMC queue).
  - Any number of consumer threads.
  - `T` must satisfy `movable<T> && is_nothrow_move_constructible_v<T>` — a throwing move is a correctness hazard once a consumer's CAS has claimed a slot, so it's rejected at compile time.
  - Capacity is rounded up to the next power of two, minimum 2.

### `ThreadPool` — general-purpose task pool

**File:** [`include/ThreadPool.h`](include/ThreadPool.h)

A conventional fixed-size worker pool: a `std::queue<std::move_only_function<void()>>` guarded by a mutex/condition-variable, `std::jthread` workers (join automatically on destruction via `request_stop()`), and a C++20-concept-constrained `enqueue()` returning a `std::future`.

- **How it works:** `enqueue()` wraps the callable in a `std::packaged_task`, pushes it behind the queue mutex, and returns the associated `std::future` immediately; a worker later pops and executes it, catching and logging any exception so one failing task never takes down a worker thread.
- **Constraints:** this is a **mutex-based**, general-purpose pool — not lock-free, and not designed for the sub-microsecond latency budget the two queues above target. Use it for coarser-grained parallel work (parsing, I/O-bound tasks, batch processing); use the SPSC/SPMC queues for the hot path.

---

## 2. Requirements

| Requirement | Notes |
|---|---|
| **C++23 compiler** | Needs `std::move_only_function`, `std::expected`, and C++20/23 concepts. Validated with **GCC 13.3** on this repository; a recent Clang or MSVC (VS 2022 17.8+) with `/std:c++latest` should also work but hasn't been exercised here. |
| **CMake ≥ 3.20** | Root [`CMakeLists.txt`](CMakeLists.txt) sets `CMAKE_CXX_STANDARD 23`. |
| **A CMake generator** | Ninja or Make on Linux; Visual Studio or Ninja on Windows. |
| **Internet access on first configure** | `FetchContent` downloads GoogleTest and Google Benchmark (shallow clones, a few MB) the first time you run `cmake -B build`. Subsequent configures reuse the cached copy. |
| **Linux extras (optional, for tuning/diagnostics)** | `pthread` (always required, linked automatically), `cpupower` (for [`scripts/tune-benchmark-host.sh`](scripts/tune-benchmark-host.sh)), `perf` and `turbostat` (for the diagnostic methodology described in [`docs/benchmark-analysis-2026-07-22.md`](docs/benchmark-analysis-2026-07-22.md)) — none of these are required just to build and run the tests/benchmarks. |
| **Windows extras** | `winmm` (linked automatically by CMake) for `timeBeginPeriod`/`timeEndPeriod` timer-resolution control in the benchmark harnesses. |

No external runtime dependencies — the library itself is header-only with no third-party includes; GoogleTest/Google Benchmark are build-time-only, test/benchmark-only dependencies.

---

## 3. Platforms

| Platform | Status |
|---|---|
| **Linux** | Primary, most exercised target. Full core-pinning, `SCHED_FIFO`, and the entire OS-tuning investigation in this repo were done on Ubuntu (kernel 7.0.0-generic). |
| **Windows** | Supported via explicit `#ifdef _WIN32` branches in the benchmark harnesses (`SetThreadAffinityMask`, `HIGH_PRIORITY_CLASS`, `timeBeginPeriod`). Build via [`build.bat`](build.bat). |
| **macOS** | Likely builds (the POSIX/`pthread_setaffinity_np` code path is shared with Linux) but is **not tested or tuned** — `pthread_setaffinity_np` for core pinning is a Linux-specific glibc extension and silently isn't the same API on macOS, so treat macOS as best-effort/untested. |
| **CPU architecture** | The core library (`SPSCQueue`/`SPMCQueue`/`ThreadPool`) is portable C++23, no architecture dependency. The TSC-based clock ([`benchmark/TscClock.hpp`](benchmark/TscClock.hpp)) is **x86/x86-64 only** (uses `RDTSC`/`RDTSCP`); it **automatically falls back to `steady_clock`** on other architectures (e.g. ARM/Apple Silicon), so `spmc_tsc_benchmark` still builds and runs everywhere, just without the TSC precision benefit off x86. |

---

## 4. Installation & Running

### Build

Building and testing are two separate, composable steps — matching CMake/CTest's own design (and avoiding the classic "`build.sh` also silently runs the whole test suite" surprise):

```bash
# Linux
./build.sh              # configure (if needed) + incremental build
./build.sh --clean      # wipe build/ first, then configure + build from scratch

./test.sh               # run unit tests + benchmarks against the existing build (~20s)
./test.sh --unit-only   # unit tests only, skips the ~15s benchmark run

# ...or drive CMake/CTest directly
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j"$(nproc)"
ctest --test-dir build --output-on-failure          # unit tests + benchmarks
ctest --test-dir build --output-on-failure -LE benchmark  # unit tests only
```

```bat
:: Windows
build.bat
build.bat --clean

test.bat
test.bat --unit-only
```

`build.sh`/`build.bat` only build — by default incrementally (fast, reuses the existing `build/` if present), or from a clean slate with `--clean`. `test.sh`/`test.bat` only run CTest against whatever's already built (they error out with a clear message if you haven't run the build script yet), defaulting to everything, `--unit-only` to skip the benchmark run.

This produces:
- Three test binaries: `threadpool_tests`, `spsc_queue_tests`, `spmc_queue_tests`
- Three benchmark binaries: `spmc_benchmark`, `spmc_tsc_benchmark`, `spmc_gbenchmark`

All six are registered with CTest; the three benchmarks carry a `benchmark` label so they can be included or excluded independently of the fast unit-test pass (see §5).

### Core Pinning & Affinity Setup

The benchmark harnesses ([`benchmark/BenchmarkMain.cpp`](benchmark/BenchmarkMain.cpp), [`benchmark/TscBenchmarkMain.cpp`](benchmark/TscBenchmarkMain.cpp)) pin the producer/consumer threads to specific logical CPUs via hardcoded constants (`MAIN_CORE`, `PROD_CORE`, `CONSUMER_CORES[]`). **These constants are tuned for one specific machine's topology and will not be correct on yours by default.**

The single most important lesson from this repo's own investigation (see [`docs/benchmark-analysis-2026-07-22.md`](docs/benchmark-analysis-2026-07-22.md) §5): **hyperthread sibling pairs are not guaranteed to be sequential** (`(0,1)(2,3)...`). Verify your real topology before trusting any core-pinning code, including this one:

```bash
lscpu --all --extended
```

Look at the `CORE` column — logical CPUs sharing the same `CORE` number are real HT siblings. On the machine this repo was tuned for, the real pairing turned out to be `(0,5)(1,2)(3,4)(6,7)`, not the sequential `(0,1)(2,3)(4,5)(6,7)` a naive read of `nproc`/`lscpu -e` might suggest. Update `MAIN_CORE`/`PROD_CORE`/`CONSUMER_CORES` in both benchmark files to match your own `CORE` column before drawing any conclusions from the numbers.

Requesting `SCHED_FIFO` real-time priority requires root (or `CAP_SYS_NICE`); without it the benchmarks still run, just at normal scheduling priority (a warning is printed):

```bash
sudo ./build/spmc_tsc_benchmark
```

### Running the Tuning Script

[`scripts/tune-benchmark-host.sh`](scripts/tune-benchmark-host.sh) automates every OS-level tuning step this repo's investigation found worth keeping: CPU governor, `irqbalance`, per-core frequency floor, C-states, RT-runtime throttle, and `isolcpus`/`nohz_full`/`rcu_nocbs`.

```bash
sudo ./scripts/tune-benchmark-host.sh            # apply (run again after the reboot it may ask for)
sudo ./scripts/tune-benchmark-host.sh --yes       # same, no confirmation prompts
sudo ./scripts/tune-benchmark-host.sh --no-grub   # runtime-only tuning, never touches GRUB/reboots
./scripts/tune-benchmark-host.sh --status         # read-only report, no root required
sudo ./scripts/tune-benchmark-host.sh --undo      # revert everything, restoring prior values
```

It's a **zero-flag, run-it-twice** design: if `isolcpus` isn't set yet, the reboot-transient steps are skipped (they'd just be wiped by the reboot the GRUB step needs), and GRUB is updated. Running the *exact same command* again after rebooting finds `isolcpus` already set, applies the rest for real, and the GRUB step auto-skips. See the script's own header comment and doc §21 for the full rationale of each step, including which ones were empirically proven **not** to fix the specific tail-latency anomaly this repo investigates, but were kept anyway as standard low-latency hygiene.

**Edit `CORES`/`CORE_LIST` at the top of the script to match your own topology** — same caveat as above.

---

## 5. Tests

Three GoogleTest binaries, all built from [`tests/`](tests/):

| Binary | Source | What it covers |
|---|---|---|
| `spsc_queue_tests` | [`test_spsc_queue.cpp`](tests/test_spsc_queue.cpp) | Capacity validation, empty/full edge cases, FIFO ordering, index wraparound, value/copy/move-only/`std::string` payload types, in-place `try_emplace`, concept rejection for non-constructible types, single-threaded and multi-threaded throughput. |
| `spmc_queue_tests` | [`test_spmc_queue.cpp`](tests/test_spmc_queue.cpp) | All of the above plus: multi-consumer "each item received exactly once" correctness under both `try_pop` and blocking `pop_wait`, heavy-object (non-trivial, allocation-heavy) payload correctness under concurrent consumers, destructor draining of unconsumed elements, `std::expected`-based `try_pop_expected`, blocking `emplace_wait`/`pop_wait` semantics. |
| `threadpool_tests` | [`test_threadpool.cpp`](tests/test_threadpool.cpp) | Lambda/rvalue/normal-function/member-function/container-argument task submission, `std::future`-based result retrieval, `hardware_concurrency()` auto-sizing, atomic-increment correctness under concurrent task submission, throughput under 1000 queued tasks, heavy CPU load handling. |

Run just the unit tests (fast, ~7s):

```bash
cd build
ctest --output-on-failure -LE benchmark
```

**Result on this repository, as of this build:**

```
100% tests passed, 0 tests failed out of 53
Total Test time (real) = 7.27 sec
```

Running `ctest --output-on-failure` with no filter (or `./test.sh` / `test.bat`, see §4) runs these same 53 tests **plus** the 3 benchmark binaries from §6 (registered as CTest tests too, `benchmark`-labeled) — 56 total, ~20s, still all green.

A few of the tests also print an informal throughput number to stdout (not a substitute for the dedicated benchmark suite in §6, but a useful smoke-test figure):

| Test | Measured throughput |
|---|---|
| `SPSCQueuePerfTest.SingleThreaded_FillAndDrain` | 44.7 M ops/sec |
| `SPSCQueuePerfTest.MultiThreaded_Int_Throughput` | 7.6 M ops/sec |
| `SPMCQueuePerfTest.SingleThreaded_FillAndDrain` | 26.8 M ops/sec |
| `SPMCQueuePerfTest.MultipleConsumers_Int_Throughput` (6 consumers) | 5.6 M ops/sec |
| `ThreadPoolTest.PerformanceTest` | 1000 tasks in 5.06 ms |

---

## 6. Benchmarks

### Latency Measurement Method

Every benchmark harness in this repo measures the same two scenarios, chosen because they stress opposite ends of the queue's operating envelope:

- **Saturated** — the queue is kept always full (producer spins as fast as it can against `try_emplace`). This measures **queueing/backpressure latency**: how long an item waits behind whatever's already in the ring. It is *not* a measure of the algorithm's intrinsic overhead — it's a measure of how deep the queue got.
- **Mechanical** — the producer is rate-limited (one item every 500 ns) so the queue is almost always empty. This measures the queue's **intrinsic, uncontended transit latency** — the closest analogue to a real "tick-to-trade" hot path.

All producer timestamps are taken **immediately before** `try_emplace`, so a delayed producer never inflates the recorded latency of the item it just produced — the recorded number is purely "time from becoming visible in the queue to being read by a consumer," i.e. consumer-side latency.

### `LatencyRecorder`

**File:** [`benchmark/LatencyRecorder.hpp`](benchmark/LatencyRecorder.hpp)

A minimal per-thread sample recorder: `reserve()` up front so `record()` never allocates on the hot path, sort-based percentile computation (matches the approach HdrHistogram/`wrk2` use conceptually, simpler to implement, no external dependency), and a `merge_from()` to combine multiple consumer threads' independent recorders into one final distribution after the run — this avoids any shared-state synchronization cost *during* measurement.

### `BenchmarkMain`

**File:** [`benchmark/BenchmarkMain.cpp`](benchmark/BenchmarkMain.cpp) → binary `spmc_benchmark`

The primary latency harness, clocked with `std::chrono::steady_clock`. Runs both scenarios (Saturated, Mechanical) across four configurations — `SPSC 1C`, `Vyukov SPMC 1C`, `2C`, `4C` — printing a `Min/Mean/P50/P90/P99/P99.9/P99.99/P99.999/Max` table for each. On Linux it requests `SCHED_FIFO` priority 99 and pins each thread per the `MAIN_CORE`/`PROD_CORE`/`CONSUMER_CORES` constants (§4 above); on Windows it raises process/thread priority and calls `timeBeginPeriod(1)`.

### `GBenchmark`

**File:** [`benchmark/GBenchmark.cpp`](benchmark/GBenchmark.cpp) → binary `spmc_gbenchmark`

The same measurement intent, integrated with Google Benchmark for statistical rigor and machine-readable output (`--benchmark_out=results.json`). Three families:
- **`RT/*`** — single-thread round-trip (`try_emplace` + `try_pop` on the same thread), Google Benchmark's native `ns/op`.
- **`Sat/*`** — Saturated scenario; percentiles are attached as custom counters (`Mean_ns`, `P50_ns`, ... `P99.99_ns`) since GB's own `ns/op` only reflects producer throughput here.
- **`Mech/*`** — Mechanical scenario, same custom-counter approach.

```bash
./build/spmc_gbenchmark --benchmark_counters_tabular=true
./build/spmc_gbenchmark --benchmark_filter="Mech"
```

### `TSCBenchmark`

**File:** [`benchmark/TscBenchmarkMain.cpp`](benchmark/TscBenchmarkMain.cpp) → binary `spmc_tsc_benchmark`

Structurally identical to `BenchmarkMain`, but clocked with `TscClock.hpp`'s `RDTSCP`-based timestamps instead of `steady_clock` — roughly 2-3x lower per-read overhead (~20-40 ns vs ~50-100 ns) and far finer granularity (~0.3 ns at a 3 GHz TSC vs ~100 ns for a typical `steady_clock`/QPC implementation). Producer timestamps use unfenced `RDTSC` (`now_raw()`) relying on the data dependency into `try_emplace` for ordering — deliberately avoiding an `LFENCE` that would otherwise pull consumer CAS cache-line traffic into the producer's timestamp.

### `TSCClock`

**File:** [`benchmark/TscClock.hpp`](benchmark/TscClock.hpp)

The RDTSC/RDTSCP primitives themselves: `now_raw()` (unfenced, for cross-thread producer timestamps), `start()`/`end()` (LFENCE'd, for same-thread interval measurement), `calibrate()` (measures TSC frequency by comparing a TSC delta against a `steady_clock` delta over ~20 ms), and an epoch mechanism (`set_epoch()`/`epoch_ns()`) to keep cycle-to-nanosecond conversion inside safe `uint64_t` multiplication range. Correct only on single-socket systems (TSC can drift across sockets); falls back to `steady_clock` on non-x86 architectures.

### OS Tuning Optimizations — `scripts/tune-benchmark-host.sh`

See §4's "Running the Tuning Script" above for usage. In brief, the script applies, in order: (1) `performance` governor + stop `irqbalance`, (2) per-core CPU frequency floor pinning, (3) C-state disabling (C1E/C6/C10), (4) RT-runtime throttle disabling, (5) `isolcpus`/`nohz_full`/`rcu_nocbs` via GRUB. Steps 1-4 are safe, instant, and reboot-transient; step 5 is persistent and requires a reboot, gated behind a confirmation prompt.

### Measurement Hardware & System Configuration

| | |
|---|---|
| **CPU** | Intel Core Ultra 5 125H (Meteor Lake, hybrid: 4 P-cores/8 threads + 8 E-cores + 2 LPE-cores) |
| **OS / Kernel** | Ubuntu, Linux 7.0.0-28-generic |
| **Tuning applied** | `performance` governor, `irqbalance` stopped, per-core frequency floor pinned, C1E/C6/C10 disabled, RT-runtime throttle disabled, `isolcpus`/`nohz_full`/`rcu_nocbs` covering all P-core HT pairs + one E-core (`0-8`) |
| **Core assignment** | main=core0(HT-A), producer=core0(HT-B) — safe to share since main blocks in `join()` for the whole run — consumers on cores 1/3/6 (real, distinct P-cores) + core 8 (E-core, only in the 4-consumer configuration) |

### Measurement Results (Mechanical scenario, TSC clock, one representative tuned run)

All values in nanoseconds.

| Queue | Min | P50 | P99 | P99.9 | Max |
|---|---|---|---|---|---|
| SPSC 1C | 91 | 208 | 244 | 183,353 | 569,459 |
| Vyukov SPMC 1C | 48 | 210 | 227 | 245,490 | 636,052 |
| Vyukov SPMC 2C | 51 | 210 | 289 | 334 | **2,666** |
| Vyukov SPMC 4C | 59 | 201 | 316 | 382 | **11,013** |

(One representative run shown for readability; run-to-run variance and the full multi-run dataset — including Saturated-scenario numbers — are in [`docs/benchmark-analysis-2026-07-22.md`](docs/benchmark-analysis-2026-07-22.md).)

### Result Evaluation

Median and P99 are sub-microsecond across **every** configuration, including single-consumer — the algorithms themselves (wait-free Vyukov producer, cache-line-isolated SPSC) are sound and HFT-class. The **2-and-4-consumer configurations are fully HFT-class end to end**: P99.9 stays sub-microsecond and Max stays in the low single-digit microseconds after tuning.

The **1-consumer configurations (both SPSC and Vyukov) show an unexplained P99.9+ cliff** — a jump from ~200-300 ns to 150-250 µs, occasionally into the low milliseconds. This repository's own investigation ([`docs/benchmark-analysis-2026-07-22.md`](docs/benchmark-analysis-2026-07-22.md)) tested **11 independent hypotheses** for this by direct measurement — wrong core-pinning topology, C-states (both deep and shallow), IRQ/timer-tick rate, the benchmark's own IPI generation, HT-sibling scheduling contention, RT-runtime throttling, `SCHED_FIFO` priority, producer backpressure, thermal/turbo throttling, and P-state ramp jitter — and eliminated every one of them. The tail's magnitude is essentially identical whether the queue is SPSC or Vyukov SPMC (two structurally unrelated algorithms), which is itself evidence the cause is not a queue-code bug but a property of "single consumer, no redundancy" on this specific COTS hardware — most likely irreducible microarchitectural noise that a 2nd/3rd/4th consumer thread naturally masks by picking up whatever the stalled consumer missed. Read the full document for the measurement trail, including several genuinely novel findings along the way (a topology bug in the original core-pinning code, a TLB-shootdown IPI source traced back to the benchmark harness's own `malloc`/thread-lifecycle behavior, and an `nohz_full` stability dependency on housekeeping-CPU business that reversed the direction of the "does removing desktop load help?" experiment).

### Advantages & Limitations

**Advantages**
- Wait-free single producer (`SPMCQueue`) / lock-free bidirectionally (`SPSCQueue`) — no locks anywhere on the hot path.
- Header-only, no runtime dependency beyond the C++23 standard library.
- Cache-line-isolated hot fields throughout (`head`/`tail`/per-slot `sequence` all `alignas(hardware_destructive_interference_size)` or explicit 64-byte alignment) to prevent false sharing.
- ABA-safe by construction (`SPMCQueue`'s sequence generation advances by capacity every wraparound).
- Ships its own measurement apparatus (three independent benchmark harnesses, percentile recording, an OS-tuning script) rather than leaving latency validation as an exercise for the integrator.
- 2C/4C configurations are demonstrated, on real hardware, to reach HFT-class tail latency (P99.9 sub-µs, Max low single-digit µs) after the documented tuning.

**Limitations**
- `SPMCQueue` is strictly **single-producer** — it is not an MPMC queue; a second concurrent producer is undefined behavior.
- `SPSCQueue` is strictly single-producer/single-consumer — no CAS at all, so it cannot safely support more than one thread on either side.
- Fixed capacity, power-of-two only, set at construction — no dynamic resizing.
- **The single-consumer tail-latency anomaly documented above is unresolved** — on this specific hardware, 1-consumer configurations should not be assumed to hold a tight P99.9+ bound without validating on your own machine.
- Core-pinning constants in the benchmark harnesses and tuning script are hardcoded for one machine's topology and must be re-derived (via `lscpu --all --extended`) for any other machine — see §4.
- TSC-based timing (`TscClock.hpp`) is x86/x86-64-only and assumes a single-socket system.
- The full OS-tuning story (`isolcpus`/`nohz_full`/GRUB changes) requires root and a reboot; it is not something you can validate in a container or a machine you don't control.

### Practical Use-Case Scenarios

- **Market-data fan-out**: a single feed-handler thread (producer) publishing normalized ticks to multiple independent downstream consumers (risk engine, strategy engine, persistence/journaling) via `SPMCQueue` — each consumer reads every tick independently, matching the "single writer, multiple readers, no work-splitting" shape `SPMCQueue` is built for.
- **Order-entry / execution hot path**: `SPSCQueue` between a single strategy thread and a single order-gateway thread — the tightest-possible two-thread handoff, no CAS overhead at all.
- **Sharded, per-instrument processing**: statically partition instruments across N `SPSCQueue`s (one per consumer, keyed by e.g. `hash(symbol) % N`) instead of a single `SPMCQueue` with dynamic work-stealing — avoids inter-consumer CAS/cache-line contention entirely and keeps per-instrument order guaranteed, at the cost of needing to know the partitioning key at publish time.
- **Logging / journaling / replication side-channel**: an `SPMCQueue` consumer dedicated purely to persistence, running alongside the business-logic consumer(s), decoupling "must not lose this event" from "must process this event fast."
- **General task offload** (`ThreadPool`): anything that doesn't belong on the latency-critical path — deserialization, batch computation, I/O-bound work — submitted via `enqueue()` and awaited via the returned `std::future` when the result is needed.
- **NOT recommended as-is**: any scenario requiring a guaranteed sub-millisecond P99.9+ bound with only one consumer thread, until the anomaly in §6 above is either resolved or validated absent on your specific target hardware.
