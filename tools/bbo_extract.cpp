// Streams a Binance bookTicker CSV (one row per change of the best bid / ask)
// and writes the two compact series the queue-imbalance study needs:
//
//   unzip -p BTCUSDT-bookTicker-2024-03-04.zip | lob_bbo_extract --out build/research/BTCUSDT-2024-03-04
//
//   PREFIX_grid.bin     book state sampled on a fixed clock (default every 1000 ms):
//                       int64 ts_ms, int64 bid, int64 ask, float64 bid_qty, float64 ask_qty, int64 age_ms
//   PREFIX_changes.bin  every change of the best bid or best ask price:
//                       int64 ts_ms, int64 bid, int64 ask
//   PREFIX_meta.json    row counts, inferred tick size, data-quality counters
//
// Prices are exact integers in units of 1e-8 (the archive prints 8 decimals),
// so no floating-point rounding enters the tick arithmetic. The tick size is
// given with --tick (rows quoting off that grid are counted and dropped), or
// inferred as the GCD of all quoted prices. A grid sample at time g holds the
// state after every row with transaction_time <= g; age_ms is the time since
// the last row, to spot stale samples.

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <string>
#include <string_view>

namespace {

struct State {
    std::int64_t bid = 0, ask = 0;
    double bq = 0, aq = 0;
    std::int64_t ts = 0;
};

// "0.62710000" -> 62710000 (units of 1e-8). Returns false on malformed input.
bool parse_e8(std::string_view s, std::int64_t& out) {
    std::int64_t whole = 0, frac = 0;
    const std::size_t dot = s.find('.');
    const std::string_view w = s.substr(0, dot);
    if (std::from_chars(w.data(), w.data() + w.size(), whole).ec != std::errc{}) return false;
    int digits = 0;
    if (dot != std::string_view::npos) {
        for (const char c : s.substr(dot + 1)) {
            if (c < '0' || c > '9' || digits == 8) return false;
            frac = frac * 10 + (c - '0');
            ++digits;
        }
    }
    for (; digits < 8; ++digits) frac *= 10;
    out = whole * 100'000'000 + frac;
    return true;
}

bool parse_double(std::string_view s, double& out) {
    return std::from_chars(s.data(), s.data() + s.size(), out).ec == std::errc{};
}

bool parse_int(std::string_view s, std::int64_t& out) {
    return std::from_chars(s.data(), s.data() + s.size(), out).ec == std::errc{};
}

class LineReader {
public:
    explicit LineReader(std::FILE* f) : f_(f), buf_(1 << 22, '\0') {}

    bool next(std::string_view& line) {
        for (;;) {
            if (const char* nl = static_cast<const char*>(std::memchr(buf_.data() + pos_, '\n', end_ - pos_))) {
                const std::size_t len = static_cast<std::size_t>(nl - (buf_.data() + pos_));
                line = std::string_view(buf_.data() + pos_, len);
                pos_ += len + 1;
                if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
                return true;
            }
            // Move the partial line to the front and refill.
            std::memmove(buf_.data(), buf_.data() + pos_, end_ - pos_);
            end_ -= pos_;
            pos_ = 0;
            if (eof_) {
                if (end_ == 0) return false;
                line = std::string_view(buf_.data(), end_);
                pos_ = end_;
                return true;
            }
            const std::size_t n = std::fread(buf_.data() + end_, 1, buf_.size() - end_, f_);
            end_ += n;
            if (n == 0) eof_ = true;
        }
    }

private:
    std::FILE* f_;
    std::string buf_;
    std::size_t pos_ = 0, end_ = 0;
    bool eof_ = false;
};

}  // namespace

int main(int argc, char** argv) {
    std::string out;
    std::int64_t grid_ms = 1000;
    std::int64_t fixed_tick = 0;  // 1e-8 units, 0 = infer
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (a == "--grid-ms" && i + 1 < argc) grid_ms = std::atoll(argv[++i]);
        else if (a == "--tick" && i + 1 < argc) {
            if (!parse_e8(argv[++i], fixed_tick) || fixed_tick <= 0) {
                std::fprintf(stderr, "bad --tick\n");
                return 2;
            }
        }
        else {
            std::fprintf(stderr, "usage: %s --out PREFIX [--grid-ms N] [--tick T] < bookTicker.csv\n", argv[0]);
            return 2;
        }
    }
    if (out.empty() || grid_ms <= 0) {
        std::fprintf(stderr, "usage: %s --out PREFIX [--grid-ms N] [--tick T] < bookTicker.csv\n", argv[0]);
        return 2;
    }

    std::FILE* grid = std::fopen((out + "_grid.bin").c_str(), "wb");
    std::FILE* changes = std::fopen((out + "_changes.bin").c_str(), "wb");
    if (!grid || !changes) {
        std::perror("open output");
        return 1;
    }

    LineReader reader(stdin);
    std::string_view line;
    std::uint64_t rows = 0, bad = 0, out_of_order = 0, crossed = 0, nonpositive_qty = 0, off_tick = 0;
    std::uint64_t price_changes = 0, grid_samples = 0;
    std::int64_t tick = 0, first_ts = 0, last_update_id = -1, update_id_regressions = 0;
    State st;
    bool have_state = false;
    std::int64_t next_grid = 0;

    auto emit_grid_until = [&](std::int64_t ts) {
        // Emit every grid point g < ts with the state as of g.
        while (next_grid < ts) {
            const std::int64_t rec[3] = {next_grid, st.bid, st.ask};
            const double q[2] = {st.bq, st.aq};
            const std::int64_t age = next_grid - st.ts;
            std::fwrite(rec, sizeof rec, 1, grid);
            std::fwrite(q, sizeof q, 1, grid);
            std::fwrite(&age, sizeof age, 1, grid);
            ++grid_samples;
            next_grid += grid_ms;
        }
    };

    while (reader.next(line)) {
        if (line.empty() || line[0] < '0' || line[0] > '9') continue;  // header
        std::string_view f[7];
        std::size_t n = 0, start = 0;
        for (std::size_t i = 0; i <= line.size() && n < 7; ++i) {
            if (i == line.size() || line[i] == ',') {
                f[n++] = line.substr(start, i - start);
                start = i + 1;
            }
        }
        std::int64_t uid = 0, bid = 0, ask = 0, ts = 0;
        double bq = 0, aq = 0;
        if (n != 7 || !parse_int(f[0], uid) || !parse_e8(f[1], bid) || !parse_double(f[2], bq) ||
            !parse_e8(f[3], ask) || !parse_double(f[4], aq) || !parse_int(f[5], ts)) {
            ++bad;
            continue;
        }
        ++rows;
        if (uid < last_update_id) ++update_id_regressions;
        last_update_id = uid;
        if (bq <= 0 || aq <= 0) ++nonpositive_qty;
        if (bid >= ask) {
            ++crossed;  // locked / crossed snapshot: not a valid book state, skip it
            continue;
        }
        if (fixed_tick) {
            if (bid % fixed_tick || ask % fixed_tick) {
                ++off_tick;
                continue;
            }
            tick = fixed_tick;
        } else {
            tick = std::gcd(tick, std::gcd(bid, ask));
        }

        if (!have_state) {
            first_ts = ts;
            next_grid = (ts / grid_ms + 1) * grid_ms;
        } else if (ts < st.ts) {
            ++out_of_order;  // keep time monotone: treat it as arriving now
            ts = st.ts;
        }
        // Grid points strictly before this row see the previous state. A point
        // g == ts is emitted only once a later row arrives, so it includes
        // every row stamped ts.
        if (have_state) emit_grid_until(ts);
        const bool price_moved = !have_state || bid != st.bid || ask != st.ask;
        st = {bid, ask, bq, aq, ts};
        have_state = true;
        if (price_moved) {
            const std::int64_t rec[3] = {ts, bid, ask};
            std::fwrite(rec, sizeof rec, 1, changes);
            ++price_changes;
        }
    }
    std::fclose(grid);
    std::fclose(changes);

    if (std::FILE* m = std::fopen((out + "_meta.json").c_str(), "w")) {
        std::fprintf(m,
                     "{\"rows\": %llu, \"bad_rows\": %llu, \"crossed_rows\": %llu, "
                     "\"nonpositive_qty_rows\": %llu, \"off_tick_rows\": %llu, \"out_of_order_ts\": %llu, "
                     "\"update_id_regressions\": %lld, \"tick_e8\": %lld, \"first_ts_ms\": %lld, "
                     "\"last_ts_ms\": %lld, \"price_changes\": %llu, \"grid_samples\": %llu, "
                     "\"grid_ms\": %lld}\n",
                     static_cast<unsigned long long>(rows), static_cast<unsigned long long>(bad),
                     static_cast<unsigned long long>(crossed),
                     static_cast<unsigned long long>(nonpositive_qty),
                     static_cast<unsigned long long>(off_tick),
                     static_cast<unsigned long long>(out_of_order),
                     static_cast<long long>(update_id_regressions), static_cast<long long>(tick),
                     static_cast<long long>(first_ts), static_cast<long long>(st.ts),
                     static_cast<unsigned long long>(price_changes),
                     static_cast<unsigned long long>(grid_samples), static_cast<long long>(grid_ms));
        std::fclose(m);
    }
    std::fprintf(stderr, "%s: %llu rows, %llu price changes, %llu grid samples, tick %.8f\n", out.c_str(),
                 static_cast<unsigned long long>(rows), static_cast<unsigned long long>(price_changes),
                 static_cast<unsigned long long>(grid_samples), static_cast<double>(tick) * 1e-8);
    return bad > rows / 1000 ? 1 : 0;
}
