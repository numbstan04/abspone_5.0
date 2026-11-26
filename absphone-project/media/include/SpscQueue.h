/**
 * @file SpscQueue.h
 * @brief Lock-free single-producer / single-consumer bounded queue (Q1 Section 16)
 *
 * [Design principles]
 * 1. Exactly one producer thread may call TryPush / Push; exactly one
 *    consumer thread may call TryPop / Pop. Calling from more than one
 *    thread on either side is undefined behaviour.
 * 2. The queue is bounded: capacity is fixed at construction. Push
 *    either succeeds (returns true) or fails (returns false) without
 *    blocking, so the producer never stalls. The producer is expected
 *    to apply back-pressure upstream when Push returns false.
 * 3. The internal storage is a power-of-two ring of T slots; head_ and
 *    tail_ are independent std::atomic<size_t> counters with
 *    release/acquire ordering, so the queue requires no locks.
 * 4. Move-only types (e.g. std::unique_ptr) are supported.
 *
 * [References]
 * - Section 16.2 (Q1 core processing loop architecture)
 * - Section 16.3 (Q1 SPSC queue choice and rationale)
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <vector>

namespace vcs { namespace media {

/**
 * @brief Lock-free SPSC bounded queue.
 *
 * @tparam T  value type. May be move-only.
 *
 * Capacity is rounded up to the next power of two to make wrap-around
 * arithmetic cheap.
 */
template <typename T>
class SpscQueue {
public:
    /**
     * @param requestedCapacity  minimum slots; rounded up to power of two.
     */
    explicit SpscQueue(size_t requestedCapacity)
        : capacity_(RoundUpPowerOfTwo(requestedCapacity < 2 ? 2 : requestedCapacity)),
          mask_(capacity_ - 1),
          slots_(capacity_) {
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }

    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    /// Maximum number of elements the queue can hold.
    size_t Capacity() const noexcept { return capacity_ - 1; }

    /// Approximate number of unread elements.
    /// Safe to call from either side.
    size_t SizeApprox() const noexcept {
        const size_t t = tail_.load(std::memory_order_acquire);
        const size_t h = head_.load(std::memory_order_acquire);
        return t - h;
    }

    /// True if the queue is (probably) empty.
    /// Safe to call from either side; might give a stale answer under
    /// concurrent push / pop. Use SizeApprox for stronger guarantees.
    bool IsEmptyApprox() const noexcept { return SizeApprox() == 0; }

    /// Try to push. Returns false if the queue is full.
    /// Producer-only.
    bool TryPush(T value) noexcept {
        const size_t t = tail_.load(std::memory_order_relaxed);
        const size_t h = head_.load(std::memory_order_acquire);
        if ((t - h) >= capacity_ - 1) {
            return false;  // full
        }
        slots_[t & mask_] = std::move(value);
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    /// Try to pop into @p out. Returns false if the queue is empty.
    /// Consumer-only.
    bool TryPop(T& out) noexcept {
        const size_t h = head_.load(std::memory_order_relaxed);
        const size_t t = tail_.load(std::memory_order_acquire);
        if (h == t) {
            return false;  // empty
        }
        out = std::move(slots_[h & mask_]);
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    /// Bulk pop up to @p max items. Returns the number actually popped.
    /// Useful in worker-thread hot loops to avoid one-at-a-time overhead.
    template <typename OutputIt>
    size_t PopBulk(OutputIt dst, size_t max) noexcept {
        size_t popped = 0;
        for (; popped < max; ++popped) {
            // Peek + pop using a temporary; this could be specialised
            // for trivially-copyable T but the move path is already cheap.
            T tmp{};
            if (!TryPop(tmp)) break;
            *dst++ = std::move(tmp);
        }
        return popped;
    }

private:
    static size_t RoundUpPowerOfTwo(size_t v) noexcept {
        size_t p = 1;
        while (p < v) p <<= 1;
        return p;
    }

    const size_t capacity_;
    const size_t mask_;
    std::vector<T> slots_;

    // Use separate cachelines for head_ and tail_ to avoid false
    // sharing between producer and consumer cores. The fallback path
    // (when hardware_destructive_interference_size is unavailable)
    // still works correctly, just slightly slower.
#if defined(__cpp_lib_hardware_interference_size)
    alignas(std::hardware_destructive_interference_size) std::atomic<size_t> head_;
    alignas(std::hardware_destructive_interference_size) std::atomic<size_t> tail_;
#else
    alignas(64) std::atomic<size_t> head_;
    alignas(64) std::atomic<size_t> tail_;
#endif
};

} // namespace media
} // namespace vcs
