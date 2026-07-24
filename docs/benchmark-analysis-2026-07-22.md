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
