#include <gtest/gtest.h>

#include <filesystem>

#include "lob/order_book.hpp"
#include "lob/replay/l2_book_builder.hpp"
#include "lob/replay/record.hpp"

using namespace lob;
using namespace lob::replay;

namespace {

struct Fixture {
    OrderBook book;
    L2BookBuilder builder{book};

    void event(std::initializer_list<std::tuple<Side, Price, Qty>> updates) {
        for (const auto& [side, price, qty] : updates) builder.stage(side, price, qty);
        builder.commit();
    }
};

}  // namespace

TEST(L2BookBuilder, BuildsLevelsFromAbsoluteQuantities) {
    Fixture f;
    f.event({{Side::Buy, 100, 5}, {Side::Buy, 99, 7}, {Side::Sell, 101, 3}});

    EXPECT_EQ(f.book.best_bid(), 100);
    EXPECT_EQ(f.book.best_ask(), 101);
    EXPECT_EQ(f.book.volume_at(Side::Buy, 99), 7U);
    EXPECT_EQ(f.builder.current(Side::Sell, 101), 3U);
}

TEST(L2BookBuilder, IncreaseAppendsDecreaseTrimsNewestFirst) {
    Fixture f;
    f.event({{Side::Sell, 101, 5}});
    f.event({{Side::Sell, 101, 8}});  // +3 joins the back of the queue
    EXPECT_EQ(f.book.depth(Side::Sell, 1)[0], (BookLevel{101, 8, 2}));

    f.event({{Side::Sell, 101, 6}});  // -2 taken from the newest chunk
    EXPECT_EQ(f.book.depth(Side::Sell, 1)[0], (BookLevel{101, 6, 2}));

    f.event({{Side::Sell, 101, 4}});  // newest chunk gone, oldest reduced
    EXPECT_EQ(f.book.depth(Side::Sell, 1)[0], (BookLevel{101, 4, 1}));

    f.event({{Side::Sell, 101, 0}});
    EXPECT_FALSE(f.book.best_ask());
    EXPECT_EQ(f.book.order_count(), 0U);
}

TEST(L2BookBuilder, RemovingUnknownLevelIsNoop) {
    Fixture f;
    f.event({{Side::Buy, 100, 0}});
    EXPECT_EQ(f.book.order_count(), 0U);
}

TEST(L2BookBuilder, EventThatMovesThroughOldPricesNeverTrades) {
    Fixture f;
    f.event({{Side::Buy, 100, 5}, {Side::Sell, 101, 5}});
    // Market moves up: the new bid at 101 is listed before the ask at 101 is
    // removed. Decreases are applied first, so the book never crosses.
    f.event({{Side::Buy, 101, 2}, {Side::Sell, 101, 0}, {Side::Sell, 102, 4}});

    EXPECT_EQ(f.book.best_bid(), 101);
    EXPECT_EQ(f.book.best_ask(), 102);
    EXPECT_EQ(f.builder.stats().crossed_levels_removed, 0U);
}

TEST(L2BookBuilder, StaleCrossingLevelsAreRemoved) {
    Fixture f;
    f.event({{Side::Sell, 101, 5}, {Side::Sell, 102, 5}});
    f.event({{Side::Buy, 102, 1}});  // inconsistent feed: crosses two asks

    EXPECT_EQ(f.book.best_bid(), 102);
    EXPECT_FALSE(f.book.best_ask());
    EXPECT_EQ(f.builder.stats().crossed_levels_removed, 2U);
}

TEST(L2BookBuilder, ResetClearsEverything) {
    Fixture f;
    f.event({{Side::Buy, 100, 5}, {Side::Sell, 101, 5}});
    f.builder.reset();
    EXPECT_EQ(f.book.order_count(), 0U);
    f.event({{Side::Buy, 90, 1}});
    EXPECT_EQ(f.book.best_bid(), 90);
}

TEST(RecordFile, RoundTrip) {
    const auto path = (std::filesystem::temp_directory_path() / "lob_roundtrip_test.lob").string();
    {
        RecordWriter w(path, "BTCUSDT", 0.01, 0.00001);
        w.write(RecordType::Level, Side::Buy, 9'123'456, 120, 7, 1'000);
        w.write(RecordType::Trade, Side::Sell, 9'123'455, 3, 8, 2'000);
    }
    RecordReader reader(path);
    EXPECT_EQ(reader.symbol(), "BTCUSDT");
    EXPECT_EQ(reader.header().record_count, 2U);
    EXPECT_DOUBLE_EQ(reader.header().tick_size, 0.01);

    Record r{};
    ASSERT_TRUE(reader.next(r));
    EXPECT_EQ(r.type, RecordType::Level);
    EXPECT_EQ(r.book_side(), Side::Buy);
    EXPECT_EQ(r.price, 9'123'456);
    EXPECT_EQ(r.qty, 120U);
    EXPECT_EQ(r.seq, 7U);
    ASSERT_TRUE(reader.next(r));
    EXPECT_EQ(r.type, RecordType::Trade);
    EXPECT_EQ(r.book_side(), Side::Sell);
    EXPECT_FALSE(reader.next(r));
    std::filesystem::remove(path);
}

TEST(RecordFile, RejectsForeignFiles) {
    const auto path = (std::filesystem::temp_directory_path() / "lob_bad_test.lob").string();
    {
        std::FILE* f = std::fopen(path.c_str(), "wb");
        std::fputs("definitely not a recording, but long enough to fill a header.....", f);
        std::fclose(f);
    }
    EXPECT_THROW(RecordReader{path}, std::runtime_error);
    std::filesystem::remove(path);
}
