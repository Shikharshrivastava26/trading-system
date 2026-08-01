#pragma once

#include "order_queue.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace me {

// ---- Wire protocol structs (fixed-size, little-endian, no framing) --------

struct WireOrder {
    uint64_t order_id;
    uint64_t client_id;
    uint8_t  side;      // 0=BUY, 1=SELL
    uint8_t  type;      // 0=LIMIT, 1=MARKET
    uint8_t  padding[6];
    double   price;
    uint64_t quantity;
};
static_assert(sizeof(WireOrder) == 40, "WireOrder must be 40 bytes");

struct WireTrade {
    uint64_t buy_order_id;
    uint64_t sell_order_id;
    double   price;
    uint64_t quantity;
};
static_assert(sizeof(WireTrade) == 32, "WireTrade must be 32 bytes");

struct WireReject {
    uint64_t order_id;
    uint8_t  reason;    // maps to RejectReason enum
    uint8_t  padding[7];
};
static_assert(sizeof(WireReject) == 16, "WireReject must be 16 bytes");

// ---------------------------------------------------------------------------

class TCPFeedHandler {
public:
    explicit TCPFeedHandler(IOrderQueue& queue);
    ~TCPFeedHandler();

    TCPFeedHandler(const TCPFeedHandler&) = delete;
    TCPFeedHandler& operator=(const TCPFeedHandler&) = delete;

    /// Blocking event loop — listens on the given port, accepts clients,
    /// reads WireOrder messages and pushes Orders into the queue.
    void run(uint16_t port);

    /// Signal the event loop to stop (thread-safe).
    void stop();

private:
    IOrderQueue& queue_;
    std::atomic<bool> running_{false};

    int listen_fd_ = -1;
    int epoll_fd_  = -1;
    int stop_read_fd_  = -1;   // pipe read end, monitored by epoll
    int stop_write_fd_ = -1;   // pipe write end, written to by stop()

    // Per-connection accumulation buffer for partial reads.
    struct ConnState {
        std::vector<uint8_t> buf;
    };
    std::unordered_map<int, ConnState> connections_;

    void accept_connections();
    void handle_client_data(int fd);
    void close_connection(int fd);
    void process_buffer(int fd);

    static Order wire_to_order(const WireOrder& w);
};

} // namespace me
