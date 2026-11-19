#!/usr/bin/env python3
"""Queue imbalance as a predictor of the next mid-price move on Binance USD-M futures.

Replicates the design of Gould & Bonart (2016), "Queue imbalance as a one-tick-ahead
price predictor in a limit order book" (Market Microstructure and Liquidity 2(2)),
on crypto perpetual futures, and asks whether the signal survives trading costs.

    python tools/research/queue_imbalance.py --raw data/raw --work build/research \
        --out docs/research

Pipeline:
  1. every data/raw/<SYMBOL>-bookTicker-<DATE>.zip is streamed through the C++
     extractor (build/lob_bbo_extract) into a 1-second grid of book states and the
     series of best-price changes (cached in --work);
  2. at each grid time t with a one-tick spread:  I(t) = (Vb - Va) / (Vb + Va),
     the target is the direction of the next mid-price change;
  3. per symbol and regime, a logistic model P(up) = sigmoid(a + b I) is fitted on
     the first day and evaluated on the following days (out of sample in time),
     then evaluated on the other regime (out of sample in regime);
  4. a taker rule (cross the spread in the direction of I when |I| > threshold,
     exit after h seconds) measures whether the predictability is tradable.

Uncertainty: 95% intervals from a moving-block bootstrap (blocks of 5 minutes) on
the test days, since consecutive samples share the same next move.

Writes results.json, results.md (the tables quoted in the README), calibration.png
auc_by_symbol.png and decay.png. Requires numpy and matplotlib.
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import re
import subprocess
from collections import defaultdict

import numpy as np

GRID = np.dtype([("ts", "<i8"), ("bid", "<i8"), ("ask", "<i8"), ("bq", "<f8"), ("aq", "<f8"), ("age", "<i8")])
CHANGES = np.dtype([("ts", "<i8"), ("bid", "<i8"), ("ask", "<i8")])
NAME = re.compile(r"(?P<sym>[A-Z0-9]+)-bookTicker-(?P<date>\d{4}-\d{2}-\d{2})\.zip$")

HORIZONS_S = (1, 10, 60)
LATENCIES_MS = (0, 50, 250, 1000)  # delay between observing I and reaching the book
REF_LATENCY_MS = 50
REF_HORIZON_S = 10
THRESHOLD = 0.8
WAIT_BUCKETS_S = ((0, 0.1), (0.1, 1), (1, 5), (5, float("inf")))
TAKER_FEE_BP = 5.0  # Binance USD-M VIP 0 taker fee (0.05%), per side
MAKER_FEE_BP = 2.0  # VIP 0 maker fee (0.02%)
BLOCK_S = 300
# Price tick (PRICE_FILTER tickSize) of each contract over the sample period.
# Rows quoting off this grid are dropped by the extractor and counted.
TICKS = {"BTCUSDT": "0.1", "ETHUSDT": "0.01", "SOLUSDT": "0.001", "XRPUSDT": "0.0001",
         "DOGEUSDT": "0.00001"}
MAX_OFF_TICK_SHARE = 1e-4
N_BOOT = 300
RNG = np.random.default_rng(20240304)


# ---------------------------------------------------------------------------
# Data
# ---------------------------------------------------------------------------

def extract(zip_path: str, prefix: str, binary: str, tick: str) -> None:
    if os.path.exists(prefix + "_meta.json"):
        return
    unzip = subprocess.Popen(["unzip", "-p", zip_path], stdout=subprocess.PIPE)
    subprocess.run([binary, "--out", prefix, "--tick", tick], stdin=unzip.stdout, check=True)
    unzip.stdout.close()
    if unzip.wait() != 0:
        raise RuntimeError(f"unzip failed on {zip_path}")


def regime_of(date: str) -> str:
    return date[:7]  # one regime per month in the default data set


class Day:
    """One symbol-day: grid samples with features and targets."""

    def __init__(self, prefix: str):
        with open(prefix + "_meta.json") as f:
            self.meta = json.load(f)
        if self.meta["off_tick_rows"] > max(10, MAX_OFF_TICK_SHARE * self.meta["rows"]):
            raise SystemExit(f"{prefix}: {self.meta['off_tick_rows']} rows off the tick grid, wrong tick size?")
        tick = self.meta["tick_e8"]
        g = np.fromfile(prefix + "_grid.bin", dtype=GRID)
        c = np.fromfile(prefix + "_changes.bin", dtype=CHANGES)
        self.tick_e8 = tick

        cts, cmid2 = c["ts"], c["bid"] + c["ask"]  # mid * 2, in 1e-8 units
        # For each change row, the index of the next row where the mid differs.
        moves = np.flatnonzero(np.diff(cmid2) != 0) + 1
        k = np.searchsorted(moves, np.arange(len(c)), side="right")
        nxt = np.where(k < len(moves), moves[np.minimum(k, len(moves) - 1)], -1)

        cur = np.searchsorted(cts, g["ts"], side="right") - 1  # row holding the state at t
        assert (cur >= 0).all()
        assert np.array_equal(c["bid"][cur], g["bid"]) and np.array_equal(c["ask"][cur], g["ask"])
        n = nxt[cur]

        spread = (g["ask"] - g["bid"]) // tick
        mid = (g["bid"] + g["ask"]) / 2.0
        depth = g["bq"] + g["aq"]
        self.all_spread_ticks = spread
        self.mid_usd_mean = float(mid.mean() * 1e-8)
        self.touch_depth_usd = float(np.median(depth * mid * 1e-8))

        keep = (spread == 1) & (n >= 0) & (depth > 0)
        self.ts = g["ts"][keep]
        self.imb = ((g["bq"] - g["aq"]) / depth)[keep]
        nn = n[keep]
        self.up = (cmid2[nn] > (g["bid"] + g["ask"])[keep]).astype(np.int8)
        self.wait_s = (cts[nn] - self.ts) / 1e3
        self.mid = mid[keep]
        self.bid = g["bid"][keep].astype(np.float64)
        self.ask = g["ask"][keep].astype(np.float64)
        self.cts = cts
        self.cbid = c["bid"].astype(np.float64)
        self.cask = c["ask"].astype(np.float64)
        self.n_grid = len(g)

    def book_at(self, t: np.ndarray) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
        """Best bid / ask in force at times t (last change at or before t), and
        whether t is still inside the recorded day."""
        j = np.searchsorted(self.cts, t, side="right") - 1
        return self.cbid[j], self.cask[j], t <= self.cts[-1]


def concat(days: list[Day], attr: str) -> np.ndarray:
    return np.concatenate([getattr(d, attr) for d in days])


# ---------------------------------------------------------------------------
# Statistics
# ---------------------------------------------------------------------------

def fit_logit(x: np.ndarray, y: np.ndarray, iters: int = 50) -> np.ndarray:
    X = np.column_stack([np.ones_like(x), x])
    w = np.zeros(2)
    for _ in range(iters):
        p = 1.0 / (1.0 + np.exp(-X @ w))
        grad = X.T @ (y - p)
        hess = (X * (p * (1 - p))[:, None]).T @ X
        step = np.linalg.solve(hess, grad)
        w += step
        if np.abs(step).max() < 1e-10:
            break
    return w


def predict(w: np.ndarray, x: np.ndarray) -> np.ndarray:
    return 1.0 / (1.0 + np.exp(-(w[0] + w[1] * x)))


def logloss(p: np.ndarray, y: np.ndarray) -> float:
    p = np.clip(p, 1e-12, 1 - 1e-12)
    return float(-np.mean(y * np.log(p) + (1 - y) * np.log(1 - p)))


def auc(score: np.ndarray, y: np.ndarray) -> float:
    """Mann-Whitney AUC with average ranks for ties."""
    _, inv, counts = np.unique(score, return_inverse=True, return_counts=True)
    first = np.cumsum(counts) - counts
    ranks = (first + (counts + 1) / 2.0)[inv]
    pos = y == 1
    n1, n0 = pos.sum(), (~pos).sum()
    return float((ranks[pos].sum() - n1 * (n1 + 1) / 2.0) / (n1 * n0))


def block_index(ts: np.ndarray) -> list[np.ndarray]:
    blocks = defaultdict(list)
    for i, b in enumerate((ts // (BLOCK_S * 1000)).tolist()):
        blocks[b].append(i)
    return [np.asarray(v) for v in blocks.values()]


def bootstrap(stat, blocks: list[np.ndarray]) -> tuple[float, float]:
    vals = []
    for _ in range(N_BOOT):
        pick = RNG.integers(0, len(blocks), len(blocks))
        idx = np.concatenate([blocks[i] for i in pick])
        vals.append(stat(idx))
    lo, hi = np.percentile(vals, [2.5, 97.5])
    return float(lo), float(hi)


# ---------------------------------------------------------------------------
# Study
# ---------------------------------------------------------------------------

def describe(days: list[Day]) -> dict:
    spread = np.concatenate([d.all_spread_ticks for d in days])
    tick_usd = days[0].tick_e8 * 1e-8
    mid = np.mean([d.mid_usd_mean for d in days])
    return {
        "tick_usd": tick_usd,
        "mean_mid_usd": float(mid),
        "tick_bp": float(tick_usd / mid * 1e4),
        "share_one_tick": float((spread == 1).mean()),
        "mean_spread_ticks": float(spread.mean()),
        "median_touch_depth_usd": float(np.median([d.touch_depth_usd for d in days])),
        "bbo_rows_per_day": float(np.mean([d.meta["rows"] for d in days])),
        "off_tick_rows": int(sum(d.meta["off_tick_rows"] for d in days)),
        "crossed_rows": int(sum(d.meta["crossed_rows"] for d in days)),
        "median_wait_s": float(np.median(concat(days, "wait_s"))),
    }


def evaluate(train: list[Day], test: list[Day], other: list[Day]) -> dict:
    xtr, ytr = concat(train, "imb"), concat(train, "up")
    xte, yte = concat(test, "imb"), concat(test, "up")
    w = fit_logit(xtr, ytr.astype(np.float64))
    base = ytr.mean()
    blocks = block_index(concat(test, "ts"))

    def stats(x, y, idx=None):
        if idx is not None:
            x, y = x[idx], y[idx]
        ll_model = logloss(predict(w, x), y)
        ll_base = logloss(np.full_like(x, base), y)
        return ll_model, ll_base

    ll_m, ll_b = stats(xte, yte)
    res = {
        "n_train": int(len(xtr)),
        "n_test": int(len(xte)),
        "coef": [float(w[0]), float(w[1])],
        "auc": auc(xte, yte),
        "auc_ci": bootstrap(lambda i: auc(xte[i], yte[i]), blocks),
        "acc_sign": float(((xte > 0) == (yte == 1))[xte != 0].mean()),
        "acc_sign_ci": bootstrap(lambda i: float(((xte[i] > 0) == (yte[i] == 1))[xte[i] != 0].mean()), blocks),
        "pseudo_r2": 1.0 - ll_m / ll_b,
        "pseudo_r2_ci": bootstrap(lambda i: 1.0 - np.divide(*stats(xte, yte, i)), blocks),
        "up_rate_test": float(yte.mean()),
    }
    # Same model, other regime: does the fitted calibration transfer?
    xo, yo = concat(other, "imb"), concat(other, "up")
    res["pseudo_r2_other_regime"] = 1.0 - logloss(predict(w, xo), yo) / logloss(np.full_like(xo, base), yo)
    # Per-day slope, to show stability.
    res["coef_by_day"] = [float(fit_logit(d.imb, d.up.astype(np.float64))[1]) for d in train + test]
    return res


def calibration(days: list[Day], bins: int = 20) -> dict:
    x, y = concat(days, "imb"), concat(days, "up")
    edges = np.linspace(-1, 1, bins + 1)
    b = np.clip(np.digitize(x, edges) - 1, 0, bins - 1)
    centers, rates, counts = [], [], []
    for i in range(bins):
        m = b == i
        if m.sum() >= 50:
            centers.append(float((edges[i] + edges[i + 1]) / 2))
            rates.append(float(y[m].mean()))
            counts.append(int(m.sum()))
    return {"centers": centers, "p_up": rates, "count": counts}


def taker_trades(days: list[Day], latency_ms: int, horizon_s: int, sign_fn=None):
    """Signal |I| > THRESHOLD observed at t; cross the spread in its direction at
    t + latency, unwind by crossing again horizon_s later.

    Returns (ts, mid-to-mid edge, gross PnL after paying the spread both ways),
    in bp of the mid at the signal."""
    ts, edge, gross = [], [], []
    for d in days:
        s = np.sign(d.imb) if sign_fn is None else sign_fn(d)
        t0 = d.ts + latency_ms
        t1 = t0 + horizon_s * 1000
        b0, a0, _ = d.book_at(t0)
        b1, a1, valid = d.book_at(t1)
        m = (np.abs(d.imb) > THRESHOLD) & valid & (s != 0)
        mid = d.mid[m]
        sm = s[m]
        edge.append(sm * ((b1[m] + a1[m]) - (b0[m] + a0[m])) / 2 / mid * 1e4)
        gross.append(np.where(sm > 0, b1[m] - a0[m], b0[m] - a1[m]) / mid * 1e4)
        ts.append(d.ts[m])
    return np.concatenate(ts), np.concatenate(edge), np.concatenate(gross)


def placebo_sign(d: Day) -> np.ndarray:
    """Sign of the imbalance one hour earlier: same marginal distribution, no
    information about the next move. Its edge should be ~0."""
    j = np.searchsorted(d.ts, d.ts - 3_600_000, side="right") - 1
    return np.where(j >= 0, np.sign(d.imb[np.maximum(j, 0)]), 0.0)


def taker(days: list[Day]) -> dict:
    def summary(ts, e, g):
        blocks = block_index(ts)
        lo, hi = bootstrap(lambda i: float(g[i].mean()), blocks)
        return {"n": int(len(e)), "mid_edge_bp": float(e.mean()), "gross_bp": float(g.mean()),
                "gross_ci": [lo, hi], "hit_rate": float((e > 0).mean()),
                "breakeven_taker_fee_bp_per_side": float(max(g.mean(), 0.0) / 2)}

    out = {"share_of_time": float(np.mean(np.concatenate([np.abs(d.imb) > THRESHOLD for d in days]))),
           "by_horizon": {}, "by_latency": {}}
    for h in HORIZONS_S:
        out["by_horizon"][h] = summary(*taker_trades(days, REF_LATENCY_MS, h))
    for lat in LATENCIES_MS:
        out["by_latency"][lat] = summary(*taker_trades(days, lat, REF_HORIZON_S))
    _, e, _ = taker_trades(days, 0, REF_HORIZON_S, placebo_sign)
    out["placebo_edge_bp"] = float(e.mean())
    return out


def auc_by_wait(days: list[Day]) -> list[dict]:
    """Diagnostic only (conditions on the future): AUC of I split by how long
    the market takes to make its next move."""
    x, y, w = concat(days, "imb"), concat(days, "up"), concat(days, "wait_s")
    out = []
    for lo, hi in WAIT_BUCKETS_S:
        m = (w >= lo) & (w < hi)
        out.append({"lo_s": lo, "hi_s": hi, "share": float(m.mean()),
                    "auc": auc(x[m], y[m]) if m.sum() > 100 else None})
    return out


def maker_side(days: list[Day], th: float = 0.5) -> dict:
    """Passive quote at the touch: how often does the next move go against it?

    A resting bid is hurt by a down move (it gets filled as the bid queue is
    consumed, then the price drops). Split by whether the bid side is thin."""
    x, y = concat(days, "imb"), concat(days, "up")
    thin_bid = x < -th  # our bid sits in the small queue
    thick_bid = x > th
    return {
        "threshold": th,
        "p_down_when_bid_queue_thin": float(1 - y[thin_bid].mean()),
        "p_down_when_bid_queue_thick": float(1 - y[thick_bid].mean()),
        "p_down_all": float(1 - y.mean()),
    }


# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------

def write_markdown(res: dict, path: str) -> None:
    regs = res["regimes"]
    lines = []
    lines.append("### Data\n")
    lines.append("| symbol | regime | tick | tick (bp) | 1-tick spread | median touch depth | BBO updates/day | median wait to next move |")
    lines.append("|---|---|---:|---:|---:|---:|---:|---:|")
    for sym in res["symbols"]:
        for r in regs:
            d = res["by"][sym][r]["describe"]
            lines.append(f"| {sym} | {r} | {d['tick_usd']:g} | {d['tick_bp']:.3f} | {d['share_one_tick']:.1%} | "
                         f"${d['median_touch_depth_usd']:,.0f} | {d['bbo_rows_per_day'] / 1e6:.1f}M | {d['median_wait_s']:.2f} s |")
    lines.append("\n### Predictive power (out of sample: fit on day 1, test on days 2-3)\n")
    lines.append("| symbol | regime | test samples | AUC [95% CI] | accuracy of sign(I) [95% CI] | pseudo-R² [95% CI] | pseudo-R² on other regime | slope b by day |")
    lines.append("|---|---|---:|---|---|---|---:|---|")
    for sym in res["symbols"]:
        for r in regs:
            e = res["by"][sym][r]["eval"]
            lines.append(
                f"| {sym} | {r} | {e['n_test']:,} | {e['auc']:.3f} [{e['auc_ci'][0]:.3f}, {e['auc_ci'][1]:.3f}] | "
                f"{e['acc_sign']:.1%} [{e['acc_sign_ci'][0]:.1%}, {e['acc_sign_ci'][1]:.1%}] | "
                f"{e['pseudo_r2']:.3f} [{e['pseudo_r2_ci'][0]:.3f}, {e['pseudo_r2_ci'][1]:.3f}] | "
                f"{e['pseudo_r2_other_regime']:.3f} | {', '.join(f'{c:.2f}' for c in e['coef_by_day'])} |")
    lines.append("\n### Why some symbols look weaker: predictive power vs time to the next move\n")
    lines.append("Diagnostic only: it conditions on the future (the wait), so it is not a usable model. "
                 "Cells: AUC (share of samples).\n")
    lines.append("| symbol | " + " | ".join(
        f"next move in {b[0]:g}-{b[1]:g} s" if b[1] != float("inf") else f"> {b[0]:g} s" for b in WAIT_BUCKETS_S) + " |")
    lines.append("|---|" + "---:|" * len(WAIT_BUCKETS_S))
    for sym in res["symbols"]:
        cells = [f"{c['auc']:.3f} ({c['share']:.0%})" if c["auc"] is not None else "-" for c in res["by"][sym]["auc_by_wait"]]
        lines.append(f"| {sym} | " + " | ".join(cells) + " |")

    lines.append(f"\n### Is it tradable? Taker rule |I| > {THRESHOLD:g}, test days of both regimes\n")
    lines.append(f"Enter by crossing the spread {REF_LATENCY_MS} ms after the signal, unwind by crossing again. "
                 "Edge = mid-to-mid move in the signal direction; gross = after paying the spread both ways; "
                 f"net = after the VIP 0 taker fee ({TAKER_FEE_BP:g} bp per side). bp of the mid.\n")
    lines.append("| symbol | signal on | horizon | hit rate | edge (bp) | gross (bp) [95% CI] | net (bp) | break-even taker fee per side (bp) |")
    lines.append("|---|---:|---:|---:|---:|---|---:|---:|")
    for sym in res["symbols"]:
        tk = res["by"][sym]["taker"]
        for h in HORIZONS_S:
            t = tk["by_horizon"][h]
            lines.append(f"| {sym} | {tk['share_of_time']:.0%} | {h} s | {t['hit_rate']:.1%} | {t['mid_edge_bp']:+.3f} | "
                         f"{t['gross_bp']:+.3f} [{t['gross_ci'][0]:+.3f}, {t['gross_ci'][1]:+.3f}] | "
                         f"{t['gross_bp'] - 2 * TAKER_FEE_BP:+.2f} | {t['breakeven_taker_fee_bp_per_side']:.2f} |")
    lines.append(f"\n### How fast the edge decays with reaction latency ({REF_HORIZON_S} s horizon)\n")
    lines.append("| symbol | " + " | ".join(f"edge / gross at {lat} ms" for lat in LATENCIES_MS) + " | placebo edge (I from 1 h earlier) |")
    lines.append("|---|" + "---:|" * (len(LATENCIES_MS) + 1))
    for sym in res["symbols"]:
        tk = res["by"][sym]["taker"]
        cells = [f"{tk['by_latency'][lat]['mid_edge_bp']:+.3f} / {tk['by_latency'][lat]['gross_bp']:+.3f}" for lat in LATENCIES_MS]
        lines.append(f"| {sym} | " + " | ".join(cells) + f" | {tk['placebo_edge_bp']:+.3f} |")
    lines.append("\n### What it means for a market maker (|I| > 0.5, test days)\n")
    lines.append("| symbol | P(next move down) when our bid is in the thin queue | when it is in the thick queue | unconditional |")
    lines.append("|---|---:|---:|---:|")
    for sym in res["symbols"]:
        m = res["by"][sym]["maker"]
        lines.append(f"| {sym} | {m['p_down_when_bid_queue_thin']:.1%} | {m['p_down_when_bid_queue_thick']:.1%} | {m['p_down_all']:.1%} |")
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


def end_labels(ax, x: float, items: list[tuple[float, str]], color: str, gap_frac: float = 0.045) -> None:
    """Direct labels at the end of lines, pushed apart vertically so they never overlap."""
    lo, hi = ax.get_ylim()
    gap = (hi - lo) * gap_frac
    items = sorted(items)
    placed: list[float] = []
    for y, _ in items:
        placed.append(max(y, placed[-1] + gap) if placed else y)
    # Re-centre the stack on the original values so labels stay near their lines.
    shift = (sum(y for y, _ in items) - sum(placed)) / len(placed)
    for (y, text), py in zip(items, placed):
        ax.annotate(text, (x, y), xytext=(x, py + shift), textcoords="data", va="center", fontsize=8,
                    color=color, annotation_clip=False)


def plot(res: dict, out: str) -> None:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    surface, text2, muted, grid = "#fcfcfb", "#52514e", "#898781", "#e1e0d9"
    series = ["#2a78d6", "#eb6834"]
    plt.rcParams.update({
        "figure.facecolor": surface, "axes.facecolor": surface, "savefig.facecolor": surface,
        "axes.edgecolor": grid, "axes.labelcolor": text2, "axes.titlecolor": "#0b0b0b",
        "axes.titlesize": 10, "axes.titleweight": "bold", "axes.titlelocation": "left",
        "axes.grid": True, "axes.axisbelow": True, "grid.color": grid, "grid.linewidth": 0.8,
        "axes.spines.top": False, "axes.spines.right": False, "xtick.color": muted,
        "ytick.color": muted, "xtick.labelcolor": text2, "ytick.labelcolor": text2,
        "font.size": 9, "legend.frameon": False, "legend.labelcolor": text2,
        "lines.linewidth": 2, "lines.solid_capstyle": "round",
    })
    syms, regs = res["symbols"], res["regimes"]

    # 1. Calibration: empirical P(up | I) per symbol (small multiples), both regimes.
    fig, axes = plt.subplots(1, len(syms), figsize=(2.3 * len(syms), 3.2), sharey=True)
    xs = np.linspace(-1, 1, 200)
    for ax, sym in zip(np.atleast_1d(axes), syms):
        for r, color in zip(regs, series):
            cal = res["by"][sym][r]["calibration"]
            a, b = res["by"][sym][r]["eval"]["coef"]
            ax.plot(xs, 1 / (1 + np.exp(-(a + b * xs))), color=color, lw=1.2, alpha=0.6)
            ax.plot(cal["centers"], cal["p_up"], "o", ms=4, color=color, mec=surface, mew=1,
                    label=r)
        ax.axhline(0.5, color=muted, lw=0.8)
        ax.set_title(sym.replace("USDT", ""))
        ax.set_xlim(-1.05, 1.05)
        ax.set_ylim(0, 1)
        ax.set_xticks([-1, 0, 1])
        ax.set_xlabel("imbalance I")
    np.atleast_1d(axes)[0].set_ylabel("P(next mid move is up)")
    handles, labels = np.atleast_1d(axes)[0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="upper right", ncol=2, bbox_to_anchor=(1.0, 1.0))
    fig.suptitle("The bigger queue wins: P(next move up) vs queue imbalance at the touch",
                 x=0.01, ha="left", fontsize=11, fontweight="bold")
    fig.text(0.01, 0.875, "Binance USD-M perpetuals, 1 s samples with a one-tick spread; dots = 5% bins, "
             "lines = logistic fit on day 1", fontsize=8.5, color=muted)
    fig.tight_layout(rect=(0, 0, 1, 0.9))
    fig.savefig(os.path.join(out, "calibration.png"), dpi=160)
    plt.close(fig)

    # 2. AUC with bootstrap CI per symbol and regime (dot plot).
    fig, ax = plt.subplots(figsize=(7.5, 3.4))
    for k, (r, color) in enumerate(zip(regs, series)):
        ys = np.arange(len(syms)) + (k - 0.5) * 0.25
        for y, sym in zip(ys, syms):
            e = res["by"][sym][r]["eval"]
            ax.plot(e["auc_ci"], [y, y], color=color, lw=2)
            ax.plot([e["auc"]], [y], "o", ms=8, color=color, mec=surface, mew=2,
                    label=r if sym == syms[0] else None)
            ax.annotate(f"{e['auc']:.3f}", (e["auc_ci"][1], y), xytext=(6, 0), textcoords="offset points",
                        va="center", fontsize=8, color=text2)
    labels = [f"{s.replace('USDT', '')}  (next move in {res['by'][s][regs[0]]['describe']['median_wait_s']:.1f} s "
              f"/ {res['by'][s][regs[1]]['describe']['median_wait_s']:.1f} s)" for s in syms]
    ax.set_yticks(np.arange(len(syms)), labels)
    ax.invert_yaxis()
    ax.axvline(0.5, color=muted, lw=1)
    ax.set_xlabel("out-of-sample AUC of I for the direction of the next mid move (0.5 = coin flip)")
    ax.legend(loc="lower right")
    fig.tight_layout(rect=(0, 0, 1, 0.92))
    fig.text(0.01, 0.97, "Out-of-sample AUC of the queue imbalance, by contract and regime (95% block-bootstrap CI)",
             fontsize=10, fontweight="bold", va="top")
    fig.savefig(os.path.join(out, "auc_by_symbol.png"), dpi=160)
    plt.close(fig)

    # 3. Decay: AUC vs time to the next move, and taker edge vs reaction latency.
    five = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4"]
    fig, (a1, a2) = plt.subplots(1, 2, figsize=(10, 4.2))
    xs = np.arange(len(WAIT_BUCKETS_S))
    for sym, color in zip(syms, five):
        ys = [c["auc"] for c in res["by"][sym]["auc_by_wait"]]
        a1.plot(xs, ys, color=color, marker="o", ms=6, mec=surface, mew=1.5, label=sym.replace("USDT", ""))
    a1.axhline(0.5, color=muted, lw=1)
    a1.set_xticks(xs, ["< 0.1 s", "0.1-1 s", "1-5 s", "> 5 s"])
    a1.set_xlim(-0.2, len(xs) - 0.35)
    a1.set_ylim(0.48, 0.95)
    end_labels(a1, xs[-1] + 0.12, [(res["by"][sym]["auc_by_wait"][-1]["auc"], sym.replace("USDT", "")) for sym in syms],
               text2)
    a1.set_xlabel("time until the next mid move")
    a1.set_ylabel("AUC (0.5 = coin flip)")
    a1.set_title("The queue predicts the move it is about to cause")
    lats = list(LATENCIES_MS)
    for sym, color in zip(syms, five):
        ys = [res["by"][sym]["taker"]["by_latency"][lat]["mid_edge_bp"] for lat in lats]
        a2.plot(range(len(lats)), ys, color=color, marker="o", ms=6, mec=surface, mew=1.5)
    a2.axhline(0, color=muted, lw=1)
    a2.set_xticks(range(len(lats)), [f"{lat} ms" for lat in lats])
    a2.set_xlim(-0.2, len(lats) - 0.35)
    a2.set_ylim(0, None)
    end_labels(a2, len(lats) - 1 + 0.12,
               [(res["by"][sym]["taker"]["by_latency"][lats[-1]]["mid_edge_bp"], sym.replace("USDT", "")) for sym in syms],
               text2)
    a2.set_xlabel("delay between seeing the signal and trading")
    a2.set_ylabel("edge over 10 s, bp of mid")
    a2.set_title(f"...and the edge is gone fast (taker fee: {TAKER_FEE_BP:g} bp per side)")
    handles, labels = a1.get_legend_handles_labels()
    fig.legend(handles, labels, loc="upper center", ncol=len(syms), bbox_to_anchor=(0.5, 1.0))
    fig.tight_layout(rect=(0, 0, 1, 0.93))
    fig.savefig(os.path.join(out, "decay.png"), dpi=160)
    plt.close(fig)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--raw", default="data/raw")
    ap.add_argument("--work", default="build/research")
    ap.add_argument("--out", default="docs/research")
    ap.add_argument("--binary", default="build/lob_bbo_extract")
    ap.add_argument("--no-plots", action="store_true")
    args = ap.parse_args()
    os.makedirs(args.work, exist_ok=True)
    os.makedirs(args.out, exist_ok=True)

    files = defaultdict(lambda: defaultdict(list))  # sym -> regime -> [(date, prefix)]
    for z in sorted(glob.glob(os.path.join(args.raw, "*-bookTicker-*.zip"))):
        m = NAME.search(os.path.basename(z))
        if not m:
            continue
        prefix = os.path.join(args.work, f"{m['sym']}-{m['date']}")
        if m["sym"] not in TICKS:
            raise SystemExit(f"no tick size known for {m['sym']}, add it to TICKS")
        extract(z, prefix, args.binary, TICKS[m["sym"]])
        files[m["sym"]][regime_of(m["date"])].append((m["date"], prefix))

    # Order symbols by relative tick size (largest first) once loaded.
    res = {"by": {}, "regimes": None, "symbols": None,
           "settings": {"taker_fee_bp": TAKER_FEE_BP, "maker_fee_bp": MAKER_FEE_BP, "block_s": BLOCK_S,
                        "n_boot": N_BOOT, "horizons_s": HORIZONS_S, "latencies_ms": LATENCIES_MS,
                        "threshold": THRESHOLD}}
    regimes = sorted({r for s in files.values() for r in s})
    if len(regimes) != 2:
        raise SystemExit(f"expected exactly two regimes (months), found {regimes}")
    res["regimes"] = regimes
    for sym, by_reg in files.items():
        days = {r: [Day(p) for _, p in sorted(by_reg[r])] for r in regimes}
        if any(len(v) < 2 for v in days.values()):
            raise SystemExit(f"{sym}: need at least two days per regime")
        out = {}
        for r in regimes:
            other = days[regimes[1] if r == regimes[0] else regimes[0]]
            out[r] = {
                "describe": describe(days[r]),
                "eval": evaluate(days[r][:1], days[r][1:], other[1:]),
                "calibration": calibration(days[r][1:]),
            }
        test_days = [d for r in regimes for d in days[r][1:]]
        out["taker"] = taker(test_days)
        out["auc_by_wait"] = auc_by_wait(test_days)
        out["maker"] = maker_side(test_days)
        res["by"][sym] = out
        e = [out[r]["eval"] for r in regimes]
        print(f"{sym}: AUC {e[0]['auc']:.3f} / {e[1]['auc']:.3f}, sign accuracy "
              f"{e[0]['acc_sign']:.1%} / {e[1]['acc_sign']:.1%}", flush=True)

    # Fastest market first: the time to the next move is what drives predictability.
    res["symbols"] = sorted(res["by"], key=lambda s: sum(res["by"][s][r]["describe"]["median_wait_s"] for r in regimes))
    with open(os.path.join(args.out, "results.json"), "w") as f:
        json.dump(res, f, indent=1)
    write_markdown(res, os.path.join(args.out, "results.md"))
    if not args.no_plots:
        plot(res, args.out)
    print(f"wrote results.json, results.md{'' if args.no_plots else ', calibration.png, auc_by_symbol.png, decay.png'} to {args.out}")


if __name__ == "__main__":
    main()
