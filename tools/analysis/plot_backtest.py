#!/usr/bin/env python3
"""Run the market-making backtest for several strategies and plot the results.

    python tools/analysis/plot_backtest.py build/btcusdt_1h.lob --out docs/img

Produces:
  pnl_by_strategy.png   cumulative net PnL of each strategy over time
  markouts.png          average markout curve (adverse selection) per strategy
  as_anatomy.png        Avellaneda-Stoikov run: price, inventory, PnL decomposition

Requires matplotlib (pip install -r tools/analysis/requirements.txt).
"""

from __future__ import annotations

import argparse
import bisect
import csv
import os
import subprocess

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

# Reference categorical palette (validated: CVD-safe adjacent pairs, light surface).
SURFACE = "#fcfcfb"
TEXT = "#0b0b0b"
TEXT_2 = "#52514e"
MUTED = "#898781"
GRID = "#e1e0d9"
SERIES = ["#2a78d6", "#eb6834", "#1baf7a"]
TICK = 0.01  # BTCUSDT price tick (markouts are written in ticks)

STRATEGIES = [
    ("Join touch", ["--strategy", "touch"]),
    ("Touch + imbalance filter", ["--strategy", "imbalance", "--imbalance", "0.25"]),
    ("Avellaneda-Stoikov", ["--strategy", "as", "--gamma", "1e-4", "--microprice"]),
]


def style() -> None:
    plt.rcParams.update({
        "figure.facecolor": SURFACE,
        "axes.facecolor": SURFACE,
        "savefig.facecolor": SURFACE,
        "axes.edgecolor": GRID,
        "axes.labelcolor": TEXT_2,
        "axes.titlecolor": TEXT,
        "axes.titlesize": 12,
        "axes.titleweight": "bold",
        "axes.titlelocation": "left",
        "axes.labelsize": 10,
        "axes.grid": True,
        "axes.axisbelow": True,
        "grid.color": GRID,
        "grid.linewidth": 0.8,
        "axes.spines.top": False,
        "axes.spines.right": False,
        "xtick.color": MUTED,
        "ytick.color": MUTED,
        "xtick.labelcolor": TEXT_2,
        "ytick.labelcolor": TEXT_2,
        "font.size": 10,
        "legend.frameon": False,
        "legend.labelcolor": TEXT_2,
        "lines.linewidth": 2,
        "lines.solid_capstyle": "round",
    })


def run(binary: str, lob: str, args: list[str], prefix: str, fee_bps: float) -> None:
    subprocess.run([binary, lob, *args, "--fee-bps", str(fee_bps), "--out", prefix],
                   check=True, stdout=subprocess.DEVNULL)


def load(prefix: str) -> tuple[list[dict], list[dict]]:
    with open(prefix + "_samples.csv") as f:
        samples = [{k: (float(v) if v else None) for k, v in r.items()} for r in csv.DictReader(f)]
    with open(prefix + "_fills.csv") as f:
        fills = [dict(r) for r in csv.DictReader(f)]
    return samples, fills


def minutes(samples: list[dict]) -> list[float]:
    t0 = samples[0]["ts_ns"]
    return [(s["ts_ns"] - t0) / 60e9 for s in samples]


def usd(x: float) -> str:
    return f"{'-' if x < 0 else '+'}${abs(x):.2f}"


def titles(ax, title: str, subtitle: str) -> None:
    ax.set_title(title, pad=26)
    ax.text(0, 1.025, subtitle, transform=ax.transAxes, fontsize=9, color=MUTED, va="bottom")


def end_label(ax, x: float, y: float, text: str, color: str, dy: float = 0.0) -> None:
    ax.plot([x], [y], "o", ms=5, color=color, mec=SURFACE, mew=1.5, zorder=4)
    ax.annotate(text, (x, y), xytext=(8, dy), textcoords="offset points",
                va="center", fontsize=9, color=TEXT_2)


def plot_pnl(results: dict, out: str, hours_label: str, fee_bps: float) -> None:
    fig, ax = plt.subplots(figsize=(9, 4.8))
    finals = []
    for (name, _), color in zip(STRATEGIES, SERIES):
        samples, _ = results[name]
        xs, ys = minutes(samples), [s["pnl"] for s in samples]
        ax.plot(xs, ys, color=color, label=name)
        finals.append((ys[-1], xs[-1], name, color))
    # Spread end labels vertically so they never collide.
    finals.sort()
    span = (max(f[0] for f in finals) - min(f[0] for f in finals)) or 1.0
    last_y = None
    for y, x, name, color in finals:
        dy = 0.0
        if last_y is not None and abs(y - last_y) < 0.06 * span:
            dy = 12.0
        end_label(ax, x, y, f"{name}  {usd(y)}", color, dy)
        last_y = y
    ax.axhline(0, color=MUTED, lw=1)
    titles(ax, f"Cumulative net PnL, BTC/USDT, {hours_label} of real Binance data",
           f"0.001 BTC quotes, 20 ms latency, maker fee {fee_bps:g} bp, marked to mid")
    ax.set_xlabel("minutes")
    ax.set_ylabel("USD")
    ax.set_xlim(right=max(f[1] for f in finals) * 1.32)
    ax.legend(loc="lower left")
    fig.tight_layout()
    fig.savefig(os.path.join(out, "pnl_by_strategy.png"), dpi=160)
    plt.close(fig)


def markout_curve(fills: list[dict]) -> list[float]:
    """Qty-weighted average markout in USD per BTC at 0 (edge), 1 s, 5 s, 30 s."""
    out = []
    for col in (None, "markout_1s", "markout_5s", "markout_30s"):
        acc = w = 0.0
        for f in fills:
            q = float(f["qty"])
            if col is None:
                sign = 1.0 if f["side"] == "buy" else -1.0
                acc += sign * (float(f["mid"]) - float(f["price"])) * q
            elif f[col]:  # empty = horizon not reached before the end of the data
                acc += float(f[col]) * TICK * q
            else:
                continue
            w += q
        out.append(acc / w if w else 0.0)
    return out


def plot_markouts(results: dict, out: str) -> None:
    fig, ax = plt.subplots(figsize=(9, 4.8))
    xs = [0, 1, 2, 3]
    ends = []
    for (name, _), color in zip(STRATEGIES, SERIES):
        _, fills = results[name]
        ys = markout_curve(fills)
        ax.plot(xs, ys, color=color, marker="o", ms=8, mec=SURFACE, mew=2, label=name)
        ends.append((ys[-1], name))
    # Nudge end labels apart when their values are close.
    ends.sort()
    span = (ends[-1][0] - ends[0][0]) or 1.0
    prev = None
    for y, name in ends:
        dy = 9.0 if prev is not None and abs(y - prev) < 0.15 * span else 0.0
        ax.annotate(f"{name}  {usd(y)}", (xs[-1], y), xytext=(10, dy),
                    textcoords="offset points", va="center", fontsize=9, color=TEXT_2)
        prev = y
    ax.axhline(0, color=MUTED, lw=1)
    ax.set_xticks(xs, ["at fill\n(edge vs mid)", "1 s", "5 s", "30 s"])
    ax.set_xlim(-0.2, 4.3)
    titles(ax, "Markouts: where the money goes after each fill",
           "Mid-price move after each fill, in our favour (+) or against us (-), "
           "USD per BTC traded, qty-weighted")
    ax.set_ylabel("USD per BTC")
    ax.legend(loc="lower left")
    fig.tight_layout()
    fig.savefig(os.path.join(out, "markouts.png"), dpi=160)
    plt.close(fig)


def plot_anatomy(samples: list[dict], fills: list[dict], out: str, name: str, label: str) -> None:
    xs = minutes(samples)
    # Cumulative spread capture (edge vs mid at fill) aligned on the samples.
    fts, cum, acc = [], [], 0.0
    for f in fills:
        sign = 1.0 if f["side"] == "buy" else -1.0
        acc += sign * (float(f["mid"]) - float(f["price"])) * float(f["qty"])
        fts.append(int(f["ts_ns"]))
        cum.append(acc)

    def spread_at(ts: float) -> float:
        i = bisect.bisect_right(fts, ts) - 1
        return cum[i] if i >= 0 else 0.0

    spread = [spread_at(s["ts_ns"]) for s in samples]
    total = [s["pnl"] for s in samples]
    inv_pnl = [t - c for t, c in zip(total, spread)]

    fig, axes = plt.subplots(3, 1, figsize=(9, 8), sharex=True,
                             gridspec_kw={"height_ratios": [1.1, 0.9, 1.2]})
    ax = axes[0]
    ax.plot(xs, [s["mid"] for s in samples], color=SERIES[0])
    ax.set_title(f"{name}: anatomy of the run ({label})")
    ax.set_ylabel("BTC mid (USD)")
    ax.ticklabel_format(axis="y", useOffset=False, style="plain")

    ax = axes[1]
    ax.fill_between(xs, [s["inventory"] for s in samples], 0, step="post",
                    color=SERIES[0], alpha=0.25, lw=0)
    ax.step(xs, [s["inventory"] for s in samples], where="post", color=SERIES[0], lw=1.5)
    ax.axhline(0, color=MUTED, lw=1)
    ax.set_ylabel("inventory (BTC)")

    ax = axes[2]
    for ys, series_label, color in ((spread, "spread capture", SERIES[2]),
                             (inv_pnl, "inventory PnL (adverse selection)", SERIES[1]),
                             (total, "net PnL", SERIES[0])):
        ax.plot(xs, ys, color=color, label=series_label)
        end_label(ax, xs[-1], ys[-1], f"{series_label}  {usd(ys[-1])}", color)
    ax.axhline(0, color=MUTED, lw=1)
    ax.set_ylabel("USD")
    ax.set_xlabel("minutes")
    ax.set_xlim(right=xs[-1] * 1.42)
    ax.legend(loc="lower left")
    fig.tight_layout()
    fig.savefig(os.path.join(out, "as_anatomy.png"), dpi=160)
    plt.close(fig)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("lob", help=".lob recording")
    ap.add_argument("--binary", default="build/lob_mm_backtest")
    ap.add_argument("--out", default="docs/img")
    ap.add_argument("--work", default="build/plots")
    ap.add_argument("--fee-bps", type=float, default=0.0)
    ap.add_argument("--label", default="1 hour")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    os.makedirs(args.work, exist_ok=True)
    style()

    results = {}
    for i, (name, flags) in enumerate(STRATEGIES):
        prefix = os.path.join(args.work, f"s{i}")
        run(args.binary, args.lob, flags, prefix, args.fee_bps)
        results[name] = load(prefix)

    plot_pnl(results, args.out, args.label, args.fee_bps)
    plot_markouts(results, args.out)
    as_name = STRATEGIES[2][0]
    plot_anatomy(*results[as_name], args.out, as_name, args.label)
    print(f"wrote pnl_by_strategy.png, markouts.png, as_anatomy.png to {args.out}")


if __name__ == "__main__":
    main()
