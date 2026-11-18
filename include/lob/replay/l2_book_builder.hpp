#pragma once

// Rebuilds an order-by-order OrderBook from aggregated (L2) price-level updates.
//
// Exchanges like Binance publish absolute quantities per price level, not
// individual orders. Each level is represented in the OrderBook as a FIFO of
// synthetic "chunk" orders:
//   * an increase appends a new chunk at the back of the queue (new liquidity
//     joins behind existing liquidity, as it would on the exchange);
//   * a decrease removes quantity from the newest chunks first.
// This keeps a meaningful notion of queue position for the market-making
// simulator built on top of it.
//
// Updates are staged and committed per exchange event: decreases first, then
// increases. Applying a consistent event in that order never produces a
// crossed intermediate state, so the matching engine never trades on replayed
// liquidity. If the feed itself would cross (stale levels), the stale opposite
// levels are removed and counted in stats().

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

#include "lob/order_book.hpp"

namespace lob::replay {

class L2BookBuilder {
public:
    struct Stats {
        std::uint64_t level_updates = 0;
        std::uint64_t events = 0;
        std::uint64_t crossed_levels_removed = 0;
    };

    explicit L2BookBuilder(OrderBook& book) : book_(book) {}

    /// Removes all replayed liquidity from the book.
    void reset() {
        for (auto* side : {&bids_, &asks_}) {
            for (auto& [price, level] : *side) {
                for (const Chunk& c : level.chunks) book_.cancel(c.id);
            }
            side->clear();
        }
        staged_.clear();
    }

    /// Stages an absolute level quantity (0 removes the level).
    void stage(Side side, Price price, Qty qty) { staged_.push_back({side, price, qty}); }

    /// Applies all staged updates as one atomic exchange event.
    void commit() {
        for (const Update& u : staged_) {
            if (u.qty < current(u.side, u.price)) apply(u.side, u.price, u.qty);
        }
        // Insert worst prices first: the book keeps the best level at the end
        // of each side's vector, so new levels are then appended instead of
        // shifting the whole side (a 5000-level snapshot listed best-first
        // would otherwise cost O(n^2) memmove).
        std::sort(staged_.begin(), staged_.end(), [](const Update& a, const Update& b) {
            if (a.side != b.side) return a.side < b.side;
            return a.side == Side::Buy ? a.price < b.price : a.price > b.price;
        });
        for (const Update& u : staged_) {
            if (u.qty > current(u.side, u.price)) apply(u.side, u.price, u.qty);
        }
        stats_.level_updates += staged_.size();
        ++stats_.events;
        staged_.clear();
    }

    [[nodiscard]] Qty current(Side side, Price price) const {
        const auto& levels = side == Side::Buy ? bids_ : asks_;
        const auto it = levels.find(price);
        return it == levels.end() ? 0 : it->second.total;
    }

    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    struct Chunk {
        OrderId id;
        Qty qty;
    };
    struct Level {
        std::vector<Chunk> chunks;  // front = oldest
        Qty total = 0;
    };
    struct Update {
        Side side;
        Price price;
        Qty qty;
    };

    void apply(Side side, Price price, Qty target) {
        auto& levels = side == Side::Buy ? bids_ : asks_;
        Level& level = levels[price];

        if (target < level.total) {
            Qty excess = level.total - target;
            while (excess > 0) {
                Chunk& newest = level.chunks.back();
                if (newest.qty <= excess) {
                    book_.cancel(newest.id);
                    excess -= newest.qty;
                    level.total -= newest.qty;
                    level.chunks.pop_back();
                } else {
                    book_.reduce(newest.id, excess);
                    newest.qty -= excess;
                    level.total -= excess;
                    excess = 0;
                }
            }
            if (level.chunks.empty()) levels.erase(price);
            return;
        }

        uncross(side, price);
        const Qty add = target - level.total;
        const OrderId id = next_id_++;
        [[maybe_unused]] const ExecResult r = book_.add_limit(id, side, price, add);
        assert(r.filled == 0 && r.rested == add);
        level.chunks.push_back({id, add});
        level.total = target;
    }

    // Drops opposite-side levels that a new level at `price` would cross.
    void uncross(Side side, Price price) {
        if (side == Side::Buy) {
            while (auto ask = book_.best_ask()) {
                if (*ask > price) break;
                apply(Side::Sell, *ask, 0);
                ++stats_.crossed_levels_removed;
            }
        } else {
            while (auto bid = book_.best_bid()) {
                if (*bid < price) break;
                apply(Side::Buy, *bid, 0);
                ++stats_.crossed_levels_removed;
            }
        }
    }

    OrderBook& book_;
    std::unordered_map<Price, Level> bids_;
    std::unordered_map<Price, Level> asks_;
    std::vector<Update> staged_;
    // Synthetic ids live in their own range so they never collide with
    // strategy order ids.
    OrderId next_id_ = OrderId{1} << 62;
    Stats stats_;
};

}  // namespace lob::replay
