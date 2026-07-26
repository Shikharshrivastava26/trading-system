#pragma once

#include "order_queue.hpp"

#include <atomic>
#include <cstddef>
#include <vector>
#include <thread>

namespace me {

// Bounded SPSC lock-free ring buffer.
// Power-of-two capacity, mask instead of modulo.
// Head and tail on separate cache lines to avoid false sharing.
class LockFreeOrderQueue : public IOrderQueue {
public:
    explicit LockFreeOrderQueue(size_t min_capacity = 1 << 20) {
        // Round up to next power of two.
        capacity_ = 1;
        while (capacity_ < min_capacity) capacity_ <<= 1;
        mask_ = capacity_ - 1;
        buffer_.resize(capacity_);
    }

    void push(Order o) override {
        size_t head = head_.load(std::memory_order_relaxed);
        for (;;) {
            size_t tail = tail_.load(std::memory_order_acquire);
            if (head - tail < capacity_) break;  // slot available
            // Ring full — spin (and count).
            spin_count_.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
        buffer_[head & mask_] = std::move(o);
        head_.store(head + 1, std::memory_order_release);
    }

    bool pop(Order& out) override {
        size_t tail = tail_.load(std::memory_order_relaxed);
        for (;;) {
            size_t head = head_.load(std::memory_order_acquire);
            if (tail < head) {
                // Data available.
                out = std::move(buffer_[tail & mask_]);
                tail_.store(tail + 1, std::memory_order_release);
                return true;
            }
            if (done_.load(std::memory_order_acquire)) {
                // Recheck after observing done — producer may have pushed then set done.
                head = head_.load(std::memory_order_acquire);
                if (tail < head) {
                    out = std::move(buffer_[tail & mask_]);
                    tail_.store(tail + 1, std::memory_order_release);
                    return true;
                }
                return false;  // shutdown and drained
            }
            std::this_thread::yield();
        }
    }

    void shutdown() override {
        done_.store(true, std::memory_order_release);
    }

    [[nodiscard]] uint64_t spin_count() const {
        return spin_count_.load(std::memory_order_relaxed);
    }

private:
    size_t capacity_;
    size_t mask_;
    std::vector<Order> buffer_;

    // Each atomic on its own cache line to prevent false sharing.
    alignas(64) std::atomic<size_t>   head_{0};
    alignas(64) std::atomic<size_t>   tail_{0};
    alignas(64) std::atomic<bool>     done_{false};
    alignas(64) std::atomic<uint64_t> spin_count_{0};
};

} // namespace me
