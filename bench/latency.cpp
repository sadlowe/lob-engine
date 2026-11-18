// Per-operation latency distribution (p50 / p99 / p99.9) for each order type.
// Throughput numbers hide tail latency; trading systems care about the tail.
//
// Note: timing granularity is limited by std::chrono::steady_clock. On Linux
// x86-64 it is ~1 ns; on Apple Silicon the underlying counter ticks at 24 MHz
// (~42 ns), so sub-tick percentiles on macOS are quantised.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

#include "lob/order_book.hpp"
#include "workload.hpp"

using namespace lob;
using namespace lob::bench;
using Clock = std::chrono::steady_clock;

namespace {

void report(const char* name, std::vector<std::int64_t>& ns) {
    if (ns.empty()) return;
    std::sort(ns.begin(), ns.end());
    auto pct = [&](double p) {
        const auto k = static_cast<std::size_t>(p * static_cast<double>(ns.size() - 1));
        return ns[k];
    };
    std::printf("%-8s %10zu %8lld %8lld %8lld %8lld %10lld\n", name, ns.size(),
                static_cast<long long>(pct(0.50)), static_cast<long long>(pct(0.90)),
                static_cast<long long>(pct(0.99)), static_cast<long long>(pct(0.999)),
                static_cast<long long>(ns.back()));
}

}  // namespace

int main() {
    WorkloadConfig cfg;
    cfg.events = 2'000'000;
    const auto events = make_workload(cfg);

    OrderBook book(events.size());
    std::uint64_t traded = 0;
    auto sink = [&](const Trade& t) { traded += t.qty; };

    std::vector<std::int64_t> lat_limit, lat_market, lat_cancel;
    lat_limit.reserve(events.size());
    lat_market.reserve(events.size() / 10);
    lat_cancel.reserve(events.size());

    const auto wall_start = Clock::now();
    for (const Event& e : events) {
        const auto t0 = Clock::now();
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
        const auto dt = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0);
        auto& bucket = e.type == EventType::Limit    ? lat_limit
                       : e.type == EventType::Market ? lat_market
                                                     : lat_cancel;
        bucket.push_back(dt.count());
    }
    const auto wall = std::chrono::duration<double>(Clock::now() - wall_start).count();

    std::printf("events: %zu  traded qty: %llu  resting orders: %zu  wall: %.3f s\n\n",
                events.size(), static_cast<unsigned long long>(traded), book.order_count(),
                wall);
    std::printf("%-8s %10s %8s %8s %8s %8s %10s\n", "op", "count", "p50", "p90", "p99", "p99.9",
                "max (ns)");
    report("limit", lat_limit);
    report("market", lat_market);
    report("cancel", lat_cancel);
    return 0;
}
