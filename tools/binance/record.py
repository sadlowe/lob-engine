#!/usr/bin/env python3
"""Record Binance spot market data as raw JSONL.gz.

Captures, for one symbol:
  * the diff-depth stream (<symbol>@depth@100ms),
  * the trade stream (<symbol>@trade),
  * full REST order book snapshots (limit=5000): one to sync, then periodic
    "checkpoint" snapshots used later to validate the reconstructed book.

Everything is stored exactly as received, together with a local receive
timestamp. The raw capture is the source of truth; normalisation and the
Binance sync protocol live in convert.py so they can be re-run and tested.

Uses Binance's public market-data-only endpoints (no API key required).

    python tools/binance/record.py --symbol BTCUSDT --minutes 5 --out data/btcusdt.jsonl.gz
"""

from __future__ import annotations

import argparse
import asyncio
import gzip
import json
import sys
import time
import urllib.request

import websockets

WS_URL = "wss://data-stream.binance.vision/stream?streams={s}@depth@100ms/{s}@trade"
REST_URL = "https://data-api.binance.vision/api/v3"


def http_get_json(url: str) -> dict:
    req = urllib.request.Request(url, headers={"User-Agent": "lob-engine-recorder"})
    with urllib.request.urlopen(req, timeout=15) as resp:
        return json.load(resp)


class Recorder:
    def __init__(self, symbol: str, out_path: str) -> None:
        self.symbol = symbol.upper()
        self.out = gzip.open(out_path, "wt", encoding="utf-8")
        self.counts = {"depth": 0, "trade": 0, "snapshot": 0, "reconnect": 0}

    def write(self, record_type: str, data: object, **extra: object) -> None:
        line = {"type": record_type, "recv_ns": time.time_ns(), **extra, "data": data}
        self.out.write(json.dumps(line, separators=(",", ":")) + "\n")

    async def snapshot(self, kind: str) -> None:
        url = f"{REST_URL}/depth?symbol={self.symbol}&limit=5000"
        snap = await asyncio.to_thread(http_get_json, url)
        self.write("snapshot", snap, kind=kind)
        self.counts["snapshot"] += 1

    async def snapshot_schedule(self, start: float, duration: float, every: float) -> None:
        # Let the stream buffer a little before the first snapshot, as the
        # Binance sync procedure requires events that precede the snapshot.
        await asyncio.sleep(1.5)
        await self.snapshot("initial")
        # Checkpoints at a fixed cadence (relative to the recording start),
        # ending with one at the very end so the whole capture is validated.
        targets = [float(t) for t in range(int(every), int(duration), int(every))]
        targets.append(duration)
        for t in targets:
            delay = start + t - time.monotonic()
            if delay > 0:
                await asyncio.sleep(delay)
            await self.snapshot("checkpoint")
        # A 5000-level snapshot can take seconds to arrive and may be ahead of
        # the websocket: keep streaming a little so its update id is covered.
        self.stop_at = time.monotonic() + 3.0

    async def stream(self) -> None:
        url = WS_URL.format(s=self.symbol.lower())
        while time.monotonic() < self.stop_at:
            try:
                async with websockets.connect(url, max_size=2**24, ping_interval=20) as ws:
                    while time.monotonic() < self.stop_at:
                        try:
                            msg = await asyncio.wait_for(ws.recv(), timeout=1.0)
                        except asyncio.TimeoutError:
                            continue
                        payload = json.loads(msg)
                        self.write("ws", payload)
                        stream = payload.get("stream", "")
                        self.counts["trade" if stream.endswith("@trade") else "depth"] += 1
            except (websockets.ConnectionClosed, OSError) as exc:
                # The converter detects the resulting sequence gap and
                # resynchronises on the next snapshot.
                self.counts["reconnect"] += 1
                self.write("reconnect", str(exc))
                await asyncio.sleep(1.0)

    async def run(self, duration: float, snapshot_every: float) -> None:
        info = await asyncio.to_thread(
            http_get_json, f"{REST_URL}/exchangeInfo?symbol={self.symbol}"
        )
        self.write("exchange_info", info["symbols"][0])

        start = time.monotonic()
        self.stop_at = float("inf")  # set by the snapshot schedule once it is done
        stream = asyncio.create_task(self.stream())
        snapshots = asyncio.create_task(self.snapshot_schedule(start, duration, snapshot_every))
        progress = asyncio.create_task(self.report(start + duration))
        try:
            pending = {stream, snapshots}
            while stream in pending:
                done, pending = await asyncio.wait(pending, return_when=asyncio.FIRST_COMPLETED)
                for task in done:
                    task.result()  # surface any failure immediately instead of losing it
        finally:
            for task in (stream, snapshots, progress):
                task.cancel()
            self.out.close()
        if self.counts["snapshot"] == 0:
            raise RuntimeError("no order book snapshot was recorded")

    async def report(self, deadline: float) -> None:
        while True:
            await asyncio.sleep(10)
            left = max(0, int(deadline - time.monotonic()))
            print(f"  {left:5d}s left  {self.counts}", file=sys.stderr, flush=True)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--symbol", default="BTCUSDT")
    ap.add_argument("--minutes", type=float, default=5.0)
    ap.add_argument("--snapshot-every", type=float, default=60.0, help="seconds between checkpoints")
    ap.add_argument("--out", required=True, help="output .jsonl.gz path")
    args = ap.parse_args()

    rec = Recorder(args.symbol, args.out)
    print(f"recording {rec.symbol} for {args.minutes} min -> {args.out}", file=sys.stderr)
    asyncio.run(rec.run(args.minutes * 60.0, args.snapshot_every))
    print(f"done: {rec.counts}", file=sys.stderr)


if __name__ == "__main__":
    main()
