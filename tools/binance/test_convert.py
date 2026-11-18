"""Tests for the Binance sync protocol in convert.py (stdlib only).

    python -m unittest discover -s tools/binance
"""

from __future__ import annotations

import gzip
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(__file__))
import convert  # noqa: E402

EXCHANGE_INFO = {
    "symbol": "BTCUSDT",
    "filters": [
        {"filterType": "PRICE_FILTER", "tickSize": "0.01000000"},
        {"filterType": "LOT_SIZE", "stepSize": "0.00001000"},
    ],
}


def depth(U: int, u: int, E: int, bids=(), asks=()) -> dict:
    return {"type": "ws", "data": {"stream": "btcusdt@depth@100ms",
            "data": {"e": "depthUpdate", "E": E, "U": U, "u": u, "b": list(bids), "a": list(asks)}}}


def trade(t: int, T: int, p: str, q: str, buyer_is_maker: bool) -> dict:
    return {"type": "ws", "data": {"stream": "btcusdt@trade",
            "data": {"e": "trade", "t": t, "T": T, "E": T, "p": p, "q": q, "m": buyer_is_maker}}}


def snapshot(last_id: int, kind: str, bids, asks) -> dict:
    return {"type": "snapshot", "kind": kind,
            "data": {"lastUpdateId": last_id, "bids": bids, "asks": asks}}


BOOK_BIDS = [["100.00", "1.00000"], ["99.00", "2.00000"]]
BOOK_ASKS = [["101.00", "1.50000"], ["102.00", "3.00000"]]


def records_of(path: str) -> list[tuple]:
    with open(path, "rb") as f:
        data = f.read()
    magic, symbol, tick, lot, count = convert.HEADER.unpack_from(data, 0)
    assert magic == convert.MAGIC
    body = data[convert.HEADER.size:]
    assert len(body) == count * convert.RECORD.size
    return [convert.RECORD.unpack_from(body, i * convert.RECORD.size) for i in range(count)]


class ConvertTest(unittest.TestCase):
    def run_capture(self, lines: list[dict]) -> tuple[convert.Stats, list[tuple]]:
        with tempfile.TemporaryDirectory() as tmp:
            raw = os.path.join(tmp, "raw.jsonl.gz")
            out = os.path.join(tmp, "out.lob")
            with gzip.open(raw, "wt") as f:
                f.write(json.dumps({"type": "exchange_info", "data": EXCHANGE_INFO}) + "\n")
                for line in lines:
                    f.write(json.dumps(line) + "\n")
            stats = convert.convert(convert.load_capture(raw), out)
            return stats, records_of(out)

    def test_sync_gap_resync_and_checkpoints(self) -> None:
        stats, recs = self.run_capture([
            depth(95, 99, 1_000),                                  # before snapshot: dropped
            depth(98, 101, 1_100, bids=[["100.00", "1.20000"]]),   # first applied (98 <= 101 <= 101)
            snapshot(100, "initial", BOOK_BIDS, BOOK_ASKS),
            depth(102, 105, 1_200, asks=[["101.00", "0"]]),
            snapshot(103, "checkpoint", BOOK_BIDS, BOOK_ASKS),     # straddled by 102..105
            depth(110, 112, 1_300),                                # gap (expected U=106)
            snapshot(111, "checkpoint", BOOK_BIDS, BOOK_ASKS),     # used to resync
            depth(113, 115, 1_400),
            depth(116, 118, 1_500),
            snapshot(116, "checkpoint", BOOK_BIDS, BOOK_ASKS),
        ])
        self.assertEqual(stats.events, 5)
        self.assertEqual(stats.resets, 2)
        self.assertEqual(stats.gaps, 1)
        self.assertEqual(stats.checkpoints, 2)
        self.assertEqual(stats.checkpoints_skipped, 0)

        types = [r[4] for r in recs]
        self.assertEqual(types[0], convert.RESET)
        self.assertEqual(types.count(convert.RESET), 2)
        self.assertEqual(types.count(convert.CP_END), 2)
        # First checkpoint excludes the price touched by the straddling event.
        first_cp = types.index(convert.CP_BOUND)
        excluded = [r for r in recs[first_cp:types.index(convert.CP_END)] if r[4] == convert.CP_EXCLUDE]
        self.assertEqual([(r[5], r[2]) for r in excluded], [(convert.SELL, 10_100)])

    def test_units_and_trade_side(self) -> None:
        _, recs = self.run_capture([
            snapshot(10, "initial", BOOK_BIDS, BOOK_ASKS),
            depth(9, 11, 2_000, bids=[["100.01", "0.00012"]]),
            trade(1, 1_500, "100.00", "0.10000", True),   # before sync point: dropped
            trade(2, 2_100, "101.00", "0.25000", False),  # buyer aggressed
            trade(3, 2_150, "100.00", "0.00100", True),   # seller aggressed
            depth(12, 13, 2_200),
        ])
        levels = [r for r in recs if r[4] == convert.LEVEL]
        self.assertIn((convert.BUY, 10_001, 12), [(r[5], r[2], r[3]) for r in levels])
        trades = [r for r in recs if r[4] == convert.TRADE]
        self.assertEqual([(r[1], r[5], r[2], r[3]) for r in trades],
                         [(2, convert.BUY, 10_100, 25_000), (3, convert.SELL, 10_000, 100)])

    def test_unsyncable_capture_fails_loudly(self) -> None:
        with self.assertRaises(ValueError):
            self.run_capture([snapshot(500, "initial", BOOK_BIDS, BOOK_ASKS), depth(1, 2, 1_000)])

    def test_rejects_off_grid_prices(self) -> None:
        with self.assertRaises(ValueError):
            convert.to_units("100.005", convert.Decimal("0.01"))


if __name__ == "__main__":
    unittest.main()
