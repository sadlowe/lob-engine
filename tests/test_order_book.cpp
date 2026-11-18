#include <gtest/gtest.h>

#include <vector>

#include "lob/order_book.hpp"

using namespace lob;

namespace {

struct Recorder {
    std::vector<Trade> trades;
    auto sink() {
        return [this](const Trade& t) { trades.push_back(t); };
    }
};

}  // namespace

TEST(OrderBook, EmptyBook) {
    OrderBook book;
    EXPECT_FALSE(book.best_bid());
    EXPECT_FALSE(book.best_ask());
    EXPECT_FALSE(book.spread());
    EXPECT_EQ(book.order_count(), 0U);
}

TEST(OrderBook, NonCrossingOrdersRest) {
    OrderBook book;
    EXPECT_EQ(book.add_limit(1, Side::Buy, 99, 10).rested, 10U);
    EXPECT_EQ(book.add_limit(2, Side::Sell, 101, 5).rested, 5U);

    EXPECT_EQ(book.best_bid(), 99);
    EXPECT_EQ(book.best_ask(), 101);
    EXPECT_EQ(book.spread(), 2);
    EXPECT_EQ(book.order_count(), 2U);
}

TEST(OrderBook, LevelsAggregateQuantity) {
    OrderBook book;
    book.add_limit(1, Side::Buy, 100, 10);
    book.add_limit(2, Side::Buy, 100, 15);
    book.add_limit(3, Side::Buy, 98, 7);

    EXPECT_EQ(book.volume_at(Side::Buy, 100), 25U);
    EXPECT_EQ(book.volume_at(Side::Buy, 99), 0U);
    const auto d = book.depth(Side::Buy, 10);
    ASSERT_EQ(d.size(), 2U);
    EXPECT_EQ(d[0], (BookLevel{100, 25, 2}));
    EXPECT_EQ(d[1], (BookLevel{98, 7, 1}));
}

TEST(OrderBook, FullFillAtMakerPrice) {
    OrderBook book;
    Recorder rec;
    book.add_limit(1, Side::Sell, 100, 10);
    const auto r = book.add_limit(2, Side::Buy, 105, 10, rec.sink());

    EXPECT_EQ(r.filled, 10U);
    EXPECT_EQ(r.rested, 0U);
    ASSERT_EQ(rec.trades.size(), 1U);
    EXPECT_EQ(rec.trades[0], (Trade{1, 2, 100, 10, Side::Buy}));
    EXPECT_EQ(book.order_count(), 0U);
}

TEST(OrderBook, PartialFillRestsRemainder) {
    OrderBook book;
    book.add_limit(1, Side::Sell, 100, 4);
    const auto r = book.add_limit(2, Side::Buy, 100, 10);

    EXPECT_EQ(r.filled, 4U);
    EXPECT_EQ(r.rested, 6U);
    EXPECT_EQ(book.best_bid(), 100);
    EXPECT_FALSE(book.best_ask());
}

TEST(OrderBook, PriceTimePriority) {
    OrderBook book;
    Recorder rec;
    book.add_limit(1, Side::Sell, 101, 5);
    book.add_limit(2, Side::Sell, 100, 5);  // better price
    book.add_limit(3, Side::Sell, 100, 5);  // same price, later
    book.add_market(4, Side::Buy, 12, rec.sink());

    ASSERT_EQ(rec.trades.size(), 3U);
    EXPECT_EQ(rec.trades[0].maker_id, 2U);
    EXPECT_EQ(rec.trades[1].maker_id, 3U);
    EXPECT_EQ(rec.trades[2].maker_id, 1U);
    EXPECT_EQ(rec.trades[2].qty, 2U);
    EXPECT_EQ(book.volume_at(Side::Sell, 101), 3U);
}

TEST(OrderBook, LimitDoesNotTradeThroughItsPrice) {
    OrderBook book;
    book.add_limit(1, Side::Sell, 100, 5);
    book.add_limit(2, Side::Sell, 102, 5);
    const auto r = book.add_limit(3, Side::Buy, 101, 8);

    EXPECT_EQ(r.filled, 5U);
    EXPECT_EQ(r.rested, 3U);
    EXPECT_EQ(book.best_bid(), 101);
    EXPECT_EQ(book.best_ask(), 102);
}

TEST(OrderBook, MarketOrderIsImmediateOrCancel) {
    OrderBook book;
    book.add_limit(1, Side::Buy, 100, 5);
    const auto r = book.add_market(2, Side::Sell, 20);

    EXPECT_EQ(r.filled, 5U);
    EXPECT_EQ(r.rested, 0U);
    EXPECT_EQ(book.order_count(), 0U);
    EXPECT_FALSE(book.best_ask());
}

TEST(OrderBook, MarketOnEmptyBookDoesNothing) {
    OrderBook book;
    const auto r = book.add_market(1, Side::Buy, 10);
    EXPECT_EQ(r.status, Status::Ok);
    EXPECT_EQ(r.filled, 0U);
}

TEST(OrderBook, CancelRemovesOrderAndEmptyLevel) {
    OrderBook book;
    book.add_limit(1, Side::Buy, 100, 10);
    book.add_limit(2, Side::Buy, 99, 10);

    EXPECT_TRUE(book.cancel(1));
    EXPECT_EQ(book.best_bid(), 99);
    EXPECT_EQ(book.level_count(Side::Buy), 1U);
    EXPECT_FALSE(book.cancel(1));
    EXPECT_FALSE(book.cancel(42));
}

TEST(OrderBook, CancelMiddleOfQueueKeepsFifo) {
    OrderBook book;
    Recorder rec;
    book.add_limit(1, Side::Sell, 100, 1);
    book.add_limit(2, Side::Sell, 100, 1);
    book.add_limit(3, Side::Sell, 100, 1);
    EXPECT_TRUE(book.cancel(2));
    book.add_market(4, Side::Buy, 2, rec.sink());

    ASSERT_EQ(rec.trades.size(), 2U);
    EXPECT_EQ(rec.trades[0].maker_id, 1U);
    EXPECT_EQ(rec.trades[1].maker_id, 3U);
}

TEST(OrderBook, FilledOrderCannotBeCancelled) {
    OrderBook book;
    book.add_limit(1, Side::Sell, 100, 5);
    book.add_limit(2, Side::Buy, 100, 5);
    EXPECT_FALSE(book.cancel(1));
    EXPECT_FALSE(book.cancel(2));
}

TEST(OrderBook, ReduceKeepsPriority) {
    OrderBook book;
    Recorder rec;
    book.add_limit(1, Side::Sell, 100, 10);
    book.add_limit(2, Side::Sell, 100, 10);
    EXPECT_EQ(book.reduce(1, 7), Status::Ok);
    EXPECT_EQ(book.volume_at(Side::Sell, 100), 13U);

    book.add_market(3, Side::Buy, 3, rec.sink());
    ASSERT_EQ(rec.trades.size(), 1U);
    EXPECT_EQ(rec.trades[0].maker_id, 1U);
}

TEST(OrderBook, ReduceByFullQuantityCancels) {
    OrderBook book;
    book.add_limit(1, Side::Buy, 100, 10);
    EXPECT_EQ(book.reduce(1, 10), Status::Ok);
    EXPECT_FALSE(book.contains(1));
    EXPECT_FALSE(book.best_bid());
    EXPECT_EQ(book.reduce(1, 1), Status::UnknownId);
    EXPECT_EQ(book.reduce(1, 0), Status::InvalidQty);
}

TEST(OrderBook, RejectsInvalidInput) {
    OrderBook book;
    EXPECT_EQ(book.add_limit(1, Side::Buy, 100, 0).status, Status::InvalidQty);
    EXPECT_EQ(book.add_market(1, Side::Buy, 0).status, Status::InvalidQty);
    book.add_limit(1, Side::Buy, 100, 5);
    EXPECT_EQ(book.add_limit(1, Side::Sell, 200, 5).status, Status::DuplicateId);
    EXPECT_EQ(book.order_count(), 1U);
}

TEST(OrderBook, SweepAcrossManyLevels) {
    OrderBook book;
    for (OrderId i = 0; i < 100; ++i) {
        book.add_limit(i + 1, Side::Sell, 1000 + static_cast<Price>(i), 1);
    }
    Recorder rec;
    const auto r = book.add_limit(1000, Side::Buy, 1049, 1000, rec.sink());

    EXPECT_EQ(r.filled, 50U);
    EXPECT_EQ(r.rested, 950U);
    EXPECT_EQ(rec.trades.front().price, 1000);
    EXPECT_EQ(rec.trades.back().price, 1049);
    EXPECT_EQ(book.best_ask(), 1050);
    EXPECT_EQ(book.best_bid(), 1049);
}

TEST(OrderBook, PoolSlotsAreRecycled) {
    OrderBook book(4);
    for (OrderId i = 1; i <= 100000; ++i) {
        book.add_limit(i, Side::Buy, 100 + static_cast<Price>(i % 7), 1);
        ASSERT_TRUE(book.cancel(i));
    }
    EXPECT_EQ(book.order_count(), 0U);
    EXPECT_EQ(book.level_count(Side::Buy), 0U);
}
