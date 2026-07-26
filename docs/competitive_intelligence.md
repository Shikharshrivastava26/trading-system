# Competitive Intelligence

**Companion to:** [initial.md](initial.md) and [implementation_plan.md](implementation_plan.md)
**Last updated:** 2026-07-25

> **Sourcing caveat — read this first.** The figures below are *publicly claimed*
> numbers from vendor material, conference talks, and project READMEs, measured on
> undisclosed hardware under undisclosed load. They are here to calibrate order of
> magnitude and to tell you what reviewers will have in their heads — **not** to
> benchmark against. Verify any number before quoting it in your README, and never
> compare your measured result to an unverified one as though they were like for
> like. Written against a May 2026 knowledge cutoff; re-check anything time-sensitive.

---

## 1. Why this document exists

This project's differentiator is not that it matches orders — hundreds of GitHub
repos match orders. It is that it **measures two concurrency strategies honestly on
identical input and explains the result**.

This document exists to keep that differentiator sharp: to know what already exists,
what reviewers will compare it to, and which questions will be asked in the first
five minutes of a conversation about it.

---

## 2. The landscape

### 2.1 Production venues (the reference point, not the target)

| System | Notes |
|---|---|
| **LSE Millennium Exchange** | C++ on Linux. Widely cited as sub-100µs order round-trip. Licensed to multiple venues worldwide. |
| **NASDAQ INET** | The lineage that made the low-latency matching architecture famous. Sub-100µs class. Its protocols — **ITCH** (market data out) and **OUCH** (order entry in) — are the de-facto vocabulary of the space. |
| **CME Globex** | MDP 3.0 for market data, iLink for order entry. Notable for publishing detailed latency statistics — worth reading for how a serious venue *reports* latency, which is directly relevant to Phase 6. |
| **LMAX Exchange** | The origin of the Disruptor pattern. Its architecture paper (2011) is the single most useful read for this project. |

**Takeaway:** production venues live in the **single-digit to low-hundreds of
microseconds, end-to-end, including network**. Your in-process, no-persistence,
no-market-data engine is measuring a strictly smaller problem — so if you measure
sub-microsecond per-order times, that does **not** mean you beat NASDAQ. Say so in
the README before someone says it to you.

### 2.2 The LMAX Disruptor — the direct intellectual ancestor

The single most relevant prior art for Phase 5. Its claims and design decisions:

- A pre-allocated ring buffer with sequence-number coordination instead of locks.
- Cache-line padding on sequence counters to avoid false sharing — **exactly** the
  trap called out in Phase 5.
- Mechanical-sympathy framing: garbage-free steady state, single-writer principle.
- Publicly claimed throughput in the millions of ops/sec on a single thread (Java).

The **single-writer principle** is the same reasoning behind this project's threading
model: push all concurrency into the queue, keep the book single-writer. Being able
to name that as a deliberate, sourced design choice — rather than a convenience —
is worth more in conversation than any benchmark number.

`exchange-core` (Java, Disruptor-based, open source) is the closest public
implementation of this whole idea and claims multi-million orders/sec. It is a
useful architecture read even though it's Java.

### 2.3 Open-source C++ comparables

| Project | Language | Relevance |
|---|---|---|
| **Liquibook** | C++ | Header-only order book with depth tracking. The closest direct comparable to Phases 1–2. Read it *after* writing your own book, not before. |
| **moodycamel::ConcurrentQueue** | C++ | The reference high-performance MPMC queue. Your hand-written ring buffer should be compared to it in Phase 6 if time allows — "mine is within X% of moodycamel" is a far stronger claim than a bare number. |
| **boost::lockfree** | C++ | `spsc_queue` is the textbook bounded SPSC ring. Use as a correctness oracle, not a target. |
| **folly::MPMCQueue / ProducerConsumerQueue** | C++ | Meta's production queues; well-documented memory-ordering reasoning. |
| **Aeron / Chronicle Queue** | C++/Java | Messaging transports used in real trading infra. Relevant to Phase 7's framing decisions. |

**Strategic note:** Phase 6 comparing your ring buffer against `boost::lockfree::spsc_queue`
and/or `moodycamel` costs perhaps two hours and converts the project from "I wrote a
queue" to "I wrote a queue and know where it sits." That is a disproportionate return
and it is the single highest-value optional addition to the plan.

### 2.4 The typical GitHub "matching engine" project

What most of them look like — and therefore what yours is implicitly compared to:

- Single-threaded, `std::map`-based, no measurement at all.
- Or: a throughput number with no percentiles, no seed, no machine spec, no
  methodology.
- Rarely: any tail-latency analysis.
- Almost never: two implementations compared on identical input.
- Almost never: a stated list of what was deliberately left out.

**Where this project wins by default:** p99 reporting, reproducibility from a seed,
the head-to-head comparison, and an honest limitations section. None of these require
more C++ skill than the median project — they require discipline. Protect them.

---

## 3. Where this design sits — honest self-assessment

| Dimension | This project | Production venue | Gap is… |
|---|---|---|---|
| Book structure | `std::map` + `std::deque` | Intrusive lists + array-indexed price ladder | **Large, and known** — see §4 |
| Price type | `double` | Integer ticks / fixed-point | **Real correctness gap**, documented in initial.md |
| Concurrency | SPSC/MPMC queue, single-writer book | Same principle | **Small — this is the strong part** |
| Allocation | Pre-allocated pool | Same principle | **Small** |
| Persistence | None | Journaled, replayable | Out of scope by choice |
| Order types | LIMIT, MARKET | Dozens | Out of scope by choice |
| Network | epoll, level-triggered | Kernel bypass (Solarflare/Onload, DPDK), busy-poll | Large, and fine to name |
| Measurement | p50/p99/p99.9, seeded, reproducible | HdrHistogram, hardware timestamps | **Small — this is the strong part** |

The pattern to notice: the gaps are concentrated in *data structures* and
*infrastructure*, while the concurrency and measurement story is genuinely close to
how the real thing is reasoned about. Lead with the second when presenting it.

---

## 4. The `std::map` question — prepare for it

This will be the first technical challenge from anyone who works in the space, so
have the answer ready rather than discovering it live.

**The critique:** `std::map` is a red-black tree — node-per-price-level allocation,
pointer chasing, cache-hostile. Real books use a **price ladder**: a flat array
indexed by tick offset from a reference price, giving O(1) access to any level and
sequential memory access across levels. Orders within a level are an intrusive
doubly-linked list so cancel is O(1) and needs no auxiliary index.

**Why the map is still the right v1 choice:**
1. It is correct and obvious, so the tests in Phase 2 are testing the *matching
   rules*, not your data structure.
2. It gives a baseline. "I replaced the map with a flat ladder and p99 dropped X%"
   is a far better result than starting with the ladder and having nothing to
   compare against.
3. The project's stated thesis is the **queue** comparison. The book is deliberately
   held constant so it isn't a confounding variable.

**The strong follow-up:** if Phase 6 shows tail latency dominated by map node
allocation — which is plausible — that is a *finding*, and "the queue wasn't the
bottleneck, the book was" is a more sophisticated conclusion than the one the project
set out to prove. Write it up as such.

**Phase 8 (unplanned, but name it if asked):** flat price ladder + intrusive lists,
re-run the identical benchmark, report the delta.

---

## 5. Questions you will be asked

Ordered roughly by likelihood. If you cannot answer one of these in two sentences,
that's a gap to close before presenting the project.

1. **Why is the trade at the resting order's price?** (Price improvement; the
   aggressor pays the advertised price. Test 7 exists precisely for this.)
2. **How do you guarantee time priority?** (Monotonic ingress sequence number, FIFO
   deque per level. Not wall clock — insufficient resolution and non-monotonic.)
3. **Why is the book single-writer?** (All concurrency pushed into the queue;
   matching logic stays provably correct. Same reasoning as LMAX's single-writer
   principle.)
4. **What memory ordering does your ring buffer use, and why is it sufficient?**
   (Release on publish, acquire on consume; the release-acquire pair makes the slot
   write visible before the index update is observed. Be able to say why not
   `seq_cst` and why not `relaxed`.)
5. **Did lock-free actually win? By how much? Why?** (Have the number *and* the
   explanation. "It didn't, under low contention, because an uncontended mutex is
   just a couple of atomics" is an excellent answer if it's what you measured.)
6. **What's your p99, and what drives the tail?** (If you only have an average, the
   conversation ends here.)
7. **Why `double` for price?** (Know it's wrong, know why — 0.01 is not representable,
   map keys can compare unequal for "equal" prices — know the mitigation, know the
   fix.)
8. **How does cancel work, and what's its complexity?** (Index → level → erase.)
9. **What happens when the ring buffer is full?** (Have a policy *and* the measured
   spin count.)
10. **What would you do differently at 10x load?** (Flat price ladder, integer ticks,
    batch dequeue to amortise, then measure again.)
11. **How do you know the lock-free version is correct?** (Identical trade output vs.
    the single-threaded baseline as a golden file, plus a clean TSan run.)
12. **What did you leave out, and why?** (The out-of-scope table. Answering this
    crisply reads as engineering judgment; hedging reads as not having thought about
    it.)

---

## 6. Positioning

**The one-sentence version:**
> A C++ matching engine built to measure two concurrency strategies against each
> other — mutex queue vs. hand-written lock-free ring buffer — on an identical
> million-order sequence, reporting p50/p99/p99.9 rather than a single average.

**Lead with the measurement discipline, not the feature list.** Matching orders is
table stakes; a reproducible head-to-head with tail latencies and a stated
methodology is not.

**Three things to say early:**
1. The single-writer design and *why* concurrency was confined to the queue.
2. The benchmark methodology — seeded, warmed up, pinned, alternated, median of N.
3. The known limitations, unprompted. Naming the `std::map` and `double` gaps before
   being asked converts both from weaknesses into evidence of judgment.

**Two things not to do:**
- Don't compare your number to a production venue's. Different problem, different
  scope. Say so first.
- Don't hide a result where the mutex won. That result, explained correctly, is more
  convincing than the expected one.

---

## 7. Highest-value additions beyond the 7 phases

Ranked by return per hour, if time allows after Phase 6:

1. **Benchmark against `boost::lockfree::spsc_queue` / `moodycamel`** (~2h) — turns
   an absolute number into a positioned one.
2. **Flat price-ladder book, same benchmark** (~1 day) — directly answers the
   loudest critique with data.
3. **Integer tick prices** (~half day) — closes a real correctness gap.
4. **HdrHistogram instead of a sorted vector** (~2h) — the tool the industry actually
   uses for percentiles; a recognisable signal.
5. **Batched dequeue** (~2h) — amortises per-order sync cost; usually a visible win
   and an easy thing to explain.

Note that items 1–5 are all *measurement or data structure* work. None of them add
features. That ratio is itself the point of the project.

---

## 8. Reading list

- **LMAX Disruptor technical paper (2011)** — ring buffer, mechanical sympathy,
  single-writer principle. The most directly relevant document to Phase 5.
- **NASDAQ ITCH and OUCH protocol specs** — short, readable, and the reason Phase 7's
  wire format should be fixed-size binary rather than text.
- **CME MDP 3.0 documentation** — how a real venue structures and reports market data.
- **`boost::lockfree::spsc_queue` source** — the canonical bounded SPSC ring, ~200
  readable lines.
- **`folly::ProducerConsumerQueue` source** — same idea with unusually good comments
  on memory ordering.
- **Liquibook source** — a real C++ order book to read *after* writing your own.
- **Anything by Carl Cook or Matt Godbolt on low-latency C++ (CppCon)** — the
  measurement discipline in Phase 6 comes from this tradition.
