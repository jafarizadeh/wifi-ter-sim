#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
WIFI-TER-SIM — Project 9 plotting (no-args default runner)

Run:
  python scripts/plot_p9.py
(or)
  python3 scripts/plot_p9.py

Defaults:
  grid   = results/p9/raw/grid.csv
  heat   = results/p9/heatmaps/heatmap.csv
  outdir = results/p9/plots

Also supports overriding with optional args:
  --grid PATH --heatmap PATH --out PATH
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path

import numpy as np
import pandas as pd

import matplotlib
matplotlib.use("Agg")  # headless-safe
import matplotlib.pyplot as plt
from matplotlib.ticker import MaxNLocator


# ------------------------ helpers ------------------------

def repo_root_from_this_file() -> Path:
    # scripts/plot_p9.py -> repo root = parent of "scripts"
    return Path(__file__).resolve().parents[1]

def ensure_out(dirpath: Path) -> None:
    dirpath.mkdir(parents=True, exist_ok=True)

def load_csv(path: Path) -> pd.DataFrame:
    return pd.read_csv(path)

def pivot_metric(df: pd.DataFrame, metric: str):
    # pivot to 2D array Z with shape (len(ys), len(xs))
    piv = df.pivot_table(index="y", columns="x", values=metric, aggfunc="mean")
    ys = np.array(sorted(df["y"].unique()))
    xs = np.array(sorted(df["x"].unique()))
    Z = piv.reindex(index=ys, columns=xs).to_numpy()
    return xs, ys, Z

def savefig(path: Path, title: str | None = None):
    if title:
        plt.title(title)
    plt.tight_layout()
    plt.savefig(path, dpi=220, bbox_inches="tight")
    plt.close()

def plot_heatmap(df: pd.DataFrame, metric: str, out_dir: Path, title: str | None = None):
    xs, ys, Z = pivot_metric(df, metric)
    plt.figure(figsize=(7.2, 5.8))
    extent = (xs.min(), xs.max(), ys.min(), ys.max())
    plt.imshow(Z, origin="lower", aspect="auto", extent=extent)
    plt.xlabel("x (m)")
    plt.ylabel("y (m)")
    cb = plt.colorbar()
    cb.set_label(metric)
    savefig(out_dir / f"heatmap_{metric}.png", title or f"Heatmap — {metric}")

def plot_surface(df: pd.DataFrame, metric: str, out_dir: Path, title: str | None = None):
    from mpl_toolkits.mplot3d import Axes3D  # noqa: F401
    xs, ys, Z = pivot_metric(df, metric)
    X, Y = np.meshgrid(xs, ys)
    fig = plt.figure(figsize=(7.2, 5.8))
    ax = fig.add_subplot(111, projection="3d")
    ax.plot_surface(X, Y, Z)
    ax.set_xlabel("x (m)")
    ax.set_ylabel("y (m)")
    ax.set_zlabel(metric)
    savefig(out_dir / f"surface_{metric}.png", title or f"Surface — {metric}")

def plot_scatter_xy(df: pd.DataFrame, metric: str, out_dir: Path, title: str | None = None):
    plt.figure(figsize=(7.2, 5.8))
    plt.scatter(df["x"], df["y"], s=22, c=df[metric])
    plt.xlabel("x (m)")
    plt.ylabel("y (m)")
    cb = plt.colorbar()
    cb.set_label(metric)
    savefig(out_dir / f"scatter_xy_{metric}.png", title or f"Scatter XY — {metric}")

def plot_relation(df: pd.DataFrame, xcol: str, ycol: str, out_dir: Path, title: str | None = None):
    plt.figure(figsize=(6.6, 5.2))
    plt.scatter(df[xcol], df[ycol], s=12, alpha=0.85)
    plt.xlabel(xcol)
    plt.ylabel(ycol)
    ax = plt.gca()
    ax.xaxis.set_major_locator(MaxNLocator(nbins=8))
    ax.yaxis.set_major_locator(MaxNLocator(nbins=8))
    savefig(out_dir / f"scatter_{xcol}_vs_{ycol}.png", title or f"{ycol} vs {xcol}")

def plot_hist(df: pd.DataFrame, col: str, out_dir: Path, bins: int = 24, title: str | None = None):
    plt.figure(figsize=(6.6, 5.2))
    plt.hist(df[col].dropna(), bins=bins)
    plt.xlabel(col)
    plt.ylabel("count")
    savefig(out_dir / f"hist_{col}.png", title or f"Histogram — {col}")

def plot_performance_vs_distance(df: pd.DataFrame, out_dir: Path):
    # If AP coords not present, assume (0,0)
    apx = float(df["ap_x"].iloc[0]) if "ap_x" in df.columns else 0.0
    apy = float(df["ap_y"].iloc[0]) if "ap_y" in df.columns else 0.0

    tmp = df.copy()
    tmp["distance_m"] = np.sqrt((tmp["x"] - apx) ** 2 + (tmp["y"] - apy) ** 2)

    if "goodput_mbps" in tmp.columns:
        plot_relation(tmp, "distance_m", "goodput_mbps", out_dir, "Goodput vs Distance")
    if "rtt_ms" in tmp.columns:
        plot_relation(tmp, "distance_m", "rtt_ms", out_dir, "RTT vs Distance")
    if "loss" in tmp.columns:
        plot_relation(tmp, "distance_m", "loss", out_dir, "Loss vs Distance")


# ------------------------ main ------------------------

def parse_args(default_grid: Path, default_heat: Path, default_out: Path):
    p = argparse.ArgumentParser(add_help=True)
    p.add_argument("--grid", default=str(default_grid), help="Path to results/p9/raw/grid.csv")
    p.add_argument("--heatmap", default=str(default_heat), help="Path to results/p9/heatmaps/heatmap.csv")
    p.add_argument("--out", default=str(default_out), help="Output directory (default: results/p9/plots)")
    return p.parse_args()

def main():
    root = repo_root_from_this_file()
    default_grid = root / "results" / "p9" / "raw" / "grid.csv"
    default_heat = root / "results" / "p9" / "heatmaps" / "heatmap.csv"
    default_out  = root / "results" / "p9" / "plots"

    args = parse_args(default_grid, default_heat, default_out)

    grid_path = Path(args.grid).expanduser().resolve()
    heat_path = Path(args.heatmap).expanduser().resolve()
    out_dir   = Path(args.out).expanduser().resolve()

    if not grid_path.exists():
        raise SystemExit(f"[plot_p9] grid.csv not found: {grid_path}")
    if not heat_path.exists():
        raise SystemExit(f"[plot_p9] heatmap.csv not found: {heat_path}")

    ensure_out(out_dir)

    grid = load_csv(grid_path)
    heat = load_csv(heat_path)

    # --- Required heatmaps from grid.csv ---
    for m in ["rssi_dbm", "goodput_mbps", "rtt_ms"]:
        if m in grid.columns:
            plot_heatmap(grid, m, out_dir)

    # --- Extra useful heatmaps ---
    for m in ["snr_db", "loss"]:
        if m in grid.columns:
            plot_heatmap(grid, m, out_dir)

    # --- Surface (nice for report/appendix) ---
    for m in ["goodput_mbps", "rssi_dbm"]:
        if m in grid.columns:
            plot_surface(grid, m, out_dir)

    # --- XY scatter (colored) ---
    for m in ["rssi_dbm", "snr_db", "goodput_mbps", "rtt_ms", "loss"]:
        if m in grid.columns:
            plot_scatter_xy(grid, m, out_dir)

    # --- Relationships ---
    if {"rssi_dbm", "goodput_mbps"}.issubset(grid.columns):
        plot_relation(grid, "rssi_dbm", "goodput_mbps", out_dir, "Goodput vs RSSI")
    if {"snr_db", "loss"}.issubset(grid.columns):
        plot_relation(grid, "snr_db", "loss", out_dir, "Loss vs SNR")
    if {"goodput_mbps", "rtt_ms"}.issubset(grid.columns):
        plot_relation(grid, "goodput_mbps", "rtt_ms", out_dir, "RTT vs Goodput")

    # --- Histograms ---
    for m in ["rssi_dbm", "snr_db", "goodput_mbps", "rtt_ms", "loss"]:
        if m in grid.columns:
            plot_hist(grid, m, out_dir)

    # --- Distance-based diagnostics ---
    plot_performance_vs_distance(grid, out_dir)

    # --- Optional: also plot from heatmap.csv fields (if you want them too) ---
    # these are "extra" fields your simulator writes
    if "loss_ratio" in heat.columns:
        tmp = heat.rename(columns={"loss_ratio": "loss"})
        plot_heatmap(tmp, "loss", out_dir, "Heatmap — loss_ratio")
    if "avg_rtt_ms" in heat.columns:
        tmp = heat.rename(columns={"avg_rtt_ms": "rtt_ms"})
        plot_heatmap(tmp, "rtt_ms", out_dir, "Heatmap — avg_rtt_ms")
    if "rssi_est_dbm" in heat.columns:
        tmp = heat.rename(columns={"rssi_est_dbm": "rssi_dbm"})
        plot_heatmap(tmp, "rssi_dbm", out_dir, "Heatmap — rssi_est_dbm")
    if "snr_est_db" in heat.columns:
        tmp = heat.rename(columns={"snr_est_db": "snr_db"})
        plot_heatmap(tmp, "snr_db", out_dir, "Heatmap — snr_est_db")

    generated = sorted([p.name for p in out_dir.iterdir() if p.suffix.lower() == ".png"])
    print(json.dumps({"out_dir": str(out_dir), "generated": generated}, indent=2))


if __name__ == "__main__":
    main()
