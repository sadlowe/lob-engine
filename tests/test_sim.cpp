#include <gtest/gtest.h>

#include <vector>

#include "lob/order_book.hpp"
#include "lob/sim/backtest.hpp"
#include "lob/sim/sim_exchange.hpp"
#include "lob/sim/strategies.hpp"

using namespace lob;
using namespace lob::sim;

namespace {

constexpr Timestamp kLat = 10;  // ns, tiny on purpose

struct Env {
    OrderBook market;
    SimExchange ex{market, SimConfig{kLat}};
    std::vector<Fill> fills;

    Env() {
        market.add_limit(1, Side::Buy, 100, 50);   // 50 lots resting at the bid
        market.add_limit(2, Side::Sell, 101, 50);
    }
    void trade(Timestamp ts, Side aggressor, Price px, Qty qty) {
        ex.on_trade({ts, aggressor, px, qty}, [&](const Fill& f) { fills.push_back(f); });
    }
    Qty filled() const {
        Qty q = 0;
        for (const auto& f : fills) q += f.qty;
        return q;
    }
};

}  // namespace

TEST(SimExchange, OrderIsNotLiveBeforeLatency) {
    Env e;
    e.ex.place(0, Side::Buy, 101 - 1, 10);
    e.trade(kLat - 1, Side::Sell, 99, 100);  // trades through, but we are not live yet
    EXPECT_EQ(e.filled(), 0U);
    e.trade(kLat, Side::Sell, 99, 100);
    EXPECT_EQ(e.filled(), 10U);
}

TEST(SimExchange, JoinsBehindExistingQueue) {
    Env e;
    const OrderId id = e.ex.place(0, Side::Buy, 100, 10);
    e.ex.on_book(kLat);
    ASSERT_NE(e.ex.find(id), nullptr);
    EXPECT_EQ(e.ex.find(id)->queue_ahead, 50U);

    e.trade(20, Side::Sell, 100, 30);  // consumes queue ahead only
    EXPECT_EQ(e.filled(), 0U);
    EXPECT_EQ(e.ex.find(id)->queue_ahead, 20U);

    e.trade(21, Side::Sell, 100, 25);  // 20 ahead, then 5 for us
    EXPECT_EQ(e.filled(), 5U);
    e.trade(22, Side::Sell, 100, 100);
    EXPECT_EQ(e.filled(), 10U);
    EXPECT_EQ(e.ex.find(id), nullptr);  // fully filled
}

TEST(SimExchange, TradeThroughFillsImmediately) {
    Env e;
    e.ex.place(0, Side::Buy, 100, 10);
    e.ex.on_book(kLat);
    e.trade(20, Side::Sell, 99, 4);  // printed below our bid: we would have been hit first
    EXPECT_EQ(e.filled(), 4U);
    ASSERT_EQ(e.fills.size(), 1U);
    EXPECT_EQ(e.fills[0].price, 100);  // maker fills at its own limit price
}

TEST(SimExchange, WrongSideOrWorsePriceDoesNotFill) {
    Env e;
    e.ex.place(0, Side::Buy, 100, 10);
    e.ex.on_book(kLat);
    e.trade(20, Side::Buy, 101, 100);   // buyer aggressor never fills our bid
    e.trade(21, Side::Sell, 101, 100);  // sell print above our bid
    EXPECT_EQ(e.filled(), 0U);
}

TEST(SimExchange, LevelShrinkMovesUsUpOnlyWhenBelowOurQueue) {
    Env e;
    const OrderId id = e.ex.place(0, Side::Buy, 100, 10);
    e.ex.on_book(kLat);
    e.market.reduce(1, 10);  // level 50 -> 40: cancels assumed behind us first
    e.ex.on_book(20);
    EXPECT_EQ(e.ex.find(id)->queue_ahead, 40U);
    e.market.reduce(1, 35);  // level -> 5
    e.ex.on_book(30);
    EXPECT_EQ(e.ex.find(id)->queue_ahead, 5U);
}

TEST(SimExchange, CancelHasLatencyAndOrderCanFillMeanwhile) {
    Env e;
    const OrderId id = e.ex.place(0, Side::Buy, 100, 10);
    e.ex.on_book(kLat);
    e.ex.cancel(20, id);
    e.trade(25, Side::Sell, 99, 3);  // cancel still in flight
    EXPECT_EQ(e.filled(), 3U);
    e.ex.on_book(20 + kLat);
    EXPECT_EQ(e.ex.find(id), nullptr);
    e.trade(40, Side::Sell, 99, 100);
    EXPECT_EQ(e.filled(), 3U);
    EXPECT_EQ(e.ex.stats().cancelled, 1U);
}

TEST(SimExchange, PostOnlyRejectsCrossingOrders) {
    Env e;
    e.ex.place(0, Side::Buy, 101, 10);  // would take the ask
    e.ex.place(0, Side::Sell, 100, 10);
    e.ex.on_book(kLat);
    EXPECT_TRUE(e.ex.orders().empty());
    EXPECT_EQ(e.ex.stats().rejected_post_only, 2U);
}

TEST(SimExchange, TradeQuantityIsSharedAcrossOurOrders) {
    Env e;
    e.ex.place(0, Side::Sell, 102, 10);
    e.ex.place(0, Side::Sell, 103, 10);
    e.ex.on_book(kLat);
    e.trade(20, Side::Buy, 104, 15);  // sweeps through both
    ASSERT_EQ(e.fills.size(), 2U);
    EXPECT_EQ(e.fills[0].price, 102);  // better price first
    EXPECT_EQ(e.fills[0].qty, 10U);
    EXPECT_EQ(e.fills[1].qty, 5U);
}

TEST(AvellanedaStoikov, InventorySkewsQuotes) {
    const double tick = 0.01;
    AvellanedaStoikov as({1e-3, 60.0, false}, tick, 100);
    MarketView m{0, 1'000'000, 1'000'001, 10, 10, 0, /*sigma2=*/1.0, /*k=*/200.0};
    const Quote flat = as.quote(m);
    m.inventory_lots = 500;  // long 5 units -> want to sell
    const Quote longq = as.quote(m);
    m.inventory_lots = -500;
    const Quote shortq = as.quote(m);

    ASSERT_TRUE(flat.bid && flat.ask && longq.bid && longq.ask && shortq.bid && shortq.ask);
    EXPECT_LT(*longq.bid, *flat.bid);
    EXPECT_LT(*longq.ask, *flat.ask);
    EXPECT_GT(*shortq.bid, *flat.bid);
    EXPECT_GT(*shortq.ask, *flat.ask);
    // Post-only clamps hold even with large skew.
    EXPECT_LT(*shortq.bid, m.best_ask);
    EXPECT_GT(*longq.ask, m.best_bid);
}

TEST(AvellanedaStoikov, SpreadWidensWithVolatility) {
    AvellanedaStoikov as({1e-3, 60.0, false}, 0.01, 100);
    MarketView m{0, 1'000'000, 1'000'001, 10, 10, 0, 1.0, 200.0};
    const Quote calm = as.quote(m);
    m.sigma2 = 25.0;
    const Quote wild = as.quote(m);
    EXPECT_GT(*wild.ask - *wild.bid, *calm.ask - *calm.bid);
}

TEST(AvellanedaStoikov, MicropriceLeansTowardImbalance) {
    AvellanedaStoikov mid({1e-5, 60.0, false}, 0.01, 100);
    AvellanedaStoikov micro({1e-5, 60.0, true}, 0.01, 100);
    // Heavy bid, thin ask: fair value is close to the ask.
    MarketView m{0, 1'000'000, 1'000'010, 1'000, 10, 0, 0.0, 200.0};
    EXPECT_GT(*micro.quote(m).bid, *mid.quote(m).bid);
}

TEST(ImbalanceTouch, StepsAwayFromThinSide) {
    ImbalanceTouch s(0.25);
    MarketView m{0, 100, 101, 10, 90, 0, 0.0, 200.0};  // thin bid
    Quote q = s.quote(m);
    EXPECT_FALSE(q.bid);
    EXPECT_EQ(q.ask, 101);
    m.bid_qty = 90;
    m.ask_qty = 10;  // thin ask
    q = s.quote(m);
    EXPECT_EQ(q.bid, 100);
    EXPECT_FALSE(q.ask);
    m.ask_qty = 90;  // balanced
    q = s.quote(m);
    EXPECT_TRUE(q.bid && q.ask);
}

TEST(Estimators, VolatilityTracksVarianceRate) {
    VolatilityEstimator v(1.0);
    double mid = 100.0;
    // Alternating +/-0.1 every 0.1 s -> variance rate 0.01 / 0.1 = 0.1 USD^2/s.
    for (int i = 1; i <= 2000; ++i) {
        mid += (i % 2 ? 0.1 : -0.1);
        v.update(static_cast<std::uint64_t>(i) * 100'000'000ULL, mid);
    }
    EXPECT_NEAR(v.variance_rate(), 0.1, 1e-6);
}

TEST(Estimators, ArrivalIsInverseMeanDistance) {
    ArrivalEstimator a(0.05, 0.001);
    for (int i = 0; i < 2000; ++i) a.update(100.02, 100.0);
    EXPECT_NEAR(a.k(), 50.0, 1e-3);
}

TEST(Backtest, AccountingAndMarkouts) {
    OrderBook market;
    market.add_limit(1, Side::Buy, 100, 50);
    market.add_limit(2, Side::Sell, 102, 50);

    BacktestConfig cfg;
    cfg.tick = 1.0;
    cfg.lot = 1.0;
    cfg.order_lots = 10;
    cfg.max_inventory_lots = 100;
    cfg.maker_fee_bps = 10.0;
    cfg.warmup_s = 0.0;
    cfg.sim.latency_ns = 0;
    cfg.markout_s = {1.0, 2.0, 3.0};
    Backtest<JoinTouch> bt(market, JoinTouch{}, cfg);

    const std::uint64_t s = 1'000'000'000ULL;
    bt.on_book(1 * s, market);  // quotes 100 / 102, mid 101
    bt.on_book(1 * s + 1, market);
    bt.on_trade({1 * s + 2, Side::Sell, 99, 10});  // our bid fills fully at 100

    // Market rallies by 4: our long 10 is worth +40 more.
    market.cancel(1);
    market.cancel(2);
    market.add_limit(3, Side::Buy, 104, 50);
    market.add_limit(4, Side::Sell, 106, 50);
    bt.on_book(5 * s, market);

    const BacktestResult r = bt.finish();
    EXPECT_EQ(r.fills, 1U);
    EXPECT_DOUBLE_EQ(r.spread_capture, 10.0);  // bought 10 @100 with mid 101
    EXPECT_DOUBLE_EQ(r.fees, 10.0 * 100.0 * 10.0 * 1e-4);
    EXPECT_DOUBLE_EQ(r.total_pnl, 10.0 * (105.0 - 100.0) - r.fees);
    EXPECT_DOUBLE_EQ(r.inventory_pnl, 40.0);
    EXPECT_DOUBLE_EQ(r.final_inventory, 10.0);
    EXPECT_DOUBLE_EQ(r.markout_ticks[0], 5.0);  // mid 105 vs fill 100
}
