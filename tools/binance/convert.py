#!/usr/bin/env python3
"""Convert a raw Binance capture (record.py) into a compact binary .lob file.

Applies the Binance diff-depth synchronisation protocol:

  1. Drop every depth event with  u <= lastUpdateId  of the snapshot.
  2. The first applied event must satisfy  U <= lastUpdateId + 1 <= u.
  3. Each following event must satisfy  U == previous u + 1. Otherwise there is
     a gap: the book is resynchronised from the next usable snapshot.

Each checkpoint snapshot (lastUpdateId = L) is placed after the last event
with u <= L. If the next event straddles L (U <= L < u), the prices it
touches cannot be compared exactly and are marked as excluded. The C++
replayer then checks that its reconstructed book matches the exchange
snapshot level by level.

Prices and quantities are converted to integer ticks / lots using exact
decimal arithmetic (no floats).

    python tools/binance/convert.py data/btcusdt.jsonl.gz data/btcusdt.lob
"""

from __future__ import annotations

import argparse
import gzip
import json
import struct
import sys
from dataclasses import dataclass, field
from decimal import Decimal
from typing import Iterable

# Must match include/lob/replay/record.hpp
HEADER = struct.Struct("<8s16sddQ16x")  # 64 bytes
RECORD = struct.Struct("<QQqQBB6x")  # 40 bytes
MAGIC = b"LOBREC01"

RESET, LEVEL, EVENT_END, TRADE = 1, 2, 3, 4
CP_BOUND, CP_LEVEL, CP_EXCLUDE, CP_END = 5, 6, 7, 8
BUY, SELL = 0, 1

assert HEADER.size == 64 and RECORD.size == 40


@dataclass
class Capture:
    symbol: str
    tick: Decimal
    lot: Decimal
    depth: list[dict] = field(default_factory=list)
    trades: list[dict] = field(default_factory=list)
    snapshots: list[dict] = field(default_factory=list)  # {"kind", "data"}


@dataclass
class Stats:
    events: int = 0
    trades: int = 0
    resets: int = 0
    gaps: int = 0
    checkpoints: int = 0
    checkpoints_skipped: int = 0
    records: int = 0


def to_units(value: str, unit: Decimal) -> int:
    q = Decimal(value) / unit
    if q != q.to_integral_value():
        raise ValueError(f"{value} is not a multiple of {unit}")
    return int(q)


def load_capture(path: str) -> Capture:
    cap: Capture | None = None
    depth: list[dict] = []
    trades: list[dict] = []
    snaps: list[dict] = []
    with gzip.open(path, "rt", encoding="utf-8") as f:
        for line in f:
            rec = json.loads(line)
            kind = rec["type"]
            if kind == "exchange_info":
                info = rec["data"]
                filters = {flt["filterType"]: flt for flt in info["filters"]}
                cap = Capture(
                    symbol=info["symbol"],
                    tick=Decimal(filters["PRICE_FILTER"]["tickSize"]).normalize(),
                    lot=Decimal(filters["LOT_SIZE"]["stepSize"]).normalize(),
                )
            elif kind == "snapshot":
                snaps.append({"kind": rec["kind"], "data": rec["data"]})
            elif kind == "ws":
                stream = rec["data"].get("stream", "")
                payload = rec["data"]["data"]
                if stream.endswith("@trade"):
                    trades.append(payload)
                elif "@depth" in stream:
                    depth.append(payload)
    if cap is None:
        raise ValueError("capture has no exchange_info header")

    # Order by final update id and drop duplicates (possible across reconnects).
    depth.sort(key=lambda e: e["u"])
    cap.depth = [e for i, e in enumerate(depth) if i == 0 or e["u"] != depth[i - 1]["u"]]
    cap.trades = sorted({t["t"]: t for t in trades}.values(), key=lambda t: (t["T"], t["t"]))
    cap.snapshots = sorted(snaps, key=lambda s: s["data"]["lastUpdateId"])
    return cap


def plan(cap: Capture, stats: Stats) -> list[tuple]:
    """Return the ordered list of actions: ("reset", snap, ts_ms),
    ("event", ev), ("checkpoint", snap, straddling_event | None, reset_snap)."""
    depth = cap.depth
    actions: list[tuple] = []
    used: set[int] = set()

    def find_start(j: int, last_id: int) -> int | None:
        k = j
        while k < len(depth) and depth[k]["u"] <= last_id:
            k += 1
        if k < len(depth) and depth[k]["U"] <= last_id + 1:
            return k
        return None

    j = 0
    si = 0
    ci = 0
    synced = False
    prev_u = -1
    reset_snap = None
    snaps = cap.snapshots

    while j < len(depth):
        if not synced:
            while si < len(snaps):
                snap = snaps[si]
                si += 1
                k = find_start(j, snap["data"]["lastUpdateId"])
                if k is not None:
                    used.add(id(snap))
                    reset_snap = snap
                    actions.append(("reset", snap, depth[k]["E"]))
                    stats.resets += 1
                    j, synced, prev_u = k, True, -1
                    break
            else:
                break  # no snapshot left to resync from
            continue

        ev = depth[j]
        if prev_u >= 0 and ev["U"] != prev_u + 1:
            stats.gaps += 1
            synced = False
            continue

        actions.append(("event", ev))
        stats.events += 1
        prev_u = ev["u"]
        j += 1

        nxt = depth[j] if j < len(depth) else None
        while ci < len(snaps):
            cp = snaps[ci]
            last_id = cp["data"]["lastUpdateId"]
            if id(cp) in used:
                ci += 1
            elif last_id < prev_u:
                ci += 1  # fell in a gap or before sync: cannot be validated
                stats.checkpoints_skipped += 1
            elif nxt is not None and nxt["U"] == prev_u + 1 and last_id < nxt["u"]:
                straddle = nxt if nxt["U"] <= last_id else None
                actions.append(("checkpoint", cp, straddle, reset_snap))
                stats.checkpoints += 1
                ci += 1
            else:
                break

    for cp in snaps[ci:]:
        if id(cp) not in used:
            stats.checkpoints_skipped += 1
    return actions


def snapshot_levels(snap: dict, cap: Capture) -> Iterable[tuple[int, int, int]]:
    for side, key in ((BUY, "bids"), (SELL, "asks")):
        for px, qty in snap["data"][key]:
            yield side, to_units(px, cap.tick), to_units(qty, cap.lot)


def convert(cap: Capture, out_path: str) -> Stats:
    stats = Stats()
    actions = plan(cap, stats)
    if not actions:
        raise ValueError("could not synchronise the depth stream with any snapshot")

    records: list[bytes] = []

    def emit(ts_ms: int, seq: int, price: int, qty: int, rtype: int, side: int = 0) -> None:
        records.append(RECORD.pack(ts_ms * 1_000_000, seq, price, qty, rtype, side))

    trades = cap.trades
    ti = 0
    start_ms = actions[0][2]
    while ti < len(trades) and trades[ti]["T"] < start_ms:
        ti += 1

    def flush_trades(until_ms: int) -> None:
        nonlocal ti
        while ti < len(trades) and trades[ti]["T"] <= until_ms:
            t = trades[ti]
            side = SELL if t["m"] else BUY  # m = buyer is maker -> seller aggressed
            emit(t["T"], t["t"], to_units(t["p"], cap.tick), to_units(t["q"], cap.lot), TRADE, side)
            stats.trades += 1
            ti += 1

    last_ts = start_ms
    for action in actions:
        if action[0] == "reset":
            _, snap, ts = action
            last_id = snap["data"]["lastUpdateId"]
            emit(ts, last_id, 0, 0, RESET)
            for side, px, qty in snapshot_levels(snap, cap):
                emit(ts, last_id, px, qty, LEVEL, side)
            emit(ts, last_id, 0, 0, EVENT_END)
        elif action[0] == "event":
            ev = action[1]
            last_ts = ev["E"]
            flush_trades(ev["E"])
            for side, key in ((BUY, "b"), (SELL, "a")):
                for px, qty in ev[key]:
                    emit(ev["E"], ev["u"], to_units(px, cap.tick), to_units(qty, cap.lot), LEVEL, side)
            emit(ev["E"], ev["u"], 0, 0, EVENT_END)
        else:
            _, cp, straddle, reset_snap = action
            last_id = cp["data"]["lastUpdateId"]
            # Only compare the price range covered by both the snapshot the
            # book was built from and this checkpoint (both are top-5000).
            bid_bound = max(
                to_units(cp["data"]["bids"][-1][0], cap.tick),
                to_units(reset_snap["data"]["bids"][-1][0], cap.tick),
            )
            ask_bound = min(
                to_units(cp["data"]["asks"][-1][0], cap.tick),
                to_units(reset_snap["data"]["asks"][-1][0], cap.tick),
            )
            emit(last_ts, last_id, bid_bound, 0, CP_BOUND, BUY)
            emit(last_ts, last_id, ask_bound, 0, CP_BOUND, SELL)
            if straddle is not None:
                for side, key in ((BUY, "b"), (SELL, "a")):
                    for px, _ in straddle[key]:
                        emit(last_ts, last_id, to_units(px, cap.tick), 0, CP_EXCLUDE, side)
            for side, px, qty in snapshot_levels(cp, cap):
                emit(last_ts, last_id, px, qty, CP_LEVEL, side)
            emit(last_ts, last_id, 0, 0, CP_END)

    with open(out_path, "wb") as f:
        f.write(
            HEADER.pack(MAGIC, cap.symbol.encode()[:16], float(cap.tick), float(cap.lot), len(records))
        )
        f.writelines(records)
    stats.records = len(records)
    return stats


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("raw", help="input .jsonl.gz from record.py")
    ap.add_argument("out", help="output .lob file")
    args = ap.parse_args()

    cap = load_capture(args.raw)
    stats = convert(cap, args.out)
    print(
        f"{cap.symbol}: tick {cap.tick} lot {cap.lot} | depth events {stats.events}, "
        f"trades {stats.trades}, resets {stats.resets}, gaps {stats.gaps}, "
        f"checkpoints {stats.checkpoints} (+{stats.checkpoints_skipped} skipped) "
        f"-> {stats.records} records",
        file=sys.stderr,
    )


if __name__ == "__main__":
    main()
