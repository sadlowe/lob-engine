#!/usr/bin/env python3
"""Download Binance USD-M futures daily bookTicker dumps and verify their SHA-256.

    python tools/research/fetch_bookticker.py --out data/raw \
        --symbols BTCUSDT ETHUSDT --dates 2024-03-04 2024-03-05

Files come from the public archive https://data.binance.vision (no API key).
Each row is one change of the best bid / best ask:
    update_id,best_bid_price,best_bid_qty,best_ask_price,best_ask_qty,transaction_time,event_time
Already downloaded files whose checksum matches are skipped.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import sys
import urllib.request

BASE = "https://data.binance.vision/data/futures/um/daily/bookTicker"
UA = {"User-Agent": "lob-engine-research"}


def get(url: str) -> bytes:
    with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=60) as r:
        return r.read()


def sha256(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def fetch(symbol: str, date: str, out: str) -> str:
    name = f"{symbol}-bookTicker-{date}.zip"
    url = f"{BASE}/{symbol}/{name}"
    path = os.path.join(out, name)
    expected = get(url + ".CHECKSUM").decode().split()[0]
    if os.path.exists(path) and sha256(path) == expected:
        return f"{name}: cached"
    tmp = path + ".part"
    with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=60) as r, \
            open(tmp, "wb") as f:
        while chunk := r.read(1 << 20):
            f.write(chunk)
    if sha256(tmp) != expected:
        os.remove(tmp)
        raise RuntimeError(f"{name}: checksum mismatch")
    os.replace(tmp, path)
    return f"{name}: {os.path.getsize(path) / 1e6:.0f} MB, checksum ok"


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default="data/raw")
    ap.add_argument("--symbols", nargs="+", required=True)
    ap.add_argument("--dates", nargs="+", required=True)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    for date in args.dates:
        for symbol in args.symbols:
            print(fetch(symbol, date, args.out), flush=True)
    return None


if __name__ == "__main__":
    sys.exit(main())
