// Market-making backtest on a .lob recording.
//
//   lob_mm_backtest <file.lob> [options]
//     --strategy as|touch|imbalance
//                           Avellaneda-Stoikov (default), join-the-touch baseline, or
//                           touch with a queue-imbalance filter
//     --imbalance T         imbalance threshold (default 0.25)
//     --gamma G             AS risk aversion (default 1e-4)
//     --tau S               AS inventory horizon in seconds (default 60)
//     --microprice          AS: use the size-weighted microprice instead of the mid
//     --latency-ms L        order entry / cancel latency (default 20)
//     --fee-bps F           maker fee in basis points, negative = rebate (default 0)
//     --size Q              order size in base units (default 0.001)
//     --max-inv Q           absolute inventory limit in base units (default 0.01)
//     --trade-guard         pull a quote when a trade prints through the known touch
//     --out PREFIX          write PREFIX_samples.csv and PREFIX_fills.csv
//     --sweep               run the comparison grid used in the README

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "lob/order_book.hpp"
#include "lob/replay/record.hpp"
#include "lob/replay/replayer.hpp"
#include "lob/sim/backtest.hpp"

using namespace lob;
using namespace lob::sim;

namespace {

struct Options {
    std::string path;
    std::string strategy = "as";
    AvellanedaStoikovParams as;
    double imbalance = 0.25;
    double latency_ms = 20.0;
    double fee_bps = 0.0;
    double size = 0.001;
    double max_inv = 0.01;
    std::string out;
    bool sweep = false;
    bool trade_guard = false;
};

template <class Strategy>
BacktestResult run(const Options& opt, Strategy strategy, const BacktestConfig& cfg,
                   const std::string& out = {}) {
    replay::RecordReader reader(opt.path);
    OrderBook book(1U << 20);
    Backtest<Strategy> bt(book, std::move(strategy), cfg);
    replay::replay(reader, book, bt);

    if (!out.empty()) {
        if (std::FILE* f = std::fopen((out + "_samples.csv").c_str(), "w")) {
            std::fprintf(f, "ts_ns,mid,inventory,pnl,bid,ask\n");
            for (const Sample& s : bt.samples()) {
                std::fprintf(f, "%llu,%.3f,%.5f,%.4f,", static_cast<unsigned long long>(s.ts_ns),
                             s.mid, s.inventory, s.pnl);
                if (s.bid) std::fprintf(f, "%.2f", static_cast<double>(*s.bid) * cfg.tick);
                std::fprintf(f, ",");
                if (s.ask) std::fprintf(f, "%.2f", static_cast<double>(*s.ask) * cfg.tick);
                std::fprintf(f, "\n");
            }
            std::fclose(f);
        }
        if (std::FILE* f = std::fopen((out + "_fills.csv").c_str(), "w")) {
            std::fprintf(f, "ts_ns,side,price,qty,mid,markout_1s,markout_5s,markout_30s\n");
            const auto& fills = bt.fills();
            for (std::size_t i = 0; i < fills.size(); ++i) {
                const FillRecord& r = fills[i];
                std::fprintf(f, "%llu,%s,%.2f,%.5f,%.3f", static_cast<unsigned long long>(r.ts_ns),
                             r.side == Side::Buy ? "buy" : "sell",
                             static_cast<double>(r.price) * cfg.tick,
                             static_cast<double>(r.qty) * cfg.lot, r.mid);
                for (std::size_t h = 0; h < kMarkouts; ++h) {
                    // Unresolved markouts (end of the recording) are left empty.
                    if (i < bt.markouts_resolved(h)) {
                        std::fprintf(f, ",%.3f", r.markout_ticks[h]);
                    } else {
                        std::fprintf(f, ",");
                    }
                }
                std::fprintf(f, "\n");
            }
            std::fclose(f);
        }
    }
    return bt.finish();
}

BacktestResult run_named(const Options& opt, const BacktestConfig& cfg, const std::string& out) {
    if (opt.strategy == "touch") return run(opt, JoinTouch{}, cfg, out);
    if (opt.strategy == "imbalance") return run(opt, ImbalanceTouch{opt.imbalance}, cfg, out);
    return run(opt, AvellanedaStoikov(opt.as, cfg.tick, cfg.order_lots), cfg, out);
}

BacktestConfig make_config(const Options& opt, const replay::FileHeader& h) {
    BacktestConfig cfg;
    cfg.tick = h.tick_size;
    cfg.lot = h.lot_size;
    cfg.sim.latency_ns = static_cast<std::uint64_t>(opt.latency_ms * 1e6);
    cfg.maker_fee_bps = opt.fee_bps;
    cfg.trade_guard = opt.trade_guard;
    cfg.order_lots = static_cast<Qty>(std::llround(opt.size / h.lot_size));
    cfg.max_inventory_lots = std::llround(opt.max_inv / h.lot_size);
    return cfg;
}

void print_header() {
    std::printf("%-26s %6s %6s %8s %10s %10s %9s %10s %8s %7s %7s %7s %8s %9s\n", "strategy", "fills",
                "thru", "volume", "spread$", "invent$", "fees$", "pnl$", "maxDD$", "mk1s", "mk5s", "mk30s",
                "|inv|", "be_fee_bp");
}

void print_row(const std::string& name, const BacktestResult& r) {
    const double breakeven_bp = r.notional > 0 ? (r.total_pnl + r.fees) / r.notional * 1e4 : 0.0;
    const double through_pct = r.fills ? 100.0 * static_cast<double>(r.orders.fills_through) /
                                             static_cast<double>(r.orders.fills)
                                       : 0.0;
    std::printf("%-26s %6llu %5.0f%% %8.4f %10.2f %10.2f %9.2f %10.2f %8.2f %7.2f %7.2f %7.2f %8.4f %9.3f\n",
                name.c_str(), static_cast<unsigned long long>(r.fills), through_pct, r.volume,
                r.spread_capture, r.inventory_pnl, r.fees, r.total_pnl, r.max_drawdown,
                r.markout_ticks[0], r.markout_ticks[1], r.markout_ticks[2], r.mean_abs_inventory,
                breakeven_bp);
}

void sweep(Options opt, const replay::FileHeader& h) {
    std::printf("Columns: spread$ = edge vs mid at fill, invent$ = mid moves on inventory\n"
                "(adverse selection + risk), mkNs = average markout in ticks N seconds after\n"
                "each fill (+ = favourable), thru = share of fills where price traded through\n"
                "our level, |inv| = mean absolute inventory, be_fee_bp =\n"
                "maker fee at which net PnL is zero. Latency %.0f ms, size %g, limit %g.\n\n",
                opt.latency_ms, opt.size, opt.max_inv);
    print_header();

    opt.strategy = "touch";
    print_row("join touch (baseline)", run_named(opt, make_config(opt, h), {}));
    opt.strategy = "imbalance";
    for (const double th : {0.1, 0.25, 0.4}) {
        opt.imbalance = th;
        char name[64];
        std::snprintf(name, sizeof name, "touch + imbalance %.2f", th);
        print_row(name, run_named(opt, make_config(opt, h), {}));
    }

    opt.strategy = "as";
    for (const bool micro : {false, true}) {
        for (const double g : {1e-5, 1e-4, 1e-3}) {
            opt.as.gamma = g;
            opt.as.use_microprice = micro;
            char name[64];
            std::snprintf(name, sizeof name, "AS g=%.0e%s", g, micro ? " +microprice" : "");
            print_row(name, run_named(opt, make_config(opt, h), {}));
        }
    }

    std::printf("\nWith trade guard (pull quotes when trades print through the known touch):\n");
    print_header();
    opt.trade_guard = true;
    opt.strategy = "touch";
    print_row("join touch + guard", run_named(opt, make_config(opt, h), {}));
    opt.strategy = "as";
    for (const double g : {1e-5, 1e-4, 1e-3}) {
        opt.as.gamma = g;
        opt.as.use_microprice = true;
        char name[64];
        std::snprintf(name, sizeof name, "AS g=%.0e +micro +guard", g);
        print_row(name, run_named(opt, make_config(opt, h), {}));
    }

    std::printf("\nLatency sensitivity (AS g=1e-4 +microprice +guard):\n");
    print_header();
    opt.as.gamma = 1e-4;
    opt.as.use_microprice = true;
    for (const double lat : {1.0, 20.0, 100.0}) {
        opt.latency_ms = lat;
        char name[64];
        std::snprintf(name, sizeof name, "latency %.0f ms", lat);
        print_row(name, run_named(opt, make_config(opt, h), {}));
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <file.lob> [options]   (see source header)\n", argv[0]);
        return 2;
    }
    Options opt;
    opt.path = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&]() -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--strategy") opt.strategy = val();
        else if (a == "--gamma") opt.as.gamma = std::atof(val());
        else if (a == "--imbalance") opt.imbalance = std::atof(val());
        else if (a == "--tau") opt.as.tau_s = std::atof(val());
        else if (a == "--microprice") opt.as.use_microprice = true;
        else if (a == "--latency-ms") opt.latency_ms = std::atof(val());
        else if (a == "--fee-bps") opt.fee_bps = std::atof(val());
        else if (a == "--size") opt.size = std::atof(val());
        else if (a == "--max-inv") opt.max_inv = std::atof(val());
        else if (a == "--out") opt.out = val();
        else if (a == "--sweep") opt.sweep = true;
        else if (a == "--trade-guard") opt.trade_guard = true;
        else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return 2;
        }
    }

    const replay::FileHeader header = replay::RecordReader(opt.path).header();
    if (opt.sweep) {
        sweep(opt, header);
        return 0;
    }

    const BacktestConfig cfg = make_config(opt, header);
    const BacktestResult r = run_named(opt, cfg, opt.out);

    std::printf("duration          %.1f min\n", r.duration_s / 60.0);
    std::printf("orders            %llu placed, %llu cancelled, %llu post-only rejects\n",
                static_cast<unsigned long long>(r.orders.placed),
                static_cast<unsigned long long>(r.orders.cancelled),
                static_cast<unsigned long long>(r.orders.rejected_post_only));
    std::printf("fills             %llu  (volume %.5f, notional $%.0f; %llu at front of queue, %llu traded through)\n",
                static_cast<unsigned long long>(r.fills), r.volume, r.notional,
                static_cast<unsigned long long>(r.orders.fills_queue),
                static_cast<unsigned long long>(r.orders.fills_through));
    std::printf("PnL               $%.2f  = spread capture %.2f + inventory %.2f - fees %.2f\n",
                r.total_pnl, r.spread_capture, r.inventory_pnl, r.fees);
    std::printf("max drawdown      $%.2f\n", r.max_drawdown);
    std::printf("Sharpe (1-min, annualised, indicative only) %.2f\n", r.sharpe_1min_annualised);
    std::printf("inventory         mean |q| %.5f, max |q| %.5f, final %.5f\n",
                r.mean_abs_inventory, r.max_abs_inventory, r.final_inventory);
    std::printf("markouts (ticks)  1s %.2f   5s %.2f   30s %.2f\n", r.markout_ticks[0],
                r.markout_ticks[1], r.markout_ticks[2]);
    if (!opt.out.empty()) std::printf("wrote %s_samples.csv and %s_fills.csv\n", opt.out.c_str(), opt.out.c_str());
    return 0;
}
