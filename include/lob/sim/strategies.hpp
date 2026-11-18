#pragma once

// Quoting strategies and the online estimators they rely on.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

#include "lob/types.hpp"

namespace lob::sim {

/// What a strategy sees on every book update.
struct MarketView {
    std::uint64_t ts_ns;
    Price best_bid;  // ticks
    Price best_ask;
    Qty bid_qty;  // lots at the touch
    Qty ask_qty;
    std::int64_t inventory_lots;  // signed
    double sigma2;                // mid-price variance rate, USD^2 per second
    double k;                     // order-arrival decay, 1 / USD
};

struct Quote {
    std::optional<Price> bid;
    std::optional<Price> ask;
};

/// EWMA estimate of the mid-price variance rate (USD^2 / s), time-weighted
/// so that irregular update intervals are handled correctly.
class VolatilityEstimator {
public:
    explicit VolatilityEstimator(double halflife_s = 60.0)
        : decay_s_(halflife_s / std::log(2.0)) {}

    void update(std::uint64_t ts_ns, double mid) {
        if (has_prev_) {
            const double dt = static_cast<double>(ts_ns - prev_ts_) * 1e-9;
            if (dt > 0.0) {
                const double d = mid - prev_mid_;
                const double alpha = 1.0 - std::exp(-dt / decay_s_);
                var_rate_ += alpha * (d * d / dt - var_rate_);
            }
        }
        prev_ts_ = ts_ns;
        prev_mid_ = mid;
        has_prev_ = true;
    }

    [[nodiscard]] double variance_rate() const noexcept { return var_rate_; }

private:
    double decay_s_;
    double var_rate_ = 0.0;
    std::uint64_t prev_ts_ = 0;
    double prev_mid_ = 0.0;
    bool has_prev_ = false;
};

/// Estimates k in the Avellaneda-Stoikov fill intensity A * exp(-k * delta)
/// from the distance between trade prints and the mid (exponential MLE:
/// k = 1 / mean distance, smoothed with an EWMA).
class ArrivalEstimator {
public:
    explicit ArrivalEstimator(double alpha = 0.01, double min_distance = 0.005)
        : alpha_(alpha), min_distance_(min_distance), mean_(min_distance) {}

    void update(double trade_price, double mid) {
        const double d = std::max(std::abs(trade_price - mid), min_distance_);
        mean_ += alpha_ * (d - mean_);
    }

    [[nodiscard]] double k() const noexcept { return 1.0 / mean_; }

private:
    double alpha_;
    double min_distance_;
    double mean_;
};

/// Avellaneda & Stoikov (2008), "High-frequency trading in a limit order book",
/// with a rolling horizon tau (no terminal time):
///   reservation price  r = s - q * gamma * sigma^2 * tau
///   total spread       d = gamma * sigma^2 * tau + (2 / gamma) * ln(1 + gamma / k)
/// q is the inventory in units of the order size. s is the mid or, optionally,
/// the size-weighted microprice, which leans quotes toward the book imbalance.
struct AvellanedaStoikovParams {
    double gamma = 1e-4;  // risk aversion (1/USD)
    double tau_s = 60.0;  // inventory horizon (seconds)
    bool use_microprice = false;
};

class AvellanedaStoikov {
public:
    AvellanedaStoikov(AvellanedaStoikovParams p, double tick, Qty order_lots)
        : p_(p), tick_(tick), order_lots_(static_cast<double>(order_lots)) {}

    [[nodiscard]] Quote quote(const MarketView& m) const {
        const double bid = static_cast<double>(m.best_bid) * tick_;
        const double ask = static_cast<double>(m.best_ask) * tick_;
        double s = 0.5 * (bid + ask);
        if (p_.use_microprice && m.bid_qty + m.ask_qty > 0) {
            const auto bq = static_cast<double>(m.bid_qty);
            const auto aq = static_cast<double>(m.ask_qty);
            s = (bid * aq + ask * bq) / (bq + aq);
        }
        const double q = static_cast<double>(m.inventory_lots) / order_lots_;
        const double risk = p_.gamma * m.sigma2 * p_.tau_s;
        const double r = s - q * risk;
        const double spread = risk + (2.0 / p_.gamma) * std::log1p(p_.gamma / m.k);

        auto b = static_cast<Price>(std::floor((r - 0.5 * spread) / tick_));
        auto a = static_cast<Price>(std::ceil((r + 0.5 * spread) / tick_));
        // Post-only: never cross the current market.
        b = std::min(b, m.best_ask - 1);
        a = std::max(a, m.best_bid + 1);
        return {b, a};
    }

private:
    AvellanedaStoikovParams p_;
    double tick_;
    double order_lots_;
};

/// Baseline: always join the best bid and best ask, no inventory skew.
class JoinTouch {
public:
    [[nodiscard]] Quote quote(const MarketView& m) const { return {m.best_bid, m.best_ask}; }
};

/// Join the touch, but step away from the side whose queue is about to be
/// depleted. Queue imbalance I = bid_qty / (bid_qty + ask_qty) is a classic
/// short-term predictor of the next mid move on large-tick assets: a thin bid
/// (I < threshold) tends to be consumed next, so being filled there is
/// usually followed by an adverse price move.
class ImbalanceTouch {
public:
    explicit ImbalanceTouch(double threshold = 0.25) : threshold_(threshold) {}

    [[nodiscard]] Quote quote(const MarketView& m) const {
        const double total = static_cast<double>(m.bid_qty + m.ask_qty);
        const double imb = total > 0 ? static_cast<double>(m.bid_qty) / total : 0.5;
        Quote q{m.best_bid, m.best_ask};
        if (imb < threshold_) q.bid.reset();
        if (imb > 1.0 - threshold_) q.ask.reset();
        return q;
    }

private:
    double threshold_;
};

}  // namespace lob::sim
