#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Project 7 (ns-3) Plotter: co-channel (cocanal) vs separate-channel (separe)

Reads:
  - results/p7/raw/p7_summary.csv
  - results/p7/raw/perflow_*.csv   (optional but recommended)

Writes PNGs to:
  - results/p7/plots/

Produces:
  1) goodput_total_vs_n.png
  2) goodput_cells_vs_n.png
  3) jain_vs_n.png
  4) per_sta_bars_n10.png
  5) per_sta_box_n10.png
  6) per_sta_cdf_n10.png
  7) per_sta_ecdf_by_cell_n10.png
  8) per_sta_box_by_cell_n10.png

Usage:
  python3 scripts/plot_p7.py
"""

from __future__ import annotations

import re
import glob
from pathlib import Path
from typing import Dict, Tuple, Optional

import numpy as np
import pandas as pd
import matplotlib.pyplot as plt


# -------------------------- Paths --------------------------
ROOT = Path(__file__).resolve().parents[1]   # repo root: wifi-ter-sim/
RAW_DIR = ROOT / "results" / "p7" / "raw"
PLOTS_DIR = ROOT / "results" / "p7" / "plots"

SUMMARY_CSV = RAW_DIR / "p7_summary.csv"
PERFLOW_GLOB = str(RAW_DIR / "perflow_*.csv")


# -------------------------- Utils --------------------------
def ensure_dirs() -> None:
    PLOTS_DIR.mkdir(parents=True, exist_ok=True)


def mbps(x_bps: np.ndarray | pd.Series | float) -> np.ndarray:
    return np.asarray(x_bps, dtype=float) / 1e6


def normalize_plan(s: str) -> str:
    s = str(s).strip().lower()
    mapping = {
        "co": "cocanal",
        "cocanal": "cocanal",
        "cochannel": "cocanal",
        "co-channel": "cocanal",
        "sep": "separe",
        "separe": "separe",
        "separate": "separe",
        "separatechannel": "separe",
        "separate-channel": "separe",
    }
    return mapping.get(s, s)


def read_summary() -> pd.DataFrame:
    if not SUMMARY_CSV.exists():
        raise FileNotFoundError(f"Missing: {SUMMARY_CSV}")

    df = pd.read_csv(SUMMARY_CSV)

    required = [
        "channelPlan", "nStaPerCell",
        "goodputCell1", "goodputCell2", "goodputTotal",
        "jainCells",
    ]
    missing = [c for c in required if c not in df.columns]
    if missing:
        raise ValueError(f"Summary CSV missing columns: {missing}\nFound: {list(df.columns)}")

    df["channelPlan"] = df["channelPlan"].map(normalize_plan)

    # numeric
    for c in ["nStaPerCell", "goodputCell1", "goodputCell2", "goodputTotal", "jainCells"]:
        df[c] = pd.to_numeric(df[c], errors="coerce")

    df = df.dropna(subset=["channelPlan", "nStaPerCell"])
    df["nStaPerCell"] = df["nStaPerCell"].astype(int)
    return df


# Parse perflow_<plan>_n<NUM>_run<RUN>.csv
PERFLOW_RE = re.compile(r"^perflow_(?P<plan>[^_]+)_n(?P<n>\d+)_run(?P<run>\d+)\.csv$", re.IGNORECASE)


def read_perflow_latest() -> Dict[Tuple[str, int], pd.DataFrame]:
    """
    Returns: (plan, nStaPerCell) -> perflow DataFrame for the *latest run* found for that key.
    Expects columns: cellId, staId, rxBytes, goodputbps
    """
    paths = sorted(glob.glob(PERFLOW_GLOB))
    chosen: Dict[Tuple[str, int], Tuple[int, Path]] = {}

    for p in paths:
        name = Path(p).name
        m = PERFLOW_RE.match(name)
        if not m:
            continue
        plan = normalize_plan(m.group("plan"))
        n = int(m.group("n"))
        run = int(m.group("run"))
        key = (plan, n)
        if key not in chosen or run > chosen[key][0]:
            chosen[key] = (run, Path(p))

    out: Dict[Tuple[str, int], pd.DataFrame] = {}
    for (plan, n), (run, path) in chosen.items():
        df = pd.read_csv(path)
        if "goodputbps" not in df.columns:
            continue
        df["goodputbps"] = pd.to_numeric(df["goodputbps"], errors="coerce")
        df["cellId"] = pd.to_numeric(df.get("cellId", np.nan), errors="coerce")
        df["staId"] = pd.to_numeric(df.get("staId", np.nan), errors="coerce")
        df = df.dropna(subset=["goodputbps", "cellId", "staId"]).copy()
        df["cellId"] = df["cellId"].astype(int)
        df["staId"] = df["staId"].astype(int)
        out[(plan, n)] = df

    return out


# -------------------------- Plots from summary --------------------------
def plot_total_goodput(df: pd.DataFrame) -> None:
    g = (
        df.groupby(["channelPlan", "nStaPerCell"], as_index=False)
        .agg(
            goodputTotal_mean=("goodputTotal", "mean"),
            goodputTotal_std=("goodputTotal", "std"),
            runs=("goodputTotal", "count"),
        )
        .sort_values(["nStaPerCell", "channelPlan"])
    )

    plt.figure()
    for plan in ["cocanal", "separe"]:
        sub = g[g["channelPlan"] == plan].sort_values("nStaPerCell")
        if sub.empty:
            continue
        x = sub["nStaPerCell"].to_numpy()
        y = mbps(sub["goodputTotal_mean"])
        yerr = mbps(sub["goodputTotal_std"].fillna(0.0))
        plt.errorbar(x, y, yerr=yerr, marker="o", capsize=3, label=f"{plan} (mean±std)")

    plt.xlabel("nStaPerCell")
    plt.ylabel("Total Goodput (Mbps)")
    plt.title("P7: Total Goodput vs nStaPerCell")
    plt.grid(True, which="both")
    plt.legend()
    plt.tight_layout()
    plt.savefig(PLOTS_DIR / "goodput_total_vs_n.png", dpi=200)
    plt.close()


def plot_cells_goodput(df: pd.DataFrame) -> None:
    g = (
        df.groupby(["channelPlan", "nStaPerCell"], as_index=False)
        .agg(
            cell1_mean=("goodputCell1", "mean"),
            cell1_std=("goodputCell1", "std"),
            cell2_mean=("goodputCell2", "mean"),
            cell2_std=("goodputCell2", "std"),
            runs=("goodputCell1", "count"),
        )
        .sort_values(["nStaPerCell", "channelPlan"])
    )

    plt.figure()
    for plan in ["cocanal", "separe"]:
        sub = g[g["channelPlan"] == plan].sort_values("nStaPerCell")
        if sub.empty:
            continue
        x = sub["nStaPerCell"].to_numpy()

        plt.errorbar(x, mbps(sub["cell1_mean"]), yerr=mbps(sub["cell1_std"].fillna(0.0)),
                     marker="o", capsize=3, label=f"{plan} - cell1")
        plt.errorbar(x, mbps(sub["cell2_mean"]), yerr=mbps(sub["cell2_std"].fillna(0.0)),
                     marker="s", capsize=3, label=f"{plan} - cell2")

    plt.xlabel("nStaPerCell")
    plt.ylabel("Goodput (Mbps)")
    plt.title("P7: Per-Cell Goodput vs nStaPerCell")
    plt.grid(True, which="both")
    plt.legend()
    plt.tight_layout()
    plt.savefig(PLOTS_DIR / "goodput_cells_vs_n.png", dpi=200)
    plt.close()


def plot_jain(df: pd.DataFrame) -> None:
    g = (
        df.groupby(["channelPlan", "nStaPerCell"], as_index=False)
        .agg(
            jain_mean=("jainCells", "mean"),
            jain_std=("jainCells", "std"),
            runs=("jainCells", "count"),
        )
        .sort_values(["nStaPerCell", "channelPlan"])
    )

    plt.figure()
    for plan in ["cocanal", "separe"]:
        sub = g[g["channelPlan"] == plan].sort_values("nStaPerCell")
        if sub.empty:
            continue
        x = sub["nStaPerCell"].to_numpy()
        y = sub["jain_mean"].to_numpy(dtype=float)
        yerr = sub["jain_std"].fillna(0.0).to_numpy(dtype=float)
        plt.errorbar(x, y, yerr=yerr, marker="o", capsize=3, label=f"{plan} (mean±std)")

    plt.ylim(0.0, 1.05)
    plt.xlabel("nStaPerCell")
    plt.ylabel("Jain Fairness (between 2 cells)")
    plt.title("P7: Jain Fairness vs nStaPerCell")
    plt.grid(True, which="both")
    plt.legend()
    plt.tight_layout()
    plt.savefig(PLOTS_DIR / "jain_vs_n.png", dpi=200)
    plt.close()


# -------------------------- Per-STA plots (N=10) --------------------------
def _persta_df(perflow: Dict[Tuple[str, int], pd.DataFrame], n: int) -> Optional[pd.DataFrame]:
    rows = []
    for (plan, nn), df in perflow.items():
        if nn != n:
            continue
        tmp = df.copy()
        tmp["plan"] = plan
        tmp["goodputMbps"] = mbps(tmp["goodputbps"])
        tmp["label"] = tmp.apply(lambda r: f"C{int(r['cellId'])}-S{int(r['staId'])}", axis=1)
        rows.append(tmp[["plan", "cellId", "staId", "label", "goodputMbps"]])
    if not rows:
        return None
    out = pd.concat(rows, ignore_index=True)
    # collapse duplicates just in case
    out = out.groupby(["plan", "cellId", "staId", "label"], as_index=False)["goodputMbps"].mean()
    return out


def plot_per_sta_bars(perflow: Dict[Tuple[str, int], pd.DataFrame], n: int = 10) -> None:
    all_df = _persta_df(perflow, n)
    if all_df is None:
        return

    plans = ["cocanal", "separe"]
    labels = sorted(
        all_df["label"].unique(),
        key=lambda s: (int(s.split("-")[0][1:]), int(s.split("-")[1][1:]))
    )

    x = np.arange(len(labels))
    width = 0.35

    plt.figure(figsize=(max(10, len(labels) * 0.55), 5))
    for i, plan in enumerate(plans):
        sub = all_df[all_df["plan"] == plan].set_index("label")["goodputMbps"]
        y = [float(sub.loc[l]) if l in sub.index else 0.0 for l in labels]
        plt.bar(x + (i - 0.5) * width, y, width=width, label=plan)

    plt.xticks(x, labels, rotation=45, ha="right")
    plt.ylabel("Per-STA Goodput (Mbps)")
    plt.title(f"P7: Per-STA Goodput (Bar) for nStaPerCell={n}")
    plt.grid(True, axis="y")
    plt.legend()
    plt.tight_layout()
    plt.savefig(PLOTS_DIR / f"per_sta_bars_n{n}.png", dpi=200)
    plt.close()


def plot_per_sta_box(perflow: Dict[Tuple[str, int], pd.DataFrame], n: int = 10) -> None:
    data = []
    labels = []
    for plan in ["cocanal", "separe"]:
        key = (plan, n)
        if key not in perflow:
            continue
        vals = mbps(perflow[key]["goodputbps"])
        if len(vals) == 0:
            continue
        data.append(vals)
        labels.append(plan)

    if not data:
        return

    plt.figure()
    plt.boxplot(data, labels=labels, showmeans=True)
    plt.ylabel("Per-STA Goodput (Mbps)")
    plt.title(f"P7: Per-STA Goodput Distribution (Box) for nStaPerCell={n}")
    plt.grid(True, axis="y")
    plt.tight_layout()
    plt.savefig(PLOTS_DIR / f"per_sta_box_n{n}.png", dpi=200)
    plt.close()


def plot_per_sta_cdf(perflow: Dict[Tuple[str, int], pd.DataFrame], n: int = 10) -> None:
    plt.figure()
    plotted = False

    for plan in ["cocanal", "separe"]:
        key = (plan, n)
        if key not in perflow:
            continue
        vals = np.sort(mbps(perflow[key]["goodputbps"]))
        if len(vals) == 0:
            continue
        y = np.arange(1, len(vals) + 1) / len(vals)
        plt.plot(vals, y, marker="o", linestyle="-", label=plan)
        plotted = True

    if not plotted:
        plt.close()
        return

    plt.xlabel("Per-STA Goodput (Mbps)")
    plt.ylabel("CDF")
    plt.title(f"P7: CDF of Per-STA Goodput for nStaPerCell={n}")
    plt.grid(True, which="both")
    plt.legend()
    plt.tight_layout()
    plt.savefig(PLOTS_DIR / f"per_sta_cdf_n{n}.png", dpi=200)
    plt.close()


def plot_per_sta_ecdf_by_cell(perflow: Dict[Tuple[str, int], pd.DataFrame], n: int = 10) -> None:
    all_df = _persta_df(perflow, n)
    if all_df is None:
        return

    plt.figure()
    plotted = False

    for plan in ["cocanal", "separe"]:
        for cell in [1, 2]:
            sub = all_df[(all_df["plan"] == plan) & (all_df["cellId"] == cell)]
            if sub.empty:
                continue
            vals = np.sort(sub["goodputMbps"].to_numpy(dtype=float))
            y = np.arange(1, len(vals) + 1) / len(vals)
            plt.plot(vals, y, marker="o", linestyle="-", label=f"{plan}-cell{cell}")
            plotted = True

    if not plotted:
        plt.close()
        return

    plt.xlabel("Per-STA Goodput (Mbps)")
    plt.ylabel("CDF")
    plt.title(f"P7: CDF of Per-STA Goodput by Cell (nStaPerCell={n})")
    plt.grid(True, which="both")
    plt.legend()
    plt.tight_layout()
    plt.savefig(PLOTS_DIR / f"per_sta_ecdf_by_cell_n{n}.png", dpi=200)
    plt.close()


def plot_per_sta_box_by_cell(perflow: Dict[Tuple[str, int], pd.DataFrame], n: int = 10) -> None:
    all_df = _persta_df(perflow, n)
    if all_df is None:
        return

    data = []
    labels = []

    for plan in ["cocanal", "separe"]:
        for cell in [1, 2]:
            sub = all_df[(all_df["plan"] == plan) & (all_df["cellId"] == cell)]
            if sub.empty:
                continue
            data.append(sub["goodputMbps"].to_numpy(dtype=float))
            labels.append(f"{plan}-cell{cell}")

    if not data:
        return

    plt.figure(figsize=(max(8, len(labels) * 1.2), 5))
    plt.boxplot(data, labels=labels, showmeans=True)
    plt.ylabel("Per-STA Goodput (Mbps)")
    plt.title(f"P7: Per-STA Goodput Distribution by Cell (Box) for nStaPerCell={n}")
    plt.grid(True, axis="y")
    plt.xticks(rotation=30, ha="right")
    plt.tight_layout()
    plt.savefig(PLOTS_DIR / f"per_sta_box_by_cell_n{n}.png", dpi=200)
    plt.close()


# -------------------------- Main --------------------------
def main() -> int:
    ensure_dirs()

    df = read_summary()

    # Required / core plots
    plot_total_goodput(df)
    plot_cells_goodput(df)
    plot_jain(df)

    # Per-STA plots (latest run per key)
    perflow = read_perflow_latest()
    if not perflow:
        print("[plot_p7] NOTE: No perflow_*.csv found; per-STA plots will be skipped.")
    else:
        plot_per_sta_bars(perflow, n=10)
        plot_per_sta_box(perflow, n=10)
        plot_per_sta_cdf(perflow, n=10)
        plot_per_sta_ecdf_by_cell(perflow, n=10)
        plot_per_sta_box_by_cell(perflow, n=10)

    print("[plot_p7] Done. Plots saved in:", PLOTS_DIR)
    for p in sorted(PLOTS_DIR.glob("*.png")):
        print(" -", p.relative_to(ROOT))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
