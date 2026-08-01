#include "mutex_order_queue.hpp"
#include "matching_engine.hpp"
#include "tcp_feed_handler.hpp"

#include <csignal>
#include <cstdio>
#include <thread>
#include <variant>

namespace {
    me::TCPFeedHandler* g_handler = nullptr;

    void signal_handler(int /*sig*/) {
        if (g_handler) g_handler->stop();
    }
}

int main() {
    constexpr uint16_t PORT = 9000;

    me::MutexOrderQueue queue;
    me::MatchingEngine  engine;
    me::TCPFeedHandler  handler(queue);
    g_handler = &handler;

    // Install SIGINT handler.
    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);

    // Consumer thread: pull orders from the queue and submit to the engine.
    std::thread consumer([&] {
        me::Order order;
        while (queue.pop(order)) {
            auto result = engine.submit_order(order);
            if (auto* trades = std::get_if<std::vector<me::Trade>>(&result)) {
                for (auto& t : *trades) {
                    std::printf("TRADE buy=%lu sell=%lu price=%.2f qty=%lu\n",
                                t.buy_order_id, t.sell_order_id, t.price, t.quantity);
                }
            } else if (auto* rej = std::get_if<me::Rejection>(&result)) {
                std::printf("REJECT order=%lu reason=%u\n",
                            rej->order_id, static_cast<unsigned>(rej->reason));
            }
        }
    });

    std::printf("Matching engine server listening on port %u\n", PORT);

    // Blocking event loop — returns when stop() is called.
    handler.run(PORT);

    std::printf("\nShutting down...\n");

    // Signal the queue so the consumer thread drains and exits.
    queue.shutdown();
    consumer.join();

    return 0;
}
