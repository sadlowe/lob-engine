#pragma once

// Binary market-data recording format (.lob), written by tools/binance/convert.py.
//
//   FileHeader (64 bytes) followed by `record_count` fixed-size Records (40 bytes).
//
// Fixed-size little-endian records keep the replay loop trivial: no parsing,
// no allocation, just sequential reads.

#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "lob/types.hpp"

namespace lob::replay {

static_assert(std::endian::native == std::endian::little, ".lob files are little-endian");

enum class RecordType : std::uint8_t {
    Reset = 1,              // clear the book (a snapshot follows)
    Level = 2,              // absolute quantity at a price level (0 = remove)
    EventEnd = 3,           // apply all Level records staged since the last EventEnd
    Trade = 4,              // side = aggressor side
    CheckpointBound = 5,    // price range covered by a validation snapshot
    CheckpointLevel = 6,    // expected level from an exchange snapshot
    CheckpointExclude = 7,  // price that cannot be compared exactly at this checkpoint
    CheckpointEnd = 8,      // compare the reconstructed book with the checkpoint
};

struct Record {
    std::uint64_t ts_ns;  // exchange timestamp
    std::uint64_t seq;    // exchange update id (depth) or trade id
    std::int64_t price;   // ticks
    std::uint64_t qty;    // lots
    RecordType type;
    std::uint8_t side;  // 0 = Buy/bid, 1 = Sell/ask
    std::uint8_t pad[6];

    [[nodiscard]] Side book_side() const noexcept { return side == 0 ? Side::Buy : Side::Sell; }
};
static_assert(sizeof(Record) == 40);

struct FileHeader {
    char magic[8];
    char symbol[16];
    double tick_size;
    double lot_size;
    std::uint64_t record_count;
    std::uint8_t reserved[16];
};
static_assert(sizeof(FileHeader) == 64);

inline constexpr char kMagic[8] = {'L', 'O', 'B', 'R', 'E', 'C', '0', '1'};

namespace detail {
struct FileCloser {
    void operator()(std::FILE* f) const noexcept { std::fclose(f); }
};
using FilePtr = std::unique_ptr<std::FILE, FileCloser>;
}  // namespace detail

/// Sequential buffered reader.
class RecordReader {
public:
    explicit RecordReader(const std::string& path) : file_(std::fopen(path.c_str(), "rb")) {
        if (!file_) throw std::runtime_error("cannot open " + path);
        if (std::fread(&header_, sizeof header_, 1, file_.get()) != 1 ||
            std::memcmp(header_.magic, kMagic, sizeof kMagic) != 0) {
            throw std::runtime_error(path + " is not a .lob recording");
        }
        buffer_.resize(8192);
    }

    [[nodiscard]] const FileHeader& header() const noexcept { return header_; }

    [[nodiscard]] std::string symbol() const {
        return {header_.symbol, strnlen(header_.symbol, sizeof header_.symbol)};
    }

    bool next(Record& out) {
        if (pos_ == len_) {
            len_ = std::fread(buffer_.data(), sizeof(Record), buffer_.size(), file_.get());
            pos_ = 0;
            if (len_ == 0) return false;
        }
        out = buffer_[pos_++];
        return true;
    }

private:
    detail::FilePtr file_;
    FileHeader header_{};
    std::vector<Record> buffer_;
    std::size_t pos_ = 0;
    std::size_t len_ = 0;
};

/// Writer, mainly for tests and tooling (the Python converter writes the same layout).
class RecordWriter {
public:
    RecordWriter(const std::string& path, const std::string& symbol, double tick, double lot)
        : file_(std::fopen(path.c_str(), "wb")) {
        if (!file_) throw std::runtime_error("cannot create " + path);
        std::memcpy(header_.magic, kMagic, sizeof kMagic);
        std::strncpy(header_.symbol, symbol.c_str(), sizeof header_.symbol);
        header_.tick_size = tick;
        header_.lot_size = lot;
        write_header();
    }

    RecordWriter(const RecordWriter&) = delete;
    RecordWriter& operator=(const RecordWriter&) = delete;

    ~RecordWriter() {
        if (file_) close();
    }

    void write(RecordType type, Side side, std::int64_t price, std::uint64_t qty,
               std::uint64_t seq = 0, std::uint64_t ts_ns = 0) {
        Record r{};
        r.ts_ns = ts_ns;
        r.seq = seq;
        r.price = price;
        r.qty = qty;
        r.type = type;
        r.side = side == Side::Buy ? 0 : 1;
        std::fwrite(&r, sizeof r, 1, file_.get());
        ++header_.record_count;
    }

    void close() {
        std::fseek(file_.get(), 0, SEEK_SET);
        write_header();
        file_.reset();
    }

private:
    void write_header() { std::fwrite(&header_, sizeof header_, 1, file_.get()); }

    detail::FilePtr file_;
    FileHeader header_{};
};

}  // namespace lob::replay
