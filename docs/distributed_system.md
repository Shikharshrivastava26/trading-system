# Distributed System — 5 Phases

**Companion to:** [implementation_plan.md](implementation_plan.md)
**Last updated:** 2026-07-31
**Status: all 5 phases implemented and live-tested with real processes** (not
just unit tests — see "How to run" under each phase).

The engine built in `implementation_plan.md` is correct and fast, but it is a
single process: one `LockFreeOrderQueue` feeding one `MatchingEngine`, all
state in one address space. This document lays out how it was turned into a
system that runs across multiple processes/machines without changing the
matching logic itself — `match_against()` and the rest of
`matching_engine.cpp`/`order_book.cpp` are untouched throughout, exactly as
planned.

Everything here is a **local WAL / flock-based stand-in for Kafka + a
coordinator** (files instead of a broker, `flock` instead of etcd/ZooKeeper
leader election) — same offset/replay/leader semantics, runnable on one
machine with no infra to install. Swapping in real Kafka later only touches
`log_backed_order_queue.hpp` and `trade_log.hpp`; nothing else changes.

---

## Phase overview

| Phase | Theme | New files |
|---|---|---|
| 1 | Replicated event log | `log_backed_order_queue.hpp`, `test_log_backed_queue.cpp` |
| 2 | Split gateway from matching engine | `gateway_main.cpp`, `engine_main.cpp` |
| 3 | Shard by symbol | `sharding.hpp`, `shard_router_queue.hpp`, `test_sharding.cpp` |
| 4 | Active + standby failover | `shard_leader_lock.hpp` |
| 5 | Pub/sub for trades & market data | `trade_log.hpp`, `trades_subscriber_main.cpp` |

Shared test file for Phases 4+5: `test_failover_and_pubsub.cpp`.

---

## Phase 1 — Replicated event log

**What it is:** [`LogBackedOrderQueue`](../include/log_backed_order_queue.hpp)
— an `IOrderQueue` implementation alongside the existing
`LockFreeOrderQueue`/`MutexOrderQueue`. `push()` fsyncs an `Order` to a
`<base>.log` file; `pop()` polls the file's actual size (via `fstat`, not a
cached counter) and checkpoints its read position to `<base>.offset` after
every record. `MatchingEngine` never changes — it still just calls `pop()`.

Two details that only showed up once this ran as real separate processes:
- **Polling, not a condition_variable.** A `condition_variable` can't be
  signaled across address spaces, so a consumer process would otherwise
  never notice another process's writes. `pop()` polls file size instead.
- **Append-mode writes, not a cached offset.** `push()` never seeks to a
  remembered position — it relies on `"a+b"` mode positioning every write at
  end-of-file, which POSIX/glibc guarantee is atomic per `write()` call. That
  makes concurrent writers (multiple gateways, Phase 2) safe with no locking.

### How to run
```sh
# Exit criterion, as an automated test: kill mid-stream, restart, resume exactly.
./build/log_queue_tests
```

---

## Phase 2 — Split gateway from matching engine

**What it is:** two new executables sharing the existing `TCPFeedHandler`/
`MatchingEngine` code unchanged:
- [`gateway_main.cpp`](../src/gateway_main.cpp) — stateless. Owns the TCP
  listener and order validation; holds no order-book state at all.
- [`engine_main.cpp`](../src/engine_main.cpp) — a log-consumer loop calling
  the unmodified `MatchingEngine::submit_order()`.

### How to run
```sh
./build/engine  0 /tmp/me_demo 1 &         # engine for shard 0
./build/gateway 9000 1 /tmp/me_demo &      # gateway, 1 shard, same base path
python3 client/order_generator.py 100      # sends orders over real TCP
```
The gateway and engine are separate OS processes talking only through the
`/tmp/me_demo_shard0.log` file — confirmed by running exactly this and
watching trades appear in the engine's output.

---

## Phase 3 — Shard by symbol

**What it is:**
- [`sharding.hpp`](../include/sharding.hpp) — `shard_for_symbol()`, an FNV-1a
  hash (chosen over `std::hash<std::string>`, which the standard doesn't
  guarantee stable across runs/processes). Any process computes the same
  shard for the same symbol with zero coordination.
- [`shard_router_queue.hpp`](../include/shard_router_queue.hpp) — the
  gateway-side `IOrderQueue` that fans a single `push()` out to the correct
  shard's `LogBackedOrderQueue` by symbol.
- `engine_main.cpp` runs one `MatchingEngine` per symbol it encounters
  (`std::unordered_map<std::string, MatchingEngine>`), since a shard can own
  several symbols.

### How to run — two symbols, two engine processes, live
```sh
./build/engine  0 /tmp/me_demo 2 &
./build/engine  1 /tmp/me_demo 2 &
./build/gateway 9000 2 /tmp/me_demo &
```
Send a mix of `"XYZ"` and `"ABC"` orders (see `client/order_generator.py`,
which now randomizes symbol per order) and watch each engine only ever print
trades for the symbol its shard owns. Verified live: XYZ landed on shard 0,
ABC on shard 1, each matched independently, in the same run.

---

## Phase 4 — Active + standby failover

**What it is:** [`shard_leader_lock.hpp`](../include/shard_leader_lock.hpp) —
an advisory `flock()` per shard standing in for Kafka consumer-group
rebalancing / etcd / ZooKeeper. Whoever holds the lock is active; the OS
releases it automatically if that process dies, **even via `kill -9`**, with
no cleanup code required.

`engine_main.cpp` now takes a `replica_id`. Every replica of a shard replays
the *entire* log independently (via `LogBackedOrderQueue`'s `consumer_id`
parameter, so each replica gets its own checkpoint and never disturbs
another's read position) — meaning a standby's `OrderBook` stays identical to
the active one at all times. Only the lock-holder prints/publishes; everyone
else stays silently warm.

### How to run — real kill -9, live promotion
```sh
./build/engine  0 /tmp/me_fo 1 primary &   # acquires the lock -> ACTIVE
./build/engine  0 /tmp/me_fo 1 standby &   # loses the race -> STANDBY, stays warm
./build/gateway 9000 1 /tmp/me_fo &
python3 client/order_generator.py 20       # both replicas silently apply these

kill -9 <primary-pid>                      # simulates a hard crash, no cleanup
python3 client/order_generator.py 20       # standby's log now shows PROMOTED,
                                            # and starts printing/publishing trades
```
This was run exactly this way: the standby's log shows
`PROMOTED to active (previous active is gone)` immediately after the kill,
followed by correct trades for the orders sent afterward.

---

## Phase 5 — Pub/sub for trades and market data

**What it is:** [`trade_log.hpp`](../include/trade_log.hpp) — a `trades`
topic with the same append-only design as Phase 1, but built for *many*
independent subscribers rather than one consumer. The active engine now
calls `trades.publish(t)` alongside its existing printf. A brand-new
subscriber has no checkpoint file yet, so it starts at offset 0 and
naturally replays full history before catching up to live trades — "historical
+ live" falls out of the same mechanism, no separate backfill path.

[`trades_subscriber_main.cpp`](../src/trades_subscriber_main.cpp) is a demo
consumer proving the point: it exists purely by pointing at the log, with
**zero changes to `gateway_main.cpp` or `engine_main.cpp`**.

### How to run — attach after the fact, see history replay
```sh
./build/engine  0 /tmp/me_ps 1 primary &
./build/gateway 9000 1 /tmp/me_ps &
python3 client/manual_test.py              # produces a trade

./build/trades_subscriber 0 /tmp/me_ps my_new_consumer
# -> immediately prints the trade that already happened, from offset 0,
#    before waiting for any new ones — no code in engine/gateway was touched.
```

---

## What stayed exactly as-is

Across all 5 phases, the following were never modified:
- `order_book.cpp` / `order_book.hpp`
- `matching_engine.cpp` / `matching_engine.hpp` — same `submit_order()`,
  same `match_against()`, same price-collar check
- The lock-free single-threaded matching logic within one symbol's book

The only change to shared data structures was adding a `symbol` field to
`Order` (and the wire protocol) — a real, necessary gap, since nothing
multi-symbol was possible before Phase 3. Everything else new is additive:
new files, new executables, nothing touched inside the matching core.

Distribution turned out to be entirely a story about what surrounds that
code — how orders reach it, how many copies of it run, where its output
goes, and who's allowed to be "the one" running it right now — not about
changing how it matches.
