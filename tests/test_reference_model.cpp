// Differential test: the optimised OrderBook must produce exactly the same
// trades and book state as a deliberately naive reference implementation
// across hundreds of thousands of random operations.

#include <gtest/gtest.h>

#include <deque>
#include <functional>
#include <map>
#include <random>
#include <unordered_map>
#include <vector>

#include "lob/order_book.hpp"

using namespace lob;

namespace {

class ReferenceBook {
public:
    std::vector<Trade> trades;

    bool add_limit(OrderId id, Side side, Price price, Qty qty) {
        if (qty == 0 || where_.contains(id)) return false;
        Qty left = side == Side::Buy
                       ? match(asks_, id, side, qty, [&](Price p) { return p <= price; })
                       : match(bids_, id, side, qty, [&](Price p) { return p >= price; });
        if (left > 0) {
            if (side == Side::Buy) {
                bids_[price].push_back({id, left});
            } else {
                asks_[price].push_back({id, left});
            }
            where_[id] = {side, price};
        }
        return true;
    }

    void add_market(OrderId id, Side side, Qty qty) {
        if (side == Side::Buy) {
            match(asks_, id, side, qty, [](Price) { return true; });
        } else {
            match(bids_, id, side, qty, [](Price) { return true; });
        }
    }

    bool cancel(OrderId id) {
        auto it = where_.find(id);
        if (it == where_.end()) return false;
        auto [side, price] = it->second;
        where_.erase(it);
        if (side == Side::Buy) {
            erase_from(bids_, price, id);
        } else {
            erase_from(asks_, price, id);
        }
        return true;
    }

    void reduce(OrderId id, Qty by) {
        auto it = where_.find(id);
        if (it == where_.end() || by == 0) return;
        auto [side, price] = it->second;
        auto& queue = side == Side::Buy ? bids_[price] : asks_[price];
        for (auto& o : queue) {
            if (o.id != id) continue;
            if (by >= o.qty) {
                cancel(id);
            } else {
                o.qty -= by;
            }
            return;
        }
    }

    std::vector<BookLevel> depth(Side side) const {
        std::vector<BookLevel> out;
        auto collect = [&](const auto& m) {
            for (const auto& [price, queue] : m) {
                Qty total = 0;
                for (const auto& o : queue) total += o.qty;
                out.push_back({price, total, static_cast<std::uint32_t>(queue.size())});
            }
        };
        if (side == Side::Buy) {
            collect(bids_);
        } else {
            collect(asks_);
        }
        return out;
    }

private:
    struct O {
        OrderId id;
        Qty qty;
    };

    template <class Map, class Crosses>
    Qty match(Map& book, OrderId taker, Side side, Qty qty, Crosses crosses) {
        while (qty > 0 && !book.empty() && crosses(book.begin()->first)) {
            auto& [price, queue] = *book.begin();
            while (qty > 0 && !queue.empty()) {
                O& maker = queue.front();
                const Qty fill = std::min(qty, maker.qty);
                trades.push_back({maker.id, taker, price, fill, side});
                qty -= fill;
                maker.qty -= fill;
                if (maker.qty == 0) {
                    where_.erase(maker.id);
                    queue.pop_front();
                }
            }
            if (queue.empty()) book.erase(book.begin());
        }
        return qty;
    }

    template <class Map>
    static void erase_from(Map& book, Price price, OrderId id) {
        auto& queue = book[price];
        for (auto it = queue.begin(); it != queue.end(); ++it) {
            if (it->id == id) {
                queue.erase(it);
                break;
            }
        }
        if (queue.empty()) book.erase(price);
    }

    std::map<Price, std::deque<O>, std::greater<>> bids_;  // best (highest) first
    std::map<Price, std::deque<O>> asks_;                  // best (lowest) first
    std::unordered_map<OrderId, std::pair<Side, Price>> where_;
};

void run_differential(std::uint64_t seed, int ops) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> op_dist(0, 99);
    std::uniform_int_distribution<Price> price_dist(980, 1020);  // narrow -> lots of crossing
    std::uniform_int_distribution<Qty> qty_dist(1, 100);
    std::bernoulli_distribution side_dist(0.5);

    OrderBook book;
    ReferenceBook ref;
    std::vector<Trade> trades;
    auto sink = [&](const Trade& t) { trades.push_back(t); };
    OrderId next_id = 1;

    for (int i = 0; i < ops; ++i) {
        const int op = op_dist(rng);
        const Side side = side_dist(rng) ? Side::Buy : Side::Sell;

        if (op < 55) {
            const OrderId id = next_id++;
            const Price px = price_dist(rng);
            const Qty q = qty_dist(rng);
            book.add_limit(id, side, px, q, sink);
            ref.add_limit(id, side, px, q);
        } else if (op < 65) {
            const OrderId id = next_id++;
            const Qty q = qty_dist(rng);
            book.add_market(id, side, q, sink);
            ref.add_market(id, side, q);
        } else if (op < 90) {
            // Includes ids that are unknown, filled or already cancelled.
            const OrderId id = std::uniform_int_distribution<OrderId>(1, next_id)(rng);
            ASSERT_EQ(book.cancel(id), ref.cancel(id)) << "op " << i;
        } else {
            const OrderId id = std::uniform_int_distribution<OrderId>(1, next_id)(rng);
            const Qty by = qty_dist(rng);
            book.reduce(id, by);
            ref.reduce(id, by);
        }

        ASSERT_EQ(trades, ref.trades) << "trade mismatch at op " << i;
        if (i % 64 == 0 || i == ops - 1) {
            ASSERT_EQ(book.depth(Side::Buy, SIZE_MAX), ref.depth(Side::Buy)) << "op " << i;
            ASSERT_EQ(book.depth(Side::Sell, SIZE_MAX), ref.depth(Side::Sell)) << "op " << i;
        }
        if (book.best_bid() && book.best_ask()) {
            ASSERT_LT(*book.best_bid(), *book.best_ask()) << "crossed book at op " << i;
        }
    }
}

}  // namespace

TEST(ReferenceModel, MatchesNaiveImplementation) {
    for (std::uint64_t seed : {1ULL, 7ULL, 42ULL, 1337ULL}) {
        SCOPED_TRACE(seed);
        run_differential(seed, 50'000);
    }
}
