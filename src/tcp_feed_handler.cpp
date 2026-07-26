#include "tcp_feed_handler.hpp"

#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <algorithm>

namespace me {

// ---- helpers --------------------------------------------------------------

namespace {

void set_nonblocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags == -1)
        throw std::runtime_error("fcntl F_GETFL: " + std::string(std::strerror(errno)));
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
        throw std::runtime_error("fcntl F_SETFL: " + std::string(std::strerror(errno)));
}

void epoll_add(int epfd, int fd, uint32_t events) {
    epoll_event ev{};
    ev.events  = events;
    ev.data.fd = fd;
    if (::epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) == -1)
        throw std::runtime_error("epoll_ctl ADD: " + std::string(std::strerror(errno)));
}

} // anon

// ---- TCPFeedHandler -------------------------------------------------------

TCPFeedHandler::TCPFeedHandler(IOrderQueue& queue)
    : queue_(queue)
{}

TCPFeedHandler::~TCPFeedHandler() {
    // Clean up any lingering fds.
    for (auto& [fd, _] : connections_)
        ::close(fd);
    if (listen_fd_  != -1) ::close(listen_fd_);
    if (epoll_fd_   != -1) ::close(epoll_fd_);
    if (stop_read_fd_  != -1) ::close(stop_read_fd_);
    if (stop_write_fd_ != -1) ::close(stop_write_fd_);
}

void TCPFeedHandler::stop() {
    if (running_.exchange(false)) {
        // Wake the epoll loop via the pipe.
        uint8_t byte = 1;
        ::write(stop_write_fd_, &byte, 1);
    }
}

void TCPFeedHandler::run(uint16_t port) {
    // Create stop-signal pipe.
    int pipe_fds[2];
    if (::pipe(pipe_fds) == -1)
        throw std::runtime_error("pipe: " + std::string(std::strerror(errno)));
    stop_read_fd_  = pipe_fds[0];
    stop_write_fd_ = pipe_fds[1];
    set_nonblocking(stop_read_fd_);

    // Listening socket.
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ == -1)
        throw std::runtime_error("socket: " + std::string(std::strerror(errno)));

    int opt = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    set_nonblocking(listen_fd_);

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == -1)
        throw std::runtime_error("bind: " + std::string(std::strerror(errno)));
    if (::listen(listen_fd_, SOMAXCONN) == -1)
        throw std::runtime_error("listen: " + std::string(std::strerror(errno)));

    // Epoll.
    epoll_fd_ = ::epoll_create1(0);
    if (epoll_fd_ == -1)
        throw std::runtime_error("epoll_create1: " + std::string(std::strerror(errno)));

    epoll_add(epoll_fd_, listen_fd_,     EPOLLIN);
    epoll_add(epoll_fd_, stop_read_fd_,  EPOLLIN);

    running_.store(true);

    constexpr int MAX_EVENTS = 64;
    epoll_event events[MAX_EVENTS];

    while (running_.load(std::memory_order_relaxed)) {
        int n = ::epoll_wait(epoll_fd_, events, MAX_EVENTS, -1);
        if (n == -1) {
            if (errno == EINTR) continue;
            throw std::runtime_error("epoll_wait: " + std::string(std::strerror(errno)));
        }

        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;

            if (fd == stop_read_fd_) {
                // Shutdown signal received.
                running_.store(false);
                break;
            }

            if (fd == listen_fd_) {
                accept_connections();
                continue;
            }

            // Client fd.
            if (events[i].events & (EPOLLHUP | EPOLLERR)) {
                close_connection(fd);
                continue;
            }

            if (events[i].events & EPOLLIN) {
                handle_client_data(fd);
            }
        }
    }
}

void TCPFeedHandler::accept_connections() {
    for (;;) {
        sockaddr_in client_addr{};
        socklen_t len = sizeof(client_addr);
        int client_fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&client_addr), &len);
        if (client_fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break; // No more pending connections.
            continue;  // Transient error — skip.
        }
        set_nonblocking(client_fd);
        epoll_add(epoll_fd_, client_fd, EPOLLIN);
        connections_[client_fd] = ConnState{};
    }
}

void TCPFeedHandler::handle_client_data(int fd) {
    auto it = connections_.find(fd);
    if (it == connections_.end()) return;

    uint8_t tmp[4096];
    for (;;) {
        ssize_t nread = ::recv(fd, tmp, sizeof(tmp), 0);
        if (nread > 0) {
            auto& buf = it->second.buf;
            buf.insert(buf.end(), tmp, tmp + nread);
        } else if (nread == 0) {
            // Peer closed.
            close_connection(fd);
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            close_connection(fd);
            return;
        }
    }

    process_buffer(fd);
}

void TCPFeedHandler::process_buffer(int fd) {
    auto it = connections_.find(fd);
    if (it == connections_.end()) return;

    auto& buf = it->second.buf;
    constexpr size_t MSG_SIZE = sizeof(WireOrder);

    while (buf.size() >= MSG_SIZE) {
        WireOrder w;
        std::memcpy(&w, buf.data(), MSG_SIZE);
        buf.erase(buf.begin(), buf.begin() + MSG_SIZE);

        queue_.push(wire_to_order(w));
    }
}

void TCPFeedHandler::close_connection(int fd) {
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    ::close(fd);
    connections_.erase(fd);
}

Order TCPFeedHandler::wire_to_order(const WireOrder& w) {
    Order o;
    o.order_id           = w.order_id;
    o.client_id          = w.client_id;
    o.side               = (w.side == 0) ? Side::BUY : Side::SELL;
    o.type               = (w.type == 0) ? OrderType::LIMIT : OrderType::MARKET;
    o.price              = w.price;
    o.quantity           = w.quantity;
    o.remaining_quantity = w.quantity;
    return o;
}

} // namespace me
