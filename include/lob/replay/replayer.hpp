#pragma once

// Drives a handler from a .lob recording: rebuilds the market book through the
// matching engine and forwards book updates and trades in exchange-time order.

#include <cstdint>

#include "lob/order_book.hpp"
#include "lob/replay/l2_book_builder.hpp"
#include "lob/replay/record.hpp"

namespace lob::replay {

struct MarketTrade {
    std::uint64_t ts_ns;
    Side aggressor;
    Price price;
    Qty qty;
};

/// Handler must provide:
///   void on_reset(std::uint64_t ts_ns);
///   void on_book(std::uint64_t ts_ns, const OrderBook& book);  // after each exchange event
///   void on_trade(const MarketTrade& trade);
template <class Handler>
void replay(RecordReader& reader, OrderBook& book, Handler& handler) {
    L2BookBuilder builder(book);
    Record r{};
    while (reader.next(r)) {
        switch (r.type) {
            case RecordType::Reset:
                builder.reset();
                handler.on_reset(r.ts_ns);
                break;
            case RecordType::Level:
                builder.stage(r.book_side(), r.price, r.qty);
                break;
            case RecordType::EventEnd:
                builder.commit();
                handler.on_book(r.ts_ns, static_cast<const OrderBook&>(book));
                break;
            case RecordType::Trade:
                handler.on_trade(MarketTrade{r.ts_ns, r.book_side(), r.price, r.qty});
                break;
            default:
                break;  // checkpoint records are only used by lob_replay
        }
    }
}

}  // namespace lob::replay
