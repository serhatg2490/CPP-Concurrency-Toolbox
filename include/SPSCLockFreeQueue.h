#ifndef SPSC_LOCKFREE_QUEUE_HPP
#define SPSC_LOCKFREE_QUEUE_HPP

#include <atomic>
#include <bit> // std::has_single_bit
#include <concepts>
#include <exception>
#include <stdexcept>
#include <memory>
#include <new> // hardware_destructive_interference_size
#include <optional>


/**
 * Modern C++23 SPSC Queue
 * - Type safety via C++20 Concepts
 * - Cache-line alignment to prevent false sharing
 * - Power-of-two masking for fast index wrapping (no modulo)
 */
template <typename T>
  requires std::is_copy_constructible_v<T> || std::is_move_constructible_v<T>
class SPSCQueue {
private:
  // Hardware cache line size to prevent false sharing
  static constexpr size_t cache_line = 64;

  const size_t capacity_mask;
  const std::unique_ptr<T[]> buffer;

  // Head and tail on separate cache lines so producer and consumer cores
  // don't invalidate each other's cache
  alignas(cache_line) std::atomic<size_t> head{0};
  alignas(cache_line) std::atomic<size_t> tail{0};

public:
  explicit SPSCQueue(size_t capacity)
      : capacity_mask(capacity - 1), buffer(std::make_unique<T[]>(capacity)) {

    // Capacity must be a power of two for bitmask indexing to work
    if (!std::has_single_bit(capacity)) {
      throw std::invalid_argument("Capacity must be a power of 2");
    }
  }

  // Non-copyable and non-movable — ownership must be stable for lock-free safety
  SPSCQueue(const SPSCQueue &) = delete;
  SPSCQueue &operator=(const SPSCQueue &) = delete;

  /**
   * Push: constructs the element in-place via perfect forwarding
   */
  template <typename... Args> bool try_emplace(Args &&...args) {
    const size_t t = tail.load(std::memory_order_relaxed);
    const size_t h = head.load(std::memory_order_acquire);

    if (((t + 1) & capacity_mask) == h) {
      return false; // Full
    }

    buffer[t] = T(std::forward<Args>(args)...);
    tail.store((t + 1) & capacity_mask, std::memory_order_release);
    return true;
  }

  /**
   * Pop: returns std::nullopt when the queue is empty
   */
  [[nodiscard]] std::optional<T> try_pop() {
    const size_t h = head.load(std::memory_order_relaxed);
    const size_t t = tail.load(std::memory_order_acquire);

    if (h == t) {
      return std::nullopt; // Empty
    }

    T result = std::move(buffer[h]);
    head.store((h + 1) & capacity_mask, std::memory_order_release);
    return result;
  }
};

#endif