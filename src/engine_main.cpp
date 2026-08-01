// Phase 2 + 3: the matching engine tier. Each process owns exactly one
// shard's log (i.e. one Kafka-partition equivalent) and runs the *unmodified*
// MatchingEngine from matching_engine.cpp against it — one instance per
// symbol it encounters, since a shard can legitimately own several symbols.
//
// Phase 4: any number of these can point at the same shard as different
// "replicas" (see replica_id below). Exactly one holds the shard's leader
// lock at a time and is active — it publishes trades and prints output.
// Every other replica still replays the same log in the background to stay
// warm, so if the active one dies (even kill -9), a standby already has an
// up-to-date book and takes over on its very next poll of the lock.
//
// Phase 5: the active replica publishes every trade to a TradeLog
// (the `trades` topic) instead of only printing it, so any number of
// independent downstream consumers can subscribe without this file changing.
//
// Run several of these against the same log_base with different shard ids
// (matching what the gateway was started with) to see multiple engine
// processes matching different symbols in parallel, exactly like the
// 10-VM / N-shard topology in docs/distributed_system.md.

#include "log_backed_order_queue.hpp"
#include "matching_engine.hpp"
#include "shard_leader_lock.hpp"
#include "sharding.hpp"
#include "trade_log.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <variant>

namespace {
    me::LogBackedOrderQueue* g_queue = nullptr;
    void signal_handler(int) {
        if (g_queue) g_queue->shutdown();
    }
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
            "usage: %s <shard_id> <log_base> [num_shards] [replica_id]\n", argv[0]);
        return 1;
    }
    uint32_t shard_id      = static_cast<uint32_t>(std::atoi(argv[1]));
    std::string log_base   = argv[2];
    uint32_t num_shards    = argc > 3 ? static_cast<uint32_t>(std::atoi(argv[3])) : 4;
    std::string replica_id = argc > 4 ? argv[4] : "r0";

    std::setvbuf(stdout, nullptr, _IOLBF, 0);  // line-buffered: kill -9 demos don't lose the last lines

    std::string shard_base = log_base + "_shard" + std::to_string(shard_id);

    // Each replica of this shard replays the log independently — a distinct
    // consumer_id gives it its own checkpoint, so an active and a standby
    // never step on each other's read position over the same file.
    me::LogBackedOrderQueue queue(shard_base, replica_id);
    g_queue = &queue;
    std::signal(SIGINT, signal_handler);

    me::ShardLeaderLock leader(shard_base + ".leader");
    me::TradeLog        trades(shard_base);

    bool active = leader.try_acquire();

    // One MatchingEngine per symbol this shard owns — created lazily on
    // first sight, since a shard doesn't know its symbol set up front.
    std::unordered_map<std::string, me::MatchingEngine> engines;

    std::printf("Engine VM online: shard %u of %u, replica '%s', reading %s.log [%s]\n",
                shard_id, num_shards, replica_id.c_str(), shard_base.c_str(),
                active ? "ACTIVE" : "STANDBY");

    me::Order order;
    while (queue.pop(order)) {
        if (!active) {
            active = leader.try_acquire();
            if (active) {
                std::printf("shard %u replica '%s': PROMOTED to active (previous active is gone)\n",
                            shard_id, replica_id.c_str());
            }
        }

        std::string symbol = order.symbol_str();

        // Defensive check: catches a gateway/engine shard-count mismatch,
        // which would otherwise silently produce two disjoint books for the
        // same symbol on different shards — exactly the bug sharding exists
        // to prevent.
        uint32_t expected = me::shard_for_symbol(symbol, num_shards);
        if (expected != shard_id) {
            std::fprintf(stderr,
                "WARNING: order for '%s' arrived on shard %u but hashes to shard %u "
                "(num_shards mismatch between gateway and engine?)\n",
                symbol.c_str(), shard_id, expected);
        }

        // Every replica applies every order, active or not — that's what
        // keeps a standby's book identical to the active one, ready to take
        // over with zero replay lag at promotion time.
        auto [it, inserted] = engines.try_emplace(symbol);
        if (inserted && active) {
            std::printf("shard %u: now serving symbol '%s'\n", shard_id, symbol.c_str());
        }

        auto result = it->second.submit_order(order);

        if (!active) continue;  // standby: stay warm, but never surface output

        if (auto* trade_list = std::get_if<std::vector<me::Trade>>(&result)) {
            for (auto& t : *trade_list) {
                trades.publish(t);
                std::printf("[%s] TRADE buy=%lu sell=%lu price=%.2f qty=%lu\n",
                            symbol.c_str(), t.buy_order_id, t.sell_order_id, t.price, t.quantity);
            }
        } else if (auto* rej = std::get_if<me::Rejection>(&result)) {
            std::printf("[%s] REJECT order=%lu reason=%u\n",
                        symbol.c_str(), rej->order_id, static_cast<unsigned>(rej->reason));
        }
    }

    std::printf("Engine VM (shard %u, replica '%s') shutting down\n",
                shard_id, replica_id.c_str());
    return 0;
}
