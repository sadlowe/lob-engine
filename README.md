# lob-engine

A low-latency **limit order book and matching engine** in modern C++20, built as the
foundation for a market-making research stack (market data replay → Avellaneda–Stoikov
market making → Python analytics).

- Price-time priority matching (limit, market/IOC, cancel, size reduction)
- Zero heap allocation in steady state (pooled orders + free list)
- **~51M mixed operations/s** on a single core, p99 latency **< 100 ns**
- Verified against a naive reference implementation on 200k random operations
- CI on Linux (GCC) and macOS (Clang), plus an AddressSanitizer/UBSan job

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

**Known trade-offs.** Opening a new level far from the touch costs O(levels) memmove
(visible in the 100-level sweep benchmark, where every level is inserted at the far end).
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
- [ ] **Phase 2: market data.** Record Binance / Coinbase L2 websocket feeds and replay them deterministically into the book.
- [ ] **Phase 3: market making.** Avellaneda–Stoikov quoting with inventory risk, fees, queue position and adverse-selection analysis, backtested on recorded data.
- [ ] **Phase 4: analytics.** `pybind11` bindings and a notebook covering PnL, Sharpe, drawdown and inventory.
- [ ] **Performance.** Open-addressing id index, `rdtsc`/`cntvct` cycle timing, and huge pages plus core pinning on Linux.

## License

MIT
