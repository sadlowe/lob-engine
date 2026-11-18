// Replays a .lob market-data recording through the matching engine and
// validates the reconstructed book against exchange checkpoint snapshots.
//
//   lob_replay <file.lob> [--bbo out.csv] [--top N]
//
// Exit code is non-zero if any checkpoint does not match.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "lob/order_book.hpp"
#include "lob/replay/l2_book_builder.hpp"
#include "lob/replay/record.hpp"

using namespace lob;
using namespace lob::replay;

namespace {

struct Checkpoint {
    std::uint64_t update_id = 0;
    Price bid_bound = 0;
    Price ask_bound = 0;
    std::set<Price> excluded[2];
    std::vector<std::pair<Price, Qty>> levels[2];

    void clear() { *this = Checkpoint{}; }
};

struct CheckResult {
    bool ok = true;
    std::size_t compared = 0;
    std::string detail;
};

CheckResult validate(const OrderBook& book, const Checkpoint& cp, double tick, double lot) {
    CheckResult res;
    for (const Side side : {Side::Buy, Side::Sell}) {
        const int s = side == Side::Buy ? 0 : 1;
        auto keep = [&](Price p) {
            const bool in_range = side == Side::Buy ? p >= cp.bid_bound : p <= cp.ask_bound;
            return in_range && !cp.excluded[s].contains(p);
        };

        std::vector<std::pair<Price, Qty>> expected;
        for (const auto& lv : cp.levels[s]) {
            if (keep(lv.first)) expected.push_back(lv);
        }
        std::vector<std::pair<Price, Qty>> actual;
        for (const BookLevel& lv : book.depth(side, SIZE_MAX)) {
            if (keep(lv.price)) actual.emplace_back(lv.price, lv.qty);
        }
        auto best_first = [&](const auto& a, const auto& b) {
            return side == Side::Buy ? a.first > b.first : a.first < b.first;
        };
        std::sort(expected.begin(), expected.end(), best_first);
        std::sort(actual.begin(), actual.end(), best_first);

        res.compared += expected.size();
        if (expected != actual && res.ok) {
            res.ok = false;
            std::size_t i = 0;
            while (i < expected.size() && i < actual.size() && expected[i] == actual[i]) ++i;
            char buf[256];
            auto show = [&](const std::vector<std::pair<Price, Qty>>& v) {
                if (i >= v.size()) return std::string("<none>");
                char b[96];
                std::snprintf(b, sizeof b, "%.2f x %.5f", static_cast<double>(v[i].first) * tick,
                              static_cast<double>(v[i].second) * lot);
                return std::string(b);
            };
            std::snprintf(buf, sizeof buf, "%s level #%zu: exchange %s, rebuilt %s",
                          side == Side::Buy ? "bid" : "ask", i, show(expected).c_str(),
                          show(actual).c_str());
            res.detail = buf;
        }
    }
    return res;
}

void print_book(const OrderBook& book, std::size_t n, double tick, double lot) {
    const auto asks = book.depth(Side::Sell, n);
    const auto bids = book.depth(Side::Buy, n);
    std::printf("\n%14s %14s\n", "price", "qty");
    for (auto it = asks.rbegin(); it != asks.rend(); ++it) {
        std::printf("ASK %10.2f %14.5f\n", static_cast<double>(it->price) * tick,
                    static_cast<double>(it->qty) * lot);
    }
    std::printf("    -------------------------\n");
    for (const auto& l : bids) {
        std::printf("BID %10.2f %14.5f\n", static_cast<double>(l.price) * tick,
                    static_cast<double>(l.qty) * lot);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <file.lob> [--bbo out.csv] [--top N]\n", argv[0]);
        return 2;
    }
    const char* bbo_path = nullptr;
    std::size_t top = 5;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--bbo") == 0 && i + 1 < argc) {
            bbo_path = argv[++i];
        } else if (std::strcmp(argv[i], "--top") == 0 && i + 1 < argc) {
            top = static_cast<std::size_t>(std::strtoul(argv[++i], nullptr, 10));
        }
    }

    RecordReader reader(argv[1]);
    const double tick = reader.header().tick_size;
    const double lot = reader.header().lot_size;

    std::FILE* bbo = nullptr;
    if (bbo_path) {
        bbo = std::fopen(bbo_path, "w");
        if (!bbo) {
            std::fprintf(stderr, "cannot create %s\n", bbo_path);
            return 2;
        }
        std::fprintf(bbo, "ts_ns,bid,bid_qty,ask,ask_qty\n");
    }

    OrderBook book(1U << 20);
    L2BookBuilder builder(book);
    Checkpoint cp;
    std::uint64_t records = 0, trades = 0, resets = 0, traded_lots = 0;
    std::uint64_t first_ts = 0, last_ts = 0;
    std::size_t cp_ok = 0, cp_total = 0, cp_levels = 0;
    double validation_secs = 0.0;

    const auto t0 = std::chrono::steady_clock::now();
    Record r{};
    while (reader.next(r)) {
        ++records;
        if (first_ts == 0) first_ts = r.ts_ns;
        last_ts = std::max(last_ts, r.ts_ns);

        switch (r.type) {
            case RecordType::Reset:
                builder.reset();
                ++resets;
                break;
            case RecordType::Level:
                builder.stage(r.book_side(), r.price, r.qty);
                break;
            case RecordType::EventEnd:
                builder.commit();
                if (bbo) {
                    const auto b = book.depth(Side::Buy, 1);
                    const auto a = book.depth(Side::Sell, 1);
                    if (!b.empty() && !a.empty()) {
                        std::fprintf(bbo, "%llu,%.2f,%.5f,%.2f,%.5f\n",
                                     static_cast<unsigned long long>(r.ts_ns),
                                     static_cast<double>(b[0].price) * tick,
                                     static_cast<double>(b[0].qty) * lot,
                                     static_cast<double>(a[0].price) * tick,
                                     static_cast<double>(a[0].qty) * lot);
                    }
                }
                break;
            case RecordType::Trade:
                ++trades;
                traded_lots += r.qty;
                break;
            case RecordType::CheckpointBound:
                (r.side == 0 ? cp.bid_bound : cp.ask_bound) = r.price;
                cp.update_id = r.seq;
                break;
            case RecordType::CheckpointExclude:
                cp.excluded[r.side].insert(r.price);
                break;
            case RecordType::CheckpointLevel:
                cp.levels[r.side].emplace_back(r.price, r.qty);
                break;
            case RecordType::CheckpointEnd: {
                const auto v0 = std::chrono::steady_clock::now();
                const CheckResult res = validate(book, cp, tick, lot);
                validation_secs +=
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - v0).count();
                ++cp_total;
                cp_levels += res.compared;
                if (res.ok) {
                    ++cp_ok;
                } else {
                    std::fprintf(stderr, "checkpoint %llu MISMATCH: %s\n",
                                 static_cast<unsigned long long>(cp.update_id),
                                 res.detail.c_str());
                }
                cp.clear();
                break;
            }
        }
    }
    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (bbo) std::fclose(bbo);

    const auto& st = builder.stats();
    std::printf("%s  tick %g  lot %g  |  %.1f min of market data\n", reader.symbol().c_str(),
                tick, lot, static_cast<double>(last_ts - first_ts) / 60e9);
    std::printf("records        %llu\n", static_cast<unsigned long long>(records));
    std::printf("depth events   %llu  (%llu level updates)\n",
                static_cast<unsigned long long>(st.events),
                static_cast<unsigned long long>(st.level_updates));
    std::printf("trades         %llu  (%.5f %s)\n", static_cast<unsigned long long>(trades),
                static_cast<double>(traded_lots) * lot, reader.symbol().c_str());
    std::printf("resets         %llu\n", static_cast<unsigned long long>(resets));
    std::printf("crossed fixes  %llu\n",
                static_cast<unsigned long long>(st.crossed_levels_removed));
    const double replay_secs = secs - validation_secs;
    std::printf("replay         %.2f ms  (%.1fM records/s, %.0f ns per depth event)\n",
                replay_secs * 1e3, static_cast<double>(records) / replay_secs / 1e6,
                replay_secs * 1e9 / static_cast<double>(std::max<std::uint64_t>(st.events, 1)));
    std::printf("validation     %.2f ms\n", validation_secs * 1e3);
    std::printf("checkpoints    %zu/%zu matched  (%zu levels compared)\n", cp_ok, cp_total,
                cp_levels);
    if (top > 0) print_book(book, top, tick, lot);

    return cp_ok == cp_total ? 0 : 1;
}
