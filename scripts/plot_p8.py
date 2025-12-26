#!/usr/bin/env python3
"""
scripts/plot_p8.py

Project 8 plotting script (ns-3 QoS/WMM):
- Reads results/p8/raw/p8_summary.csv
- Produces publication-quality plots (PNG only):
  * Delay/Jitter vs BE rate for VO & VI (OFF vs ON)
  * Goodput vs BE rate for VO, VI, BE (OFF vs ON)
  * Loss vs BE rate for VO & VI (OFF vs ON) with proper y-scaling
  * Optional bar chart for a chosen BE rate (default 40 Mbps): OFF vs ON

Outputs:
- PNG figures in results/p8/plots/

Requirements:
  pip install pandas numpy matplotlib
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
from typing import List, Tuple

import numpy as np
import pandas as pd

import matplotlib
matplotlib.use("Agg")  # headless-safe
import matplotlib.pyplot as plt


REQUIRED_COLS = [
    "mode", "beRateMbps", "seed", "run",
    "goodputBE", "goodputVO", "goodputVI",
    "delayVO_ms", "jitterVO_ms", "lossVO",
    "delayVI_ms", "jitterVI_ms", "lossVI",
]

METRICS = [
    "goodputBE", "goodputVO", "goodputVI",
    "delayVO_ms", "jitterVO_ms", "lossVO",
    "delayVI_ms", "jitterVI_ms", "lossVI",
]


@dataclass
class PlotCfg:
    dpi: int = 200
    ci: str = "95"                  # "none" | "std" | "95"
    min_n_for_ci: int = 5           # show CI only if n >= this
    show_raw_points: bool = True    # scatter per-run points behind means
    raw_alpha: float = 0.25
    grid: bool = True
    logy_delay: bool = False
    logy_jitter: bool = False
    loss_scale: str = "auto"        # "auto" | "0-1" | "log"


def repo_root_from_script() -> Path:
    # Assumes this script is in <repo>/scripts/plot_p8.py
    return Path(__file__).resolve().parents[1]


def ensure_dir(p: Path) -> None:
    p.mkdir(parents=True, exist_ok=True)


def read_and_validate(csv_path: Path) -> pd.DataFrame:
    if not csv_path.exists():
        raise FileNotFoundError(f"CSV not found: {csv_path}")

    df = pd.read_csv(csv_path)

    missing = [c for c in REQUIRED_COLS if c not in df.columns]
    if missing:
        raise ValueError(
            f"CSV missing columns: {missing}\nFound columns: {list(df.columns)}"
        )

    # Normalize mode to OFF/ON
    df["mode"] = df["mode"].astype(str).str.strip().str.upper()

    # Coerce numeric columns
    for c in [x for x in REQUIRED_COLS if x != "mode"]:
        df[c] = pd.to_numeric(df[c], errors="coerce")

    # Drop broken rows
    df = df.dropna(subset=["mode", "beRateMbps", "run"]).copy()

    # Sort for plotting
    df = df.sort_values(["beRateMbps", "mode", "run"]).reset_index(drop=True)
    return df


def t_critical_95(n: int) -> float:
    """Approx t critical for 95% CI (two-sided) without scipy."""
    if n <= 1:
        return float("nan")
    dof = n - 1
    lookup = {
        1: 12.706, 2: 4.303, 3: 3.182, 4: 2.776, 5: 2.571,
        6: 2.447, 7: 2.365, 8: 2.306, 9: 2.262, 10: 2.228,
        15: 2.131, 20: 2.086, 25: 2.060, 30: 2.042, 40: 2.021,
        60: 2.000, 120: 1.980,
    }
    if dof in lookup:
        return lookup[dof]
    keys = sorted(lookup.keys())
    if dof < keys[0]:
        return lookup[keys[0]]
    if dof > keys[-1]:
        return 1.96
    lo = max(k for k in keys if k < dof)
    hi = min(k for k in keys if k > dof)
    w = (dof - lo) / (hi - lo)
    return lookup[lo] * (1 - w) + lookup[hi] * w


def aggregate(df: pd.DataFrame) -> pd.DataFrame:
    g = df.groupby(["mode", "beRateMbps"], as_index=False)
    agg = g[METRICS].agg(["mean", "std", "count"])
    agg.columns = ["_".join([c for c in col if c]) for col in agg.columns.to_flat_index()]
    agg = agg.rename(columns={"mode_": "mode", "beRateMbps_": "beRateMbps"})

    # CI95 half-width
    for m in METRICS:
        n = agg[f"{m}_count"].astype(int).to_numpy()
        std = agg[f"{m}_std"].astype(float).to_numpy()
        sem = std / np.sqrt(np.maximum(n, 1))
        tvals = np.array([t_critical_95(int(x)) for x in n], dtype=float)
        agg[f"{m}_ci95"] = tvals * sem

    return agg.sort_values(["beRateMbps", "mode"]).reset_index(drop=True)


def _yerr(sub: pd.DataFrame, metric: str, cfg: PlotCfg) -> np.ndarray:
    n = sub[f"{metric}_count"].to_numpy(dtype=float)

    if cfg.ci == "none":
        return np.zeros(len(sub), dtype=float)

    if cfg.ci == "std":
        y = sub[f"{metric}_std"].to_numpy(dtype=float)
        y = np.nan_to_num(y, nan=0.0)
        return np.where(n >= 2, y, 0.0)

    # cfg.ci == "95"
    y = sub[f"{metric}_ci95"].to_numpy(dtype=float)
    y = np.nan_to_num(y, nan=0.0)
    return np.where(n >= cfg.min_n_for_ci, y, 0.0)


def line_plot(
    df_raw: pd.DataFrame,
    agg: pd.DataFrame,
    metric: str,
    ylabel: str,
    title: str,
    cfg: PlotCfg,
    ylog: bool = False,
    loss_plot: bool = False,
) -> plt.Figure:
    fig, ax = plt.subplots(figsize=(7.2, 4.6))

    mode_order = ["OFF", "ON"] if set(["OFF", "ON"]).issubset(set(agg["mode"])) else sorted(agg["mode"].unique())

    style = {
        "OFF": dict(marker="o", linestyle="-", linewidth=2.0),
        "ON":  dict(marker="s", linestyle="--", linewidth=2.0),
    }

    x_ticks = sorted(agg["beRateMbps"].unique().tolist())

    for mode in mode_order:
        sub = agg[agg["mode"] == mode].sort_values("beRateMbps")
        if sub.empty:
            continue

        x = sub["beRateMbps"].to_numpy(dtype=float)
        y = sub[f"{metric}_mean"].to_numpy(dtype=float)
        yerr = _yerr(sub, metric, cfg)

        # raw points
        if cfg.show_raw_points:
            raw_sub = df_raw[df_raw["mode"] == mode]
            ax.scatter(
                raw_sub["beRateMbps"].to_numpy(dtype=float),
                raw_sub[metric].to_numpy(dtype=float),
                s=18,
                alpha=cfg.raw_alpha,
                zorder=1,
            )

        st = style.get(mode, dict(marker="o", linestyle="-", linewidth=2.0))
        ax.errorbar(
            x, y, yerr=yerr,
            capsize=3,
            label=mode,
            zorder=3,
            **st
        )

    ax.set_xlabel("BE offered rate (Mbps)")
    ax.set_ylabel(ylabel)
    ax.set_title(title)
    ax.set_xticks(x_ticks)

    if cfg.grid:
        ax.grid(True, which="both", linestyle="--", linewidth=0.8, alpha=0.6)

    ax.legend(loc="best")

    if loss_plot:
        if cfg.loss_scale == "0-1":
            ax.set_ylim(0.0, 1.0)
        elif cfg.loss_scale == "log":
            ax.set_yscale("log")
            ax.set_ylim(1e-6, 1.0)
        else:
            y_all = agg[f"{metric}_mean"].to_numpy(dtype=float)
            e_all = agg[f"{metric}_ci95"].to_numpy(dtype=float)
            ymax = float(np.nanmax(y_all + np.nan_to_num(e_all, nan=0.0)))
            if not np.isfinite(ymax) or ymax <= 0.0:
                ymax = 1e-4
            ax.set_ylim(0.0, max(1.2 * ymax, 1e-4))

            if np.nanmax(np.abs(y_all)) == 0.0:
                ax.text(
                    0.5, 0.5, "All loss values are 0 in this dataset",
                    transform=ax.transAxes,
                    ha="center", va="center",
                    fontsize=10,
                    bbox=dict(boxstyle="round,pad=0.3", alpha=0.2),
                )

    if ylog:
        ax.set_yscale("log")

    fig.tight_layout()
    return fig


def bar_compare_at_rate(
    agg: pd.DataFrame,
    be_rate: float,
    metrics: List[Tuple[str, str]],
    cfg: PlotCfg,
) -> plt.Figure:
    fig, ax = plt.subplots(figsize=(10.5, 4.2))

    sub = agg[np.isclose(agg["beRateMbps"].astype(float), float(be_rate))].copy()
    if sub.empty:
        ax.text(0.5, 0.5, f"No data for BE rate={be_rate}", ha="center", va="center")
        ax.axis("off")
        fig.tight_layout()
        return fig

    mode_order = ["OFF", "ON"]
    sub["mode"] = sub["mode"].astype(str)
    sub = sub.set_index("mode").reindex(mode_order)

    labels = [lbl for _, lbl in metrics]
    x = np.arange(len(metrics))
    width = 0.38

    for i, mode in enumerate(mode_order):
        if mode not in sub.index or sub.loc[mode].isna().all():
            continue

        means = np.array([sub.loc[mode, f"{m}_mean"] for m, _ in metrics], dtype=float)

        yerr = []
        for m, _ in metrics:
            n = int(sub.loc[mode, f"{m}_count"]) if np.isfinite(sub.loc[mode, f"{m}_count"]) else 0
            if cfg.ci == "95" and n >= cfg.min_n_for_ci:
                yerr.append(float(sub.loc[mode, f"{m}_ci95"]))
            elif cfg.ci == "std" and n >= 2:
                yerr.append(float(sub.loc[mode, f"{m}_std"]))
            else:
                yerr.append(0.0)
        yerr = np.array(yerr, dtype=float)

        offset = (i - 0.5) * width
        ax.bar(x + offset, means, width, yerr=yerr, capsize=3, label=mode)

    ax.set_xticks(x)
    ax.set_xticklabels(labels)
    ax.set_title(f"OFF vs ON at BE rate = {be_rate} Mbps")
    if cfg.grid:
        ax.grid(True, axis="y", linestyle="--", linewidth=0.8, alpha=0.6)
    ax.legend(loc="best")
    fig.tight_layout()
    return fig


def save_png(fig: plt.Figure, png_path: Path, cfg: PlotCfg) -> None:
    fig.savefig(png_path, dpi=cfg.dpi)
    plt.close(fig)


def main() -> None:
    root = repo_root_from_script()

    p = argparse.ArgumentParser(description="Plot Project 8 (QoS/WMM) results from p8_summary.csv")
    p.add_argument("--csv", type=str, default=str(root / "results" / "p8" / "raw" / "p8_summary.csv"),
                   help="Path to p8_summary.csv")
    p.add_argument("--outdir", type=str, default=str(root / "results" / "p8"),
                   help="Project output dir (contains raw/, logs/, plots/...)")
    p.add_argument("--dpi", type=int, default=200, help="PNG DPI")
    p.add_argument("--ci", type=str, default="95", choices=["none", "std", "95"],
                   help="Error bars: none | std | 95 (95% CI)")
    p.add_argument("--min-n-ci", type=int, default=5,
                   help="Show 95% CI only if count >= this (default: 5)")
    p.add_argument("--no-raw", action="store_true", help="Disable raw per-run scatter points")
    p.add_argument("--logy-delay", action="store_true", help="Log scale for delay plots")
    p.add_argument("--logy-jitter", action="store_true", help="Log scale for jitter plots")
    p.add_argument("--loss-scale", type=str, default="auto", choices=["auto", "0-1", "log"],
                   help="Loss y-axis scaling: auto (zoom), 0-1, or log")
    p.add_argument("--bar-be-rate", type=float, default=40.0,
                   help="BE rate for OFF/ON bar comparison plot")
    args = p.parse_args()

    outdir = Path(args.outdir).resolve()
    csv_path = Path(args.csv).resolve()
    plots_dir = outdir / "plots"
    ensure_dir(plots_dir)

    cfg = PlotCfg(
        dpi=int(args.dpi),
        ci=str(args.ci),
        min_n_for_ci=int(args.min_n_ci),
        show_raw_points=(not args.no_raw),
        logy_delay=bool(args.logy_delay),
        logy_jitter=bool(args.logy_jitter),
        loss_scale=str(args.loss_scale),
    )

    # nicer defaults
    plt.rcParams.update({
        "figure.facecolor": "white",
        "axes.spines.top": False,
        "axes.spines.right": False,
        "axes.titleweight": "bold",
        "axes.labelsize": 12,
        "axes.titlesize": 14,
        "legend.frameon": True,
        "legend.framealpha": 0.9,
        "font.size": 11,
    })

    df = read_and_validate(csv_path)
    agg = aggregate(df)

    # Print aggregated stats to console (NO CSV written)
    print("\n=== Aggregated stats (mode, beRateMbps) ===")
    cols_to_show = [
        "mode", "beRateMbps",
        "goodputBE_mean", "goodputVO_mean", "goodputVI_mean",
        "delayVO_ms_mean", "jitterVO_ms_mean", "lossVO_mean",
        "delayVI_ms_mean", "jitterVI_ms_mean", "lossVI_mean",
    ]
    existing = [c for c in cols_to_show if c in agg.columns]
    print(agg[existing].to_string(index=False))

    # ---- Save PNGs only ----
    save_png(
        line_plot(df, agg, "delayVO_ms", "Mean delay VO (ms)", "VO delay vs BE rate",
                  cfg, ylog=cfg.logy_delay),
        plots_dir / "p8_vo_delay.png", cfg
    )
    save_png(
        line_plot(df, agg, "jitterVO_ms", "Mean jitter VO (ms)", "VO jitter vs BE rate",
                  cfg, ylog=cfg.logy_jitter),
        plots_dir / "p8_vo_jitter.png", cfg
    )
    save_png(
        line_plot(df, agg, "delayVI_ms", "Mean delay VI (ms)", "VI delay vs BE rate",
                  cfg, ylog=cfg.logy_delay),
        plots_dir / "p8_vi_delay.png", cfg
    )
    save_png(
        line_plot(df, agg, "jitterVI_ms", "Mean jitter VI (ms)", "VI jitter vs BE rate",
                  cfg, ylog=cfg.logy_jitter),
        plots_dir / "p8_vi_jitter.png", cfg
    )

    save_png(
        line_plot(df, agg, "goodputVO", "Goodput VO (Mbps)", "VO goodput vs BE rate", cfg),
        plots_dir / "p8_vo_goodput.png", cfg
    )
    save_png(
        line_plot(df, agg, "goodputVI", "Goodput VI (Mbps)", "VI goodput vs BE rate", cfg),
        plots_dir / "p8_vi_goodput.png", cfg
    )
    save_png(
        line_plot(df, agg, "goodputBE", "Goodput BE (Mbps)", "BE goodput vs BE rate", cfg),
        plots_dir / "p8_be_goodput.png", cfg
    )

    save_png(
        line_plot(df, agg, "lossVO", "Loss ratio VO", "VO loss vs BE rate",
                  cfg, loss_plot=True),
        plots_dir / "p8_vo_loss.png", cfg
    )
    save_png(
        line_plot(df, agg, "lossVI", "Loss ratio VI", "VI loss vs BE rate",
                  cfg, loss_plot=True),
        plots_dir / "p8_vi_loss.png", cfg
    )

    bar_metrics = [
        ("delayVO_ms", "VO delay (ms)"),
        ("jitterVO_ms", "VO jitter (ms)"),
        ("delayVI_ms", "VI delay (ms)"),
        ("jitterVI_ms", "VI jitter (ms)"),
    ]
    save_png(
        bar_compare_at_rate(agg, args.bar_be_rate, bar_metrics, cfg),
        plots_dir / "p8_bar_off_on.png", cfg
    )

    print("\n P8 PNG plots generated:")
    print(f"  - {plots_dir}")


if __name__ == "__main__":
    main()
