#!/usr/bin/env python3
from __future__ import annotations

import csv
import math
import os
from pathlib import Path
from typing import Dict, List, Optional, Tuple

import matplotlib.pyplot as plt


def ensure_dir(p: Path) -> None:
    p.mkdir(parents=True, exist_ok=True)


def read_csv_rows(path: Path) -> List[dict]:
    rows: List[dict] = []
    if not path.exists():
        return rows
    with path.open("r", newline="") as f:
        reader = csv.DictReader(f)
        for r in reader:
            if not r:
                continue
            # Skip duplicated header lines (common when appending CSV)
            if any(k == v for k, v in r.items() if k is not None and v is not None):
                continue
            rows.append(r)
    return rows


def to_float(x: Optional[str], default: float = float("nan")) -> float:
    if x is None:
        return default
    s = str(x).strip()
    if s == "" or s.lower() == "nan":
        return default
    try:
        return float(s)
    except ValueError:
        return default


def to_int(x: Optional[str], default: int = 0) -> int:
    if x is None:
        return default
    s = str(x).strip()
    if s == "":
        return default
    try:
        return int(float(s))
    except ValueError:
        return default


def quantile(values: List[float], q: float) -> float:
    """Simple quantile without numpy; values must be non-empty."""
    v = sorted(values)
    if len(v) == 1:
        return v[0]
    q = max(0.0, min(1.0, q))
    pos = q * (len(v) - 1)
    lo = int(math.floor(pos))
    hi = int(math.ceil(pos))
    if lo == hi:
        return v[lo]
    w = pos - lo
    return v[lo] * (1.0 - w) + v[hi] * w


def robust_vmin_vmax(values: List[float]) -> Tuple[float, float]:
    """Return a contrast-friendly [vmin, vmax]."""
    clean = [x for x in values if not math.isnan(x)]
    if not clean:
        return (0.0, 1.0)
    vmin = min(clean)
    vmax = max(clean)
    if vmax == vmin:
        return (vmin - 0.5, vmax + 0.5)

    q05 = quantile(clean, 0.05)
    q95 = quantile(clean, 0.95)

    # If spread is very small, keep min/max; else use percentile window.
    if (q95 - q05) <= 0.1 * (vmax - vmin):
        return (vmin, vmax)

    return (q05, q95)


def build_grid(
    rows: List[dict],
    x_key: str,
    y_key: str,
    val_key: str,
    val_transform=lambda x: x,
    invalid_pred=lambda x: False,
) -> Tuple[List[float], List[float], List[List[Optional[float]]]]:
    pts: Dict[Tuple[float, float], float] = {}

    xs: List[float] = []
    ys: List[float] = []

    for r in rows:
        x = to_float(r.get(x_key), float("nan"))
        y = to_float(r.get(y_key), float("nan"))
        if math.isnan(x) or math.isnan(y):
            continue
        xs.append(x)
        ys.append(y)

        raw = to_float(r.get(val_key), float("nan"))
        if math.isnan(raw) or invalid_pred(raw):
            pts[(x, y)] = float("nan")
        else:
            pts[(x, y)] = val_transform(raw)

    xs_u = sorted(set(xs))
    ys_u = sorted(set(ys))

    grid: List[List[Optional[float]]] = []
    for yy in ys_u:
        row_vals: List[Optional[float]] = []
        for xx in xs_u:
            v = pts.get((xx, yy), float("nan"))
            if math.isnan(v):
                row_vals.append(None)
            else:
                row_vals.append(v)
        grid.append(row_vals)

    return xs_u, ys_u, grid


def grid_to_values(grid: List[List[Optional[float]]]) -> List[float]:
    out: List[float] = []
    for row in grid:
        for v in row:
            if v is None:
                continue
            out.append(v)
    return out


def draw_heatmap(
    xs: List[float],
    ys: List[float],
    grid: List[List[Optional[float]]],
    title: str,
    cbar_label: str,
    out_png: Path,
    annotate: bool = True,
) -> None:
    values = grid_to_values(grid)
    if not values:
        return

    vmin, vmax = robust_vmin_vmax(values)

    # Convert grid (with None) to a masked-like structure using NaN
    data: List[List[float]] = []
    for row in grid:
        data.append([float("nan") if v is None else float(v) for v in row])

    plt.close("all")
    fig, ax = plt.subplots(figsize=(10, 6), dpi=150)

    # Determine cell boundaries so ticks align to actual x/y positions
    # Assumes uniform step; if not uniform, we still label by tick positions.
    im = ax.imshow(
        data,
        origin="lower",
        aspect="auto",
        interpolation="nearest",
        vmin=vmin,
        vmax=vmax,
    )

    ax.set_title(title, pad=12)
    ax.set_xlabel("x (m)")
    ax.set_ylabel("y (m)")

    ax.set_xticks(list(range(len(xs))))
    ax.set_xticklabels([f"{x:g}" for x in xs])
    ax.set_yticks(list(range(len(ys))))
    ax.set_yticklabels([f"{y:g}" for y in ys])

    # Light grid between cells
    ax.set_xticks([i - 0.5 for i in range(1, len(xs))], minor=True)
    ax.set_yticks([i - 0.5 for i in range(1, len(ys))], minor=True)
    ax.grid(which="minor", linestyle="-", linewidth=0.5, alpha=0.35)
    ax.tick_params(which="minor", bottom=False, left=False)

    cbar = fig.colorbar(im, ax=ax)
    cbar.set_label(cbar_label)

    # Annotate values if grid is small enough
    if annotate and (len(xs) * len(ys) <= 64):
        for yi in range(len(ys)):
            for xi in range(len(xs)):
                v = grid[yi][xi]
                if v is None:
                    continue
                ax.text(
                    xi,
                    yi,
                    f"{v:.3f}",
                    ha="center",
                    va="center",
                    fontsize=8,
                    alpha=0.9,
                )

    fig.tight_layout()
    fig.savefig(out_png, dpi=300, bbox_inches="tight")
    plt.close(fig)


def draw_scatter_rtt_goodput(
    heat_rows: List[dict],
    out_png: Path,
    ap_x: float = 0.0,
    ap_y: float = 0.0,
) -> None:
    pts: List[Tuple[float, float, float]] = []  # (goodput_mbps, rtt_ms, dist_m)
    for r in heat_rows:
        x = to_float(r.get("x"), float("nan"))
        y = to_float(r.get("y"), float("nan"))
        gp_bps = to_float(r.get("goodputbps"), float("nan"))
        rtt = to_float(r.get("rttMeanMs"), float("nan"))
        if math.isnan(x) or math.isnan(y) or math.isnan(gp_bps) or math.isnan(rtt):
            continue
        if rtt < 0:
            continue
        gp = gp_bps / 1e6
        d = math.hypot(x - ap_x, y - ap_y)
        pts.append((gp, rtt, d))

    if len(pts) < 3:
        return

    plt.close("all")
    fig, ax = plt.subplots(figsize=(9, 6), dpi=150)

    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    cs = [p[2] for p in pts]

    sc = ax.scatter(xs, ys, c=cs, s=55, alpha=0.9)
    ax.set_title("P8 - RTT vs Goodput (colored by distance)", pad=12)
    ax.set_xlabel("Goodput (Mbps)")
    ax.set_ylabel("RTT mean (ms)")
    ax.grid(True, linestyle="--", alpha=0.35)

    cbar = fig.colorbar(sc, ax=ax)
    cbar.set_label("Distance from AP (m)")

    fig.tight_layout()
    fig.savefig(out_png, dpi=300, bbox_inches="tight")
    plt.close(fig)


def group_qos(rows: List[dict]) -> Dict[str, Dict[str, dict]]:
    """
    Returns:
      flowType -> qosMode -> metrics dict
    """
    out: Dict[str, Dict[str, dict]] = {}
    for r in rows:
        mode = (r.get("qosMode") or "").strip().lower()
        flow = (r.get("flowType") or "").strip()
        if not mode or not flow:
            continue
        gp_mbps = to_float(r.get("goodputbps"), float("nan")) / 1e6
        delay_ms = to_float(r.get("meanDelayMs"), float("nan"))
        loss = to_float(r.get("lossRate"), float("nan"))
        out.setdefault(flow, {})[mode] = {
            "goodput_mbps": gp_mbps,
            "delay_ms": delay_ms,
            "loss_rate": loss,
        }
    return out


def draw_qos_bars(
    qos_rows: List[dict],
    out_dir: Path,
) -> None:
    g = group_qos(qos_rows)
    if not g:
        return

    flows = sorted(g.keys())
    have_on = all("on" in g[f] for f in flows)
    have_off = all("off" in g[f] for f in flows)
    if not (have_on and have_off):
        return

    metrics = [
        ("delay_ms", "Mean delay (ms)", "P8 - QoS ON/OFF Delay"),
        ("goodput_mbps", "Mean goodput (Mbps)", "P8 - QoS ON/OFF Goodput"),
        ("loss_rate", "Loss rate", "P8 - QoS ON/OFF Loss"),
    ]

    for key, ylabel, title in metrics:
        plt.close("all")
        fig, ax = plt.subplots(figsize=(10, 6), dpi=150)

        x = list(range(len(flows)))
        width = 0.36

        off_vals = [g[f]["off"][key] for f in flows]
        on_vals = [g[f]["on"][key] for f in flows]

        ax.bar([i - width / 2 for i in x], off_vals, width=width, label="QoS OFF")
        ax.bar([i + width / 2 for i in x], on_vals, width=width, label="QoS ON")

        ax.set_title(title, pad=12)
        ax.set_ylabel(ylabel)
        ax.set_xticks(x)
        ax.set_xticklabels(flows)
        ax.grid(True, axis="y", linestyle="--", alpha=0.35)
        ax.legend()

        fig.tight_layout()
        fig.savefig(out_dir / f"p8_qos_{key}_on_off.png", dpi=300, bbox_inches="tight")
        plt.close(fig)

        # Delta plot (ON - OFF)
        plt.close("all")
        fig, ax = plt.subplots(figsize=(10, 6), dpi=150)
        delta = [g[f]["on"][key] - g[f]["off"][key] for f in flows]

        ax.bar(x, delta)
        ax.axhline(0.0, linestyle="--", linewidth=1.0, alpha=0.6)

        ax.set_title(f"P8 - QoS Delta {key} (ON - OFF)", pad=12)
        ax.set_ylabel(f"Delta ({ylabel})")
        ax.set_xticks(x)
        ax.set_xticklabels(flows)
        ax.grid(True, axis="y", linestyle="--", alpha=0.35)

        fig.tight_layout()
        fig.savefig(out_dir / f"p8_qos_delta_{key}.png", dpi=300, bbox_inches="tight")
        plt.close(fig)


def draw_baseline_sweeps(baseline_rows: List[dict], out_dir: Path) -> None:
    if not baseline_rows:
        return

    # Clean and normalize
    clean = []
    for r in baseline_rows:
        transport = (r.get("transport") or "").strip().lower()
        if transport in ("", "transport"):
            continue
        d = to_float(r.get("distance"), float("nan"))
        gp = to_float(r.get("goodputbps"), float("nan")) / 1e6
        rtt = to_float(r.get("rttMeanMs"), float("nan"))
        if math.isnan(d) or math.isnan(gp):
            continue
        clean.append((transport, d, gp, rtt))

    if len(clean) < 2:
        return

    transports = sorted(set(t for t, _, _, _ in clean))

    # Distance vs Goodput
    plt.close("all")
    fig, ax = plt.subplots(figsize=(9, 6), dpi=150)
    for t in transports:
        pts = sorted([(d, gp) for tt, d, gp, _ in clean if tt == t], key=lambda x: x[0])
        if len(pts) < 2:
            continue
        ax.plot([p[0] for p in pts], [p[1] for p in pts], marker="o", label=t.upper())
    ax.set_title("P8 - Baseline Distance vs Goodput", pad=12)
    ax.set_xlabel("Distance (m)")
    ax.set_ylabel("Goodput (Mbps)")
    ax.grid(True, linestyle="--", alpha=0.35)
    ax.legend()
    fig.tight_layout()
    fig.savefig(out_dir / "p8_baseline_distance_goodput.png", dpi=300, bbox_inches="tight")
    plt.close(fig)

    # Distance vs RTT (only if RTT is valid)
    any_rtt = any((not math.isnan(rtt)) and rtt >= 0 for _, _, _, rtt in clean)
    if any_rtt:
        plt.close("all")
        fig, ax = plt.subplots(figsize=(9, 6), dpi=150)
        for t in transports:
            pts = sorted([(d, rtt) for tt, d, _, rtt in clean if tt == t and (not math.isnan(rtt)) and rtt >= 0],
                         key=lambda x: x[0])
            if len(pts) < 2:
                continue
            ax.plot([p[0] for p in pts], [p[1] for p in pts], marker="o", label=t.upper())
        ax.set_title("P8 - Baseline Distance vs RTT", pad=12)
        ax.set_xlabel("Distance (m)")
        ax.set_ylabel("RTT mean (ms)")
        ax.grid(True, linestyle="--", alpha=0.35)
        ax.legend()
        fig.tight_layout()
        fig.savefig(out_dir / "p8_baseline_distance_rtt.png", dpi=300, bbox_inches="tight")
        plt.close(fig)


def main() -> None:
    # Resolve repo root assuming this script is under WIFI-TER-SIM/scripts/
    here = Path(__file__).resolve()
    repo_root = here.parents[1]
    p8_dir = repo_root / "results" / "p8"
    plots_dir = p8_dir / "plots"
    ensure_dir(plots_dir)

    heatmap_csv = p8_dir / "heatmaps" / "heatmap.csv"
    qos_csv = p8_dir / "raw" / "qos_summary.csv"
    baseline_csv = p8_dir / "raw" / "baseline_summary.csv"

    heat_rows = read_csv_rows(heatmap_csv)
    qos_rows = read_csv_rows(qos_csv)
    baseline_rows = read_csv_rows(baseline_csv)

    # Heatmap: Goodput (Mbps)
    if heat_rows:
        xs, ys, gp_grid = build_grid(
            heat_rows,
            x_key="x",
            y_key="y",
            val_key="goodputbps",
            val_transform=lambda bps: bps / 1e6,
            invalid_pred=lambda v: v < 0,
        )
        draw_heatmap(
            xs,
            ys,
            gp_grid,
            title="P8 - Goodput Heatmap (autoscaled)",
            cbar_label="Goodput (Mbps)",
            out_png=plots_dir / "p8_goodput_heatmap.png",
            annotate=True,
        )

        # Heatmap: RTT (ms)
        xs2, ys2, rtt_grid = build_grid(
            heat_rows,
            x_key="x",
            y_key="y",
            val_key="rttMeanMs",
            val_transform=lambda ms: ms,
            invalid_pred=lambda v: v < 0,
        )
        draw_heatmap(
            xs2,
            ys2,
            rtt_grid,
            title="P8 - RTT Heatmap (autoscaled)",
            cbar_label="RTT mean (ms)",
            out_png=plots_dir / "p8_rtt_heatmap.png",
            annotate=True,
        )

        # Scatter: RTT vs Goodput
        draw_scatter_rtt_goodput(
            heat_rows,
            out_png=plots_dir / "p8_rtt_vs_goodput_scatter.png",
            ap_x=0.0,
            ap_y=0.0,
        )

    # QoS comparisons
    if qos_rows:
        draw_qos_bars(qos_rows, plots_dir)

    # Baseline sweeps (if multiple distances/transports exist)
    if baseline_rows:
        draw_baseline_sweeps(baseline_rows, plots_dir)

    print(f"[P8] Saved plots to: {plots_dir}")


if __name__ == "__main__":
    # High-quality defaults
    plt.rcParams.update({
        "figure.dpi": 150,
        "savefig.dpi": 300,
        "font.size": 12,
        "axes.titlesize": 16,
        "axes.labelsize": 13,
        "legend.fontsize": 12,
    })
    main()
