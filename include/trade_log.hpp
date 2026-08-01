#pragma once

#include "order.hpp"

#include <chrono>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#include <sys/stat.h>

#ifdef _WIN32
#include <io.h>
#define ME_TL_FSYNC(fd) _commit(fd)
#define ME_TL_FILENO(f) _fileno(f)
#else
#include <unistd.h>
#define ME_TL_FSYNC(fd) fsync(fd)
#define ME_TL_FILENO(f) fileno(f)
#endif

namespace me {

// Phase 5: the `trades` topic. Same append-only, offset-addressed design as
// LogBackedOrderQueue, but for Trade records instead of Order, and built to
// have *many* independent subscribers rather than one engine consumer.
//
// A brand-new consumer_id has no checkpoint file yet, so it starts at offset
// 0 and naturally replays the entire trade history before catching up to
// live trades — "historical + live" comes for free from the same mechanism,
// no separate backfill path needed.
class TradeLog {
public:
    explicit TradeLog(const std::string& base_path)
        : log_path_(base_path + "_trades.log") {
        log_ = std::fopen(log_path_.c_str(), "a+b");
        if (!log_) throw std::runtime_error("TradeLog: cannot open " + log_path_);
    }

    ~TradeLog() {
        if (log_) std::fclose(log_);
    }

    // Called only by the active engine for its shard.
    void publish(const Trade& t) {
        std::lock_guard<std::mutex> lk(mu_);
        std::fwrite(&t, sizeof(Trade), 1, log_);
        std::fflush(log_);
        ME_TL_FSYNC(ME_TL_FILENO(log_));
    }

    // Opaque per-subscriber cursor over one TradeLog file. Any number of
    // these can exist at once, at any offset, without affecting each other
    // or the publisher.
    class Subscriber {
    public:
        Subscriber(const std::string& base_path, const std::string& subscriber_id)
            : log_path_(base_path + "_trades.log"),
              checkpoint_path_(base_path + "_trades." + subscriber_id + ".offset") {
            log_ = std::fopen(log_path_.c_str(), "a+b");
            if (!log_) throw std::runtime_error("TradeLog::Subscriber: cannot open " + log_path_);
            offset_ = read_checkpoint();
        }

        ~Subscriber() {
            if (log_) std::fclose(log_);
        }

        // Non-blocking: returns false immediately if nothing new is available.
        bool try_next(Trade& out) {
            std::lock_guard<std::mutex> lk(mu_);
            struct stat st{};
            if (fstat(ME_TL_FILENO(log_), &st) != 0) return false;
            uint64_t available = static_cast<uint64_t>(st.st_size) / sizeof(Trade);
            if (offset_ >= available) return false;

            std::fseek(log_, static_cast<long>(offset_ * sizeof(Trade)), SEEK_SET);
            if (std::fread(&out, sizeof(Trade), 1, log_) != 1) return false;
            ++offset_;
            write_checkpoint(offset_);
            return true;
        }

        [[nodiscard]] uint64_t offset() const { return offset_; }

    private:
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
            if (!f) throw std::runtime_error("TradeLog::Subscriber: cannot write " + checkpoint_path_);
            std::fwrite(&offset, sizeof(offset), 1, f);
            std::fflush(f);
            ME_TL_FSYNC(ME_TL_FILENO(f));
            std::fclose(f);
        }

        std::string log_path_;
        std::string checkpoint_path_;
        std::FILE*  log_ = nullptr;
        std::mutex  mu_;
        uint64_t    offset_ = 0;
    };

private:
    std::string log_path_;
    std::FILE*  log_ = nullptr;
    std::mutex  mu_;
};

} // namespace me
