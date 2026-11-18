// Throughput benchmarks (Google Benchmark).

#include <benchmark/benchmark.h>

#include "lob/order_book.hpp"
#include "workload.hpp"

using namespace lob;
using namespace lob::bench;

namespace {

const std::vector<Event>& workload() {
    static const std::vector<Event> events = make_workload();
    return events;
}

template <class Sink>
inline void apply(OrderBook& book, const Event& e, Sink& sink) {
    switch (e.type) {
        case EventType::Limit:
            book.add_limit(e.id, e.side, e.price, e.qty, sink);
            break;
        case EventType::Market:
            book.add_market(e.id, e.side, e.qty, sink);
            break;
        case EventType::Cancel:
            book.cancel(e.id);
            break;
    }
}

void BM_MixedOrderFlow(benchmark::State& state) {
    const auto& events = workload();
    std::uint64_t traded = 0;
    auto sink = [&](const Trade& t) { traded += t.qty; };

    for (auto _ : state) {
        state.PauseTiming();
        OrderBook book(events.size());
        state.ResumeTiming();
        for (const Event& e : events) apply(book, e, sink);
        benchmark::DoNotOptimize(traded);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(events.size()));
}
BENCHMARK(BM_MixedOrderFlow)->Unit(benchmark::kMillisecond);

void BM_AddThenCancel(benchmark::State& state) {
    OrderBook book;
    // Keep a realistic resting book underneath.
    for (OrderId i = 1; i <= 10'000; ++i) {
        book.add_limit(i, i % 2 ? Side::Buy : Side::Sell,
                       i % 2 ? 99'900 - static_cast<Price>(i % 50)
                             : 100'100 + static_cast<Price>(i % 50),
                       10);
    }
    OrderId id = 1'000'000;
    for (auto _ : state) {
        book.add_limit(id, Side::Buy, 99'950, 10);
        benchmark::DoNotOptimize(book.cancel(id));
        ++id;
    }
    state.SetItemsProcessed(state.iterations() * 2);
}
BENCHMARK(BM_AddThenCancel);

// Builds N ask levels then sweeps them all with one market order.
// (Rebuilding inside the timed loop avoids PauseTiming overhead, which would
// dominate at this scale.) items = N limit orders + 1 sweeping market order.
void BM_BuildAndSweep(benchmark::State& state) {
    const auto levels = static_cast<Price>(state.range(0));
    std::uint64_t traded = 0;
    auto sink = [&](const Trade& t) { traded += t.qty; };
    OrderBook book;
    OrderId id = 1;
    for (auto _ : state) {
        for (Price p = 0; p < levels; ++p) book.add_limit(id++, Side::Sell, 1000 + p, 10);
        book.add_market(id++, Side::Buy, static_cast<Qty>(levels) * 10, sink);
        benchmark::DoNotOptimize(traded);
    }
    state.SetItemsProcessed(state.iterations() * (state.range(0) + 1));
}
BENCHMARK(BM_BuildAndSweep)->Arg(1)->Arg(10)->Arg(100);

}  // namespace

BENCHMARK_MAIN();
