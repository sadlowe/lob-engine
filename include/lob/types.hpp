#pragma once

#include <cstdint>

namespace lob {

using OrderId = std::uint64_t;
// Prices are integer ticks: no floating point on the hot path, exact comparisons.
using Price = std::int64_t;
using Qty = std::uint64_t;

enum class Side : std::uint8_t { Buy, Sell };

[[nodiscard]] constexpr Side opposite(Side s) noexcept {
    return s == Side::Buy ? Side::Sell : Side::Buy;
}

struct Trade {
    OrderId maker_id;
    OrderId taker_id;
    Price price;  // always the resting (maker) order's price
    Qty qty;
    Side taker_side;

    friend bool operator==(const Trade&, const Trade&) = default;
};

enum class Status : std::uint8_t { Ok, DuplicateId, InvalidQty, UnknownId };

struct ExecResult {
    Status status = Status::Ok;
    Qty filled = 0;  // quantity executed immediately against the book
    Qty rested = 0;  // quantity left resting in the book
};

struct BookLevel {
    Price price;
    Qty qty;
    std::uint32_t orders;

    friend bool operator==(const BookLevel&, const BookLevel&) = default;
};

}  // namespace lob
