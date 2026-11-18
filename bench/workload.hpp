#pragma once

// Synthetic but market-like order flow: a random-walk mid price, limit orders
// clustered near the top of book, frequent cancels and occasional aggressive
// orders. Generated up front so that benchmarks only time the book itself.

#include <cstdint>
#include <random>
#include <vector>

#include "lob/types.hpp"

namespace lob::bench {

enum class EventType : std::uint8_t { Limit, Market, Cancel };

struct Event {
    EventType type;
    Side side;
    OrderId id;
    Price price;
    Qty qty;
};

struct WorkloadConfig {
    std::size_t events = 1'000'000;
    double limit_ratio = 0.55;
    double cancel_ratio = 0.40;  // remainder are market orders
    double marketable_ratio = 0.05;
    std::uint64_t seed = 42;
};

inline std::vector<Event> make_workload(const WorkloadConfig& cfg = {}) {
    std::mt19937_64 rng(cfg.seed);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    std::geometric_distribution<int> depth_offset(0.25);  // most orders near the touch
    std::uniform_int_distribution<Qty> qty(1, 100);
    std::bernoulli_distribution coin(0.5);

    std::vector<Event> out;
    out.reserve(cfg.events);
    std::vector<OrderId> live;  // ids that may still be resting
    live.reserve(cfg.events);

    Price mid = 100'000;
    OrderId next_id = 1;

    for (std::size_t i = 0; i < cfg.events; ++i) {
        if (i % 64 == 0) mid += coin(rng) ? 1 : -1;
        const double r = u(rng);
        const Side side = coin(rng) ? Side::Buy : Side::Sell;

        if (r < cfg.limit_ratio || live.empty()) {
            const Price off = 1 + depth_offset(rng);
            const bool aggressive = u(rng) < cfg.marketable_ratio;
            Price px = side == Side::Buy ? mid - off : mid + off;
            if (aggressive) px = side == Side::Buy ? mid + 2 : mid - 2;
            out.push_back({EventType::Limit, side, next_id, px, qty(rng)});
            live.push_back(next_id++);
        } else if (r < cfg.limit_ratio + cfg.cancel_ratio) {
            std::uniform_int_distribution<std::size_t> pick(0, live.size() - 1);
            const std::size_t k = pick(rng);
            out.push_back({EventType::Cancel, side, live[k], 0, 0});
            live[k] = live.back();
            live.pop_back();
        } else {
            out.push_back({EventType::Market, side, next_id++, 0, qty(rng)});
        }
    }
    return out;
}

}  // namespace lob::bench
