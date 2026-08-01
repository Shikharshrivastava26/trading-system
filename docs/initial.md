# Initial Design — C++ Order Matching Engine

**Status:** Design frozen for v1
**Owner:** Shikhar
**Last updated:** 2026-07-25

---

## 1. Goal

Build a simplified exchange matching engine that receives buy/sell orders, matches
them by **price-time priority**, and reports **real, measured** performance numbers.

The project deliberately extends existing production experience (transaction systems,
message queues, multithreaded C++) into three areas that experience does *not* already
cover:

1. **Lock-free concurrency** — a hand-written ring buffer, benchmarked head-to-head
   against the mutex version.
2. **Low-latency memory management** — pre-allocated object pools, zero heap
   allocation on the hot path.
3. **Kernel-level network I/O** — an `epoll` event loop feeding the same queue
   interface used in-process.

### What "done" means

The project is finished when the README can state, backed by reproducible numbers:

- throughput in orders/sec for both queue implementations,
- p50 / p99 / p99.9 latency for both, on an identical seeded order sequence,
- a written explanation, in the author's own words, of *why* the numbers came out
  the way they did.

A benchmark that produces one average number is explicitly **not** the goal.

---

## 2. Scope

### In scope (v1)

- LIMIT and MARKET orders, BUY and SELL.
- Price-time priority matching with partial fills and multi-level walks.
- Order cancellation.
- A price collar (fat-finger / circuit-breaker check) as a pre-match filter.
- Two interchangeable order-queue implementations behind one interface.
- A pre-allocated order pool.
- Unit tests covering every matching rule.
- A reproducible, seeded benchmark harness.
- A TCP feed handler over `epoll` (bonus layer, Phase 7).

### Explicitly out of scope

These are omitted on purpose; each is noted so a reader knows the omission was a
decision, not an oversight.

| Omitted | Why |
|---|---|
| Multi-symbol books | One book keeps the concurrency story clean. Multi-symbol is a sharding problem, not a matching problem. |
| Iceberg / stop / IOC / FOK order types | Adds combinatorial test surface without teaching anything new about latency. |
| Persistence, journaling, crash recovery | A real exchange needs it; it would dominate the latency profile and obscure the queue comparison. |
| Self-trade prevention, margin, position limits | Risk-system concerns, not matching-engine concerns. |
| Decimal/fixed-point prices | `double` is used for v1 simplicity. See "Known compromises" below. |
| Market data fan-out (ITCH-style feed) | Out-bound path; the project measures the in-bound path. |

### Known compromises (call these out in the README rather than hiding them)

- **`double` for price.** Real venues use integer ticks or fixed-point decimals
  because binary floating point cannot represent `0.01` exactly, and `std::map`
  keyed on `double` can produce two "equal" price levels that compare unequal.
  v1 accepts this; the mitigation is that all test and benchmark prices are
  generated on a fixed tick grid from integers, so no price is ever the result of
  floating-point arithmetic. **If v2 happens, the first change is
  `int64_t price_ticks`.**
- **`std::map<double, std::deque<Order>>`** is a node-based container — every price
  level is a separate allocation and lookups chase pointers. This is the textbook
  starting structure and it is what the tests are written against. It is also the
  single most likely thing the benchmark will indict. That is a *feature* of the
  project: measure it, then say so.

---

## 3. Architecture

```
clients (buyers/sellers)
        │  TCP
        ▼
TCP feed handler (epoll)                 ── Phase 7
        │
        ▼
price collar check ──── reject ──▶ order rejected, returned to client   ── Phase 3
        │  (within allowed band)
        ▼
order queue (mutex/condvar  OR  lock-free SPSC/MPMC)   ── Phases 4 & 5
        │
        ▼
matching engine                                         ── Phases 1 & 2
   order book: bids sorted high→low, asks low→high,
   FIFO within each price level
        │
        ▼
trade executed → last price updated
```

For local development and benchmarking the TCP layer is bypassed: the harness calls
`submit_order()` directly, in-process. This is what makes the queue comparison in
Phase 6 honest — network jitter would swamp the difference being measured.

---

## 4. Core types

### `Order`

```cpp
struct Order {
    uint64_t   order_id;
    uint64_t   client_id;
    enum class Side { BUY, SELL }      side;
    enum class Type { LIMIT, MARKET }  type;
    double     price;              // ignored for MARKET orders
    uint64_t   quantity;
    uint64_t   remaining_quantity;
    uint64_t   timestamp;          // used for time priority
};
```

`timestamp` is a monotonic sequence number assigned at ingress, **not** wall-clock
time. Wall clock is non-monotonic and has insufficient resolution to order two
orders that arrive in the same microsecond; a sequence counter makes time priority
total and deterministic.

### `Trade`

```cpp
struct Trade {
    uint64_t buy_order_id;
    uint64_t sell_order_id;
    double   price;                // always the RESTING order's price
    uint64_t quantity;
    uint64_t timestamp;
};
```

The price rule is the one non-obvious matching invariant: the aggressor gets price
improvement, the resting order gets the price it advertised. A buy limit at 101
hitting a resting ask at 100 trades at **100**.

### `OrderBook`

```cpp
class OrderBook {
    std::map<double, std::deque<Order>, std::greater<double>> bids; // high → low
    std::map<double, std::deque<Order>>                       asks; // low → high
public:
    void   add_resting_order(Order o);
    bool   cancel_order(uint64_t order_id);
    double best_bid() const;
    double best_ask() const;
    bool   empty_side(Side s) const;
};
```

`std::greater<double>` on bids means `bids.begin()` is always the best bid and
`asks.begin()` is always the best ask — both sides walk outward from `begin()` with
identical code shape.

Cancellation needs an auxiliary `unordered_map<order_id, location>` index, otherwise
cancel is O(total orders). Phase 1 may ship the linear scan; Phase 2 must not.

### `PriceCollar`

```cpp
class PriceCollar {
    double band_percent;   // e.g. 10.0 for ±10%
public:
    bool is_valid(double order_price, double last_price) const;
};
```

Runs *before* an order reaches the queue. Rejects any order priced too far from the
current last traded price — a simplified version of real exchange circuit breakers
and fat-finger checks. Two edge cases must be decided explicitly and tested:
**(a)** what happens before the first trade, when `last_price` is undefined, and
**(b)** whether MARKET orders are collar-checked at all (they have no price).
v1 answer: no collar before the first trade; MARKET orders bypass the collar.

### `MatchingEngine`

```cpp
class MatchingEngine {
    OrderBook    book;
    PriceCollar  collar;
    double       last_price;
public:
    // Walks the opposite side of the book from best price outward, matching until
    // the incoming order is filled or no more crosses exist. Any unfilled
    // remainder rests in the book.
    std::vector<Trade> submit_order(Order incoming);
    bool cancel_order(uint64_t order_id);
};
```

### `IOrderQueue` — the seam that makes the benchmark valid

```cpp
class IOrderQueue {
public:
    virtual void push(Order o)      = 0;
    virtual bool pop(Order& out)    = 0;
    virtual ~IOrderQueue()          = default;
};

class MutexOrderQueue    : public IOrderQueue { /* mutex + condvar + std::queue */ };
class LockFreeOrderQueue : public IOrderQueue { /* SPSC or MPMC ring buffer */ };
```

One interface, two implementations, one benchmark harness — a direct comparison
rather than two different codebases producing two unrelated numbers.

Note the deliberate tension: the `virtual` call costs a few nanoseconds per order,
which is a real fraction of a lock-free push. Phase 6 must either (a) template the
harness on the queue type and report both, or (b) measure the vcall overhead and
state it. Ignoring it invalidates the comparison.

### `OrderPool`

```cpp
class OrderPool {
    // pre-allocated fixed-size free list of Order slots
public:
    Order* acquire();
    void   release(Order* o);
};
```

Keeps `new`/`delete` off the hot path so the matching loop does zero heap allocation
under load.

### `TCPFeedHandler` (Phase 7)

```cpp
class TCPFeedHandler {
    // single epoll event loop; accepts many client sockets, parses incoming bytes
    // into Order structs, pushes each into the same IOrderQueue used in-process
public:
    void run();
};
```

---

## 5. Threading model

**v1 (recommended start):** one producer thread → `IOrderQueue` → one consumer
thread running the matching loop. Clean SPSC. The order book has exactly one writer,
so `OrderBook` itself never needs to be thread-safe.

**Stretch:** multiple producer threads (simulating many client connections) → MPMC
queue → still one matching consumer. Input concurrency rises; the single-writer book
invariant — and therefore correctness — is untouched.

This is the central design decision of the project and the README should say so
plainly: *concurrency was pushed entirely into the queue so that the matching logic
stays single-threaded and provably correct.*

---

## 6. Correctness tests

Written **before** the code they cover (Phase 2 gates on this).

| # | Test | Asserts |
|---|---|---|
| 1 | Exact price match | one trade, both orders fully filled |
| 2 | Price priority | higher bid matched before lower bid |
| 3 | Time priority | earlier order at same price matched first |
| 4 | Partial fill | remainder correctly stays in book |
| 5 | Post-partial book state | no further cross exists |
| 6 | Multi-level walk | aggressive order sweeps several price levels in one submission |
| 7 | Price improvement | aggressor executes at resting price, not its own limit |
| 8 | Cancel | removed from book, never matches later |
| 9 | Collar reject | order too far from last price is rejected |
| 10 | Collar accept | order within band passes through |

Additional invariants worth asserting once the engine is under load (Phase 6):
**conservation** — total filled quantity on the buy side equals total on the sell
side; and **no-crossed-book** — after every `submit_order` returns,
`best_bid() < best_ask()`.

---

## 7. Benchmark plan

- **Load:** N threads submitting a large, **seeded and reproducible** batch of
  randomly generated orders (target 1,000,000).
- **Metrics:** orders/sec throughput, and latency percentiles p50 / p99 / p99.9,
  measured submitted→matched. Never a bare average.
- **Method:** run the *identical* order sequence twice — once with
  `MutexOrderQueue`, once with `LockFreeOrderQueue` — and report head-to-head.
- **Optional:** repeat over the TCP path for end-to-end latency including
  serialization, reported alongside (not instead of) the in-process number.

Latency must be recorded into a pre-sized array and percentiles computed *after* the
run. Computing them online, or logging per-order, measures the instrumentation.

---

## 8. File structure

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
├── src/                       (matching .cpp files)
├── tests/
│   └── test_matching_engine.cpp
├── benchmarks/
│   └── benchmark_queue.cpp
├── client/
│   └── order_generator.py     (simple TCP test client)
├── docs/
│   ├── initial.md
│   ├── implementation_plan.md
│   └── competitive_intelligence.md
├── CMakeLists.txt
└── README.md
```

---

## 9. Glossary

- **Aggressor / taker** — the incoming order that crosses the spread and removes
  liquidity.
- **Resting / maker** — an order already in the book providing liquidity.
- **Cross** — the condition `best_bid >= best_ask`, meaning a trade is possible. A
  correct book never stays crossed after a submission returns.
- **Price-time priority** — better price wins; at equal price, earlier arrival wins.
- **Price improvement** — the aggressor filling at a better price than its own limit,
  because the resting order's price is used.
- **Price collar** — a band around the last traded price outside which orders are
  rejected.
- **SPSC / MPMC** — single-producer single-consumer / multi-producer multi-consumer.

---

## 10. Next document

See [implementation_plan.md](implementation_plan.md) for the phase-by-phase build
order, and [competitive_intelligence.md](competitive_intelligence.md) for how this
design compares to real venues and to other public projects in the same space.
