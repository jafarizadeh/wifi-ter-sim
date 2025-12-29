#!/usr/bin/env python3
# -*- coding: utf-8 -*-

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys

import numpy as np
import pandas as pd
import plotly.graph_objects as go


# -------------------- Paths --------------------
def repo_root() -> Path:
    # WIFI-TER-SIM/scripts/plot_p7.py -> parents[1] == repo root
    return Path(__file__).resolve().parents[1]


def resolve_results_dir(user_path: str | None, project: str = "p7") -> Path:
    root = repo_root()
    if user_path:
        p = Path(user_path)
        if p.exists():
            return p.resolve()
        p2 = (root / user_path).resolve()
        if p2.exists():
            return p2
        raise FileNotFoundError(f"--results not found: {user_path}")
    return (root / "results" / project).resolve()


def ensure_dir(d: Path) -> None:
    d.mkdir(parents=True, exist_ok=True)


# -------------------- Safe save --------------------
def save_fig_safe(fig: go.Figure, out_dir: Path, stem: str, strict_png: bool) -> None:
    """
    Always writes HTML. Writes PNG via kaleido if available.
    If strict_png=True then PNG failure is fatal.
    """
    ensure_dir(out_dir)

    html = out_dir / f"{stem}.html"
    fig.write_html(str(html), include_plotlyjs="cdn")

    png = out_dir / f"{stem}.png"
    try:
        fig.write_image(str(png), scale=2)
    except Exception as e:
        if strict_png:
            raise RuntimeError(f"PNG export failed for '{stem}': {e}") from e


# -------------------- Helpers --------------------
def must_cols(df: pd.DataFrame, cols: list[str]) -> None:
    missing = [c for c in cols if c not in df.columns]
    if missing:
        raise ValueError(f"Missing columns: {missing}. Available: {list(df.columns)}")


def to_num(df: pd.DataFrame, cols: list[str]) -> None:
    for c in cols:
        if c in df.columns:
            df[c] = pd.to_numeric(df[c], errors="coerce")


def mean_std(x: pd.Series) -> tuple[float, float, int]:
    v = pd.to_numeric(x, errors="coerce").dropna().astype(float)
    n = int(len(v))
    if n == 0:
        return np.nan, np.nan, 0
    if n == 1:
        return float(v.iloc[0]), 0.0, 1
    return float(v.mean()), float(v.std(ddof=1)), n


def bps_to_mbps(series: pd.Series) -> pd.Series:
    s = pd.to_numeric(series, errors="coerce")
    return s / 1e6


def plan_pretty(p: str) -> str:
    p = str(p).strip().lower()
    if p in ("cocanal", "cochannel", "co-channel", "co_canal"):
        return "co-canal"
    if p in ("separe", "separee", "separé", "separe_", "separate"):
        return "séparé"
    return p


def read_csv_robust(path: Path) -> pd.DataFrame:
    if not path.exists() or path.stat().st_size == 0:
        raise FileNotFoundError(f"Missing CSV: {path}")
    try:
        return pd.read_csv(path)
    except Exception:
        # more forgiving
        return pd.read_csv(path, engine="python")


# -------------------- Mandatory plots --------------------
def figure1_goodput_total_vs_n(summary: pd.DataFrame, out_dir: Path, strict_png: bool) -> None:
    must_cols(summary, ["channelPlan", "nStaPerCell", "goodputTotal"])
    df = summary.copy()

    df["plan"] = df["channelPlan"].apply(plan_pretty)
    df["N"] = pd.to_numeric(df["nStaPerCell"], errors="coerce")
    df["goodputTotal_Mbps"] = bps_to_mbps(df["goodputTotal"])

    rows = []
    for (plan, n), g in df.groupby(["plan", "N"], dropna=True):
        m, s, cnt = mean_std(g["goodputTotal_Mbps"])
        rows.append({"plan": plan, "N": n, "mean": m, "std": s, "count": cnt})
    agg = pd.DataFrame(rows).dropna(subset=["N"]).sort_values(["plan", "N"])

    fig = go.Figure()
    for plan, g in agg.groupby("plan"):
        g = g.sort_values("N")
        fig.add_trace(
            go.Scatter(
                x=g["N"],
                y=g["mean"],
                mode="lines+markers",
                name=plan,
                error_y=dict(type="data", array=g["std"].fillna(0.0)),
                hovertemplate="N=%{x}<br>Total=%{y:.3f} Mbps<extra></extra>",
            )
        )

    fig.update_layout(
        title="P7 - Figure 1: Total goodput vs N (co-canal vs séparé)",
        xaxis_title="N (stations per cell)",
        yaxis_title="Total goodput (Mbps)",
        template="plotly_white",
        hovermode="x unified",
        legend_title="Channel plan",
    )
    save_fig_safe(fig, out_dir, "p7_fig1_total_goodput_vs_n", strict_png)


def figure2_goodput_per_cell_bars(summary: pd.DataFrame, out_dir: Path, strict_png: bool) -> None:
    must_cols(summary, ["channelPlan", "nStaPerCell", "goodputCell1", "goodputCell2"])
    df = summary.copy()

    df["plan"] = df["channelPlan"].apply(plan_pretty)
    df["N"] = pd.to_numeric(df["nStaPerCell"], errors="coerce")
    df["cell1_Mbps"] = bps_to_mbps(df["goodputCell1"])
    df["cell2_Mbps"] = bps_to_mbps(df["goodputCell2"])

    Ns = sorted([int(x) for x in df["N"].dropna().unique().tolist()])

    for n in Ns:
        gN = df[df["N"] == n].copy()
        if gN.empty:
            continue

        rows = []
        for plan, g in gN.groupby("plan"):
            m1, s1, _ = mean_std(g["cell1_Mbps"])
            m2, s2, _ = mean_std(g["cell2_Mbps"])
            rows.append({"plan": plan, "cell": "Cell 1", "mean": m1, "std": s1})
            rows.append({"plan": plan, "cell": "Cell 2", "mean": m2, "std": s2})
        agg = pd.DataFrame(rows)

        fig = go.Figure()
        for cell, gc in agg.groupby("cell"):
            fig.add_trace(
                go.Bar(
                    x=gc["plan"],
                    y=gc["mean"],
                    error_y=dict(type="data", array=gc["std"].fillna(0.0)),
                    name=cell,
                    hovertemplate="plan=%{x}<br>goodput=%{y:.3f} Mbps<extra></extra>",
                )
            )

        fig.update_layout(
            title=f"P7 - Figure 2: Per-cell goodput (N={n})",
            xaxis_title="Channel plan",
            yaxis_title="Goodput (Mbps)",
            barmode="group",
            template="plotly_white",
        )
        save_fig_safe(fig, out_dir, f"p7_fig2_percell_goodput_n{n}", strict_png)


def figure3_jain_vs_n(summary: pd.DataFrame, out_dir: Path, strict_png: bool) -> None:
    must_cols(summary, ["channelPlan", "nStaPerCell", "jainCells"])
    df = summary.copy()

    df["plan"] = df["channelPlan"].apply(plan_pretty)
    df["N"] = pd.to_numeric(df["nStaPerCell"], errors="coerce")
    df["jain"] = pd.to_numeric(df["jainCells"], errors="coerce")

    rows = []
    for (plan, n), g in df.groupby(["plan", "N"], dropna=True):
        m, s, cnt = mean_std(g["jain"])
        rows.append({"plan": plan, "N": n, "mean": m, "std": s, "count": cnt})
    agg = pd.DataFrame(rows).dropna(subset=["N"]).sort_values(["plan", "N"])

    fig = go.Figure()
    for plan, g in agg.groupby("plan"):
        g = g.sort_values("N")
        fig.add_trace(
            go.Scatter(
                x=g["N"],
                y=g["mean"],
                mode="lines+markers",
                name=plan,
                error_y=dict(type="data", array=g["std"].fillna(0.0)),
                hovertemplate="N=%{x}<br>Jain=%{y:.4f}<extra></extra>",
            )
        )

    fig.update_layout(
        title="P7 - Figure 3: Jain fairness between cells vs N",
        xaxis_title="N (stations per cell)",
        yaxis_title="Jain index (0..1)",
        template="plotly_white",
        hovermode="x unified",
        legend_title="Channel plan",
        yaxis=dict(range=[0, 1.05]),
    )
    save_fig_safe(fig, out_dir, "p7_fig3_jain_vs_n", strict_png)


# -------------------- Recommended plots (per-flow) --------------------
def load_perflow(raw_dir: Path) -> pd.DataFrame | None:
    """
    Supports:
      1) raw/perflow/perflow_*.csv (preferred)
      2) raw/perflow_all.csv (optional consolidated)
    """
    perflow_dir = raw_dir / "perflow"
    rows: list[pd.DataFrame] = []

    # 1) raw/perflow/perflow_*.csv
    if perflow_dir.exists():
        files = sorted(perflow_dir.glob("perflow_*.csv"))
        for f in files:
            # expected name: perflow_{plan}_n{N}_run{run}.csv
            m = re.match(r"perflow_(\w+)_n(\d+)_run(\d+)\.csv", f.name)
            if not m:
                continue
            plan, n, run = m.group(1), int(m.group(2)), int(m.group(3))
            df = read_csv_robust(f)
            if df.empty:
                continue
            if not {"cellId", "staId", "goodputbps"}.issubset(set(df.columns)):
                continue
            df = df.copy()
            df["plan"] = plan_pretty(plan)
            df["N"] = n
            df["run"] = run
            df["goodput_Mbps"] = bps_to_mbps(df["goodputbps"])
            rows.append(df[["plan", "N", "run", "cellId", "staId", "goodput_Mbps"]])

    # 2) raw/perflow_all.csv (fallback)
    perflow_all = raw_dir / "perflow_all.csv"
    if not rows and perflow_all.exists():
        df = read_csv_robust(perflow_all)
        if {"cellId", "staId", "goodputbps"}.issubset(set(df.columns)):
            df = df.copy()
            df["plan"] = "unknown"
            df["N"] = np.nan
            df["run"] = np.nan
            df["goodput_Mbps"] = bps_to_mbps(df["goodputbps"])
            rows.append(df[["plan", "N", "run", "cellId", "staId", "goodput_Mbps"]])

    if not rows:
        return None
    return pd.concat(rows, ignore_index=True)


def recommended_violin_persta(perflow: pd.DataFrame, out_dir: Path, strict_png: bool) -> None:
    if perflow is None or perflow.empty:
        return

    # Choose largest N if known; otherwise use all
    Ns = sorted([int(x) for x in perflow["N"].dropna().unique().tolist()]) if "N" in perflow.columns else []
    if Ns:
        Nmax = Ns[-1]
        g = perflow[perflow["N"] == Nmax].copy()
        title_n = f"(N={Nmax})"
        stem = f"p7_rec_violin_persta_n{Nmax}"
    else:
        g = perflow.copy()
        title_n = "(all N)"
        stem = "p7_rec_violin_persta_all"

    if g.empty:
        return

    g["cell"] = g["cellId"].apply(lambda c: f"Cell {int(c)+1}" if pd.notna(c) else "Cell ?")
    g["group"] = g["plan"].astype(str) + " | " + g["cell"].astype(str)

    fig = go.Figure()
    for grp, gg in g.groupby("group"):
        fig.add_trace(
            go.Violin(
                y=gg["goodput_Mbps"],
                name=grp,
                box_visible=True,
                meanline_visible=True,
                points="outliers",
                hovertemplate="goodput=%{y:.3f} Mbps<extra></extra>",
            )
        )

    fig.update_layout(
        title=f"P7 (Recommended) - Per-STA goodput distribution {title_n}",
        yaxis_title="Per-STA goodput (Mbps)",
        template="plotly_white",
    )
    save_fig_safe(fig, out_dir, stem, strict_png)


# -------------------- Main --------------------
def main() -> int:
    ap = argparse.ArgumentParser(description="Plot Project 7 (channel planning) results.")
    ap.add_argument("--results", type=str, default=None, help="Path to results/p7 (default: <repo>/results/p7)")
    ap.add_argument("--strict-png", action="store_true", help="Fail if PNG export fails (requires kaleido).")
    args = ap.parse_args()

    res_dir = resolve_results_dir(args.results, "p7")
    raw_dir = res_dir / "raw"
    plot_dir = res_dir / "plots"
    ensure_dir(plot_dir)

    summary_path = raw_dir / "p7_summary.csv"
    if not summary_path.exists():
        print(f"[P7] ERROR: Missing: {summary_path}", file=sys.stderr)
        print("Run: ./scripts/run_p7.sh", file=sys.stderr)
        return 2

    summary = read_csv_robust(summary_path)

    # Mandatory
    figure1_goodput_total_vs_n(summary, plot_dir, args.strict_png)
    figure2_goodput_per_cell_bars(summary, plot_dir, args.strict_png)
    figure3_jain_vs_n(summary, plot_dir, args.strict_png)

    # Recommended (if exists)
    perflow = load_perflow(raw_dir)
    if perflow is not None and not perflow.empty:
        recommended_violin_persta(perflow, plot_dir, args.strict_png)

    print("[OK] P7 plots saved to:", plot_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
