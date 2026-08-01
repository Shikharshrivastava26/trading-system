#pragma once

#include <cstdint>
#include <string>

namespace me {

// FNV-1a: deterministic across processes/machines/runs (unlike
// std::hash<std::string>, which the standard does not require to be stable
// even within one binary's lifetime across runs). This is what lets a
// gateway and an engine, running as separate processes, independently
// compute the same shard for the same symbol.
[[nodiscard]] inline uint64_t fnv1a(const std::string& s) {
    uint64_t hash = 14695981039346656037ull;
    for (unsigned char c : s) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    return hash;
}

// Every order for `symbol` maps to exactly this shard, from any gateway,
// every time — this is the entire mechanism that guarantees a buyer and a
// seller of the same symbol always land on the same engine process.
[[nodiscard]] inline uint32_t shard_for_symbol(const std::string& symbol, uint32_t num_shards) {
    return static_cast<uint32_t>(fnv1a(symbol) % num_shards);
}

} // namespace me
