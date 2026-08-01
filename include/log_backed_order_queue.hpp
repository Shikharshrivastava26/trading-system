#pragma once

#include "order_queue.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#include <sys/stat.h>

#ifdef _WIN32
#include <io.h>
#define ME_FSYNC(fd) _commit(fd)
#define ME_FILENO(f) _fileno(f)
#else
#include <unistd.h>
#define ME_FSYNC(fd) fsync(fd)
#define ME_FILENO(f) fileno(f)
#endif

namespace me {

// Phase 1 stand-in for a replicated event log (Kafka in the real
// deployment). Every push() is fsync'd to disk before returning, and the
// consumer's read position is checkpointed to disk after every pop(), so a
// process that dies and restarts against the same file pair resumes exactly
// where it left off — no order lost, none replayed twice.
//
// Orders are fixed-size POD, so the log is just an array of raw Order
// records; "offset N" means byte N * sizeof(Order). This mirrors how a
// Kafka partition is addressed by integer offset.
//
// Producer and consumer are expected to be *different processes* (gateway
// vs. engine) — exactly like a Kafka partition. Two consequences follow:
//
//  1. pop() cannot rely on an in-process condition_variable to learn about
//     new data (a condition_variable can't be signaled across address
//     spaces); instead it polls the file's actual size via fstat, the way a
//     real log consumer polls a broker.
//  2. Multiple gateways can legitimately write to the *same* shard file at
//     once (several gateway VMs, one partition). push() never seeks to a
//     remembered offset before writing — it opens the file in append mode
//     and lets the OS position each write at the current end-of-file, which
//     POSIX/glibc guarantee is atomic per write() call. That's what makes
//     concurrent multi-gateway writers safe without any locking here, the
//     same guarantee a real broker gives producers.
class LogBackedOrderQueue : public IOrderQueue {
public:
    // consumer_id distinguishes independent readers of the *same* log file
    // (e.g. a Phase 4 standby replaying alongside the active engine) — each
    // gets its own checkpoint file and therefore its own read position, none
    // of them disturbing each other or the underlying log.
    explicit LogBackedOrderQueue(const std::string& base_path, const std::string& consumer_id = "")
        : log_path_(base_path + ".log"),
          checkpoint_path_(base_path + (consumer_id.empty() ? "" : ("." + consumer_id)) + ".offset") {
        log_ = std::fopen(log_path_.c_str(), "a+b");
        if (!log_) throw std::runtime_error("LogBackedOrderQueue: cannot open " + log_path_);

        consumer_offset_ = read_checkpoint();
    }

    ~LogBackedOrderQueue() override {
        if (log_) std::fclose(log_);
    }

    void push(Order o) override {
        std::lock_guard<std::mutex> lk(mu_);
        // No fseek: "a+b" mode positions every write at end-of-file,
        // regardless of what other processes have appended in the meantime.
        std::fwrite(&o, sizeof(Order), 1, log_);
        std::fflush(log_);
        ME_FSYNC(ME_FILENO(log_));
    }

    // Blocking pop (polls; see class comment for why). Returns false only
    // after shutdown() + the log is fully drained.
    bool pop(Order& out) override {
        for (;;) {
            {
                std::lock_guard<std::mutex> lk(mu_);
                uint64_t available = current_file_record_count();
                if (consumer_offset_ < available) {
                    std::fseek(log_, static_cast<long>(consumer_offset_ * sizeof(Order)), SEEK_SET);
                    if (std::fread(&out, sizeof(Order), 1, log_) != 1) {
                        throw std::runtime_error("LogBackedOrderQueue: short read at offset " +
                                                  std::to_string(consumer_offset_));
                    }
                    ++consumer_offset_;
                    write_checkpoint(consumer_offset_);
                    return true;
                }
                if (done_.load(std::memory_order_acquire)) return false;  // shutdown and drained
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    void shutdown() override {
        done_.store(true, std::memory_order_release);
    }

    [[nodiscard]] uint64_t write_offset() const { return current_file_record_count(); }
    [[nodiscard]] uint64_t consumer_offset() const { return consumer_offset_; }

private:
    // Reads the file's true size on disk rather than trusting an in-memory
    // counter, since another process may have appended to it since we last
    // looked (this is the poll step).
    [[nodiscard]] uint64_t current_file_record_count() const {
        struct stat st{};
        if (fstat(ME_FILENO(log_), &st) != 0) return 0;
        return static_cast<uint64_t>(st.st_size) / sizeof(Order);
    }

    [[nodiscard]] uint64_t read_checkpoint() const {
        std::FILE* f = std::fopen(checkpoint_path_.c_str(), "rb");
        if (!f) return 0;
        uint64_t offset = 0;
        if (std::fread(&offset, sizeof(offset), 1, f) != 1) offset = 0;
        std::fclose(f);
        return offset;
    }

    void write_checkpoint(uint64_t offset) const {
        std::FILE* f = std::fopen(checkpoint_path_.c_str(), "wb");
        if (!f) throw std::runtime_error("LogBackedOrderQueue: cannot write " + checkpoint_path_);
        std::fwrite(&offset, sizeof(offset), 1, f);
        std::fflush(f);
        ME_FSYNC(ME_FILENO(f));
        std::fclose(f);
    }

    std::string log_path_;
    std::string checkpoint_path_;
    std::FILE*  log_ = nullptr;

    std::mutex        mu_;
    uint64_t          consumer_offset_ = 0;
    std::atomic<bool> done_{false};
};

} // namespace me
