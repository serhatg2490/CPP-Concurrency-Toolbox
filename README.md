# CPP-Concurrency-Toolbox

Header-only C++23 lock-free queues and a thread pool, with a benchmark suite for measuring their tail latency on your own hardware.

Just add `include/` to your include path — no linking, no runtime dependencies.

---

## Components

| Component | Header | Use it for |
|---|---|---|
| `SPSCQueue<T>` | `SPSCLockFreeQueue.h` | One producer → one consumer. The fastest handoff; no CAS at all. |
| `SPMCQueue<T>` | `SPMCLockFreeQueue.h` | One producer → a pool of interchangeable workers. Each item goes to **exactly one** consumer (load balancing). |
| `SPMCBroadcastQueue<T, N>` | `SPMCBroadcastQueue.h` | One producer → N independent subscribers. **Every** consumer sees **every** item (fan-out). |
| `ThreadPool` | `ThreadPool.h` | General task offload. Mutex-based, not for the latency-critical path. |

**Choosing between `SPMCQueue` and `SPMCBroadcastQueue`:** if your consumers are interchangeable workers sharing one job, use `SPMCQueue`. If they're different systems that each need the same data (e.g. strategy engine + risk engine + journaling), use `SPMCBroadcastQueue`.

All three queues are **single-producer only** — a second concurrent producer is undefined behavior.

---

## Usage

### `SPSCQueue<T>`

```cpp
#include <SPSCLockFreeQueue.h>

SPSCQueue<Order> q(1024);          // capacity MUST be a power of two

// Producer thread
if (!q.try_emplace(order_id, price)) { /* queue full */ }

// Consumer thread
if (auto item = q.try_pop()) {
    process(*item);
}
```

`T` must be copy- or move-constructible.

### `SPMCQueue<T>`

```cpp
#include <SPMCLockFreeQueue.h>

SPMCQueue<Order> q(1024);          // rounded up to a power of two

// Producer
if (!q.try_emplace(order_id, price)) { /* full */ }
q.emplace_wait(order_id, price);   // or block until a slot frees

// Consumers (any number of threads)
if (auto item = q.try_pop()) { process(*item); }
Order item = q.pop_wait();         // or block until an item arrives
```

Also provides `try_pop_expected()` (`std::expected<T, QueueError>`), `capacity()`, `size_approx()`, `empty()`.

`T` must satisfy `std::movable<T> && std::is_nothrow_move_constructible_v<T>`.

### `SPMCBroadcastQueue<T, N>`

`N` (the consumer count) is a **compile-time** template parameter. Each consumer gets a handle by index and must be driven by exactly one thread.

```cpp
#include <SPMCBroadcastQueue.h>

SPMCBroadcastQueue<Tick, 3> q(1024);   // exactly 3 consumers

// Producer — fails only if the SLOWEST consumer is a full lap behind
if (!q.try_publish(symbol, price)) { /* full */ }

// Consumer thread i (i = 0, 1, 2)
auto& c = q.consumer(i);
c.try_consume([](const Tick& t) {      // zero-copy: read in place
    process(t);
});

if (auto t = c.try_copy()) {           // or copy it out
    process(*t);
}
```

`T` must satisfy `std::default_initializable<T> && std::movable<T> && std::is_nothrow_move_assignable_v<T>`.

Also provides `capacity()`, `backlog_approx()`, and `Consumer::position()`.

### `ThreadPool`

```cpp
#include <ThreadPool.h>

ThreadPool pool;                       // defaults to hardware_concurrency()
auto fut = pool.enqueue([](int x) { return x * 2; }, 21);
int result = fut.get();                // 42
```

Workers join automatically on destruction.

---

## Requirements

- **C++23 compiler** — validated with GCC 13.3; recent Clang or MSVC 2022 (17.8+) should work but is untested here.
- **CMake ≥ 3.20**
- **Internet access on first configure** — `FetchContent` downloads GoogleTest and Google Benchmark (build-time only; the library itself has no dependencies).

**Platforms:** Linux is the primary, fully exercised target. Windows is supported (benchmarks use `SetThreadAffinityMask`/`timeBeginPeriod`). macOS likely builds but is untested — core pinning uses a Linux-specific glibc API.

The library itself is portable C++23. Only the benchmark's TSC clock is x86-only, and it falls back to `steady_clock` elsewhere.

---

## Build & Test

```bash
./build.sh              # incremental build
./build.sh --clean      # rebuild from scratch

./test.sh               # unit tests + benchmarks (~20s)
./test.sh --unit-only   # unit tests only (~8s)
```

Windows: `build.bat` / `test.bat`, same flags.

Or drive CMake directly:

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j"$(nproc)"
ctest --test-dir build --output-on-failure -LE benchmark   # unit tests only
```

This produces four test binaries and three benchmark binaries (`spmc_benchmark`, `spmc_tsc_benchmark`, `spmc_gbenchmark`). All are registered with CTest; the benchmarks carry a `benchmark` label so you can filter them in or out.

Current status: **63 unit tests, all passing** (~8s).

---

## Benchmarks

Three harnesses measuring the same two scenarios:

- **Mechanical** — producer rate-limited to one item per 500 ns, so the queue stays near-empty. This is the meaningful number: intrinsic, uncontended transit latency.
- **Saturated** — producer spins flat out against a full queue. Measures queue-depth/backpressure, not algorithmic overhead.

| Binary | Clock |
|---|---|
| `spmc_benchmark` | `steady_clock` |
| `spmc_tsc_benchmark` | `RDTSC`/`RDTSCP` (~20-40 ns overhead vs ~50-100 ns) |
| `spmc_gbenchmark` | Google Benchmark integration, JSON output |

```bash
sudo ./build/spmc_tsc_benchmark      # sudo enables SCHED_FIFO; runs fine without it
./build/spmc_gbenchmark --benchmark_counters_tabular=true
```

Run them one at a time — they pin to fixed cores and will contend with each other if run in parallel.

### Before you trust the numbers

The benchmarks pin threads to hardcoded logical CPUs (`MAIN_CORE`, `PROD_CORE`, `CONSUMER_CORES[]` in `BenchmarkMain.cpp`, `TscBenchmarkMain.cpp`, `GBenchmark.cpp`). **These are tuned for one specific machine and will be wrong on yours.**

Check your real topology first — hyperthread siblings are *not* necessarily sequential:

```bash
lscpu --all --extended
```

Logical CPUs sharing a `CORE` number are real HT siblings. On the reference machine the pairing was `(0,5)(1,2)(3,4)(6,7)`, not the `(0,1)(2,3)...` you might assume. Update the constants to match before drawing conclusions.

Optionally, `scripts/tune-benchmark-host.sh` applies OS-level low-latency tuning (governor, C-states, RT throttle, `isolcpus`/`nohz_full`). Edit `CORES`/`CORE_LIST` at the top to match your topology:

```bash
sudo ./scripts/tune-benchmark-host.sh     # run twice: once, reboot, then again
./scripts/tune-benchmark-host.sh --status # read-only, no root needed
sudo ./scripts/tune-benchmark-host.sh --undo
```

### Results

Reference machine: Intel Core Ultra 5 125H (Meteor Lake hybrid), Ubuntu / Linux 7.0.0, fully tuned, root + `SCHED_FIFO` 99.

Mechanical scenario, TSC clock, nanoseconds:

| Queue | P50 | P99 | P99.9 | Max |
|---|---|---|---|---|
| SPSC 1 consumer | 208 | 244 | 183,353 | 569,459 |
| SPMC 1 consumer | 210 | 227 | 245,490 | 636,052 |
| **SPMC 2 consumers** | 210 | 289 | **334** | **2,666** |
| **SPMC 4 consumers** | 201 | 316 | **382** | **11,013** |
| Broadcast 1 consumer | 162 – 172 | 190 – 710 | 177,811 – 1,253,051 | 553,752 – 1,732,861 |
| Broadcast 2 consumers | 156 – 169 | 215 – 234 | 260,890 – 282,663 | 676,964 – 691,243 |
| Broadcast 4 consumers | 180 – 193 | 215 – 228 | 364,170 – 704,219 | 1,013,061 – 1,975,218 |

SPSC/SPMC rows are one representative run. Broadcast rows are min–max ranges across three runs — run-to-run variance is large enough that any single run would be misleading.

**What this means:**

- **Median and P99 are sub-microsecond everywhere.** The algorithms are sound.
- **`SPMCQueue` with 2+ consumers is the strongest configuration** — sub-microsecond P99.9, single-digit-microsecond max. This is the one to reach for if tail latency matters.
- **Single-consumer configurations show a P99.9 cliff** (~200 ns jumping to 150-250 µs). This repo's investigation tested and eliminated 11 separate hypotheses for it — core pinning, C-states, IRQ/timer ticks, HT contention, RT throttling, thermal throttling, and more. Both SPSC and SPMC show the same magnitude despite being unrelated algorithms, which points at irreducible hardware noise rather than a queue bug. With 2+ consumers it disappears, because another consumer picks up the slack when one stalls.
- **`SPMCBroadcastQueue` keeps that tail at every consumer count**, by design: every consumer must see every item before a slot frees, so one stalled consumer stalls all of them. There's no redundancy to mask jitter — more consumers means more chances to hit it, not fewer.

Full measurement trail, per-harness data, and the Saturated numbers: [`docs/benchmark-analysis-2026-07-22.md`](docs/benchmark-analysis-2026-07-22.md).

---

## Limitations

- **Single producer only** for all three queues — concurrent producers are undefined behavior. `SPSCQueue` is additionally single-*consumer* only.
- **Fixed capacity**, power-of-two, set at construction. No resizing.
- **`SPMCBroadcastQueue`'s `N` is compile-time** and its throughput is capped by the slowest consumer. It is not a drop-in `SPMCQueue` replacement — different problem, different tail-latency profile.
- **Don't assume a tight P99.9 bound** for single-consumer setups, or for `SPMCBroadcastQueue` at any consumer count, without measuring on your own hardware.
- **Benchmark core-pinning constants are machine-specific** and must be re-derived for your CPU.
- **TSC timing is x86-only** and assumes a single socket.
- **`ThreadPool` is mutex-based** — fine for coarse-grained work, not for the sub-microsecond path.
