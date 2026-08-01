#pragma once

#include "order.hpp"

#include <vector>
#include <cstddef>
#include <cassert>

namespace me {

// Pre-allocated fixed-size pool of Order slots.
// Single-threaded: one pool per producer thread avoids locking.
class OrderPool {
public:
    explicit OrderPool(size_t capacity)
        : storage_(capacity), free_list_(capacity) {
        for (size_t i = 0; i < capacity; ++i)
            free_list_[i] = &storage_[i];
        top_ = capacity;
    }

    // Acquire a slot. Returns nullptr if exhausted.
    Order* acquire() {
        if (top_ == 0) return nullptr;
        return free_list_[--top_];
    }

    // Release a slot back to the pool.
    void release(Order* o) {
        assert(top_ < free_list_.size());
        free_list_[top_++] = o;
    }

    [[nodiscard]] size_t available() const { return top_; }
    [[nodiscard]] size_t capacity()  const { return storage_.size(); }

private:
    std::vector<Order>   storage_;
    std::vector<Order*>  free_list_;
    size_t               top_ = 0;
};

} // namespace me
