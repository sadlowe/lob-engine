"""Tests for the extractor + queue-imbalance pipeline.

    python -m unittest tools/research/test_queue_imbalance.py   (after building lob_bbo_extract)
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest

import numpy as np

sys.path.insert(0, os.path.dirname(__file__))
import queue_imbalance as qi  # noqa: E402

BINARY = os.environ.get("LOB_BBO_EXTRACT", "build/lob_bbo_extract")
HEADER = "update_id,best_bid_price,best_bid_qty,best_ask_price,best_ask_qty,transaction_time,event_time\n"


def row(uid, bid, bq, ask, aq, ts):
    return f"{uid},{bid:.8f},{bq:.8f},{ask:.8f},{aq:.8f},{ts},{ts + 5}\n"


class Pipeline(unittest.TestCase):
    def run_extract(self, rows: list[str], tick: str = "0.1") -> str:
        d = tempfile.mkdtemp()
        prefix = os.path.join(d, "X")
        subprocess.run([BINARY, "--out", prefix, "--tick", tick], input="".join([HEADER, *rows]).encode(),
                       check=True)
        return prefix

    def test_grid_targets_and_horizons(self):
        rows = [
            row(1, 100.0, 9.0, 100.1, 1.0, 500),    # I = +0.8
            row(2, 100.0, 9.0, 100.1, 2.0, 1000),   # same ms as grid point 1000: must be included
            row(3, 100.0, 1.0, 100.1, 9.0, 1500),   # I = -0.8
            row(4, 99.9, 5.0, 100.0, 5.0, 2500),    # mid moves down
            row(5, 99.9, 5.0, 100.0, 5.0, 2600),    # queue-only update
            row(6, 100.0, 5.0, 100.1, 5.0, 3500),   # mid moves up
            row(7, 100.05, 5.0, 100.1, 5.0, 3600),  # off the 0.1 grid: dropped
            row(8, 100.0, 5.0, 100.1, 5.0, 14000),
        ]
        prefix = self.run_extract(rows)
        g = np.fromfile(prefix + "_grid.bin", dtype=qi.GRID)
        self.assertEqual(g["ts"].tolist(), list(range(1000, 14000, 1000)))
        self.assertEqual(g["aq"][0], 2.0)  # row stamped exactly at the grid time is included
        self.assertEqual(g["age"][0], 0)

        day = qi.Day(prefix)
        self.assertEqual(day.meta["off_tick_rows"], 1)
        # t=1000 (I>0): next move is down at 2500; t=2000 (I<0): down; t=3000: up at 3500.
        self.assertEqual(day.ts[:3].tolist(), [1000, 2000, 3000])
        self.assertEqual(day.up[:3].tolist(), [0, 0, 1])
        self.assertAlmostEqual(day.imb[0], (9 - 2) / 11)
        self.assertAlmostEqual(day.wait_s[0], 1.5)
        # Samples after the last move have no target and are dropped.
        self.assertEqual(len(day.ts), 3)
        # Book 1 s after t=2000 is the one set at 2600.
        fb, fa, valid = day.book_at(day.ts + 1000)
        self.assertEqual(fb[1], 99.9e8)
        self.assertTrue(valid[1])
        _, _, valid = day.book_at(np.array([14001]))
        self.assertFalse(valid[0])

    def test_taker_pays_the_spread_and_respects_latency(self):
        rows = [
            row(1, 100.0, 19.0, 100.1, 1.0, 500),    # I = +0.9 at the 1000 grid point
            row(2, 100.0, 1.0, 100.1, 19.0, 1020),   # flips before a 50 ms trader can act (no price change)
            row(3, 100.3, 5.0, 100.4, 5.0, 1040),    # price jumps 3 ticks inside the latency window
            row(4, 100.5, 5.0, 100.6, 5.0, 5000),
            row(5, 100.4, 5.0, 100.5, 5.0, 30000),
        ]
        day = qi.Day(self.run_extract(rows))
        self.assertEqual(day.ts[0], 1000)
        _, e0, g0 = qi.taker_trades([day], latency_ms=0, horizon_s=10)
        _, e50, g50 = qi.taker_trades([day], latency_ms=50, horizon_s=10)
        mid = 100.05
        # Instant: buy at 100.1, sell at 100.5 (book at 11 s), edge = mid move 100.05 -> 100.55.
        self.assertAlmostEqual(e0[0], 0.5 / mid * 1e4)
        self.assertAlmostEqual(g0[0], 0.4 / mid * 1e4)
        # 50 ms late: buy at 100.4, sell at 100.5.
        self.assertAlmostEqual(e50[0], 0.2 / mid * 1e4)
        self.assertAlmostEqual(g50[0], 0.1 / mid * 1e4)

    def test_auc_matches_pairwise_definition(self):
        rng = np.random.default_rng(1)
        x = rng.integers(0, 5, 300).astype(float)  # many ties
        y = (rng.random(300) < 0.3 + 0.1 * x).astype(np.int8)
        pos, neg = x[y == 1], x[y == 0]
        brute = ((pos[:, None] > neg[None, :]).sum() + 0.5 * (pos[:, None] == neg[None, :]).sum()) / (
            len(pos) * len(neg))
        self.assertAlmostEqual(qi.auc(x, y), brute)

    def test_logit_recovers_coefficients(self):
        rng = np.random.default_rng(2)
        x = rng.uniform(-1, 1, 200_000)
        y = (rng.random(len(x)) < 1 / (1 + np.exp(-(0.2 + 3.0 * x)))).astype(float)
        a, b = qi.fit_logit(x, y)
        self.assertAlmostEqual(a, 0.2, delta=0.03)
        self.assertAlmostEqual(b, 3.0, delta=0.05)


if __name__ == "__main__":
    unittest.main()
