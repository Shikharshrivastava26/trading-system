#pragma once

#include <fcntl.h>
#include <string>
#include <sys/file.h>
#include <unistd.h>

namespace me {

// Phase 4's leader election, minus an external coordinator. An advisory
// exclusive flock() on a per-shard file stands in for what Kafka's consumer
// group protocol / etcd / ZooKeeper would do in a real deployment: exactly
// one process can hold it at a time, and — the property that makes this
// actually work as failover — the OS releases the lock automatically if the
// holding process dies, even via kill -9, with no cleanup code required.
//
// Any number of processes can independently call try_acquire() in a loop;
// whichever currently holds the lock is "active" for that shard, everyone
// else is standby.
class ShardLeaderLock {
public:
    explicit ShardLeaderLock(const std::string& path) : path_(path) {
        fd_ = ::open(path_.c_str(), O_CREAT | O_RDWR, 0644);
    }

    ~ShardLeaderLock() {
        if (fd_ >= 0) ::close(fd_);  // also releases any held flock
    }

    // Non-blocking. Returns true if the lock is now held (either newly
    // acquired this call, or already held from a previous call).
    bool try_acquire() {
        if (held_) return true;
        if (fd_ < 0) return false;
        if (::flock(fd_, LOCK_EX | LOCK_NB) == 0) {
            held_ = true;
        }
        return held_;
    }

    [[nodiscard]] bool held() const { return held_; }

private:
    std::string path_;
    int         fd_    = -1;
    bool        held_  = false;
};

} // namespace me
