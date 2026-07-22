#pragma once

// ============================================================================
//  LatencyRecorder — simple helper for P99.9 / P99.99 measurement
// ============================================================================
//
//  Usage:
//    LatencyRecorder rec;
//    rec.reserve(2'000'000);
//    rec.record(end_ns - start_ns);     // value in nanoseconds
//    rec.print_report("queue name");
//
//  Design decisions:
//  • Zero dynamic allocation on the hot path (pre-reserved).
//  • Percentiles computed via sort + direct index — simple and consistent with
//    production tools (HdrHistogram, wrk2). HDR Histogram would be more
//    efficient but requires an external dependency.
//  • Each consumer thread keeps its own recorder; merged at the end.
//    This eliminates mutex/atomic overhead on the hot path.
// ============================================================================

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <vector>

class LatencyRecorder {
    // Class-level static — reset via reset_header() between scenarios.
    inline static bool header_printed_ = false;

public:
    LatencyRecorder() = default;

    explicit LatencyRecorder(std::size_t capacity) {
        samples_.reserve(capacity);
    }

    // Hot path — no allocation if reserve() was called.
    void record(std::uint64_t ns) {
        samples_.push_back(ns);
        sorted_ = false;
    }

    // Appends all samples from another recorder into this one.
    void merge_from(LatencyRecorder& other) {
        samples_.insert(samples_.end(),
                        other.samples_.begin(), other.samples_.end());
        sorted_ = false;
    }

    void reserve(std::size_t n) { samples_.reserve(n); }
    std::size_t count() const noexcept { return samples_.size(); }
    bool empty() const noexcept { return samples_.empty(); }

    // p in [0, 100].  E.g. percentile(99.9) -> P99.9 value (ns).
    // Sorts on first call; subsequent calls are O(1).
    std::uint64_t percentile(double p) {
        if (samples_.empty()) return 0;
        ensure_sorted();
        // Floor indexing rather than linear interpolation —
        // simple and consistent with HdrHistogram / wrk2.
        auto idx = static_cast<std::size_t>(p / 100.0 * static_cast<double>(samples_.size()));
        if (idx >= samples_.size()) idx = samples_.size() - 1;
        return samples_[idx];
    }

    std::uint64_t min_ns() {
        ensure_sorted();
        return samples_.empty() ? 0 : samples_.front();
    }

    std::uint64_t max_ns() {
        ensure_sorted();
        return samples_.empty() ? 0 : samples_.back();
    }

    double mean_ns() {
        if (samples_.empty()) return 0.0;
        std::uint64_t sum =
            std::accumulate(samples_.begin(), samples_.end(), std::uint64_t{0});
        return static_cast<double>(sum) / static_cast<double>(samples_.size());
    }

    // ── Output ──────────────────────────────────────────────────────────────

    void print_report(const char* label) {
        if (samples_.empty()) {
            std::printf("  %-28s  [no data]\n", label);
            return;
        }
        ensure_sorted();

        if (!header_printed_) {
            header_printed_ = true;
            std::printf("\n");
            std::printf("  %-28s  %8s  %8s  %8s  %8s  %8s  %9s  %9s  %9s  %8s\n",
                        "Queue",
                        "Min", "Mean", "P50", "P90", "P99",
                        "P99.9", "P99.99", "P99.999", "Max");
            std::printf("  %s\n",
                "─────────────────────────────────────────────────────────"
                "────────────────────────────────────────────────────────────");
        }

        std::printf("  %-28s  %7lluns  %7.0fns  %7lluns  %7lluns  %7lluns"
                    "  %8lluns  %8lluns  %8lluns  %7lluns\n",
            label,
            (unsigned long long)min_ns(),
            mean_ns(),
            (unsigned long long)percentile(50.0),
            (unsigned long long)percentile(90.0),
            (unsigned long long)percentile(99.0),
            (unsigned long long)percentile(99.9),
            (unsigned long long)percentile(99.99),
            (unsigned long long)percentile(99.999),
            (unsigned long long)max_ns());
    }

    // Resets the table header; the next print_report call will reprint it.
    static void reset_header() { header_printed_ = false; }

private:
    std::vector<std::uint64_t> samples_;
    bool sorted_ = false;

    void ensure_sorted() {
        if (!sorted_) {
            std::sort(samples_.begin(), samples_.end());
            sorted_ = true;
        }
    }
};
