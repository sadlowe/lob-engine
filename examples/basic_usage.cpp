#include <cstdio>

#include "lob/order_book.hpp"

using namespace lob;

namespace {

void print_book(const OrderBook& book) {
    const auto asks = book.depth(Side::Sell, 5);
    const auto bids = book.depth(Side::Buy, 5);
    std::printf("      price      qty  orders\n");
    for (auto it = asks.rbegin(); it != asks.rend(); ++it) {
        std::printf("ASK %7lld %8llu %7u\n", static_cast<long long>(it->price),
                    static_cast<unsigned long long>(it->qty), it->orders);
    }
    std::printf("    ----------------------\n");
    for (const auto& l : bids) {
        std::printf("BID %7lld %8llu %7u\n", static_cast<long long>(l.price),
                    static_cast<unsigned long long>(l.qty), l.orders);
    }
    std::printf("\n");
}

}  // namespace

int main() {
    OrderBook book;

    book.add_limit(1, Side::Buy, 9'998, 50);
    book.add_limit(2, Side::Buy, 9'999, 20);
    book.add_limit(3, Side::Buy, 9'999, 30);
    book.add_limit(4, Side::Sell, 10'001, 25);
    book.add_limit(5, Side::Sell, 10'002, 40);
    book.add_limit(6, Side::Sell, 10'003, 60);

    std::printf("Initial book:\n");
    print_book(book);

    std::printf("Aggressive BUY 50 @ 10'002:\n");
    const auto res = book.add_limit(7, Side::Buy, 10'002, 50, [](const Trade& t) {
        std::printf("  trade: maker #%llu  %llu @ %lld\n",
                    static_cast<unsigned long long>(t.maker_id),
                    static_cast<unsigned long long>(t.qty), static_cast<long long>(t.price));
    });
    std::printf("  filled %llu, rested %llu\n\n", static_cast<unsigned long long>(res.filled),
                static_cast<unsigned long long>(res.rested));

    book.cancel(1);
    std::printf("After cancelling order #1:\n");
    print_book(book);
    return 0;
}
