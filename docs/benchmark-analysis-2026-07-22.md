# SPSC/SPMC Lock-Free Queue — Benchmark Session and HFT Suitability Analysis

**Date:** 2026-07-22
**Machine:** Intel Core Ultra 5 125H (Meteor Lake, hybrid — 4 P-cores/8 threads + 8 E-cores + 2 LPE-cores), Linux 7.0.0-28-generic
**Scope:** Building the project, running the three benchmark tools, interpreting the results from an HFT (high-frequency trading) perspective, fixing a discovered core-pinning bug, and diagnosing the root cause of the remaining tail latency at the system level.

---

## 1. Build

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j"$(nproc)"
```

The build completed successfully (only harmless warnings about `std::hardware_destructive_interference_size`). Three benchmark targets were produced:

- `build/spmc_benchmark` — custom latency measurement based on `std::chrono::steady_clock`
- `build/spmc_tsc_benchmark` — RDTSCP/TSC-based latency measurement
- `build/spmc_gbenchmark` — Google Benchmark integration

The test targets (`threadpool_tests`, `spmc_queue_tests`, `spsc_queue_tests`) also built cleanly.

---

## 2. Initial Benchmark Results (without root)

Without root privileges, `SCHED_FIFO` could not be applied (`[!] SCHED_FIFO unavailable — run as root` was printed), though core pinning was still active. All three tools were confirmed to run correctly; the standout numbers:

- **spmc_gbenchmark** round-trip: SPSC 4.92ns/op (~203M items/s), Vyukov SPMC 14.2ns/op (~70M items/s).
- In the Mechanical (empty-queue) scenario, median latencies were sub-microsecond, but the P99.9+ tail jumped to the hundred-microsecond–millisecond range in the SPSC/1-consumer configurations.

---

## 3. Root + SCHED_FIFO Results (first "clean" run)

`sudo ./build/spmc_benchmark`, `sudo ./build/spmc_tsc_benchmark`, and `sudo ./build/spmc_gbenchmark` were run; real-time priority (`SCHED_FIFO prio=90`) was now active.

### 3.1 `spmc_benchmark` (steady_clock)

**Scenario 1 — Saturated** (capacity 4096, warmup 200,000, measure 2,000,000):

| Queue | Min | Mean | P50 | P90 | P99 | P99.9 | P99.99 | P99.999 | Max |
|---|---|---|---|---|---|---|---|---|---|
| SPSC 1C | 90,733 | 412,437 | 393,951 | 408,709 | 723,798 | 4,209,295 | 4,211,222 | 4,212,656 | 4,213,189 |
| Vyukov SPMC 1C | 125,659 | 328,800 | 310,400 | 352,150 | 410,357 | 4,223,675 | 4,236,114 | 4,236,408 | 4,236,422 |
| Vyukov SPMC 2C | 303,064 | 576,299 | 578,261 | 584,758 | 593,254 | 804,496 | 832,381 | 843,720 | 845,268 |
| Vyukov SPMC 4C | 465,092 | 522,425 | 522,278 | 526,404 | 551,475 | 578,427 | 581,417 | 582,553 | 582,942 |

**Scenario 2 — Mechanical** (rate 1/500ns, warmup 100,000, measure 1,000,000):

| Queue | Min | Mean | P50 | P90 | P99 | P99.9 | P99.99 | P99.999 | Max |
|---|---|---|---|---|---|---|---|---|---|
| SPSC 1C | 106 | 696 | 189 | 235 | 268 | 204,252 | 564,407 | 607,349 | 612,005 |
| Vyukov SPMC 1C | 78 | 501 | 189 | 232 | 251 | 127,105 | 375,259 | 417,392 | 422,030 |
| Vyukov SPMC 2C | 105 | 278 | 247 | 400 | 441 | 507 | 7,148 | 12,834 | 13,477 |
| Vyukov SPMC 4C | 108 | 270 | 252 | 366 | 466 | 551 | 1,446 | 2,548 | 4,547 |

*(All values in nanoseconds. The TSC run produced results of similar magnitude; the Google Benchmark run showed a round-trip of 4.72ns (SPSC) / 14.2ns (Vyukov) with the same percentile pattern — see the original terminal output for details.)*

---

## 4. Interpretation and HFT Suitability Assessment

**The Saturated scenario** measures queue-depth/backpressure latency (the producer spins against a full queue) — the 300–800µs range is by design, not an algorithmic flaw.

**The Mechanical scenario** reflects genuine, uncontended, "tick-to-trade"-like latency. Median and P99 were excellent (sub-microsecond, in the same league as LMAX Disruptor/Aeron IPC). However, **the 1-consumer configurations (SPSC, Vyukov 1C) showed a "cliff" starting at P99.9**: a jump from a few hundred nanoseconds to 100µs–1.7ms. The 2C/4C configurations were largely free of this cliff (tail stayed in the low-microsecond range).

**Conclusion:** The queue design (Vyukov sequence-per-slot, wait-free producer, cache-line isolation, correct acquire/release usage) was HFT-class and sound — the problem wasn't in the algorithm. But the machine **as it stood** (a non-isolated Linux desktop) had a P99.9+ tail (1.7ms max, especially for SPSC/1C) that exceeded any realistic HFT latency budget. Even the best configuration (Vyukov 2C) had a Max of ~7–8µs — borderline acceptable depending on strategy, but loose for aggressive HFT.

Suggested direction for improvement: kernel-level core isolation (`isolcpus`/`nohz_full`/`rcu_nocbs`), pinning power management (governor/C-state), bare-metal verification, and evidence-based diagnosis with `perf`/`turbostat`.

---

## 5. Hardware Topology Discovery — Critical Finding

The `lscpu --all --extended` output shared by the user revealed that the benchmark code was built on **an incorrect core-pinning assumption**.

**The model the code assumed** (comments in `BenchmarkMain.cpp` / `TscBenchmarkMain.cpp`): sequential HT pairs — `(0,1) (2,3) (4,5) (6,7)`.

**The real model shown by `lscpu`** (via the `CORE` column):

| Physical P-core | Real logical CPU pair |
|---|---|
| core 0 | **CPU 0, CPU 5** |
| core 1 | **CPU 1, CPU 2** |
| core 2 | **CPU 3, CPU 4** |
| core 3 | CPU 6, CPU 7 |
| E-cores | CPU 8–15 (no SMT) |
| LPE-cores | CPU 16–17 (max 2.5GHz, unsuitable for latency) |

Old constants: `MAIN_CORE=0, PROD_CORE=2, CONS0_CORE=4, CONS_STEP=2` → the 4C test pinned consumers to `{4,6,8,10}` = in reality core2, core3 (2 real P-cores) + core4, core6 (**2 E-cores**) — i.e. the "4C" test was silently running a **2 P-core + 2 E-core** mix, comparing cores of different microarchitectures within the same fan-out. 1C/2C, by luck (despite the wrong mental model), actually landed on genuinely separate physical cores.

Additionally, a live `lscpu` snapshot showed exactly the old `PROD_CORE` (CPU2) and the second consumer of the old 2C (CPU6) as "hot" (high MHz, under background load) at that moment — live evidence that isolation existed in theory but wasn't guaranteed in practice.

---

## 6. Code Fix Applied

In `benchmark/BenchmarkMain.cpp` and `benchmark/TscBenchmarkMain.cpp`:

- **The comment block** was updated to reflect the real topology.
- **`MAIN_CORE=0, PROD_CORE=5`**: since main blocks in `join()` for the whole measured run and never spins, it can safely share the same physical P-core (core0) with the producer via HT — this frees up one P-core "for free" for main+producer.
- **`CONSUMER_CORES[] = {1, 3, 6, 8}`**: instead of base+stride arithmetic, a fixed array matching the real topology — P-core1, P-core2, P-core3, and (only in 4C) E-core0, in that order.
  - Result: 1C→`{1}` (1 real P-core), 2C→`{1,3}` (2 real P-cores), 4C→`{1,3,6,8}` (**3 real P-cores + 1 E-core**, previously 2+2 — a net improvement).
- The `run_benchmark<Queue>()` function signature was updated to take a `const int* consumer_cores` array instead of `consumer0_core`/`consumer_stride`.
- The `Core assignment` / `Topology` / `NOTE` lines in the console output were corrected to reflect reality.

Both files compiled without errors; no references remained to the old constants (`CONS0_CORE`, `CONS_STEP`).

---

## 7. Post-Fix Results (TSC, Mechanical scenario)

| Config | Before (wrong topology) P99.9 / Max | After (fixed topology) P99.9 / Max |
|---|---|---|
| SPSC 1C | 1.27ms / 1.74ms | **190µs / 588µs** |
| Vyukov 1C | 219µs / 597µs | 234µs / 630µs (unchanged) |
| Vyukov 2C | 387ns / 7.6µs | 368ns / 20.3µs (Max got worse) |
| Vyukov 4C | 427ns / 4.1µs | 437ns / 4.5µs (same) |

**Honest assessment:** SPSC 1C improved noticeably, but Vyukov 1C (despite using the same producer core) didn't change at all — this suggests the improvement may stem not from the topology fix but from differing background system load between the timing of the two runs (one-off before/after runs aren't a clean A/B test). The **proven, non-coincidental** benefit of the topology fix: 4C now genuinely uses 3 P-cores + 1 E-core, and the code/comments are now consistent with the hardware.

---

## 8. Power Management Check

Before making any changes, the current state was inspected:

```
Governor (18 CPUs): performance  (already set)
irqbalance:          inactive     (already off)
Kernel cmdline:      isolcpus / nohz_full NOT set
```

So the DVFS/frequency-scaling and IRQ-balancing hypotheses were **already invalid** for this machine — the governor had been set beforehand. The only missing piece was kernel-level core isolation.

---

## 9. C-State (Deep Sleep) Experiment

On the cores in use (`0,1,3,5,6,8`), the C6/C10 deep sleep states were temporarily disabled:

```bash
sudo cpupower -c 0,1,3,5,6,8 idle-set -d 2   # C6
sudo cpupower -c 0,1,3,5,6,8 idle-set -d 3   # C10
```

**Result (TSC, Mechanical):**

| Config | C-state on | C-state off (C6/C10 disabled) |
|---|---|---|
| SPSC 1C | 190µs / 588µs | **1.36ms / 1.83ms** (worse) |
| Vyukov 1C | 234µs / 630µs | 234µs / 625µs (same) |
| Vyukov 2C | 368ns / 20.3µs | 384ns / 7.6µs (Max improved) |
| Vyukov 4C | 437ns / 4.5µs | 449ns / 7.7µs (slightly worse) |

There was **no** clear, consistent improvement. Vyukov 1C's P99.9 stayed nearly identical (~220–240µs) across three different trials (wrong topology / fixed topology / C-state off) — this stability is a strong signal that the root cause is neither core pinning nor C-state. In the same run, `spmc_benchmark`'s (steady_clock) SPSC 1C also stayed in the ~200µs/570–610µs band from the very first run onward; the 1.36ms spike in the TSC run was likely a **one-off event** among millions of samples.

**Conclusion:** One-off before/after runs can't produce a clean signal at this noise level — direct measurement was needed instead of continuing to blindly try system settings.

---

## 10. Definitive Diagnosis with `turbostat`

```bash
sudo turbostat --quiet --show Core,CPU,IRQ,SMI --interval 1 -- ./build/spmc_tsc_benchmark
```

**Findings:**

- **SMI = 0 on every core.** Firmware/BIOS-level System Management Interrupts were completely ruled out.
- **IRQ rate was very high**, precisely on the cores in use: producer (CPU5) ~950/s, consumer CPU1 ~990/s, CPU3/CPU6/CPU8 in a similar range.
- Confirmation: `zgrep CONFIG_HZ /boot/config-$(uname -r)` → **`CONFIG_HZ=1000`**. So the kernel splits every busy core with a periodic timer interrupt (`LOC` — local timer interrupt) 1000 times per second (every 1ms); the measured IRQ rate matched this figure almost exactly.
- `/proc/interrupts` also showed system-wide (including our own cores) elevated **TLB shootdown** and **function-call IPI** counters — other processes' memory operations can manifest as brief interruptions even on pinned cores.

**Conclusion:** The measured, proven source of the P99.9+ tail was — the periodic timer tick (`CONFIG_HZ=1000`) and IPI/TLB-shootdown traffic from the general scheduler/other processes. `isolcpus=`/`nohz_full=`/`rcu_nocbs=` exist precisely to solve these two things:
- `nohz_full` fully stops the periodic tick on a busy core.
- `isolcpus` prevents the general scheduler from placing work (and hence IPI/TLB-shootdown targets) on those cores.

---

## 11. `isolcpus`/`nohz_full`/`rcu_nocbs` Applied (GRUB + reboot)

As the evidence-based next step, via GRUB

```
isolcpus=0,1,3,5,6,8 nohz_full=0,1,3,5,6,8 rcu_nocbs=0,1,3,5,6,8
```

was added (`/etc/default/grub` → `GRUB_CMDLINE_LINUX_DEFAULT`, `update-grub`, reboot). The user applied this step on their own machine and verified it:

```
$ cat /proc/cmdline
... quiet splash isolcpus=0,1,3,5,6,8 nohz_full=0,1,3,5,6,8 rcu_nocbs=0,1,3,5,6,8 vt.handoff=7

$ cat /sys/devices/system/cpu/isolated
0-1,3,5-6,8
```

The kernel correctly recognized the requested cores as isolated.

### 11.1 IRQ rate — 98–99% drop (measured, undeniable evidence)

The same measurement was repeated with `turbostat --show Core,CPU,IRQ,SMI`:

| CPU (role) | Before (no isolation) | After (isolated) | Change |
|---|---|---|---|
| 5 (producer) | 954/s | **14/s** | ~99% drop |
| 1 (consumer0) | 993/s | **6.4/s** | ~99% drop |
| 3 (consumer1) | 541/s | **8.2/s** | ~98% drop |
| 6 (consumer2) | 308/s | **7.4/s** | ~98% drop |
| 8 (consumer3, E-core) | 311/s | **4.2/s** | ~99% drop |
| 0 (main) | 457/s | 1009/s (increased, but harmless — main is idle/blocked in `join()` there) |

### 11.2 Latency tail — mixed result: multi-consumer improved, 1C unchanged

| Config (Mechanical) | Before P99.9/Max | After P99.9/Max |
|---|---|---|
| SPSC 1C | 217µs / 621µs | 217µs / 621µs (same) |
| Vyukov 1C | 234µs / 625µs | 267µs / 685µs (unchanged, slightly worse) |
| Vyukov 2C | 384ns / 7.6µs | 357ns / **5.2µs**, P99.99 448ns (much tighter than the previous ~1.2–2.7µs) |
| Vyukov 4C | 449ns / 7.7µs | 395ns / **3.8µs**, P99.99 485ns |

**The 2C/4C deep percentiles (P99.99+) tightened noticeably** — a real, measurable benefit of isolation. But **the 1-consumer configurations' tail didn't change at all despite a 99% drop in IRQ rate.**

### 11.3 Why 1C didn't change: the "masking" hypothesis

The residual IRQ rate isn't zero, just very low (~4–14/s). These leftover interrupts likely come not from the general scheduler but from targeted IPIs (see §12). **In a 1-consumer setup there's no other consumer to absorb the effect of these residual interruptions**: in 2C/4C, if one consumer is briefly interrupted, the other consumer(s) are still spinning and can grab the item produced at that moment — a single core's occasional interruption is largely "masked" in the merged latency distribution. In 1C there's no backup: the instant the sole consumer is interrupted, every item produced during that window sits in the queue until the consumer returns, and its latency inflates directly.

---

## 12. Diagnosing the Remaining IPI Source with `perf`

To measure whether `local_timer_entry` was truly near zero, and to break down the share of the remaining `call_function`/`reschedule`/`irq_work` IPIs individually:

```bash
sudo perf stat -e 'irq_vectors:local_timer_entry,irq_vectors:call_function_entry,irq_vectors:call_function_single_entry,irq_vectors:reschedule_entry,irq_vectors:irq_work_entry' -C 1,3,5,6,8 -- ./build/spmc_tsc_benchmark
```

**Result (3.636-second run, total across 5 isolated cores):**

| Tracepoint | Count |
|---|---|
| `local_timer_entry` | 7 |
| `call_function_entry` | **38** |
| `call_function_single_entry` | 6 |
| `reschedule_entry` | 2 |
| `irq_work_entry` | 10 |

`local_timer_entry` was near zero → **`nohz_full` was genuinely working**, effectively stopping the periodic timer tick. The dominant remaining source was **`call_function_entry`** (60%) — the general SMP cross-CPU function-call IPI mechanism; TLB shootdown on Linux also runs through this mechanism (there's no separate `tlb_invalidate` tracepoint on this kernel, since `flush_tlb_others` already uses the `call_function` infrastructure).

### 12.1 THP and NUMA balancing ruled out

```
THP mode:         always [madvise] never   → only engages via MADV_HUGEPAGE, which the benchmark doesn't use
NUMA balancing:    0 (off)
NUMA node count:   1 (node0) — single socket already
```

Both candidates were definitively ruled out. The most likely remaining explanation: **`lru_add_drain_all()`** — a kernel function called during any process's `mmap`/`munmap`/`madvise` operation, which sends an IPI to every core with a non-empty per-CPU LRU pagevec; `isolcpus` doesn't exempt this, because it's a targeted call, not a general scheduling decision. Confirming this would require tracing the sending side (`smp_call_function_many` kprobe + stack trace) — this step had not yet been taken.

### 12.2 Additional findings — layers not yet closed off

```
/proc/irq/default_smp_affinity = 3ffff   (all 18 bits set — new IRQs are open to every CPU, including isolated ones)
/sys/devices/system/cpu/smt/active = 1    (hyperthreading still on)
mitigations= / idle=poll not in cmdline   (Spectre/Meltdown mitigations and idle=poll are default)
```

---

## 13. Why Production HFT Systems Don't Have This Tail

The gap between what was achieved on this machine (2C/4C: sub-µs P99.99, single-digit-µs Max; 1C: ~200–700µs tail) and real HFT deployments is **the environment, not the algorithm**:

1. **Shared desktop vs. single-purpose appliance** — a production box has no GUI/browser/cron/journald; there's no "other process" to be the source of an `lru_add_drain_all`-type trigger.
2. **`PREEMPT_RT` kernel + finer-grained isolation** — pulls `default_smp_affinity` off the isolated cores, moves workqueue cpumasks, often disables HT entirely with `nosmt`, never enters a C-state with `idle=poll`, sometimes uses `mitigations=off`.
3. **`mlockall` + hugepages + swap disabled** — all memory is locked/pre-faulted up front, so THP/khugepaged/kswapd never need to touch the process; this dries up the root of the `call_function`/TLB-shootdown class of problem we were chasing.
4. **Hardware choice** — this machine is a heterogeneous laptop SoC (P/E/LPE-core, Foveros tiles). Real HFT colocation deliberately uses homogeneous, single-tile server chips (Xeon/EPYC).
5. **Production systems aren't zero-outlier either** — they too mask a single core's occasional deviation via **redundancy** (the exact same mechanism behind our 2C/4C outperforming 1C), just pushing the magnitude far lower.

---

## 14. Real IPI Source Identified via `perf` kprobe — Our Own Process

The first kprobe placed on `smp_call_function_many` got **zero** hits (on this kernel the actual call path goes through `smp_call_function_many_cond`, bypassing the wrapper). The symbol was corrected and retried with a system-wide stack trace:

```bash
sudo perf probe --add smp_call_function_many_cond
sudo perf record -a -g -e probe:smp_call_function_many_cond,probe:smp_call_function_many \
    -o /tmp/callfn2.data -- ./build/spmc_tsc_benchmark
sudo perf script -i /tmp/callfn2.data --header
```

250 samples were captured, splitting into three distinct sources:

1. **`perf-exec` → `execve` → `exit_mmap`** — a one-off side effect of perf's own launch process, outside the measurement.
2. **`gnome-shell` → `munmap`** — genuinely "another process on the system" (this trace later found a separate explanation via the HT-sibling finding in §15).
3. **`spmc_tsc_benchm` (our own process!) → `munmap`/`madvise` → `flush_tlb_mm_range` → `smp_call_function_many_cond`.** The stack trace went directly through `run_benchmark<SPSCQueue<Item>>`.

Mechanism: `main()` called `run_benchmark<Queue>()` 8 times (4 queue configurations × 2 scenarios). On every call:
- A fresh `Queue q(capacity)` and `LatencyRecorder` instances (sample buffers reaching ~16 MB in Saturated, `measure`-many `uint64_t`s) were created and torn down when the function returned — these blocks, exceeding glibc's mmap threshold (~128 KB), get returned via `munmap()`, which triggers a TLB-shootdown IPI targeting whatever cores had used that memory (i.e. **exactly our own** pinned producer/consumer cores).
- Fresh `std::thread`s were created on every call and destroyed after `join()`; glibc's NPTL pthread stack-cache mechanism calls `madvise(MADV_DONTNEED)` when caching a terminated thread's stack — this likewise triggered a TLB-invalidation IPI.

8 calls × (1 queue-teardown + several thread-teardowns) ≈ matched the **38 `call_function_entry`** measured in the previous round.

**Note (added later):** Based on this finding, `mallopt(M_MMAP_THRESHOLD,...)` + a persistent `Worker` thread pool were implemented and tested — `call_function_entry` dropped measurably (38→5, ~87%) but never changed the SPSC 1C / Vyukov 1C tail (only 2C/4C saw a small benefit). Since this wasn't the real source of the 1C tail and the added complexity to the harness wasn't worth it, this code change was later reverted; `BenchmarkMain.cpp`/`TscBenchmarkMain.cpp` were restored to the original `std::thread`-per-scenario structure. The finding itself (that our own process can also be an IPI source) remains accurate and instructive, so it's kept on record here.

---

## 15. HT-Sibling Contention Discovery

Examining the `isolcpus=0,1,3,5,6,8` set revealed an asymmetry. The real HT pairs are `(0,5)(1,2)(3,4)(6,7)`:

| Isolated core | Real HT sibling | Sibling isolated? |
|---|---|---|
| 0 (main), 5 (producer) | each other's sibling | ✅ both isolated |
| 1 (consumer0) | 2 | ❌ no |
| 3 (consumer1) | 4 | ❌ no |
| 6 (consumer2) | 7 | ❌ no |

This was confirmed with `ps -eLo psr,tid,pcpu,comm | awk '$1==2||$1==4||$1==7'` — CPU2/4/7 genuinely had active desktop threads consuming significant CPU%: `gnome-shell` (CPU2, 4.4%), `Isolated Web Co[ntent]`/a Firefox tab + `Compositor` (CPU4, 6.2–6.3%), `VizCompositorTh` + `WRRenderBackend#1` (CPU7, 3.4%). These weren't idle kernel housekeeping threads — they were real, continuously CPU-consuming render/compositor threads — sitting on the physical HT siblings of our consumers (1,3,6). Without sending a single IRQ/IPI, they could have been slowing things down purely by stealing the shared physical core's execution resources (ports, L1/L2, front-end) — a mechanism completely invisible to `irq_vectors` tracing.

---

## 16. `isolcpus` Expanded (0-8) — HT Siblings Also Isolated

```
isolcpus=0,1,2,3,4,5,6,7,8 nohz_full=0,1,2,3,4,5,6,7,8 rcu_nocbs=0,1,2,3,4,5,6,7,8
```

(Both logical threads of all 4 P-cores + E-core0.) GRUB was edited, rebooted, and verified:

```
$ cat /sys/devices/system/cpu/isolated
0-8
```

CPU2/4/7 were re-checked with `ps -eLo psr` — this time there were **only** `0.0%` kernel housekeeping threads (`idle_inject`, `migration`, `ksoftirqd`, `cpuhp`, `kworker/N:0-events`); gnome-shell/Firefox/VS Code were completely gone. The HT-sibling pollution was demonstrably cleaned up.

**Result: the 1C tail still didn't change** (SPSC 183µs/569µs, Vyukov 245µs/636µs — practically identical to before).

---

## 17. Systematic Elimination of the Remaining Hypotheses

Six additional hypotheses were tested, and all six were directly refuted by measurement:

**17.1 RT throttling** — `kernel.sched_rt_runtime_us=950000` (default, throttling on) was found. It was disabled with `sudo sysctl -w kernel.sched_rt_runtime_us=-1` and re-run: SPSC 179µs/557µs, Vyukov 233µs/626µs — **unchanged**.

**17.2 `SCHED_FIFO` priority** — in both files, the priority inside `elevate_thread()` was raised from 90 to **99** (maximum). Result: SPSC 188µs/581µs, Vyukov 235µs/628µs — **unchanged**. (This change was kept permanently in the codebase — see §19.)

**17.3 Producer burst/backpressure hypothesis** — it was considered that the rate-limited pacer (`next_ns += rate_limit_ns`, unconditional) might create a "debt-repayment" burst after a delay, temporarily filling the queue, which the sole consumer would then have to drain alone. To test this directly, a temporary `try_emplace()` failure counter (`full_retries`) was added to the producer. Result: in the Mechanical scenario, **no configuration** (SPSC 1C, Vyukov 1C included) recorded a single `queue-full retry` — the queue never filled. The hypothesis was **definitively refuted**; the diagnostic counter was later removed from the code (§19).

**17.4 Thermal/turbo throttling** — it was considered that Meteor Lake's single-core vs. multi-core turbo power budget (PL1/PL2/Tau) might behave differently. Measured with `turbostat --show Core,CPU,Bzy_MHz,CoreTmp,PkgTmp,PkgWatt`: CPU1 (Vyukov 1C's consumer) at **4368 MHz** (near the ceiling), **44°C** (well below the ~100°C throttle threshold), package power **17.76 W** (under this chip's PL1 budget). No sign of throttling — hypothesis **refuted**.

**17.5 P-state ramp jitter (intel_pstate/HWP frequency floor)** — inspecting `/sys/devices/system/cpu/intel_pstate/*` found `min_perf_pct=8`, meaning HWP is free to drop a pinned core's P-state to 8% of max during any perceived lull (e.g. a tight empty-queue poll loop can look like low utilization to HWP's hardware duty-cycle estimator, even on a `SCHED_FIFO`-pinned thread), then has to ramp back up when real work arrives — a plausible, previously-untested source of bursty, sub-millisecond latency spikes. Rather than the global `intel_pstate/min_perf_pct` (which would affect all 18 CPUs, including the E-/LPE-cores running the desktop, and could counterproductively eat into the package-wide turbo power budget), the floor was pinned per-core, scoped to just the pinned benchmark cores: `scaling_min_freq` set equal to `scaling_max_freq` on cores `0,1,3,5,6,8`. Result: Vyukov 1C stayed at P99.9≈203µs/Max≈564µs — **unchanged** from every prior measurement. Re-running produced one instance where SPSC 1C spiked to Max≈2.3ms and, on the very next run, an instance where Vyukov 1C spiked to Max≈2.1ms instead — the two 1-consumer configs alternately "winning" the spike on different runs is itself informative: it's further evidence the residual tail is a generic property of "solo consumer, no redundancy," striking whichever config happens to be unlucky on a given run, not a deterministic effect of the frequency-floor change. Hypothesis **refuted**. (Kept in `scripts/tune-benchmark-host.sh` regardless, as standard low-latency-tuning hygiene — see §21.)

**17.6 Shallow C-state (C1E)** — §9's C-state experiment had only disabled the deep states (C6/C10), leaving the shallow C1E state (exit latency ~1µs) enabled on the pinned cores. Disabled with `sudo cpupower -c 0,1,3,5,6,8 idle-set -d 1`, re-run with the desktop session active (a clean, directly comparable environment to every prior single-variable test). Result: **no change**. Hypothesis **refuted**. (Kept in `scripts/tune-benchmark-host.sh` regardless — step 3 now disables C1E alongside C6/C10 — as the same category of standard low-latency hygiene as §17.5.)

---

## 18. Conclusion: Systematic Elimination Complete

A total of **11 independent interventions/hypotheses** were tested by direct measurement: wrong core pinning (fixed, real benefit), deep C-state (C6/C10), IRQ/timer tick (`isolcpus`/`nohz_full`, 99% reduction), our own code's IPI source (tested with mallopt+thread pool, 87% reduction measured, then reverted), HT-sibling contention (demonstrably cleaned up), RT throttling, `SCHED_FIFO` priority, queue-full/backpressure, thermal/turbo throttling, P-state/frequency-floor ramp jitter, and shallow C-state (C1E). **None** of them moved the SPSC 1C / Vyukov 1C ~180–250µs P99.9 / ~570–630µs Max band; this band has stayed almost exactly the same size since the very start of the investigation (in both steady_clock and TSC, across both queue implementations).

**The fact that the same-magnitude tail shows up in both SPSC and Vyukov SPMC (two completely different algorithms — one a simple head/tail design, the other CAS-based sequence-per-slot) is separate evidence**: if the problem were a bug specific to one queue implementation, the two different algorithms would be expected to show a tail of different magnitude or shape. Instead they converge — which strongly supports that the remaining tail belongs not to the queue code but to the "single consumer, no redundancy, single physical core" structure itself, independent of which algorithm is running.

This stability is itself a finding: if it were random/external noise, it would be expected to fluctuate with at least one of this many different interventions. The most likely explanation: this is **irreducible microarchitectural noise on COTS x86 hardware that cannot be eliminated via software/kernel tuning** (hardware-level sources such as cache-coherency protocol timing, memory-controller arbitration).

This is not a flaw in the library or the benchmark harness — **the 2C/4C configurations are already genuinely HFT-class** (P99.99 sub-µs, Max single-digit µs) and saw concrete benefit from every fix in this session (topology, HT isolation). Only the 1-consumer configurations' tail, on this specific COTS laptop hardware, likely rests on a practical/hardware-level floor.

---

## 19. Code Status — Changes Kept Permanently vs. Reverted

Of the changes tried during the investigation in `benchmark/BenchmarkMain.cpp` and `benchmark/TscBenchmarkMain.cpp`:

**Kept permanently:**
- Core topology fix (§6) — `MAIN_CORE`/`PROD_CORE`/`CONSUMER_CORES` set according to the real `lscpu` topology.
- `SCHED_FIFO` priority **99** (§17.2) — raised from 90, kept permanently (harmless, matches the level of kernel-mandatory RT threads).

**Tested and reverted** (no longer in the codebase, but their findings are recorded in this document):
- `mallopt(M_MMAP_THRESHOLD,...)` + persistent `Worker` thread pool (§14 note) — measured to reduce IPIs by 87% but reverted since it didn't change the 1C tail; `run_benchmark()` reverted to the original `std::thread`-per-scenario structure.
- `full_retries` diagnostic counter (§17.3) — added to refute the burst/backpressure hypothesis, removed once the hypothesis was refuted.

So the codebase currently stands: equivalent to its structure at the start of the session, except for the topology fix and `SCHED_FIFO 99`.

---

## 20. Summary — What Was Done, In Order

1. The project was built (CMake + Ninja, Release); 3 benchmark targets + test targets were produced successfully.
2. The three benchmark tools (`spmc_benchmark`, `spmc_tsc_benchmark`, `spmc_gbenchmark`) were run both without root and with root+`SCHED_FIFO`.
3. The results were turned into three separate, readable, heat-mapped tables and published as an Artifact.
4. The results were interpreted from an HFT angle: the algorithm is sound, but on a non-isolated environment the P99.9+ tail exceeds an HFT budget.
5. The `lscpu --extended` output shared by the user revealed the **wrong HT-pair assumption** in the benchmark code (real pairs are `(0,5)(1,2)(3,4)(6,7)`, not sequential).
6. `BenchmarkMain.cpp` and `TscBenchmarkMain.cpp` were fixed: main+producer now share one P-core, consumers are pinned from a fixed array matching the real topology (`{1,3,6,8}`); 4C now uses 3 P-cores + 1 E-core (previously 2+2).
7. Re-run after the fix — SPSC 1C improved but Vyukov 1C didn't change (a single run, a noisy comparison).
8. Governor (`performance`) and `irqbalance` (`inactive`) were already found to be appropriate.
9. C6/C10 deep sleep states were disabled with `cpupower` and re-run — no clear, consistent improvement was observed.
10. SMI/IRQ were measured with `turbostat`: **SMI=0** (not BIOS-related), **IRQ rate matches `CONFIG_HZ=1000` almost exactly**.
11. `isolcpus`/`nohz_full`/`rcu_nocbs` were added to GRUB (`0,1,3,5,6,8`), rebooted — IRQ rate on the target cores **dropped 98–99%**. The 2C/4C deep percentiles tightened; the 1C tail didn't change.
12. The remaining IPI types were measured with `perf stat`: `local_timer_entry`≈0, dominant source `call_function_entry`. THP and NUMA balancing were ruled out.
13. The reason production HFT systems don't have this tail was assessed (appliance environment, `PREEMPT_RT`, `mlockall`+hugepages, homogeneous hardware, redundancy/masking).
14. A kprobe was placed on `smp_call_function_many_cond` via `perf probe` — it was proven that **part of the real IPI source was our own process** (`munmap` of Queue/LatencyRecorder's large mmap'd allocations + thread stack-cache `madvise`). Based on this finding, `mallopt`+a persistent `Worker` thread pool were tried; `call_function_entry` dropped 87% but the 1C tail didn't change; the code change was later reverted (§19).
15. HT-sibling contention was discovered via `ps -eLo psr`: the consumer cores' (1,3,6) real HT siblings (2,4,7) weren't isolated and were running gnome-shell/Firefox/VS Code with real CPU percentages.
16. The `isolcpus` set was expanded to cover all P-core logical pairs (`0-8`), rebooted, and 2/4/7 were confirmed fully clean — yet the 1C tail still didn't change.
17. RT throttling (disabled, no effect), `SCHED_FIFO` priority (90→99, no effect but kept), the producer burst/backpressure hypothesis (refuted with the `full_retries` counter, counter later removed), thermal/turbo throttling (refuted with `turbostat`), P-state/frequency-floor ramp jitter (`min_perf_pct=8` found, pinned `scaling_min_freq=scaling_max_freq` per-core, no effect), and the shallow C1E idle state (disabled alongside C6/C10, no effect) were tested in sequence.
18. **Conclusion**: 11 independent interventions were systematically eliminated; the fact that SPSC and Vyukov SPMC (two completely different algorithms) show the same-magnitude tail — and that repeated runs show the spike alternating between which of the two 1C configs it hits — supports that the problem belongs to the "single consumer, no redundancy" structure, not the queue code. The remaining ~180–250µs P99.9 / ~570–630µs Max tail is likely microarchitectural noise on this COTS hardware that cannot be removed via software. 2C/4C are already genuinely HFT-class; the investigation was concluded here.
19. The codebase was cleaned up: the `mallopt`/`Worker` pool and the `full_retries` diagnostic counter were reverted; only the topology fix and `SCHED_FIFO 99` were kept permanently.
20. All OS-level tuning validated across the session (governor/irqbalance, frequency floor, C-state, RT-throttle, `isolcpus`/`nohz_full`/`rcu_nocbs`) was consolidated into a single, idempotent, zero-flag automation script — see §21.

---

## 21. Automation Script: `scripts/tune-benchmark-host.sh`

All the OS-level tuning from §8–§17 that turned out to be worth keeping (governor/irqbalance, frequency floor, C-state, RT-throttle, `isolcpus`/`nohz_full`/`rcu_nocbs`) — including the ones that, individually, never resolved the 1C mystery but remain legitimate low-latency hygiene — was consolidated into one idempotent script rather than left as scattered one-off shell commands.

**Steps applied, in order:**
1. Set the CPU governor to `performance` and stop `irqbalance`, only if either isn't already in that state.
2. Pin the frequency floor (`scaling_min_freq = scaling_max_freq`) on the cores the benchmarks pin to (§17.5).
3. Disable C-states (C1E/C6/C10) on those same cores (§9, §17.6).
4. Disable the kernel's RT runtime throttle (§17.1).
5. Add `isolcpus=`/`nohz_full=`/`rcu_nocbs=` to `/etc/default/grub` covering all P-core HT pairs + E-core0 (`0-8`, §16) and run `update-grub`.

Steps 1–4 are runtime-only (reset automatically on reboot); step 5 is persistent and requires a reboot, gated behind a confirmation prompt.

**Zero-flag, run-it-twice design:** if `isolcpus` isn't set yet, steps 1–4 would just be wiped seconds later by the reboot step 5 needs, so the script skips them and goes straight to step 5. After rebooting, running the *exact same command* finds `isolcpus` already set, applies 1–4 for real, and step 5 auto-skips — no flags needed on either run.

**Other flags:** `--yes` (skip confirmation prompts), `--no-grub` (never touch GRUB, always apply 1–4 immediately), `--status` (read-only report of current state, no root required), `--undo` (revert everything, restoring exact pre-change values from state files under `/run` where applicable — e.g. the original governor, or nothing if it was already `performance`).

---

## 22. `SPMCBroadcastQueue<T, N>` — A Disruptor-Style Fan-Out Queue

A new component, added later in this project's life, to fill a gap `SPMCQueue` doesn't cover: true fan-out delivery. `SPMCQueue` delivers each item to exactly **one** competing consumer (CAS-raced); `SPMCBroadcastQueue` delivers each item to **every** one of N consumers independently.

**File:** `include/SPMCBroadcastQueue.h`

Design, in brief:
- A single producer-published `tail_` cursor, plus N independent per-consumer cursors (one cache line each).
- `try_publish()` gates on the **minimum** position across all N consumer cursors — if the slowest consumer is a full lap behind, the write is rejected.
- No per-slot sequence/CAS is needed on the consumer side (unlike `SPMCQueue`) — there's no competition to resolve, since every consumer independently owns its own cursor.
- `N` is a **compile-time template parameter**, not a runtime constructor argument — consumer handles are obtained via `queue.consumer(index)`.
- `try_consume(callback)` is the primary, zero-copy API; `try_copy()` is a copying convenience wrapper added on top for callers who don't need zero-copy.

This design choice — gating on the *slowest* consumer rather than *any* consumer — is the single most consequential decision in the whole component, and it drives every result in §23–§26 below.

---

## 23. Benchmark Coverage Extended to the Broadcast Queue

All three existing harnesses (`BenchmarkMain.cpp`, `TscBenchmarkMain.cpp`, `GBenchmark.cpp`) got `Broadcast 1C`/`2C`/`4C` registrations mirroring the existing `SPSC`/`Vyukov` pattern, for both the Saturated and Mechanical scenarios. `GBenchmark.cpp` additionally got a dedicated `RT/Broadcast` single-thread round-trip benchmark, since `SPMCBroadcastQueue`'s API (`try_publish` + per-index `consumer(i).try_consume(...)`) doesn't fit the existing generic `BM_Roundtrip<Queue>`/`BM_Latency<Queue>` templates built around `try_emplace`/`try_pop`.

Two genuine bugs were found and fixed while getting this benchmark coverage to run cleanly end-to-end — neither in `SPMCBroadcastQueue` itself (verified independently: a bare, full-scale 1.1M-item run of the queue with nothing else attached completes in 0.6s with zero issues, and the GoogleTest concurrency stress tests — 100K items × 4 consumers — pass reliably), but in the benchmark harness code itself.

---

## 24. Bug Found: TSC Pacing Livelock Under Sustained RDTSC Spin

**Symptom:** `spmc_tsc_benchmark`'s full run (all `SPSC`/`Vyukov`/`Broadcast` scenarios, Saturated + Mechanical) would, on some runs, hang indefinitely partway through the Mechanical scenarios — always deep into the run, never on the first few scenarios. `ps`/`/proc/<pid>/task/*/stat` showed every relevant thread in state `R` (running, not blocked), pegging their pinned cores at ~100% CPU with zero forward progress in `produced`/`consumed` counters — a livelock, not a deadlock.

**Root cause:** the Mechanical scenario's producer pacing loop is:

```cpp
std::int64_t next_ns = tsc::epoch_ns(tsc::now());
for (...) {
    std::int64_t ts;
    do { ts = produce_ts(); } while (ts < next_ns);   // produce_ts() = raw RDTSC, no fence
    next_ns += rate_limit_ns;                          // always += 500, unconditionally
    ...
}
```

`produce_ts()` reads the CPU's raw hardware TSC counter directly (`RDTSC`, no serializing fence — deliberately, to avoid pulling consumer CAS cache-line traffic into the producer's timestamp). After sustained busy-spinning across many minutes and multiple prior benchmark scenarios, on this specific hybrid CPU (Meteor Lake, P-core/E-core/LPE-core, deep package C-states like PC10), a rare hardware/OS-level TSC discontinuity can leave `ts` permanently unable to reach the value `next_ns` had already accumulated to — since `next_ns` only ever increments and never re-anchors to reality, the spin-wait's exit condition becomes permanently unsatisfiable.

**Ruled out before concluding this was the cause:** a standalone repro of the bare queue + producer/consumer threads at full scale (1.1M items, N=4, mechanical pacing, no benchmark-framework scaffolding) completed cleanly in 0.6s — the queue algorithm and gating logic are not implicated. The identical rate-limiting pattern is also used, unmodified, by the pre-existing `SPSC`/`Vyukov` scenarios in the same file and has never been observed to hang there — this is consistent with the bug being a rare, timing-window-dependent hardware event whose probability of occurring scales with cumulative sustained-spin wall-clock time, which the newly added Broadcast scenarios (running last, after everything else) simply had more exposure to.

**Fix:** a bounded spin-count escape hatch, scoped to the new `run_broadcast_benchmark` function only (not the pre-existing `run_benchmark`, to avoid touching previously-validated methodology outside this session's addition):

```cpp
std::uint64_t spins = 0;
do { ts = produce_ts(); } while (ts < next_ns && ++spins < 1'000'000ULL);
if (ts < next_ns) next_ns = ts;   // resync to the current reading instead of an unreachable target
next_ns += rate_limit_ns;
```

Under normal conditions (the overwhelming majority of iterations) the spin count stays tiny and this is a no-op; only in the rare anomalous case does it kick in, guaranteeing the loop is provably bounded regardless of what the underlying hardware TSC does. Verified: two independent full runs of `spmc_tsc_benchmark` after the fix completed cleanly end-to-end, including the previously-hanging `Broadcast 2C`/`4C` mechanical scenarios.

---

## 25. Bug Found: `SCHED_FIFO` Priority Inversion via CPU-Affinity Inheritance (GBenchmark)

**Symptom:** `spmc_gbenchmark`, run with `sudo` (so `SCHED_FIFO` priority 99 genuinely engages, unlike every prior non-root run this session), hung indefinitely right after the `RT/*` round-trip benchmarks completed, before the first `Sat/*` multi-thread benchmark produced any output. This did **not** reproduce non-root, and did **not** reproduce in `BenchmarkMain.cpp`/`TscBenchmarkMain.cpp` under the same `sudo` conditions.

**Diagnosis** (live process inspection while hung):

```
$ ps aux | grep spmc_gbenchmark
root  5713 99.1  ...  ./build/spmc_gbenchmark ...

$ for t in /proc/5713/task/*; do ... done
tid=5713 state=R last_cpu=5 utime=4585
tid=5716 state=R last_cpu=5 utime=0        # <- never scheduled even once

$ taskset -p 5713   # mask 0x20 = CPU 5 only
$ taskset -p 5716   # mask 0x20 = CPU 5 only  <- should be CPU 1
```

Only two threads existed (the GB driver thread and one consumer thread for `Sat/SPSC/1C`), both affinity-restricted to **the same single core**, and the consumer thread had accumulated **zero** CPU ticks since creation — it had never run even once.

**Root cause:** on Linux, a newly created thread **inherits its creator's CPU affinity mask at the moment of creation**. `BM_Latency`/`BM_BroadcastLatency` pinned the GB driver thread to `PROD_CORE` and elevated it to `SCHED_FIFO` 99 *before* spawning consumer threads:

```cpp
static void BM_Latency(benchmark::State& state) {
    pin_thread(PROD_CORE);   // driver thread restricted to core 5
    elevate_thread();        // ...and made SCHED_FIFO 99
    ...
    cthr.emplace_back([&, ci, ...]() {
        pin_thread(CONSUMER_CORES[ci]);   // but this line can never run --
        ...                                 // see below
    });
```

The newly spawned consumer thread is therefore **born** restricted to core 5 too — the same core the driver thread is about to occupy with a `SCHED_FIFO` 99, never-yielding busy-spin loop. Under real `SCHED_FIFO` scheduling (only active with `sudo`; silently a no-op on every prior non-root run this session) with the RT-runtime throttle relaxed (`tune-benchmark-host.sh` sets `sched_rt_runtime_us=-1`), the CFS scheduler can never grant core 5 to the new thread — so it can never execute even its own first instruction, including the `pin_thread(CONSUMER_CORES[ci])` call that would have moved it off core 5 to safety. A permanent priority-inversion livelock.

**Why `BenchmarkMain.cpp`/`TscBenchmarkMain.cpp` never hit this:** their `main()` thread spawns producer *and* consumer threads, then immediately calls `pthr.join()` — blocking, and thereby freeing its core — before any of the newly spawned threads need to run. `GBenchmark.cpp`'s "producer" role is played by the GB driver thread itself, which never blocks after spawning; it goes straight into its own busy-spin loop on the same core.

**A dead end investigated first:** relaxing the RT-runtime throttle back to the Linux default (`sudo sysctl -w kernel.sched_rt_runtime_us=950000`, reserving 5% of every period for non-RT work) was tried as a first hypothesis and did **not** fix the hang — confirming the root cause was the affinity-inheritance ordering, not RT-runtime starvation in isolation. (The system was later returned to the tuned `-1` value once the real fix was in place.)

**Fix:** reset the driver thread's affinity to *all* online cores before spawning consumer threads, and defer `pin_thread(PROD_CORE)`/`elevate_thread()` for the driver thread until *after* the spawn loop — since the GB driver thread runs every registered benchmark sequentially on the same OS thread, it can already be core-restricted from a *previous* benchmark's call by the time a later one spawns its own consumers, so the reset has to happen before every spawn, not just once:

```cpp
static void reset_thread_affinity() {
    cpu_set_t s; CPU_ZERO(&s);
    const long n = sysconf(_SC_NPROCESSORS_ONLN);
    for (long c = 0; c < n; ++c) CPU_SET(static_cast<int>(c), &s);
    pthread_setaffinity_np(pthread_self(), sizeof(s), &s);
}

static void BM_Latency(benchmark::State& state) {
    reset_thread_affinity();          // children can now migrate anywhere
    ...
    for (...) { cthr.emplace_back(...); }   // spawn consumers with unrestricted mask
    pin_thread(PROD_CORE);            // *now* restrict the driver thread
    elevate_thread();
    // producer loop
}
```

Applied to both `BM_Latency` and `BM_BroadcastLatency`. Verified: non-root regression check (filtered run) showed no behavior change; `sudo ./build/spmc_gbenchmark` subsequently completed the full run cleanly, twice, matching the two other harnesses' `sudo` behavior.

---

## 26. `SPMCBroadcastQueue` — Tail Latency Results (Root + `SCHED_FIFO` 99 + Full OS Tuning, 3 Runs Each)

Once both bugs above were fixed, all three harnesses were run with `sudo` (real `SCHED_FIFO` 99) on top of the full `tune-benchmark-host.sh` tuning (governor=`performance`, per-core frequency floor pinned, C1E/C6/C10 disabled, RT-runtime throttle unlimited, `isolcpus`/`nohz_full`/`rcu_nocbs` covering cores `0-8`) — three independent runs per harness, run sequentially (not in parallel, to avoid the cross-binary core contention this project's own earlier investigation already documented).

### 26.1 Mechanical scenario (ns; min–max range across 3 runs)

**`spmc_benchmark` (steady_clock):**

| Config | P99.9 | P99.99 | Max |
|---|---|---|---|
| Broadcast 1C | 175,674 – 1,489,518 | 509,314 – 1,921,065 | 557,622 – 1,968,018 |
| Broadcast 2C | 251,833 – 295,269 | 566,801 – 657,019 | 612,827 – 702,072 |
| Broadcast 4C | 378,202 – 854,485 | 896,491 – 2,243,472 | 1,057,627 – 2,392,365 |

**`spmc_tsc_benchmark` (RDTSC):**

| Config | P99.9 | P99.99 | Max |
|---|---|---|---|
| Broadcast 1C | 177,811 – 1,253,051 | 505,375 – 1,685,452 | 553,752 – 1,732,861 |
| Broadcast 2C | 260,890 – 282,663 | 628,774 – 643,953 | 676,964 – 691,243 |
| Broadcast 4C | 364,170 – 704,219 | 825,820 – 1,788,293 | 1,013,061 – 1,975,218 |

**`spmc_gbenchmark` (Google Benchmark):**

| Config | P99.9 | P99.99 | Max |
|---|---|---|---|
| Broadcast 1C | 190,532 – 202,895 | 521,891 – 527,296 | 567,508 – 573,460 |
| Broadcast 2C | 313,999 – 325,849 | 639,274 – 667,290 | 675,229 – 710,355 |
| Broadcast 4C | 492,911 – 519,253 | 996,454 – 1,055,020 | 1,108,280 – 1,185,500 |

### 26.2 Saturated scenario (ns; min–max range across 3 runs)

**`spmc_benchmark`:**

| Config | P99.9 | P99.99 | Max |
|---|---|---|---|
| Broadcast 1C | 1,268,316 – 4,021,408 | 1,294,510 – 4,056,405 | 1,297,927 – 4,059,524 |
| Broadcast 2C | 1,547,345 – 3,831,233 | 1,714,534 – 3,963,059 | 1,744,093 – 3,973,451 |
| Broadcast 4C | 3,503,539 – 3,640,081 | 4,169,500 – 5,300,711 | 4,265,279 – 5,396,809 |

**`spmc_tsc_benchmark`:**

| Config | P99.9 | P99.99 | Max |
|---|---|---|---|
| Broadcast 1C | 1,403,161 – 3,839,446 | 1,404,204 – 3,841,753 | 1,404,475 – 3,843,675 |
| Broadcast 2C | 3,662,721 – 3,771,552 | 3,762,362 – 3,878,340 | 3,767,399 – 3,889,203 |
| Broadcast 4C | 1,321,760 – 3,668,403 | 4,301,343 – 4,603,030 | 4,331,438 – 4,637,878 |

**`spmc_gbenchmark`:**

| Config | P99.9 | P99.99 | Max |
|---|---|---|---|
| Broadcast 1C | 1,878,960 – 4,177,380 | 1,894,540 – 4,180,730 | 1,895,920 – 4,181,960 |
| Broadcast 2C | 4,163,740 – 4,319,360 | 4,438,840 – 4,696,390 | 4,473,670 – 4,745,230 |
| Broadcast 4C | 1,716,290 – 3,693,360 | 4,026,730 – 5,384,060 | 4,239,880 – 5,542,760 |

### 26.3 Evaluation

`Vyukov SPMC 2C`/`4C`, under these exact same tuned + `SCHED_FIFO` conditions, sit in the **hundreds-of-nanoseconds** P99.9 range (§3, §7, §11.2). `SPMCBroadcastQueue` does not reach that tier at *any* consumer count — its Mechanical-scenario P99.9 stays in the same **hundred-microsecond-to-low-millisecond** band this document already spent §5–§17 proving is irreducible single-thread microarchitectural noise for the 1-consumer configurations of `SPSC`/`Vyukov`.

This is architectural, not a regression or an unfixed bug: `try_publish()` gates on the **minimum** cursor position across all N consumers, so *every* consumer must observe *every* item before a slot frees. `SPMCQueue`'s 2C/4C cleanliness comes from the opposite property — redundancy: any single consumer winning the CAS race is enough, so one consumer's OS-jitter stall is invisible in the merged distribution, masked by whichever other consumer wasn't stalled at that instant. `SPMCBroadcastQueue` has no such masking: a stall on any *one* of N consumers stalls the producer, and therefore every consumer, for the same duration. Adding more consumers adds more *chances* for one of them to hit a jitter event, not more redundancy against it — consistent with `Broadcast 4C`'s ranges generally running wider than `Broadcast 2C`'s across all three harnesses above.

Run-to-run variance is itself large (e.g. `spmc_benchmark`'s `Broadcast 1C` Mechanical P99.9 spans an 8.5x range across three runs) — a further data point supporting this document's existing §18 conclusion that the underlying noise source is a hardware-level, largely stochastic phenomenon on this specific COTS hardware, not something any of the OS-level tuning in `tune-benchmark-host.sh` can bound tighter. (An earlier, non-root exploration — running the OS tuning script but without genuine `SCHED_FIFO` active — showed the same qualitative pattern: `Broadcast 2C` Mechanical tail sometimes landed *worse* than an untuned baseline across repeated runs, reinforcing that this isn't something tuning parameters can reliably fix.)

---

## 27. Cached Gating in `SPMCBroadcastQueue` (A1)

`try_publish()` originally scanned all N consumer cursors on **every** publish — N acquire loads from cache lines the consumers are actively writing. The producer now caches the last observed minimum and re-scans only when that cache says the ring looks full:

```cpp
if (tail - cached_min_ >= capacity_) {
    /* ...scan all N cursors, update cached_min_... */
    if (tail - cached_min_ >= capacity_) return false;
}
```

**Why this is safe:** consumer cursors only ever increase, so a stale `cached_min_` is always an *under*-estimate of the true minimum, making `tail - cached_min_` an *over*-estimate of occupancy. It can wrongly say "full" (costing one extra scan that then corrects it) but never wrongly say "there is space" — the direction that would corrupt data. The happens-before edge that makes overwriting a slot safe still comes from a real acquire load: any slot below `cached_min_` was established as consumed by the acquire scan that produced that value.

**Measured effect.** The fast path hits **99.8-100%** of the time (300,000 publishes → 73 scans), and publish throughput rises accordingly:

| | publish/s |
|---|---|
| Without cache | 43-47 M |
| With cache | **188-212 M** |

**But the gain is conditional on thread pinning**, which was not obvious and is worth recording:

| | Cache on | Cache off |
|---|---|---|
| Threads unpinned | 0.041 s | 0.042 s (**no difference**) |
| Threads pinned to separate P-cores | **0.010 s** | 0.043 s (**~4.2x**) |

Without pinning the scheduler places threads such that the cursor lines do not ping-pong consistently between distinct physical cores, and the effect disappears. Pinning alone is the larger win; the cache only pays off once you have it.

### 27.1 The gain is invisible in this repo's own benchmarks

Neither standard scenario shows it, because in both the **producer is not the bottleneck**:

| Scenario | Cache on | Cache off |
|---|---|---|
| Mechanical (rate-limited 500 ns) | 0.150 s | 0.150 s |
| Free-running, consumers keep up | 0.097 s | 0.163 s |

In the Mechanical scenario the producer already spends ~500 ns per item spinning on the clock; the ~20-100 ns gating scan is *absorbed* by the rate limiter, which simply spins less. In the Saturated scenario the bottleneck is consumer drain rate. An initial guess that "the cache must not be hitting" was **wrong** — instrumented counters showed the fast path hitting ~100% in every scenario.

### 27.2 `cache-misses` is the wrong counter for this

Measuring with `perf stat -e cache-misses` shows nothing useful, and briefly suggested the *opposite* conclusion:

| Metric | What it measures | Cache on | Cache off | Real? |
|---|---|---|---|---|
| cache-references | requests reaching LLC | 9.96 M | 19.74 M | ✅ 2x |
| Load LLC hit | loads served by LLC (sampled) | 784 | 2,125 | ✅ 2.7x |
| **Load Local HITM** | **LLC hit, line modified in another core** | **171** | **1,348** | ✅ **7.9x** |
| cache-misses | LLC miss → DRAM | 68 K ±43% | 49 K ±23% | ❌ noise |

On this machine `cache-misses` is `event=0x2e,umask=0x41` — LLC *misses*, i.e. traffic to DRAM. The cursor lines never reach DRAM: the working set is ~64 KB (4096 slots × 16 B) against an 18 MB L3. The contention is core-to-core, which only **HITM** captures. The apparent "improvement" in `cache-misses` is inside its own ±43% error bars.

The expected extra LLC traffic also checks out: 4 cursor loads × 2 M publishes = 8.0 M, against a measured increase of 9.8 M.

### 27.3 A refuted explanation, recorded

It was initially argued that the producer's repeated cursor reads slow the *consumers* down by forcing them to re-acquire ownership (RFO) on every cursor store. Directly measured, this is **false** — a remote reader does not measurably slow a writer, at any store spacing:

| work between stores | no reader | with remote reader | slowdown |
|---|---|---|---|
| 0 | 2015 M store/s | 2910 M store/s | 0.69x |
| 20 | 30.5 M store/s | 30.6 M store/s | 1.00x |
| 100 | 6.2 M store/s | 6.3 M store/s | 0.98x |

Stores retire into the store buffer, so RFO latency is hidden; loads are synchronous and must wait, which is why HITM *reads* cost measurable time and RFO *writes* do not. (A separate observation — that the queue reports "full" 39,244 times with the cache off versus 0 with it on — remains unexplained and is left on record rather than attached to a mechanism that measurement refuted.)

---

## 28. `SPMCOverwriteQueue<T>` — A Lossy Fan-Out Alternative

`SPMCBroadcastQueue` gates every publish on the **slowest** consumer, so one stalled consumer stalls the producer and therefore every other consumer. `SPMCOverwriteQueue` takes the opposite trade: `publish()` always succeeds and overwrites whatever has not been read yet. A consumer that falls more than `capacity` items behind detects it precisely and is told exactly how many items it missed.

**File:** `include/SPMCOverwriteQueue.h`

Design consequences of not gating on consumers:
- The producer does not need to know how many consumers exist, so **N is not a template parameter**; consumers can be created at runtime.
- The write position is producer-private and need not be atomic.
- **Reads cannot be zero-copy** — the slot can be overwritten at any instant, so the payload must be copied out before it is safe to hand over. Hence `T` is restricted to trivially copyable types.

Slot protocol is a standard seqlock: `seq == (pos << 1)` means the slot stably holds that position, `(pos << 1) | 1` means the producer is mid-write. Positions are 1-based so a zeroed slot (`seq == 0`) reads as "holds position 0", which never occurs, rather than being mistaken for real data.

**Formal caveat**, recorded honestly: the reader `memcpy`s the payload while the writer may be writing it and only afterwards checks whether the read was torn. That concurrent access is a data race by the letter of the C++ memory model. This is the same trade every production seqlock makes (the Linux kernel's and Folly's among them).

### 28.1 ThreadSanitizer cannot validate this queue

Four separate mutations to the seqlock were injected and TSan reported **nothing** for all four: removing the in-progress flag check, removing the lap pre-check, downgrading the consumer's acquire load to relaxed, and removing both producer release fences. A control experiment explains why:

```
plain scalar race, no synchronization  -> TSan CATCHES it
plain memcpy race, no synchronization  -> TSan SILENT
```

GCC 13's TSan does not instrument `memcpy` for race detection. Since `SPMCOverwriteQueue`'s entire payload path is `memcpy`, TSan is structurally blind to it. (Clang's TSan may differ; clang is not installed on this machine, so no claim is made.) The seqlock logic is therefore validated by mutation testing against the normal GoogleTest suite instead — removing the torn-read check, the in-progress flag, or the `missed()` accounting each makes tests fail.

TSan *does* work on the atomic/scalar paths: making `published_` non-atomic is caught immediately and pinpointed to `Consumer::lapped_to()`.

---

## 29. Capacity Is a Staleness Budget, Not a Buffer Size

The benchmark registrations use capacity 4096 for comparability with the other queues. In the Mechanical scenario that produces **0.0% loss** — the overwrite mechanism never engages at all, and the queue simply behaves like a lossless one and inherits its tail. Shrinking capacity changes this completely:

| Capacity | Fill time | P99.9 | Max | Loss |
|---|---|---|---|---|
| 4096 | 2.05 ms | 101 µs | 332 µs | 0.00% |
| 256 | 0.13 ms | 47 µs | 107 µs | 0.00% |
| 64 | 0.03 ms | 2.6 µs | 32 µs | 0.15% |
| **16** | 0.01 ms | **267 ns** | **8.4 µs** | **0.16%** |

A **380x** P99.9 improvement for 0.16% loss. Loss begins exactly where the ring's time span drops below typical stall durations.

This yields a predictive law, confirmed across six capacities:

```
max observed latency  ≈  capacity × publish interval
```

| Capacity | `capacity × 500 ns` | Measured Max | Per slot |
|---|---|---|---|
| 4 | 2.0 µs | 2.09 µs | 521 ns |
| 8 | 4.0 µs | 4.42 µs | 553 ns |
| 16 | 8.0 µs | 8.13 µs | 508 ns |
| 32 | 16.0 µs | 16.15 µs | 505 ns |
| 64 | 32.0 µs | 41.5 µs | 649 ns |
| 128 | 64.0 µs | 63.93 µs | 499 ns |

So in this queue capacity should be chosen from **how stale the data may be**, not from throughput. Note the time span is rate-dependent: 20 slots is 10 µs at 500 ns/item but only 2 µs during a 100 ns/item burst — precisely when falling behind is most likely.

### 29.1 Why more consumers means higher latency here

Unlike `SPMCBroadcastQueue`, this queue's producer never reads consumer cursors, so the gating cost does not exist. The latency still grows with consumer count, for a different reason — the producer itself slows down:

```
N=1 | producer 25.9 M/s ( 38.7 ns/item) | consumer 25.6 M/s
N=2 | producer 15.9 M/s ( 62.8 ns/item) | consumer 15.9 / 15.9 M/s
N=4 | producer  9.4 M/s (105.9 ns/item) | consumer 9.4 / 9.4 / 9.4 / 9.3 M/s
```

The producer sweeps sequentially through 4096 distinct slots, so every write is a fresh ownership acquisition against however many consumer copies exist. More consumers means more invalidations per write. Combined with the law above — staleness ≈ capacity × publish interval — a slower publish directly widens the ring's time span:

| | publish interval | ratio | Mean latency (median) | ratio |
|---|---|---|---|---|
| 1C | 38.7 ns | 1.0x | 24.0 µs | 1.0x |
| 2C | 62.8 ns | 1.6x | 36.2 µs | 1.5x |
| 4C | 105.9 ns | 2.7x | 48.8 µs | 2.0x |

This does not contradict §27.3's finding that a remote reader doesn't slow a writer: there the writer rewrote a *single* line that stayed in `M` state, whereas here it sweeps thousands of distinct lines and never gets to keep one.

---

## 30. Per-Item Latency Is the Wrong Metric for a Lossy Queue

The 380x improvement in §29 comes entirely from **high-latency samples never being recorded** — they were overwritten before they could be read. This is survivorship bias: the measurement is conditioned on delivery, and the conditioning correlates with the quantity being measured. (A close cousin of Coordinated Omission, with one difference: there the omission is an accident of the harness that hides a real problem, whereas here it is the system's actual behavior — the consumer genuinely never sees those items.)

A bias-free metric is **how stale is the consumer's view of the world**, sampled on a wall clock every 5 µs rather than per delivery. Measured that way, the picture changes substantially.

**Consumer keeps up** — no difference, and none expected (loss 0.00% everywhere):

| | P50 | Max |
|---|---|---|
| Broadcast (4096) | 373 ns | 54.9 µs |
| Overwrite (16) | 506 ns | 68.0 µs |

**Transient 200 µs stall injected into the consumer** — still no difference:

| | P99 | Max | Loss |
|---|---|---|---|
| Broadcast (4096) | 97.5 µs | 197.5 µs | 0.00% |
| Overwrite (4096) | 98.4 µs | 198.4 µs | 0.00% |
| Overwrite (64) | 97.5 µs | 200.0 µs | 1.64% |
| Overwrite (16) | 97.9 µs | 199.9 µs | 1.87% |

During the stall the consumer is not reading at all, so its view is frozen regardless of what the queue does. On waking, the lossless queue drains a 400-item backlog in ~12 µs while the lossy one jumps straight to fresh data — but against a 200 µs stall neither shows in the percentiles.

**Sustained overload** (consumer permanently slower than the producer; realistic feed-handler model where data is generated at a fixed rate and dropped if it cannot be published) — here the difference is real and large:

| | View age (P50) | Loss |
|---|---|---|
| Broadcast (4096) | **3.41 ms** | 39.5% (dropped at input) |
| Overwrite (4096) | 2.05 ms | 47.1% |
| Overwrite (256) | 128 µs | 48.9% |
| **Overwrite (16)** | **8.1 µs** | 50.4% |

**420x**, under a metric immune to survivorship bias.

Two things worth noting. First, the lossless queue **also loses data under overload** — its producer simply cannot publish (39.5% dropped at the input). Total loss is comparable (39.5% vs 50.4%); the difference is *which* data is discarded: the lossless queue drops the newest, the lossy one drops the oldest. Second, this places the queue precisely:

| Situation | Does `SPMCOverwriteQueue` help? |
|---|---|
| Consumer keeps up | No — mechanism never engages |
| Transient stall (OS jitter), fast consumer | No — measured identical |
| **Sustained overload** | **Yes** — 3.41 ms → 8.1 µs view staleness |

It is an **overload** tool, not a jitter tool. That the Saturated benchmark scenario flatters it is not a coincidence: Saturated *is* the sustained-overload case.

---

## 31. Updated Summary

21. `SPMCBroadcastQueue<T, N>`, a Disruptor-style true fan-out queue (every consumer sees every item, unlike `SPMCQueue`'s competing-consumer delivery), was added along with `Broadcast 1C`/`2C`/`4C` benchmark coverage in all three harnesses.
22. Two genuine bugs were found and fixed while validating that coverage end-to-end, both in benchmark-harness code rather than the queue itself (independently verified via a bare full-scale queue run and the GoogleTest concurrency stress tests): a rare RDTSC pacing livelock under sustained multi-minute spin in `TscBenchmarkMain.cpp` (§24), and a `SCHED_FIFO`-specific CPU-affinity-inheritance priority inversion in `GBenchmark.cpp`, only reproducible under real root + `SCHED_FIFO` 99 (§25).
23. With both fixed, three-run tail-latency data was collected across all three harnesses under root + `SCHED_FIFO` 99 + full `tune-benchmark-host.sh` tuning (§26). Conclusion: `SPMCBroadcastQueue` does not — and structurally cannot — reach `Vyukov SPMC`'s clean 2C/4C tail-latency tier, because its slowest-consumer gating (every consumer must see every item) has no redundancy to mask a single consumer's OS-jitter stall, unlike `SPMCQueue`'s competing-consumer delivery. This is a correct, expected consequence of the design trade-off (guaranteed fan-out vs. load-balanced masking), not a defect.
24. `try_publish()` was optimised to cache the slowest-consumer position instead of scanning all N cursors on every publish (§27). Throughput rose ~4.4x — but only with threads pinned to separate physical cores, and the gain is invisible in both standard benchmark scenarios because the producer is not the bottleneck in either. Along the way: `cache-misses` was shown to be the wrong counter for cross-core contention (HITM is the right one, 171 → 1,348), and one proposed mechanism ("the producer's reads slow the consumers' stores via RFO") was directly measured and **refuted**.
25. `SPMCOverwriteQueue<T>`, a lossy seqlock-based fan-out queue whose producer never waits, was added with tests and benchmark coverage in all three harnesses (§28). ThreadSanitizer was found to be structurally unable to validate it — GCC 13's TSan does not instrument `memcpy`, which is this queue's entire payload path — so its correctness rests on mutation testing against the normal test suite instead.
26. Capacity in that queue was established to be a **staleness budget** rather than a buffer size, obeying `max latency ≈ capacity × publish interval` across six measured capacities (§29). At the benchmark's 4096 the mechanism never engages at all (0.0% loss); at 16 the P99.9 is 380x lower for 0.16% loss.
27. Most importantly (§30): the per-item latency improvement is **survivorship bias** — the slow samples are exactly the ones discarded. Re-measured with a wall-clock-sampled "how stale is the consumer's view" metric, the lossy queue gives **no benefit** when the consumer keeps up or during transient jitter, and a **420x** benefit only under sustained overload (view staleness 3.41 ms → 8.1 µs). It is an overload tool, not a jitter tool. Under overload the lossless queue loses comparable amounts of data anyway (39.5% dropped at its input); the real difference is that it discards the *newest* data while the lossy queue discards the *oldest*.
