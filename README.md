# lob-engine

A low-latency **limit order book and matching engine** in modern C++20, built as the
foundation for a market-making research stack (market data replay → Avellaneda–Stoikov
market making → Python analytics).

- Price-time priority matching (limit, market/IOC, cancel, size reduction)
- Zero heap allocation in steady state (pooled orders + free list)
- **~51M mixed operations/s** on a single core, p99 latency **< 100 ns**
- **Replays real Binance BTC/USDT data.** The reconstructed book matched the exchange's
  own snapshots exactly, on 49,846 of 49,846 price levels checked.
- Verified against a naive reference implementation on 200k random operations
- CI on Linux (GCC) and macOS (Clang), plus an AddressSanitizer/UBSan job. The CI also
  replays a real market-data sample and fails if the book drifts.

## Results

Measured on an Apple M5, Release build (`-O3`), single thread, 2M-event synthetic order
flow (55% limit, 40% cancel, 5% market; random-walk mid; orders clustered near the touch).

**Throughput** (`build/lob_bench`, Google Benchmark)

| Benchmark | Time | Throughput |
|---|---|---|
| Mixed order flow (1M events) | 19.7 ms | **50.9M ops/s** |
| Add + cancel near top of book | 23.7 ns / pair | 84.4M ops/s |
| Build 10 levels + sweep with one market order | 192 ns | 57.2M ops/s |

**Per-operation latency** (`build/lob_latency`, ns)

| Op | p50 | p90 | p99 | p99.9 |
|---|---|---|---|---|
| limit | 41 | 42 | 83 | 167 |
| market | 41 | 42 | 84 | 167 |
| cancel | ≤ 42 | 42 | 42 | 125 |

> On Apple Silicon `steady_clock` ticks at 24 MHz (~42 ns), so latency percentiles are
> quantised to that resolution; most operations actually complete in a fraction of a tick.
> On Linux x86-64 the clock resolution is ~1 ns.

## Real market data: Binance L2 replay

```
 Binance websocket ──► record.py ──► raw .jsonl.gz ──► convert.py ──► .lob ──► lob_replay
 (depth@100ms, trades)  (+ REST snapshots)  (source of truth)  (sync protocol,  (binary,   (engine + checks
                                                                integer ticks)   40 B/rec)  vs exchange)
```

Result on the bundled 5-minute sample (`data/sample/btcusdt_sample.jsonl.gz`):

```
BTCUSDT  tick 0.01  lot 1e-05  |  5.1 min of market data
depth events   2979  (49763 level updates)
trades         3608  (47.86207 BTCUSDT)
resets         1
crossed fixes  0
replay         9.76 ms  (10.9M records/s, 3276 ns per depth event)
checkpoints    5/5 matched  (49846 levels compared)
```

**How correctness is established.** The recorder captures the diff-depth stream and,
every minute, a full 5000-level REST snapshot. `convert.py` implements Binance's sync
protocol (drop stale events, check that `U <= lastUpdateId+1 <= u`, then require
`U == prev_u + 1`, and resync from a snapshot when there is a gap). It then places each
checkpoint snapshot at its exact update id. If an event straddles that id, the prices
it touches are excluded from the comparison, because they cannot be compared exactly.
`lob_replay` then compares its reconstructed book with the exchange's snapshot, level by
level. Any mismatch makes it exit non-zero, and CI runs it on every push.

**From L2 to an order-by-order book.** Binance publishes aggregated quantity per price
level, so `L2BookBuilder` represents each level as a FIFO of synthetic orders. Increases
join the back of the queue and decreases trim the newest liquidity first. This keeps a
meaningful queue position for the market-making simulator (phase 3). Each exchange event
is applied atomically: decreases first, then increases from the worst price to the best.
That order never creates a crossed intermediate state, so replayed liquidity never trades
against itself. Prices and quantities are converted to integer ticks and lots with exact
decimal arithmetic.

**What profiling real data revealed.** With synthetic flow concentrated at the touch, the
engine runs at ~20 ns/op. Real Binance updates hit the whole 5000-level book, so deep
level inserts shift memory (~25% of replay time). The L2 adapter's per-level allocations
cost about another 25%. At ~3 µs per 100 ms exchange event, replay runs about 30,000×
faster than real time, which is fast enough for multi-day backtests. These two costs are
the next optimisation targets.

## Design

```
            bids_ (ascending)                       asks_ (descending)
   [ 9995 | 9997 | 9998 | 9999 ]  best ->  <- best  [ 10004 | 10002 | 10001 ]
                           │                                           │
                    Level{price, qty, head, tail}                     ...
                           │
             pool_[3] <-> pool_[17] <-> pool_[5]     FIFO of orders (intrusive list)
```

| Choice | Why |
|---|---|
| Integer tick prices | Exact comparisons, no floating point on the hot path. |
| Contiguous order pool + free list | Orders are recycled, so there is no `new`/`delete` per order and memory stays cache-friendly. |
| Intrusive doubly linked FIFO per level (32-bit indices) | O(1) append, O(1) cancel from anywhere in the queue, and indices are half the size of pointers. |
| Sorted `std::vector` of levels, **best price at `back()`** | Nearly all activity happens at the top of book, so fills `pop_back()` and inserts near the touch move almost no memory. It beats a node-based `std::map` on cache behaviour. |
| Trade callback as a template parameter | It gets inlined, with no `std::function` and no virtual dispatch. |

**Known trade-offs.** Opening a new level far from the touch costs O(levels) memmove.
It shows in the 100-level sweep benchmark and in real-data replay (see above).
`std::unordered_map` is used for the id → order index; it is the next hotspot to replace
(see roadmap).

## Correctness

- **Unit tests** cover price-time priority, partial fills, not trading through a limit
  price, IOC semantics, cancels in the middle of a queue, size reductions that keep
  priority, invalid input and pool recycling.
- **Differential testing.** `tests/test_reference_model.cpp` runs 4 seeds × 50k random
  operations against a deliberately simple `std::map<Price, std::deque>` implementation.
  It asserts identical trade sequences, identical full-depth snapshots and a never-crossed
  book after every operation.
- **Sanitizers.** The full suite also runs under ASan + UBSan in CI.

## Build

Requires CMake ≥ 3.20 and a C++20 compiler (GCC 11+, Clang 14+). GoogleTest and Google
Benchmark are fetched automatically.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure   # tests
./build/lob_bench                            # throughput
./build/lob_latency                          # latency percentiles
./build/basic_usage                          # small demo
```

Sanitizer build: `cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DLOB_SANITIZE=ON`

**Replay the bundled real-data sample** (Python 3.9+, standard library only):

```bash
python3 tools/binance/convert.py data/sample/btcusdt_sample.jsonl.gz build/sample.lob
./build/lob_replay build/sample.lob --bbo build/bbo.csv   # also dumps best bid/offer per event
python3 -m unittest discover -s tools/binance             # sync-protocol tests
```

**Record your own data** (public market-data endpoints, no API key):

```bash
python3 -m venv .venv && .venv/bin/pip install -r tools/binance/requirements.txt
.venv/bin/python tools/binance/record.py --symbol BTCUSDT --minutes 30 --out data/btcusdt.jsonl.gz
```

## Usage

```cpp
#include "lob/order_book.hpp"

lob::OrderBook book;
book.add_limit(1, lob::Side::Sell, 10'001, 25);
book.add_limit(2, lob::Side::Sell, 10'002, 40);

auto r = book.add_limit(3, lob::Side::Buy, 10'002, 50, [](const lob::Trade& t) {
    // t.maker_id, t.taker_id, t.price, t.qty
});
// r.filled == 50, r.rested == 0, book.best_ask() == 10'002

book.cancel(2);
```

## Roadmap

- [x] **Phase 1: matching engine.** Order book, tests, benchmarks, CI.
- [x] **Phase 2: market data.** Record Binance L2 and trades, replay them deterministically, and validate the book against exchange snapshots.
- [ ] **Phase 3: market making.** Avellaneda–Stoikov quoting with inventory risk, fees, queue position and adverse-selection analysis, backtested on recorded data.
- [ ] **Phase 4: analytics.** `pybind11` bindings and a notebook covering PnL, Sharpe, drawdown and inventory.
- [ ] **Performance.** Faster deep-level inserts (a tick-indexed ladder or a hybrid of both structures), an allocation-free L2 adapter, an open-addressing id index, `rdtsc`/`cntvct` cycle timing, and huge pages plus core pinning on Linux.

## License

MIT
