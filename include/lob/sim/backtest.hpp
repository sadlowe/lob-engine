#pragma once

// Event-driven market-making backtest: replayed market + simulated execution
// + strategy + accounting (PnL decomposition, markouts, risk metrics).

#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

#include "lob/order_book.hpp"
#include "lob/replay/replayer.hpp"
#include "lob/sim/sim_exchange.hpp"
#include "lob/sim/strategies.hpp"

namespace lob::sim {

inline constexpr std::size_t kMarkouts = 3;

struct BacktestConfig {
    SimConfig sim;
    double tick = 0.01;
    double lot = 0.00001;
    Qty order_lots = 100;                     // 0.001 BTC
    std::int64_t max_inventory_lots = 1'000;  // 0.01 BTC
    double maker_fee_bps = 0.0;               // negative = rebate
    // Pull our quote on a side as soon as a trade prints through the last
    // known touch on that side: the 100 ms depth feed is stale at that point.
    bool trade_guard = false;
    double warmup_s = 60.0;                   // estimators only, no quoting
    double vol_halflife_s = 60.0;
    double sample_every_s = 1.0;
    std::array<double, kMarkouts> markout_s = {1.0, 5.0, 30.0};
};

struct FillRecord {
    std::uint64_t ts_ns;
    Side side;
    Price price;
    Qty qty;
    double mid;  // at fill time
    std::array<double, kMarkouts> markout_ticks{};  // signed: + = price moved in our favour
};

struct Sample {
    std::uint64_t ts_ns;
    double mid;
    double inventory;  // base asset
    double pnl;        // USD, marked to mid, net of fees
    std::optional<Price> bid;
    std::optional<Price> ask;
};

struct BacktestResult {
    double duration_s = 0;
    std::uint64_t fills = 0;
    double volume = 0;         // base asset
    double notional = 0;       // USD
    double total_pnl = 0;      // USD, net
    double spread_capture = 0; // sum of edge vs mid at fill
    double inventory_pnl = 0;  // mid moves on held inventory (incl. adverse selection)
    double fees = 0;
    double max_drawdown = 0;
    double sharpe_1min_annualised = 0;
    double mean_abs_inventory = 0;
    double max_abs_inventory = 0;
    double final_inventory = 0;
    std::array<double, kMarkouts> markout_ticks{};  // qty-weighted average
    SimExchange::Stats orders;
};

template <class Strategy>
class Backtest {
public:
    Backtest(const OrderBook& market, Strategy strategy, BacktestConfig cfg)
        : exchange_(market, cfg.sim),
          strategy_(std::move(strategy)),
          cfg_(cfg),
          vol_(cfg.vol_halflife_s) {}

    void on_reset(std::uint64_t /*ts*/) {}

    void on_book(std::uint64_t ts, const OrderBook& book) {
        exchange_.on_book(ts);
        const auto bb = book.best_bid();
        const auto ba = book.best_ask();
        if (!bb || !ba) return;

        if (start_ts_ == 0) {
            start_ts_ = ts;
            next_sample_ = ts;
        }
        last_ts_ = ts;
        best_bid_ = *bb;
        best_ask_ = *ba;
        mid_ = 0.5 * static_cast<double>(*bb + *ba) * cfg_.tick;
        vol_.update(ts, mid_);
        resolve_markouts(ts);

        if (static_cast<double>(ts - start_ts_) * 1e-9 >= cfg_.warmup_s) {
            MarketView view{ts,
                            *bb,
                            *ba,
                            book.volume_at(Side::Buy, *bb),
                            book.volume_at(Side::Sell, *ba),
                            inventory_,
                            vol_.variance_rate(),
                            arrivals_.k()};
            Quote q = strategy_.quote(view);
            if (inventory_ + static_cast<std::int64_t>(cfg_.order_lots) > cfg_.max_inventory_lots) {
                q.bid.reset();
            }
            if (inventory_ - static_cast<std::int64_t>(cfg_.order_lots) < -cfg_.max_inventory_lots) {
                q.ask.reset();
            }
            manage(Side::Buy, q.bid, ts);
            manage(Side::Sell, q.ask, ts);
        }

        if (ts >= next_sample_) {
            samples_.push_back({ts, mid_, static_cast<double>(inventory_) * cfg_.lot, pnl(),
                                working_price(bid_id_), working_price(ask_id_)});
            next_sample_ = ts + static_cast<std::uint64_t>(cfg_.sample_every_s * 1e9);
        }
    }

    void on_trade(const MarketTrade& t) {
        if (mid_ > 0) arrivals_.update(static_cast<double>(t.price) * cfg_.tick, mid_);
        exchange_.on_trade(t, [this](const Fill& f) { handle_fill(f); });
        if (cfg_.trade_guard && mid_ > 0) {
            if (t.aggressor == Side::Sell && t.price < best_bid_) manage(Side::Buy, std::nullopt, t.ts_ns);
            if (t.aggressor == Side::Buy && t.price > best_ask_) manage(Side::Sell, std::nullopt, t.ts_ns);
        }
    }

    [[nodiscard]] BacktestResult finish() const {
        BacktestResult r;
        r.duration_s = static_cast<double>(last_ts_ - start_ts_) * 1e-9;
        r.fills = fills_.size();
        r.volume = volume_;
        r.notional = notional_;
        r.total_pnl = pnl();
        r.spread_capture = spread_capture_;
        r.fees = fees_;
        r.inventory_pnl = r.total_pnl - spread_capture_ + fees_;
        r.final_inventory = static_cast<double>(inventory_) * cfg_.lot;
        r.orders = exchange_.stats();

        double peak = 0, sum_abs = 0;
        for (const Sample& s : samples_) {
            peak = std::max(peak, s.pnl);
            r.max_drawdown = std::max(r.max_drawdown, peak - s.pnl);
            sum_abs += std::abs(s.inventory);
            r.max_abs_inventory = std::max(r.max_abs_inventory, std::abs(s.inventory));
        }
        if (!samples_.empty()) r.mean_abs_inventory = sum_abs / static_cast<double>(samples_.size());

        // Sharpe of 1-minute PnL changes, annualised (crypto trades 24/7).
        std::vector<double> rets;
        const std::size_t step = static_cast<std::size_t>(60.0 / cfg_.sample_every_s);
        for (std::size_t i = step; i < samples_.size(); i += step) {
            rets.push_back(samples_[i].pnl - samples_[i - step].pnl);
        }
        if (rets.size() > 1) {
            double mean = 0, var = 0;
            for (double x : rets) mean += x;
            mean /= static_cast<double>(rets.size());
            for (double x : rets) var += (x - mean) * (x - mean);
            var /= static_cast<double>(rets.size() - 1);
            if (var > 0) r.sharpe_1min_annualised = mean / std::sqrt(var) * std::sqrt(525'600.0);
        }

        for (std::size_t h = 0; h < kMarkouts; ++h) {
            double w = 0, acc = 0;
            for (std::size_t i = 0; i < markout_next_[h]; ++i) {
                const auto q = static_cast<double>(fills_[i].qty);
                acc += fills_[i].markout_ticks[h] * q;
                w += q;
            }
            r.markout_ticks[h] = w > 0 ? acc / w : 0.0;
        }
        return r;
    }

    [[nodiscard]] const std::vector<FillRecord>& fills() const noexcept { return fills_; }
    [[nodiscard]] const std::vector<Sample>& samples() const noexcept { return samples_; }
    /// Number of fills whose markout at horizon h is known (the last fills of
    /// a run may not have reached the horizon yet).
    [[nodiscard]] std::size_t markouts_resolved(std::size_t h) const { return markout_next_[h]; }

private:
    [[nodiscard]] double pnl() const {
        return cash_ + static_cast<double>(inventory_) * cfg_.lot * mid_ - fees_;
    }

    [[nodiscard]] std::optional<Price> working_price(OrderId id) const {
        if (const SimOrder* o = id ? exchange_.find(id) : nullptr; o && !o->cancel_requested()) {
            return o->price;
        }
        return std::nullopt;
    }

    // Keeps at most one working order per side. An unchanged price is left
    // alone so the order keeps its queue position.
    void manage(Side side, std::optional<Price> want, std::uint64_t ts) {
        OrderId& id = side == Side::Buy ? bid_id_ : ask_id_;
        const SimOrder* o = id ? exchange_.find(id) : nullptr;
        if (o && (o->cancel_requested() || !want || *want != o->price)) {
            exchange_.cancel(ts, id);
            o = nullptr;
        }
        if (!o) {
            id = 0;
            if (want) id = exchange_.place(ts, side, *want, cfg_.order_lots);
        }
    }

    void handle_fill(const Fill& f) {
        const double px = static_cast<double>(f.price) * cfg_.tick;
        const double qty = static_cast<double>(f.qty) * cfg_.lot;
        const double sign = f.side == Side::Buy ? 1.0 : -1.0;
        inventory_ += f.side == Side::Buy ? static_cast<std::int64_t>(f.qty)
                                          : -static_cast<std::int64_t>(f.qty);
        cash_ -= sign * px * qty;
        fees_ += cfg_.maker_fee_bps * 1e-4 * px * qty;
        spread_capture_ += sign * (mid_ - px) * qty;
        volume_ += qty;
        notional_ += px * qty;
        fills_.push_back({f.ts, f.side, f.price, f.qty, mid_, {}});
    }

    void resolve_markouts(std::uint64_t ts) {
        for (std::size_t h = 0; h < kMarkouts; ++h) {
            const auto horizon = static_cast<std::uint64_t>(cfg_.markout_s[h] * 1e9);
            std::size_t& i = markout_next_[h];
            while (i < fills_.size() && fills_[i].ts_ns + horizon <= ts) {
                FillRecord& f = fills_[i];
                const double sign = f.side == Side::Buy ? 1.0 : -1.0;
                f.markout_ticks[h] = sign * (mid_ - static_cast<double>(f.price) * cfg_.tick) / cfg_.tick;
                ++i;
            }
        }
    }

    SimExchange exchange_;
    Strategy strategy_;
    BacktestConfig cfg_;
    VolatilityEstimator vol_;
    ArrivalEstimator arrivals_;

    OrderId bid_id_ = 0;
    OrderId ask_id_ = 0;
    std::int64_t inventory_ = 0;
    double cash_ = 0, fees_ = 0, spread_capture_ = 0, volume_ = 0, notional_ = 0;
    double mid_ = 0;
    Price best_bid_ = 0, best_ask_ = 0;
    std::uint64_t start_ts_ = 0, last_ts_ = 0, next_sample_ = 0;
    std::vector<FillRecord> fills_;
    std::vector<Sample> samples_;
    std::array<std::size_t, kMarkouts> markout_next_{};
};

}  // namespace lob::sim
