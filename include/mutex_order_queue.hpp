#pragma once

#include "order_queue.hpp"

#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>

namespace me {

class MutexOrderQueue : public IOrderQueue {
public:
    void push(Order o) override {
        {
            std::lock_guard<std::mutex> lk(mu_);
            queue_.push(std::move(o));
        }
        cv_.notify_one();
    }

    // Blocking pop. Returns false only after shutdown + drain.
    bool pop(Order& out) override {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [&] { return !queue_.empty() || done_.load(std::memory_order_relaxed); });
        if (queue_.empty()) return false;  // shutdown and drained
        out = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    void shutdown() override {
        done_.store(true, std::memory_order_relaxed);
        cv_.notify_all();
    }

private:
    std::mutex              mu_;
    std::condition_variable cv_;
    std::queue<Order>       queue_;
    std::atomic<bool>       done_{false};
};

} // namespace me
