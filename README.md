# C++ Order Matching Engine

A simplified exchange matching engine that receives buy/sell orders, matches them
by **price-time priority**, and reports real, measured performance numbers. The
project explores lock-free concurrency, low-latency memory management, and
kernel-level network I/O — areas chosen to extend beyond existing production
experience with transaction systems and multithreaded C++.

## Architecture

```
clients (buyers/sellers)
        |  TCP
        v
TCP feed handler (epoll)
        |
        v
price collar check ---- reject --> order rejected
        |  (within allowed band)
        v
order queue (mutex/condvar  OR  lock-free SPSC)
        |
        v
matching engine
   order book: bids sorted high->low, asks low->high,
   FIFO within each price level
        |
        v
trade executed -> last price updated
```

The central design decision: **concurrency is pushed entirely into the queue so
that the matching logic stays single-threaded and provably correct.** The order
book has exactly one writer — the consumer thread — and never needs to be
thread-safe itself.

For benchmarking, the TCP layer is bypassed: the harness calls `push()` directly,
in-process. This makes the queue comparison honest — network jitter would swamp
the difference being measured.

## Building

```bash
# Requirements: C++17, CMake >= 3.16, GCC/Clang, Linux (epoll)

# Debug build (tests)
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Debug ..
cmake --build . -j$(nproc)
ctest --output-on-failure

# Release build (benchmark)
mkdir build-release && cd build-release
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build . -j$(nproc)
./benchmark
```

## Running the tests

```bash
cd build && ctest --output-on-failure
```

21 tests across three suites:
- **Matching** (8 tests) — exact match, price priority, time priority, partial fill,
  multi-level walk, price improvement, cancel, market orders.
- **Collar** (4 tests) — reject, accept, inactive before first trade, market bypass.
- **Invariants + ThreadedRunner + OrderPool** (9 tests) — quantity conservation,
  no-crossed-book, 1M-order identical output for both queue types, pool lifecycle.

## Running the TCP server

Terminal 1:
```bash
cd build-release && ./server
```

Terminal 2:
```bash
python3 client/order_generator.py 100
```

The server prints each trade as it executes. Ctrl-C for clean shutdown.

## Reproducing the benchmark

```bash
cd build-release && ./benchmark
```

The benchmark uses **seed 42**, pre-generates 500K orders, runs 5 repetitions per
queue type, and reports the median. Threads are pinned to cores 0 and 1 via
`pthread_setaffinity_np`. For best reproducibility, set the CPU governor to
`performance`:

```bash
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
```

### Results

**Machine:** AMD Ryzen 5 3550H (4C/8T), 8 cores, g++ 15.2.1, Linux 6.12.73-1-MANJARO

| Queue | Throughput (orders/s) | p50 (ms) | p99 (ms) | p99.9 (ms) | max (ms) |
|---|---|---|---|---|---|
| Mutex | 1.40 M/s | 167 | 257 | 259 | 259 |
| Lock-free SPSC | 2.18 M/s | 96 | 195 | 196 | 196 |

**Spin count:** 0 (ring never filled — consumer kept up with producer).
**Lock-free throughput speedup:** 1.56x.

### Analysis

The lock-free SPSC queue is 1.56x faster in throughput. The win comes from
eliminating the mutex acquire/release on every push and pop — each of which
involves at least one atomic compare-and-swap plus a potential kernel futex call
under contention, compared to a single `store(release)` / `load(acquire)` pair
in the ring buffer path.

The latency numbers are **queuing delay** — how long an order sits between being
pushed by the producer and being popped by the consumer — not per-order matching
time. They are high (tens to hundreds of ms) because the producer submits orders
faster than the consumer can match them, creating a backlog. The consumer does
real matching work (tree lookups, deque operations) for each order, while the
producer just copies a pre-generated struct. This is realistic: in a real
exchange, the ingress rate can burst above the matching rate.

The spin count of 0 means the ring buffer (1M slots, ~64 MB) never saturated —
the producer never had to wait for the consumer to free a slot. In a scenario
with a smaller ring or a faster producer, spins would appear and would be worth
reporting.

**The bottleneck is `std::map`.** Each price-level lookup chases pointers through
a red-black tree, and each new price level is a heap allocation. This is the
single most likely target for optimization in a v2 — replacing `std::map` with a
flat sorted array or a hash map with sorted keys would improve cache locality
substantially.

`alignas(64)` on the ring buffer's head and tail atomics is load-bearing.
Without it, the two threads would ping-pong the same cache line (false sharing),
and the lock-free version would likely measure *slower* than the mutex version.

## Known limitations

These are omitted on purpose; each is noted so a reader knows the omission was a
decision, not an oversight.

| Omitted | Why |
|---|---|
| Multi-symbol books | One book keeps the concurrency story clean. Multi-symbol is a sharding problem, not a matching problem. |
| Iceberg / stop / IOC / FOK order types | Adds combinatorial test surface without teaching anything new about latency. |
| Persistence, journaling, crash recovery | A real exchange needs it; it would dominate the latency profile and obscure the queue comparison. |
| Self-trade prevention, margin, position limits | Risk-system concerns, not matching-engine concerns. |
| Decimal/fixed-point prices | `double` is used for v1 simplicity — see below. |
| Market data fan-out (ITCH-style feed) | Out-bound path; the project measures the in-bound path. |

### Known compromises

- **`double` for price.** Real venues use integer ticks or fixed-point decimals
  because binary floating point cannot represent `0.01` exactly, and `std::map`
  keyed on `double` can produce two "equal" price levels that compare unequal.
  All test and benchmark prices are generated on a fixed tick grid from integers,
  so no price is ever the result of floating-point arithmetic. If v2 happens, the
  first change is `int64_t price_ticks`.

- **`std::map<double, std::deque<Order>>`** is a node-based container — every
  price level is a separate allocation and lookups chase pointers. This is the
  textbook starting structure. The benchmark indicts it (it's the bottleneck),
  which is a feature of the project: measure it, then say so.

## File structure

```
matching-engine/
├── include/
│   ├── order.hpp
│   ├── order_book.hpp
│   ├── matching_engine.hpp
│   ├── price_collar.hpp
│   ├── order_queue.hpp
│   ├── mutex_order_queue.hpp
│   ├── lockfree_order_queue.hpp
│   ├── order_pool.hpp
│   └── tcp_feed_handler.hpp
├── src/
│   ├── order_book.cpp
│   ├── matching_engine.cpp
│   ├── price_collar.cpp
│   ├── tcp_feed_handler.cpp
│   └── server_main.cpp
├── tests/
│   ├── scratch_test.cpp
│   ├── test_matching_engine.cpp
│   └── test_threaded_runner.cpp
├── benchmarks/
│   └── benchmark_queue.cpp
├── client/
│   └── order_generator.py
├── docs/
│   ├── initial.md
│   ├── implementation_plan.md
│   └── competitive_intelligence.md
├── CMakeLists.txt
└── README.md
```
