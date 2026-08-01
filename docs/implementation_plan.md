# Implementation Plan — 7 Phases

**Companion to:** [initial.md](initial.md)
**Last updated:** 2026-07-25

Each phase has: a goal, concrete deliverables, an **exit criterion** that must be
demonstrable (not "feels done"), and the traps that phase is known to contain.

A phase is not finished until its exit criterion is met. Resist the urge to start
Phase 5 (the fun one) before Phase 2's tests are green — the whole value of the
benchmark rests on the engine being provably correct first.

---

## Phase overview

| Phase | Theme | Exit criterion |
|---|---|---|
| 1 | Data model + order book | Book maintains sorted, FIFO-correct state; can be inspected |
| 2 | Matching engine + tests | All 10 correctness tests green, single-threaded |
| 3 | Price collar | Collar tests green; reject path returns a reason |
| 4 | Order pool + mutex queue | Producer/consumer runs 1M orders, zero allocation on hot path |
| 5 | Lock-free queue | Same 1M orders through ring buffer, identical trade output |
| 6 | Benchmark + analysis | Head-to-head table with p50/p99/p99.9, reproducible from a seed |
| 7 | TCP feed handler + writeup | Python client submits orders over TCP; README complete |

---

## Phase 1 — Core data model and order book

**Goal:** a correct, single-threaded, inspectable order book. No matching yet.

### Deliverables
- `include/order.hpp` — `Order`, `Trade`, `Side`, `Type`.
- `include/order_book.hpp` + `src/order_book.cpp`.
- `CMakeLists.txt` building a static lib plus an empty test target.
- A `dump()` / `to_string()` on the book for eyeballing state during development.

### Build notes
- Ingress assigns `timestamp` from a monotonic `uint64_t` counter, not wall clock.
  Time priority must be total and deterministic.
- `bids` uses `std::greater<double>`, `asks` uses default `std::less` — so
  `begin()` is the best price on both sides and both walks share code shape.
- `add_resting_order` pushes to the **back** of the deque at that price level.
  FIFO is the entire time-priority mechanism; a `push_front` here silently breaks
  test 3 and nothing else.
- Set up the cancel index (`unordered_map<order_id, {side, price}>`) now even if
  cancel itself is a linear scan in this phase. Retrofitting it later touches every
  mutation path.

### Traps
- **Empty-side queries.** `best_bid()` on an empty book must have a defined answer.
  Pick one (`std::optional<double>`, or a sentinel) and use it consistently — a
  half-and-half convention causes bugs in Phase 2's walk loop.
- **Deleting the price level.** When a deque empties, erase the map entry. Leaving
  empty levels makes `begin()` point at a level with no orders and the matching
  loop spins.

### Exit criterion
A hand-written scratch test inserts six orders across three price levels on both
sides and `dump()` prints them in exactly the expected price-then-arrival order.

---

## Phase 2 — Matching engine and the correctness suite

**Goal:** the heart of the project. Price-time priority matching with partial fills
and multi-level walks, proven by tests.

### Deliverables
- `include/matching_engine.hpp` + `src/matching_engine.cpp`.
- `tests/test_matching_engine.cpp` — all 10 tests from initial.md §6.
- Catch2 or GoogleTest wired into CMake, running via `ctest`.
- O(1) cancel via the index built in Phase 1.

### Write the tests first
Not dogma — leverage. Tests 3 (time priority) and 7 (price improvement) are the two
that a from-memory implementation gets wrong most often, and both are two-line tests.
Writing them first turns a subtle logic bug into an immediate red bar.

### The matching loop, in words
```
while incoming has remaining quantity AND opposite side is non-empty:
    best = opposite.begin()                    // best price level
    if incoming is LIMIT and does not cross best->price: break
    front = best->second.front()               // earliest order at that level
    fill  = min(incoming.remaining, front.remaining)
    emit Trade{ ..., price = front.price, quantity = fill }   // RESTING price
    decrement both; last_price = front.price
    if front fully filled: pop_front (and erase level if now empty)
if incoming still has remaining AND is LIMIT: rest it in the book
if incoming still has remaining AND is MARKET: drop the remainder (v1 policy)
```

### Traps
- **Price improvement (test 7).** The trade price is `front.price`, *never*
  `incoming.price`. Getting this backwards still passes tests 1–4.
- **Crossing comparison direction.** BUY crosses when `incoming.price >= ask`;
  SELL crosses when `incoming.price <= bid`. Write both, test both.
- **MARKET orders have no price.** They must never reach the collar and must never
  be compared against a limit. Branch early.
- **Iterator invalidation.** Erasing the map entry while holding an iterator into it
  is UB. Capture what you need, then erase.
- **Unfilled MARKET remainder.** Decide the policy (v1: discard) and test it.

### Exit criterion
`ctest` green on all 10 tests, plus the two invariants: conservation of quantity, and
`best_bid() < best_ask()` after every submission.

---

## Phase 3 — Price collar

**Goal:** a pre-match risk filter, plugged in ahead of the queue.

### Deliverables
- `include/price_collar.hpp` + `src/price_collar.cpp`.
- Rejection carries a **reason**, not a bare `false` — Phase 7's client needs
  something to display. A small `enum class RejectReason` is enough.
- Tests 9 and 10, plus the two edge cases below.

### Decisions to make explicit (and test)
- **Before the first trade** `last_price` is undefined. v1: collar is inactive until
  the first trade sets a reference price. The alternative (seed a reference price at
  startup) is what real venues do with a previous-day close — mention this in the
  README, it shows you know why the field exists.
- **MARKET orders** have no price and bypass the collar entirely.
- **Reject vs. clamp.** v1 rejects. Clamping silently changes a client's intent and
  is the wrong default for anything resembling an exchange.

### Trap
The collar reads `last_price`, which the matching thread writes. In the threaded
phases this is a cross-thread read. Either do the collar check on the producer side
against a `std::atomic<double>` snapshot, or move it inside the matching thread.
**Decide now**, in this phase, before threads exist — retrofitting it is worse.
v1 recommendation: `std::atomic<double> last_price`, relaxed load on the producer
side. The collar is a coarse ±10% band; a slightly stale reference is acceptable and
that reasoning is worth a sentence in the README.

### Exit criterion
Collar tests green, and a rejected order is observably *never* enqueued.

---

## Phase 4 — Order pool and the mutex queue

**Goal:** make the submit path threaded and allocation-free. This is the baseline
that Phase 5 must beat.

### Deliverables
- `include/order_queue.hpp` — the `IOrderQueue` interface.
- `include/mutex_order_queue.hpp` — `std::mutex` + `std::condition_variable` +
  `std::queue`.
- `include/order_pool.hpp` — pre-allocated free list.
- A runner: producer thread → queue → consumer thread running `submit_order`.
- Clean shutdown: a sentinel/poison-pill or an atomic `done` flag that the consumer
  observes *after* draining, so no order is lost at teardown.

### Build notes
- Size the pool for the full benchmark run (1M+ slots) so the hot path never hits an
  exhausted pool and falls back to `new`.
- `acquire()`/`release()` are called from different threads. Either make the free
  list itself lock-free, or keep it thread-local per producer. Simplest correct v1:
  one pool per producer thread.
- Verify "zero allocation on the hot path" rather than asserting it: override global
  `operator new` with a counter, run the benchmark, and check the count is flat after
  warm-up. This is a strong README line because it is *measured*.

### Traps
- **Lost wakeup.** Always `notify` while or after mutating under the lock, and always
  use the predicate form of `wait`.
- **Spurious wakeups.** The predicate form handles them; the bare form does not.
- **`pop` semantics.** The interface says `bool pop(Order&)`. Decide whether that is
  blocking-until-available or try-pop, and make *both* implementations agree — a
  mismatch here quietly turns Phase 6 into a comparison of two different behaviours.
  v1: blocking with a shutdown escape.

### Exit criterion
1M orders through the mutex queue produce byte-identical trade output to the
single-threaded Phase 2 run over the same seeded sequence.

---

## Phase 5 — Lock-free queue

**Goal:** the headline learning of the project — a hand-written ring buffer.

### Deliverables
- `include/lockfree_order_queue.hpp` — bounded ring buffer, SPSC first.
- MPMC variant as the stretch goal (CAS on a sequence number per slot, the
  Vyukov-style bounded queue).
- Same 1M-order run, identical trade output.

### Build notes — SPSC
- Power-of-two capacity, mask instead of modulo.
- `head` and `tail` are `std::atomic<size_t>`, each on **its own cache line**
  (`alignas(64)`), otherwise the two threads ping-pong the same line and the
  lock-free version can measure *slower* than the mutex one. This is the single most
  instructive bug in the whole project — if you hit it, keep the before/after numbers
  and put them in the README.
- Memory ordering: producer publishes with `release`, consumer reads with `acquire`.
  Don't reach for `seq_cst` everywhere out of caution, and don't reach for `relaxed`
  without being able to say why it's safe.
- Full ring: decide between spin, block, or reject-and-count. For a benchmark,
  **count the spins** — that number is itself a result worth reporting.

### Traps
- **False sharing** (above). Measure it deliberately: run once without `alignas`,
  once with.
- **ABA** — not a concern for SPSC with monotonically increasing indices; it *is* a
  concern for a naive MPMC free-list. Note why in a comment.
- **Copying `Order` through the ring.** `Order` is a POD of ~64 bytes; a copy is
  fine and simpler than pointer indirection. If you instead pass `Order*` from the
  pool, the pool must outlive every in-flight pointer.
- **Benchmarking a queue with no consumer pressure.** If the consumer is much faster
  than the producer the ring is always empty and you are measuring an uncontended
  fast path. Report the producer/consumer rate ratio alongside the result.

### Exit criterion
Identical trade output to Phases 2 and 4, plus a ThreadSanitizer run
(`-fsanitize=thread`) that is clean — or with every reported race individually
explained.

---

## Phase 6 — Benchmark and analysis

**Goal:** the numbers. This phase is the deliverable the rest of the project exists
to support.

### Deliverables
- `benchmarks/benchmark_queue.cpp`.
- A seeded generator producing a deterministic order stream (fixed seed → identical
  sequence → the two queues are compared on genuinely identical input).
- Latency recorded per order into a pre-sized `std::vector<uint64_t>`; percentiles
  computed after the run.
- A results table, checked into the README.

### Method
- **Warm-up** for a fixed count before recording — first-touch page faults and
  branch-predictor training otherwise land entirely in the mutex run if it goes first.
- **Alternate the order** of the two runs across repetitions; report the median of
  several repetitions, not a single run.
- **Pin threads** (`pthread_setaffinity_np`) and note the topology — same physical
  core vs. different cores vs. different sockets changes the answer materially.
- Record the machine: CPU model, core count, compiler and flags, kernel, and whether
  the CPU governor was set to `performance`. Numbers without this context are not
  reproducible.
- Timestamp with `std::chrono::steady_clock` or `rdtsc`; if `rdtsc`, calibrate it and
  say so.

### Results table shape
| Queue | Throughput (orders/s) | p50 (ns) | p99 (ns) | p99.9 (ns) | max (ns) |
|---|---|---|---|---|---|
| Mutex | | | | | |
| Lock-free SPSC | | | | | |

Also report: producer thread count, whether the virtual-call overhead was measured
or templated away, and the spin count on a full ring.

### Traps
- **The vcall through `IOrderQueue`** is real overhead at this scale. Either template
  the harness on the queue type, or measure the vcall cost and state it. Silently
  ignoring it undermines the comparison.
- **Measuring the generator.** Pre-generate the entire order sequence into a vector
  *before* starting the clock.
- **Expecting lock-free to always win.** Under low contention, an uncontended
  `std::mutex` is a couple of atomics and is genuinely competitive. If the mutex
  wins in some configuration, **report that** — an honest surprising result is worth
  more than a tidy expected one, and being able to explain it is the actual signal.
- **p99 dominated by the allocator or the map.** If tail latency is bad, profile
  before blaming the queue; `std::map` node allocation is the prime suspect.

### Exit criterion
Anyone can clone the repo, run one command with the documented seed, and reproduce
the table within run-to-run noise.

---

## Phase 7 — TCP feed handler and writeup

**Goal:** close the loop from network to trade, then write it all down.

### Deliverables
- `include/tcp_feed_handler.hpp` + `src/tcp_feed_handler.cpp` — a single `epoll`
  loop, non-blocking sockets, accepting many clients.
- A wire format: fixed-size binary header + payload. Do **not** invent a
  text protocol; framing is the point of the exercise.
- `client/order_generator.py` — connects, submits orders, prints trades and rejects.
- `README.md` — design, benchmark numbers, and the mutex-vs-lock-free tradeoff in
  your own words.

### Build notes
- Non-blocking sockets + level-triggered `epoll` for v1. Edge-triggered is faster and
  strictly harder to get right (you must drain until `EAGAIN` every time); attempt it
  only after level-triggered works.
- **Partial reads are the whole difficulty.** TCP is a byte stream: one `read()` may
  deliver half an order, or three and a half. Every connection needs its own
  accumulation buffer and a parse loop that consumes only complete messages.
- Reuse the exact same `IOrderQueue` instance the in-process path uses. That reuse is
  the architectural payoff of Phase 4's interface and deserves a README paragraph.
- Handle `EPOLLHUP`/`EPOLLERR` and clean up per-connection state, or a long run leaks
  file descriptors.

### README contents (this is the actual final artifact)
1. What it is, in three sentences.
2. Architecture diagram (reuse the one in initial.md).
3. How to build and run the tests.
4. How to reproduce the benchmark, with the seed.
5. The results table + the machine spec.
6. **The analysis** — why the numbers came out that way, what surprised you, what
   the bottleneck actually was.
7. Known limitations — lift the "out of scope" and "known compromises" tables from
   initial.md verbatim. Stating them is a strength, not an apology.

### Exit criterion
`python client/order_generator.py` on one terminal, engine on another, trades print.
End-to-end latency measured and reported *alongside* — not in place of — the
in-process number.

---

## Sequencing notes

- **Phases 1–3 are strictly sequential.** Each builds directly on the last.
- **Phases 4 and 5 share an interface**, so Phase 5 is a drop-in once Phase 4's
  runner exists.
- **Phase 6 needs both**, and is where most of the project's actual value lands.
- **Phase 7 is genuinely optional.** If time runs short, a finished Phase 6 with a
  strong analysis is a better project than a rushed Phase 7 bolted onto a thin
  benchmark. Cut here, not from testing or measurement.

## Risk register

| Risk | Mitigation |
|---|---|
| Lock-free queue is subtly broken and only fails under load | Identical-output check against the Phase 2 baseline + TSan; treat trade output as a golden file |
| `double` price keys cause phantom price levels | Generate all prices from an integer tick grid; never compute a price |
| Benchmark numbers not reproducible | Fixed seed, pinned threads, recorded machine spec, median of N runs |
| False sharing makes lock-free look bad | Explicitly test with and without `alignas(64)`; keep both numbers |
| Scope creep into order types / multi-symbol | The out-of-scope table in initial.md is the contract |
| Phase 7 eats the schedule | It is explicitly cuttable; Phase 6 is the deliverable |
