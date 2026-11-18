#pragma once

// Simulated execution of the strategy's own orders against replayed market data.
//
// Our orders are NOT inserted into the market book (they never existed on the
// real exchange). Their fills are inferred from the real trade prints with a
// queue-position model:
//
//  * Latency: a new order becomes live `latency_ns` after it is sent; a cancel
//    takes effect `latency_ns` after it is sent (the order can still fill
//    in between).
//  * Post-only: an order that would cross the market on arrival is rejected.
//  * Queue position: on arrival the order joins behind the quantity already
//    resting at its price (queue_ahead). Liquidity added later queues behind it.
//  * Trades at our price consume queue_ahead first, then fill us.
//  * Trades through our price (e.g. a sell printing below our bid) would have
//    hit us first: we fill up to the trade size.
//  * Non-trade decreases of the level are assumed to come from behind us
//    first (cancels of the newest orders, matching L2BookBuilder's FIFO
//    model), so queue_ahead only shrinks when the level shrinks below it:
//    queue_ahead = min(queue_ahead, level quantity). This is conservative.
//
// Known simplifications: no market impact (our fills do not remove real
// liquidity), and order state is only re-evaluated on market events.

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

#include "lob/order_book.hpp"
#include "lob/replay/replayer.hpp"

namespace lob::sim {

using Timestamp = std::uint64_t;  // nanoseconds
using replay::MarketTrade;

inline constexpr Timestamp kNever = std::numeric_limits<Timestamp>::max();

struct SimConfig {
    Timestamp latency_ns = 20'000'000;  // one-way order entry / cancel latency
};

struct Fill {
    Timestamp ts;
    OrderId id;
    Side side;
    Price price;
    Qty qty;
};

enum class OrderState : std::uint8_t { Pending, Live, Done };

struct SimOrder {
    OrderId id;
    Side side;
    Price price;
    Qty qty;
    Qty filled = 0;
    Qty queue_ahead = 0;
    Timestamp active_at = 0;
    Timestamp cancel_at = kNever;
    OrderState state = OrderState::Pending;

    [[nodiscard]] Qty remaining() const noexcept { return qty - filled; }
    [[nodiscard]] bool cancel_requested() const noexcept { return cancel_at != kNever; }
};

class SimExchange {
public:
    struct Stats {
        std::uint64_t placed = 0;
        std::uint64_t cancelled = 0;
        std::uint64_t rejected_post_only = 0;
        std::uint64_t fills = 0;
        std::uint64_t fills_through = 0;  // price traded through our level
        std::uint64_t fills_queue = 0;    // reached the front of the queue at our price
    };

    explicit SimExchange(const OrderBook& market, SimConfig cfg = {})
        : market_(market), cfg_(cfg) {}

    /// Sends a post-only limit order. Returns its id.
    OrderId place(Timestamp now, Side side, Price price, Qty qty) {
        const OrderId id = next_id_++;
        SimOrder o{id, side, price, qty};
        o.active_at = now + cfg_.latency_ns;
        orders_.push_back(o);
        ++stats_.placed;
        return id;
    }

    /// Sends a cancel; it takes effect after the configured latency.
    void cancel(Timestamp now, OrderId id) {
        for (SimOrder& o : orders_) {
            if (o.id == id && o.state != OrderState::Done && !o.cancel_requested()) {
                o.cancel_at = now + cfg_.latency_ns;
            }
        }
    }

    /// Call after every market book update.
    void on_book(Timestamp now) {
        advance(now);
        for (SimOrder& o : orders_) {
            if (o.state == OrderState::Live) {
                o.queue_ahead = std::min(o.queue_ahead, market_.volume_at(o.side, o.price));
            }
        }
    }

    /// Call for every market trade. `on_fill(const Fill&)` must not call
    /// place() or cancel().
    template <class OnFill>
    void on_trade(const MarketTrade& t, OnFill&& on_fill) {
        advance(t.ts_ns);
        const Side our_side = opposite(t.aggressor);

        eligible_.clear();
        for (SimOrder& o : orders_) {
            if (o.state != OrderState::Live || o.side != our_side) continue;
            const bool reached = our_side == Side::Buy ? t.price <= o.price : t.price >= o.price;
            if (reached) eligible_.push_back(&o);
        }
        // Best price first, then time priority.
        std::sort(eligible_.begin(), eligible_.end(), [](const SimOrder* a, const SimOrder* b) {
            if (a->price != b->price) {
                return a->side == Side::Buy ? a->price > b->price : a->price < b->price;
            }
            return a->id < b->id;
        });

        Qty available = t.qty;
        for (SimOrder* o : eligible_) {
            if (available == 0) break;
            const bool through = our_side == Side::Buy ? t.price < o->price : t.price > o->price;
            if (!through) {
                const Qty consumed = std::min(o->queue_ahead, available);
                o->queue_ahead -= consumed;
                available -= consumed;
            }
            const Qty x = std::min(o->remaining(), available);
            if (x == 0) continue;
            o->filled += x;
            available -= x;
            ++stats_.fills;
            ++(through ? stats_.fills_through : stats_.fills_queue);
            if (o->remaining() == 0) o->state = OrderState::Done;
            on_fill(Fill{t.ts_ns, o->id, o->side, o->price, x});
        }
        prune();
    }

    /// Returns the order if it is still pending or live.
    [[nodiscard]] const SimOrder* find(OrderId id) const {
        for (const SimOrder& o : orders_) {
            if (o.id == id && o.state != OrderState::Done) return &o;
        }
        return nullptr;
    }

    [[nodiscard]] const std::vector<SimOrder>& orders() const noexcept { return orders_; }
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    void advance(Timestamp now) {
        for (SimOrder& o : orders_) {
            if (o.state == OrderState::Pending && o.active_at <= now) {
                if (crosses(o)) {
                    o.state = OrderState::Done;
                    ++stats_.rejected_post_only;
                } else {
                    o.state = OrderState::Live;
                    o.queue_ahead = market_.volume_at(o.side, o.price);
                }
            }
            if (o.state != OrderState::Done && o.cancel_at <= now) {
                o.state = OrderState::Done;
                ++stats_.cancelled;
            }
        }
        prune();
    }

    [[nodiscard]] bool crosses(const SimOrder& o) const {
        if (o.side == Side::Buy) {
            const auto ask = market_.best_ask();
            return ask && o.price >= *ask;
        }
        const auto bid = market_.best_bid();
        return bid && o.price <= *bid;
    }

    void prune() {
        std::erase_if(orders_, [](const SimOrder& o) { return o.state == OrderState::Done; });
    }

    const OrderBook& market_;
    SimConfig cfg_;
    std::vector<SimOrder> orders_;
    std::vector<SimOrder*> eligible_;
    OrderId next_id_ = 1;
    Stats stats_;
};

}  // namespace lob::sim
