#pragma once

#include "log_backed_order_queue.hpp"
#include "order_queue.hpp"
#include "sharding.hpp"

#include <memory>
#include <stdexcept>
#include <vector>

namespace me {

// Lives in the gateway process. Fronts TCPFeedHandler as an IOrderQueue, but
// push() doesn't hold anything in memory — it routes straight to the
// LogBackedOrderQueue for whichever shard owns the order's symbol, so any
// gateway VM produces into the exact same place any other gateway VM would
// for that symbol.
//
// Only push() is meaningful here: a gateway is stateless and never consumes
// orders back off the log, so pop() is intentionally unsupported.
class ShardRouterQueue : public IOrderQueue {
public:
    ShardRouterQueue(const std::string& base_path, uint32_t num_shards)
        : num_shards_(num_shards) {
        shards_.reserve(num_shards);
        for (uint32_t i = 0; i < num_shards; ++i) {
            shards_.push_back(std::make_unique<LogBackedOrderQueue>(
                base_path + "_shard" + std::to_string(i)));
        }
    }

    void push(Order o) override {
        uint32_t shard = shard_for_symbol(o.symbol_str(), num_shards_);
        shards_[shard]->push(std::move(o));
    }

    bool pop(Order&) override {
        throw std::logic_error("ShardRouterQueue: gateway does not consume orders");
    }

    // Gateways come and go independently of the engine tier, so shutting one
    // down must not stop the shards themselves — other gateways may still be
    // writing to them, and the owning engine keeps running regardless.
    void shutdown() override {}

    [[nodiscard]] uint32_t num_shards() const { return num_shards_; }

private:
    uint32_t num_shards_;
    std::vector<std::unique_ptr<LogBackedOrderQueue>> shards_;
};

} // namespace me
