// Phase 5 demo consumer: attaches to one shard's `trades` topic under a
// fresh subscriber id and prints every trade, historical and live, without
// gateway.cpp or engine_main.cpp knowing this program exists. That's the
// whole point — new consumers (risk, OMS, analytics, this) cost zero engine
// changes, they just point a Subscriber at the same log.

#include "trade_log.hpp"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {
    volatile std::sig_atomic_t g_stop = 0;
    void signal_handler(int) { g_stop = 1; }
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s <shard_id> <log_base> <subscriber_id>\n", argv[0]);
        return 1;
    }
    std::string shard_base = std::string(argv[2]) + "_shard" + argv[1];
    std::string subscriber_id = argv[3];

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    me::TradeLog::Subscriber sub(shard_base, subscriber_id);

    std::printf("Subscriber '%s' attached to %s_trades.log, starting at offset %lu\n",
                subscriber_id.c_str(), shard_base.c_str(), sub.offset());

    me::Trade t;
    while (!g_stop) {
        if (sub.try_next(t)) {
            std::printf("[%s] saw trade buy=%lu sell=%lu price=%.2f qty=%lu (offset now %lu)\n",
                        subscriber_id.c_str(), t.buy_order_id, t.sell_order_id,
                        t.price, t.quantity, sub.offset());
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    std::printf("Subscriber '%s' stopped at offset %lu\n", subscriber_id.c_str(), sub.offset());
    return 0;
}
