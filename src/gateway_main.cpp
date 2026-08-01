// Phase 2 + 3: the gateway tier. Stateless — no MatchingEngine here at all.
// Accepts client TCP connections, and for every order, routes it to the
// correct symbol shard's replicated log via ShardRouterQueue. Any number of
// these can run behind a load balancer; they never coordinate with each
// other or hold any order-book state.

#include "shard_router_queue.hpp"
#include "tcp_feed_handler.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>

namespace {
    me::TCPFeedHandler* g_handler = nullptr;

    void signal_handler(int /*sig*/) {
        if (g_handler) g_handler->stop();
    }
}

int main(int argc, char** argv) {
    uint16_t port        = argc > 1 ? static_cast<uint16_t>(std::atoi(argv[1])) : 9000;
    uint32_t num_shards  = argc > 2 ? static_cast<uint32_t>(std::atoi(argv[2])) : 4;
    std::string log_base = argc > 3 ? argv[3] : "/tmp/me_orders";

    me::ShardRouterQueue router(log_base, num_shards);
    me::TCPFeedHandler   handler(router);
    g_handler = &handler;

    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);

    std::printf("Gateway listening on port %u, routing across %u shards at %s_shardN.log\n",
                port, num_shards, log_base.c_str());

    handler.run(port);  // blocking; returns on stop()

    std::printf("\nGateway shutting down (shards keep running independently)...\n");
    return 0;
}
