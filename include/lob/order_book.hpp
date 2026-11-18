#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <unordered_map>
#include <vector>

#include "lob/types.hpp"

namespace lob {

/// Single-instrument limit order book with price-time priority matching.
///
/// Design (see README for the rationale):
///  - Orders live in a contiguous pool and are recycled through a free list,
///    so steady-state operation performs no heap allocation.
///  - Each price level is an intrusive doubly linked FIFO of pool indices
///    (O(1) append, O(1) cancel from anywhere in the queue).
///  - Levels are kept in a sorted std::vector per side, with the best price at
///    back(). Almost all activity happens near the top of book, so inserts and
///    erases touch the end of the vector and move very little memory.
///
/// Not thread-safe: one book is meant to be owned by one thread.
/// Trade callbacks must not call back into the same book.
class OrderBook {
public:
    explicit OrderBook(std::size_t expected_orders = 1U << 16) {
        pool_.reserve(expected_orders);
        free_.reserve(expected_orders);
        index_.reserve(expected_orders);
        bids_.reserve(1024);
        asks_.reserve(1024);
    }

    /// Limit order: matches against the opposite side up to `price`, then rests
    /// any remainder. `on_trade(const Trade&)` is invoked once per fill.
    template <class OnTrade>
    ExecResult add_limit(OrderId id, Side side, Price price, Qty qty, OnTrade&& on_trade) {
        if (qty == 0) return {Status::InvalidQty, 0, 0};
        if (index_.contains(id)) return {Status::DuplicateId, 0, 0};

        const Qty remaining = match(id, side, price, qty, on_trade);
        ExecResult result{Status::Ok, qty - remaining, 0};
        if (remaining > 0) {
            rest(id, side, price, remaining);
            result.rested = remaining;
        }
        return result;
    }

    ExecResult add_limit(OrderId id, Side side, Price price, Qty qty) {
        return add_limit(id, side, price, qty, [](const Trade&) {});
    }

    /// Market order with immediate-or-cancel semantics: any unfilled quantity
    /// is discarded, never rested.
    template <class OnTrade>
    ExecResult add_market(OrderId id, Side side, Qty qty, OnTrade&& on_trade) {
        if (qty == 0) return {Status::InvalidQty, 0, 0};
        if (index_.contains(id)) return {Status::DuplicateId, 0, 0};

        const Price limit = side == Side::Buy ? std::numeric_limits<Price>::max()
                                              : std::numeric_limits<Price>::min();
        const Qty remaining = match(id, side, limit, qty, on_trade);
        return {Status::Ok, qty - remaining, 0};
    }

    ExecResult add_market(OrderId id, Side side, Qty qty) {
        return add_market(id, side, qty, [](const Trade&) {});
    }

    /// Removes a resting order. Returns false if the id is unknown
    /// (never seen, already filled, or already cancelled).
    bool cancel(OrderId id) {
        const auto it = index_.find(id);
        if (it == index_.end()) return false;
        const std::uint32_t idx = it->second;
        index_.erase(it);

        const Order& o = pool_[idx];
        Levels& lv = levels(o.side);
        const auto lvl = find_level(lv, o.side, o.price);
        assert(lvl != lv.end() && lvl->price == o.price);
        unlink(*lvl, idx);
        if (lvl->head == kNil) lv.erase(lvl);
        release(idx);
        return true;
    }

    /// Reduces a resting order's quantity in place, keeping its time priority
    /// (exchanges typically only preserve priority on size reductions).
    /// Reducing by the full remaining quantity or more cancels the order.
    Status reduce(OrderId id, Qty by) {
        if (by == 0) return Status::InvalidQty;
        const auto it = index_.find(id);
        if (it == index_.end()) return Status::UnknownId;

        Order& o = pool_[it->second];
        if (by >= o.qty) {
            cancel(id);
            return Status::Ok;
        }
        Levels& lv = levels(o.side);
        const auto lvl = find_level(lv, o.side, o.price);
        assert(lvl != lv.end() && lvl->price == o.price);
        o.qty -= by;
        lvl->qty -= by;
        return Status::Ok;
    }

    // ---- Queries -----------------------------------------------------------

    [[nodiscard]] std::optional<Price> best_bid() const noexcept {
        if (bids_.empty()) return std::nullopt;
        return bids_.back().price;
    }

    [[nodiscard]] std::optional<Price> best_ask() const noexcept {
        if (asks_.empty()) return std::nullopt;
        return asks_.back().price;
    }

    [[nodiscard]] std::optional<Price> spread() const noexcept {
        if (bids_.empty() || asks_.empty()) return std::nullopt;
        return asks_.back().price - bids_.back().price;
    }

    [[nodiscard]] Qty volume_at(Side side, Price price) const {
        const Levels& lv = levels(side);
        const auto it = find_level(lv, side, price);
        return (it != lv.end() && it->price == price) ? it->qty : 0;
    }

    /// Top `n` levels of one side, best price first.
    [[nodiscard]] std::vector<BookLevel> depth(Side side, std::size_t n) const {
        const Levels& lv = levels(side);
        std::vector<BookLevel> out;
        out.reserve(std::min(n, lv.size()));
        for (auto it = lv.rbegin(); it != lv.rend() && out.size() < n; ++it) {
            out.push_back({it->price, it->qty, it->count});
        }
        return out;
    }

    [[nodiscard]] std::size_t order_count() const noexcept { return index_.size(); }
    [[nodiscard]] std::size_t level_count(Side side) const noexcept {
        return levels(side).size();
    }
    [[nodiscard]] bool contains(OrderId id) const { return index_.contains(id); }

private:
    static constexpr std::uint32_t kNil = std::numeric_limits<std::uint32_t>::max();

    struct Order {
        OrderId id;
        Price price;
        Qty qty;
        std::uint32_t prev;
        std::uint32_t next;
        Side side;
    };

    struct Level {
        Price price;
        Qty qty;  // aggregate resting quantity
        std::uint32_t head;
        std::uint32_t tail;
        std::uint32_t count;
    };

    // bids_: ascending prices, asks_: descending prices -> best is always back().
    using Levels = std::vector<Level>;

    Levels& levels(Side s) noexcept { return s == Side::Buy ? bids_ : asks_; }
    const Levels& levels(Side s) const noexcept { return s == Side::Buy ? bids_ : asks_; }

    template <class Vec>
    static auto find_level(Vec& lv, Side side, Price price) -> decltype(lv.begin()) {
        if (side == Side::Buy) {
            return std::lower_bound(lv.begin(), lv.end(), price,
                                    [](const Level& l, Price p) { return l.price < p; });
        }
        return std::lower_bound(lv.begin(), lv.end(), price,
                                [](const Level& l, Price p) { return l.price > p; });
    }

    template <class OnTrade>
    Qty match(OrderId taker, Side side, Price limit, Qty qty, OnTrade& on_trade) {
        Levels& opp = levels(opposite(side));
        while (qty > 0 && !opp.empty()) {
            Level& lvl = opp.back();
            const bool crosses = side == Side::Buy ? lvl.price <= limit : lvl.price >= limit;
            if (!crosses) break;

            while (qty > 0 && lvl.head != kNil) {
                const std::uint32_t idx = lvl.head;
                Order& maker = pool_[idx];
                const Qty fill = std::min(qty, maker.qty);
                on_trade(Trade{maker.id, taker, lvl.price, fill, side});
                qty -= fill;
                maker.qty -= fill;
                lvl.qty -= fill;
                if (maker.qty == 0) {
                    lvl.head = maker.next;
                    if (lvl.head != kNil) {
                        pool_[lvl.head].prev = kNil;
                    } else {
                        lvl.tail = kNil;
                    }
                    --lvl.count;
                    index_.erase(maker.id);
                    release(idx);
                }
            }
            if (lvl.head == kNil) opp.pop_back();
        }
        return qty;
    }

    void rest(OrderId id, Side side, Price price, Qty qty) {
        const std::uint32_t idx = acquire();
        Levels& lv = levels(side);
        auto it = find_level(lv, side, price);
        if (it == lv.end() || it->price != price) {
            it = lv.insert(it, Level{price, 0, kNil, kNil, 0});
        }
        Level& lvl = *it;
        pool_[idx] = Order{id, price, qty, lvl.tail, kNil, side};
        if (lvl.tail != kNil) {
            pool_[lvl.tail].next = idx;
        } else {
            lvl.head = idx;
        }
        lvl.tail = idx;
        lvl.qty += qty;
        ++lvl.count;
        index_.emplace(id, idx);
    }

    void unlink(Level& lvl, std::uint32_t idx) noexcept {
        const Order& o = pool_[idx];
        if (o.prev != kNil) {
            pool_[o.prev].next = o.next;
        } else {
            lvl.head = o.next;
        }
        if (o.next != kNil) {
            pool_[o.next].prev = o.prev;
        } else {
            lvl.tail = o.prev;
        }
        lvl.qty -= o.qty;
        --lvl.count;
    }

    std::uint32_t acquire() {
        if (!free_.empty()) {
            const std::uint32_t idx = free_.back();
            free_.pop_back();
            return idx;
        }
        pool_.emplace_back();
        return static_cast<std::uint32_t>(pool_.size() - 1);
    }

    void release(std::uint32_t idx) { free_.push_back(idx); }

    std::vector<Order> pool_;
    std::vector<std::uint32_t> free_;
    std::unordered_map<OrderId, std::uint32_t> index_;
    Levels bids_;
    Levels asks_;
};

}  // namespace lob
