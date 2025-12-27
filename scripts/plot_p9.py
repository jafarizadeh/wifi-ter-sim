#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Project 9 (ns-3) - Plotting script (NO statsmodels / NO plotly required)

Reads:
  results/p9/heatmaps/heatmap.csv
  results/p9/raw/grid.csv

Writes PNGs into:
  results/p9/plots/

Usage (from WIFI-TER-SIM/scripts):
  python3 plot_p9.py

Optional:
  python3 plot_p9.py --results ../results/p9 --apX 0 --apY 0
  python3 plot_p9.py --standard ax --transport udp
"""

from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path

import numpy as np
import pandas as pd
import matplotlib.pyplot as plt
import matplotlib.tri as mtri

# --------------------------- helpers ---------------------------

def repo_root_from_here() -> Path:
    # .../WIFI-TER-SIM/scripts/plot_p9.py -> repo root is parent of scripts
    return Path(__file__).resolve().parents[1]

def safe_mkdir(p: Path) -> None:
    p.mkdir(parents=True, exist_ok=True)

def read_csv_if_exists(p: Path) -> pd.DataFrame | None:
    if not p.exists():
        return None
    try:
        df = pd.read_csv(p)
        return df
    except Exception as e:
        print(f"[ERROR] Cannot read {p}: {e}", file=sys.stderr)
        return None

def to_numeric(df: pd.DataFrame, cols: list[str]) -> pd.DataFrame:
    for c in cols:
        if c in df.columns:
            df[c] = pd.to_numeric(df[c], errors="coerce")
    return df

def filter_df(df: pd.DataFrame, standard: str | None, transport: str | None, rate_control: str | None) -> pd.DataFrame:
    out = df.copy()
    for col, val in [("standard", standard), ("transport", transport), ("rateControl", rate_control)]:
        if val is None:
            continue
        if col in out.columns:
            out = out[out[col].astype(str).str.lower() == val.lower()]
    return out

def distance_xy(df: pd.DataFrame, apx: float, apy: float) -> np.ndarray:
    return np.sqrt((df["x"].to_numpy() - apx) ** 2 + (df["y"].to_numpy() - apy) ** 2)

def binned_stat(x: np.ndarray, y: np.ndarray, bins: int = 20, stat: str = "median"):
    """Return bin centers and binned statistic (median/mean) with robust NaN handling."""
    m = np.isfinite(x) & np.isfinite(y)
    x = x[m]
    y = y[m]
    if x.size == 0:
        return np.array([]), np.array([])

    edges = np.linspace(x.min(), x.max(), bins + 1)
    centers = 0.5 * (edges[:-1] + edges[1:])
    out = np.full(bins, np.nan)

    for i in range(bins):
        sel = (x >= edges[i]) & (x < edges[i + 1]) if i < bins - 1 else (x >= edges[i]) & (x <= edges[i + 1])
        if np.any(sel):
            if stat == "mean":
                out[i] = np.nanmean(y[sel])
            else:
                out[i] = np.nanmedian(y[sel])
    return centers, out

def savefig(fig, path: Path, dpi: int = 200):
    fig.tight_layout()
    fig.savefig(path, dpi=dpi)
    plt.close(fig)

def tri_heatmap(x, y, z, title: str, outpath: Path, apx: float, apy: float, cmap: str = "turbo", vmin=None, vmax=None):
    """Scattered 2D heatmap using triangulation (works for irregular grids)."""
    m = np.isfinite(x) & np.isfinite(y) & np.isfinite(z)
    x = x[m]; y = y[m]; z = z[m]
    if x.size < 3:
        return
    tri = mtri.Triangulation(x, y)
    fig, ax = plt.subplots(figsize=(7.2, 6.2))
    tpc = ax.tricontourf(tri, z, levels=30, cmap=cmap, vmin=vmin, vmax=vmax)
    cb = fig.colorbar(tpc, ax=ax)
    cb.ax.set_ylabel(title)
    ax.scatter([apx], [apy], marker="*", s=160, edgecolor="k")
    ax.set_xlabel("x (m)")
    ax.set_ylabel("y (m)")
    ax.set_title(title)
    ax.set_aspect("equal", adjustable="box")
    ax.grid(True, alpha=0.25)
    savefig(fig, outpath)

def scatter_vs_distance(d, y, ylabel: str, title: str, outpath: Path, bins: int = 25):
    m = np.isfinite(d) & np.isfinite(y)
    d = d[m]; y = y[m]
    if d.size == 0:
        return
    fig, ax = plt.subplots(figsize=(7.6, 4.6))
    ax.scatter(d, y, s=18, alpha=0.5)
    # binned median trend
    xc, ym = binned_stat(d, y, bins=bins, stat="median")
    ax.plot(xc, ym, linewidth=2)
    ax.set_xlabel("Distance to AP (m)")
    ax.set_ylabel(ylabel)
    ax.set_title(title)
    ax.grid(True, alpha=0.25)
    savefig(fig, outpath)

def cdf_plot(values: np.ndarray, xlabel: str, title: str, outpath: Path):
    v = values[np.isfinite(values)]
    if v.size == 0:
        return
    v = np.sort(v)
    p = np.linspace(0, 1, v.size, endpoint=True)
    fig, ax = plt.subplots(figsize=(7.0, 4.6))
    ax.plot(v, p, linewidth=2)
    ax.set_xlabel(xlabel)
    ax.set_ylabel("CDF")
    ax.set_title(title)
    ax.grid(True, alpha=0.25)
    savefig(fig, outpath)

# --------------------------- main ---------------------------

def main():
    parser = argparse.ArgumentParser(description="Plot Project 9 results (no plotly/statsmodels).")
    parser.add_argument("--results", type=str, default=None, help="Path to results/p9 (default auto-detect).")
    parser.add_argument("--apX", type=float, default=0.0, help="AP X coordinate (default 0.0)")
    parser.add_argument("--apY", type=float, default=0.0, help="AP Y coordinate (default 0.0)")
    parser.add_argument("--standard", type=str, default=None, help="Filter: ax/ac/n (optional)")
    parser.add_argument("--transport", type=str, default=None, help="Filter: udp/tcp (optional)")
    parser.add_argument("--rateControl", type=str, default=None, help="Filter: adaptive/constant (optional)")
    parser.add_argument("--bins", type=int, default=25, help="Bins for distance trend (default 25)")
    args = parser.parse_args()

    root = repo_root_from_here()
    p9 = Path(args.results).expanduser().resolve() if args.results else (root / "results" / "p9")
    heat_csv = p9 / "heatmaps" / "heatmap.csv"
    grid_csv = p9 / "raw" / "grid.csv"
    out_dir = p9 / "plots"
    safe_mkdir(out_dir)

    heat = read_csv_if_exists(heat_csv)
    grid = read_csv_if_exists(grid_csv)

    if heat is None and grid is None:
        print(f"[ERROR] Cannot find CSVs in: {p9}", file=sys.stderr)
        print(f"  expected: {heat_csv}", file=sys.stderr)
        print(f"  expected: {grid_csv}", file=sys.stderr)
        return 2

    # Normalize numeric types
    if heat is not None:
        heat = to_numeric(heat, ["x","y","associated","offered_mbps","goodput_mbps","avg_rtt_ms","rtt_replies",
                                 "tx_bytes","rx_bytes","loss_ratio","rssi_est_dbm","snr_est_db","seed","run","channelWidth"])
        heat = filter_df(heat, args.standard, args.transport, args.rateControl)

    if grid is not None:
        grid = to_numeric(grid, ["x","y","seed","run","rssi_dbm","snr_db","goodput_mbps","rtt_ms","delay_ms","loss"])
        # grid doesn't include standard/transport columns, so no filtering beyond possible merge
        # if heat is available, we'll plot mainly from heat; otherwise from grid.

    # Pick a "base" dataframe for most plots
    if heat is not None and len(heat) > 0:
        df = heat.copy()
        # ensure column names consistent with grid.csv
        df["rssi_dbm"] = df.get("rssi_est_dbm", np.nan)
        df["snr_db"] = df.get("snr_est_db", np.nan)
        df["rtt_ms"] = df.get("avg_rtt_ms", np.nan)
        df["loss"] = df.get("loss_ratio", np.nan)
        df["delay_ms"] = np.nan  # may be filled via merge below
        # attach delay from grid if possible (match x,y,seed,run)
        if grid is not None and len(grid) > 0:
            mg = grid[["x","y","seed","run","delay_ms"]].copy()
            df = df.merge(mg, on=["x","y","seed","run"], how="left", suffixes=("","_g"))
            if "delay_ms_g" in df.columns:
                df["delay_ms"] = df["delay_ms_g"]
                df.drop(columns=["delay_ms_g"], inplace=True)
    else:
        df = grid.copy()

    # Add distance
    if "x" in df.columns and "y" in df.columns:
        df["distance_m"] = distance_xy(df, args.apX, args.apY)
    else:
        print("[ERROR] CSV missing x/y columns.", file=sys.stderr)
        return 2

    # --------------------------- summary ---------------------------
    summary_path = out_dir / "summary_p9.txt"
    with open(summary_path, "w", encoding="utf-8") as f:
        f.write(f"Results folder: {p9}\n")
        f.write(f"Rows used: {len(df)}\n")
        f.write(f"AP: ({args.apX}, {args.apY})\n")
        if heat is not None:
            f.write(f"heatmap.csv rows after filters: {len(heat)}\n")
            for k in ["standard","transport","rateControl","channelWidth"]:
                if k in heat.columns:
                    f.write(f"{k} unique: {sorted(set(map(str, heat[k].dropna().unique().tolist())))}\n")
        for col in ["goodput_mbps","offered_mbps","rtt_ms","delay_ms","loss","rssi_dbm","snr_db","associated"]:
            if col in df.columns:
                s = pd.to_numeric(df[col], errors="coerce")
                f.write(f"\n[{col}]\n")
                f.write(s.describe(percentiles=[0.05,0.25,0.5,0.75,0.95]).to_string() + "\n")

    # --------------------------- 2D heatmaps ---------------------------
    x = df["x"].to_numpy()
    y = df["y"].to_numpy()

    # Goodput
    if "goodput_mbps" in df.columns:
        tri_heatmap(x, y, df["goodput_mbps"].to_numpy(),
                    "Goodput (Mbps)", out_dir / "heatmap_goodput_mbps.png", args.apX, args.apY)

    # RTT
    if "rtt_ms" in df.columns:
        tri_heatmap(x, y, df["rtt_ms"].to_numpy(),
                    "RTT (ms)", out_dir / "heatmap_rtt_ms.png", args.apX, args.apY, cmap="viridis")

    # Delay (FlowMonitor)
    if "delay_ms" in df.columns and np.isfinite(df["delay_ms"]).any():
        tri_heatmap(x, y, df["delay_ms"].to_numpy(),
                    "Mean delay (ms)", out_dir / "heatmap_delay_ms.png", args.apX, args.apY, cmap="viridis")

    # Loss
    if "loss" in df.columns:
        tri_heatmap(x, y, df["loss"].to_numpy(),
                    "Loss ratio", out_dir / "heatmap_loss.png", args.apX, args.apY, cmap="magma", vmin=0.0, vmax=1.0)

    # RSSI/SNR
    if "rssi_dbm" in df.columns:
        tri_heatmap(x, y, df["rssi_dbm"].to_numpy(),
                    "Estimated RSSI (dBm)", out_dir / "heatmap_rssi_dbm.png", args.apX, args.apY, cmap="plasma")
    if "snr_db" in df.columns:
        tri_heatmap(x, y, df["snr_db"].to_numpy(),
                    "Estimated SNR (dB)", out_dir / "heatmap_snr_db.png", args.apX, args.apY, cmap="plasma")

    # Association (binary)
    if "associated" in df.columns:
        tri_heatmap(x, y, df["associated"].to_numpy(),
                    "Associated (1=yes)", out_dir / "heatmap_associated.png", args.apX, args.apY, cmap="cividis", vmin=0, vmax=1)

    # --------------------------- distance trends ---------------------------
    d = df["distance_m"].to_numpy()

    if "goodput_mbps" in df.columns:
        scatter_vs_distance(d, df["goodput_mbps"].to_numpy(),
                            "Goodput (Mbps)", "Goodput vs Distance", out_dir / "goodput_vs_distance.png", bins=args.bins)
        cdf_plot(df["goodput_mbps"].to_numpy(), "Goodput (Mbps)", "CDF of Goodput", out_dir / "cdf_goodput.png")

    if "rtt_ms" in df.columns:
        scatter_vs_distance(d, df["rtt_ms"].to_numpy(),
                            "RTT (ms)", "RTT vs Distance", out_dir / "rtt_vs_distance.png", bins=args.bins)
        cdf_plot(df["rtt_ms"].to_numpy(), "RTT (ms)", "CDF of RTT", out_dir / "cdf_rtt.png")

    if "delay_ms" in df.columns and np.isfinite(df["delay_ms"]).any():
        scatter_vs_distance(d, df["delay_ms"].to_numpy(),
                            "Mean delay (ms)", "Mean delay vs Distance", out_dir / "delay_vs_distance.png", bins=args.bins)
        cdf_plot(df["delay_ms"].to_numpy(), "Mean delay (ms)", "CDF of Mean delay", out_dir / "cdf_delay.png")

    if "loss" in df.columns:
        scatter_vs_distance(d, df["loss"].to_numpy(),
                            "Loss ratio", "Loss vs Distance", out_dir / "loss_vs_distance.png", bins=args.bins)
        cdf_plot(df["loss"].to_numpy(), "Loss ratio", "CDF of Loss", out_dir / "cdf_loss.png")

    if "rssi_dbm" in df.columns:
        scatter_vs_distance(d, df["rssi_dbm"].to_numpy(),
                            "RSSI (dBm)", "RSSI estimate vs Distance", out_dir / "rssi_vs_distance.png", bins=args.bins)
        cdf_plot(df["rssi_dbm"].to_numpy(), "RSSI (dBm)", "CDF of RSSI estimate", out_dir / "cdf_rssi.png")

    if "snr_db" in df.columns:
        scatter_vs_distance(d, df["snr_db"].to_numpy(),
                            "SNR (dB)", "SNR estimate vs Distance", out_dir / "snr_vs_distance.png", bins=args.bins)
        cdf_plot(df["snr_db"].to_numpy(), "SNR (dB)", "CDF of SNR estimate", out_dir / "cdf_snr.png")

    # --------------------------- correlation matrix (numeric) ---------------------------
    num_cols = [c for c in ["goodput_mbps","offered_mbps","rtt_ms","delay_ms","loss","rssi_dbm","snr_db","distance_m"] if c in df.columns]
    if len(num_cols) >= 2:
        corr = df[num_cols].corr(numeric_only=True)
        fig, ax = plt.subplots(figsize=(7.5, 6.5))
        im = ax.imshow(corr.to_numpy(), aspect="auto")
        fig.colorbar(im, ax=ax)
        ax.set_xticks(range(len(num_cols)), labels=num_cols, rotation=45, ha="right")
        ax.set_yticks(range(len(num_cols)), labels=num_cols)
        ax.set_title("Correlation matrix")
        # annotate
        for i in range(len(num_cols)):
            for j in range(len(num_cols)):
                ax.text(j, i, f"{corr.iloc[i,j]:.2f}", ha="center", va="center", fontsize=8)
        savefig(fig, out_dir / "corr_matrix.png")

    print(f"[OK] Plots saved to: {out_dir}")
    print(f"[OK] Summary saved to: {summary_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
