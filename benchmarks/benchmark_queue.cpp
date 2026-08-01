#include "matching_engine.hpp"
#include "mutex_order_queue.hpp"
#include "lockfree_order_queue.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <pthread.h>
#include <sched.h>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void pin_thread(int core) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
}

static std::string read_line_containing(const char* path, const char* key) {
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        if (line.find(key) != std::string::npos) return line;
    }
    return "(unknown)";
}

static void print_machine_info() {
    std::printf("=== Machine Info ===\n");
    std::printf("CPU       : %s\n", read_line_containing("/proc/cpuinfo", "model name").c_str());
    std::printf("Cores     : %u\n", std::thread::hardware_concurrency());
#ifdef __VERSION__
    std::printf("Compiler  : %s %s\n",
#ifdef __clang__
        "clang",
#else
        "g++",
#endif
        __VERSION__);
#endif
    std::printf("Kernel    : %s\n", read_line_containing("/proc/version", "Linux").c_str());
    std::printf("\n");
}

// ---------------------------------------------------------------------------
// Order generator — deterministic, pre-generated
// ---------------------------------------------------------------------------

static std::vector<me::Order> generate_orders(size_t count, uint64_t seed = 42) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<uint64_t> client_dist(1, 1000);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_real_distribution<double> price_dist(90.0, 110.0);
    std::uniform_int_distribution<uint64_t> qty_dist(1, 100);

    std::vector<me::Order> orders(count);
    for (size_t i = 0; i < count; ++i) {
        auto& o = orders[i];
        o.order_id = i + 1;
        o.client_id = client_dist(rng);
        o.side = side_dist(rng) ? me::Side::BUY : me::Side::SELL;
        o.type = me::OrderType::LIMIT;
        o.price = std::round(price_dist(rng) * 100.0) / 100.0;
        o.quantity = qty_dist(rng);
        o.remaining_quantity = o.quantity;
        o.timestamp = 0; // engine assigns
    }
    return orders;
}

// ---------------------------------------------------------------------------
// Benchmark result
// ---------------------------------------------------------------------------

struct BenchResult {
    double throughput_mops; // million orders per second
    double p50_ns;
    double p99_ns;
    double p999_ns;
    double max_ns;
    uint64_t spin_count;    // only meaningful for lock-free
};

// ---------------------------------------------------------------------------
// Templated benchmark harness
// ---------------------------------------------------------------------------

template <typename QueueT>
BenchResult run_once(const std::vector<me::Order>& orders,
                     size_t warmup, int producer_core, int consumer_core) {
    const size_t N = orders.size();

    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    // Pre-sized timestamp arrays
    std::vector<TimePoint> t_push(N);
    std::vector<TimePoint> t_pop(N);

    QueueT queue;
    me::MatchingEngine engine(10.0);

    auto wall_start = Clock::now();

    // Consumer
    std::thread consumer([&] {
        pin_thread(consumer_core);
        me::Order o;
        size_t idx = 0;
        while (queue.pop(o)) {
            t_pop[idx] = Clock::now();
            engine.submit_order(std::move(o));
            ++idx;
        }
    });

    // Producer
    std::thread producer([&] {
        pin_thread(producer_core);
        for (size_t i = 0; i < N; ++i) {
            t_push[i] = Clock::now();
            queue.push(orders[i]);
        }
        queue.shutdown();
    });

    producer.join();
    consumer.join();

    auto wall_end = Clock::now();
    double wall_secs = std::chrono::duration<double>(wall_end - wall_start).count();

    // Compute latencies (skip warmup)
    std::vector<double> latencies;
    latencies.reserve(N - warmup);
    for (size_t i = warmup; i < N; ++i) {
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t_pop[i] - t_push[i]).count();
        latencies.push_back(static_cast<double>(ns));
    }
    std::sort(latencies.begin(), latencies.end());

    auto pct = [&](double p) -> double {
        size_t idx = static_cast<size_t>(p / 100.0 * (latencies.size() - 1));
        return latencies[idx];
    };

    BenchResult r{};
    r.throughput_mops = (static_cast<double>(N) / wall_secs) / 1e6;
    r.p50_ns  = pct(50);
    r.p99_ns  = pct(99);
    r.p999_ns = pct(99.9);
    r.max_ns  = latencies.back();

    // Spin count — only available on LockFreeOrderQueue
    if constexpr (std::is_same_v<QueueT, me::LockFreeOrderQueue>) {
        r.spin_count = queue.spin_count();
    } else {
        r.spin_count = 0;
    }

    return r;
}

// ---------------------------------------------------------------------------
// Run multiple repetitions, return median of each metric
// ---------------------------------------------------------------------------

template <typename QueueT>
BenchResult run_repeated(const std::vector<me::Order>& orders,
                         size_t warmup, int reps,
                         int producer_core, int consumer_core) {
    std::vector<BenchResult> results;
    results.reserve(reps);
    for (int r = 0; r < reps; ++r) {
        results.push_back(run_once<QueueT>(orders, warmup, producer_core, consumer_core));
    }

    auto median_of = [&](auto accessor) {
        std::vector<double> vals;
        for (auto& r : results) vals.push_back(accessor(r));
        std::sort(vals.begin(), vals.end());
        return vals[vals.size() / 2];
    };

    BenchResult med{};
    med.throughput_mops = median_of([](auto& r) { return r.throughput_mops; });
    med.p50_ns  = median_of([](auto& r) { return r.p50_ns; });
    med.p99_ns  = median_of([](auto& r) { return r.p99_ns; });
    med.p999_ns = median_of([](auto& r) { return r.p999_ns; });
    med.max_ns  = median_of([](auto& r) { return r.max_ns; });
    med.spin_count = static_cast<uint64_t>(median_of([](auto& r) { return static_cast<double>(r.spin_count); }));
    return med;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main() {
    constexpr size_t NUM_ORDERS = 500'000;
    constexpr size_t WARMUP     = 10'000;
    constexpr int    REPS       = 5;

    unsigned hw = std::thread::hardware_concurrency();
    int producer_core = 0;
    int consumer_core = hw > 1 ? 1 : 0;

    print_machine_info();

    std::printf("Benchmark config: %zu orders, %zu warmup, %d reps\n", NUM_ORDERS, WARMUP, REPS);
    std::printf("Thread pinning  : producer=core %d, consumer=core %d\n\n", producer_core, consumer_core);

    std::printf("Generating orders (seed=42) ...\n");
    auto orders = generate_orders(NUM_ORDERS);
    std::printf("Done.\n\n");

    std::printf("Running MutexOrderQueue benchmark ...\n");
    auto mutex_res = run_repeated<me::MutexOrderQueue>(orders, WARMUP, REPS, producer_core, consumer_core);

    std::printf("Running LockFreeOrderQueue benchmark ...\n");
    auto lf_res = run_repeated<me::LockFreeOrderQueue>(orders, WARMUP, REPS, producer_core, consumer_core);

    // Print results table
    std::printf("\n");
    std::printf("=== Head-to-Head Results (median of %d runs) ===\n", REPS);
    std::printf("%-24s %16s %16s\n", "Metric", "Mutex", "LockFree");
    std::printf("%-24s %16s %16s\n", "------------------------", "----------------", "----------------");
    std::printf("%-24s %13.3f M/s %13.3f M/s\n", "Throughput",    mutex_res.throughput_mops, lf_res.throughput_mops);
    std::printf("%-24s %13.0f ns  %13.0f ns\n",  "Latency p50",   mutex_res.p50_ns,  lf_res.p50_ns);
    std::printf("%-24s %13.0f ns  %13.0f ns\n",  "Latency p99",   mutex_res.p99_ns,  lf_res.p99_ns);
    std::printf("%-24s %13.0f ns  %13.0f ns\n",  "Latency p99.9", mutex_res.p999_ns, lf_res.p999_ns);
    std::printf("%-24s %13.0f ns  %13.0f ns\n",  "Latency max",   mutex_res.max_ns,  lf_res.max_ns);
    std::printf("%-24s %16s %16lu\n",             "Spin count (LF)", "n/a", lf_res.spin_count);
    std::printf("\n");

    double speedup = lf_res.throughput_mops / mutex_res.throughput_mops;
    std::printf("Lock-free throughput speedup: %.2fx\n", speedup);

    return 0;
}
