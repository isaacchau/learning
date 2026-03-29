#ifndef LOCKFREE_RINGBUFFER_H
#define LOCKFREE_RINGBUFFER_H

#include <atomic>
#include <vector>
#include <cstddef>
#include <chrono>
#include <thread>
#include <type_traits>

// Single-Producer-Single-Consumer lock-free ring buffer.
// Power-of-2 sizing with bitwise modulo for fast indexing.
// Cache-line aligned head/tail to prevent false sharing.

// Spin-wait backoff thresholds
namespace SpinBackoff {
    constexpr int YIELD_THRESHOLD      = 100;   // First 100 spins: just yield
    constexpr int SLEEP_1US_THRESHOLD  = 200;   // Next 100 spins: sleep 1 microsecond
    constexpr int SLEEP_10US_THRESHOLD = 300;   // Next 100 spins: sleep 10 microseconds
    constexpr int SLEEP_100US_THRESHOLD= 400;   // Next 100 spins: sleep 100 microseconds
    constexpr int SLEEP_1MS_DURATION   = 1;     // After 400 spins: sleep 1 millisecond
}
template <typename T>
class LockFreeRingBuffer {
public:
    explicit LockFreeRingBuffer(size_t requested_size)
        : capacity_(nextPowerOf2(requested_size))
        , mask_(capacity_ - 1)
        , buffer_(capacity_)
    {
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }

    LockFreeRingBuffer(const LockFreeRingBuffer&) = delete;
    LockFreeRingBuffer& operator=(const LockFreeRingBuffer&) = delete;

    // Non-blocking push. Returns false if queue is full.
    bool push(const T& item) {
        const size_t head = head_.load(std::memory_order_relaxed);
        if ((head - tail_.load(std::memory_order_acquire)) >= capacity_) {
            return false;
        }
        buffer_[head & mask_] = item;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // Move-push variant.
    bool push(T&& item) {
        const size_t head = head_.load(std::memory_order_relaxed);
        if ((head - tail_.load(std::memory_order_acquire)) >= capacity_) {
            return false;
        }
        buffer_[head & mask_] = std::move(item);
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // Blocking push with progressive backoff. Returns false on timeout.
    // If push fails (queue full), item is NOT moved and can be retried.
    bool push_wait(const T& item, int timeout_ms) {
        if (push(item)) return true;
        return waitAndRetry([this, &item]() { return push(item); }, timeout_ms);
    }

    bool push_wait(T&& item, int timeout_ms) {
        if (push(std::move(item))) return true;
        // If push failed, the move didn't execute (assignment was not reached).
        // item is still valid and can be retried.
        return waitAndRetry([this, &item]() { return push(std::move(item)); }, timeout_ms);
    }

    // Non-blocking pop. Returns false if queue is empty.
    bool pop(T& item) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) {
            return false;
        }
        item = std::move(buffer_[tail & mask_]);
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Blocking pop with progressive backoff. Returns false on timeout.
    bool pop_wait(T& item, int timeout_ms) {
        if (pop(item)) return true;
        return waitAndRetry([this, &item]() { return pop(item); }, timeout_ms);
    }

    size_t size() const {
        const size_t head = head_.load(std::memory_order_acquire);
        const size_t tail = tail_.load(std::memory_order_acquire);
        return head - tail;
    }

    size_t capacity() const { return capacity_; }
    bool empty() const { return size() == 0; }
    bool full() const { return size() >= capacity_; }

private:
    static size_t nextPowerOf2(size_t n) {
        if (n == 0) return 1;
        if ((n & (n - 1)) == 0) return n; // already power of 2
        --n;
        n |= n >> 1;
        n |= n >> 2;
        n |= n >> 4;
        n |= n >> 8;
        n |= n >> 16;
        n |= n >> 32;
        return n + 1;
    }

    // Progressive backoff: yield → 1us → 10us → 100us → 1ms
    template <typename Func>
    bool waitAndRetry(Func tryOp, int timeout_ms) {
        auto deadline = std::chrono::steady_clock::now()
                      + std::chrono::milliseconds(timeout_ms);
        int spin = 0;

        while (std::chrono::steady_clock::now() < deadline) {
            if (spin < SpinBackoff::YIELD_THRESHOLD) {
                std::this_thread::yield();
            } else if (spin < SpinBackoff::SLEEP_1US_THRESHOLD) {
                std::this_thread::sleep_for(std::chrono::microseconds(1));
            } else if (spin < SpinBackoff::SLEEP_10US_THRESHOLD) {
                std::this_thread::sleep_for(std::chrono::microseconds(10));
            } else if (spin < SpinBackoff::SLEEP_100US_THRESHOLD) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(SpinBackoff::SLEEP_1MS_DURATION));
            }
            if (tryOp()) return true;
            ++spin;
        }
        return false;
    }

    const size_t capacity_;
    const size_t mask_;
    std::vector<T> buffer_;

    // Cache-line aligned to prevent false sharing between producer and consumer
    alignas(64) std::atomic<size_t> head_;
    alignas(64) std::atomic<size_t> tail_;
};

#endif // LOCKFREE_RINGBUFFER_H
