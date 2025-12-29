#!/usr/bin/env python3
# -*- coding: utf-8 -*-

from __future__ import annotations

import argparse
from pathlib import Path
import sys

import numpy as np
import pandas as pd
import plotly.graph_objects as go


# ---------- paths ----------
def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def resolve_results_dir(user_path: str | None, project: str = "p5") -> Path:
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


def save_fig_safe(fig: go.Figure, out_dir: Path, stem: str, strict_png: bool) -> None:
    """
    Always saves HTML.
    Saves PNG via kaleido if available.
    If strict_png=True, PNG export failure is fatal.
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


# ---------- helpers ----------
def pick_col(df: pd.DataFrame, candidates: list[str], required: bool = True) -> str | None:
    for c in candidates:
        if c in df.columns:
            return c
    if required:
        raise ValueError(f"Missing required column. Tried: {candidates}")
    return None


def mean_std(x: pd.Series) -> tuple[float, float, int]:
    v = pd.to_numeric(x, errors="coerce").dropna().astype(float)
    n = int(len(v))
    if n == 0:
        return np.nan, np.nan, 0
    if n == 1:
        return float(v.iloc[0]), 0.0, 1
    return float(v.mean()), float(v.std(ddof=1)), n


def as_int_sorted(vals) -> list[int]:
    out: list[int] = []
    for v in vals:
        try:
            out.append(int(float(v)))
        except Exception:
            pass
    return sorted(set(out))


# ---------- plots ----------
def plot_persta_bars(persta: pd.DataFrame, out_dir: Path, strict_png: bool) -> None:
    # expected cols: transport,nSta,run,staId,rxBytes,goodputbps
    transport_col = pick_col(persta, ["transport"])
    n_col = pick_col(persta, ["nSta", "nsta"])
    sta_col = pick_col(persta, ["staId", "staID", "sta"])
    gbps_col = pick_col(persta, ["goodputbps", "goodput_bps", "goodputBps"])

    for c in [n_col, sta_col, gbps_col]:
        persta[c] = pd.to_numeric(persta[c], errors="coerce")

    persta["goodput_Mbps"] = persta[gbps_col] / 1e6

    agg = (
        persta.groupby([transport_col, n_col, sta_col])["goodput_Mbps"]
        .agg(["mean", "std", "count"])
        .reset_index()
        .rename(columns={"mean": "mean_Mbps", "std": "std_Mbps", "count": "n"})
    )

    transports = [t for t in agg[transport_col].dropna().unique().tolist()]
    nstas = as_int_sorted(agg[n_col].dropna().unique().tolist())

    for t in transports:
        for n in nstas:
            g = agg[(agg[transport_col] == t) & (agg[n_col] == n)].copy()
            if g.empty:
                continue
            g = g.sort_values(sta_col)

            fig = go.Figure()
            fig.add_trace(
                go.Bar(
                    x=[f"STA{int(s)}" for s in g[sta_col].to_numpy()],
                    y=g["mean_Mbps"],
                    error_y=dict(type="data", array=g["std_Mbps"].fillna(0.0)),
                    hovertemplate="STA=%{x}<br>goodput=%{y:.3f} Mbps<extra></extra>",
                    name=f"{t} | N={n}",
                )
            )
            fig.update_layout(
                title=f"P5 - Goodput per STA (transport={t}, N={n})",
                xaxis_title="Station",
                yaxis_title="Goodput (Mbps)",
                template="plotly_white",
            )
            save_fig_safe(fig, out_dir, f"persta_{t}_n{n}", strict_png)


def plot_jain_vs_n(summary: pd.DataFrame, out_dir: Path, strict_png: bool) -> None:
    transport_col = pick_col(summary, ["transport"])
    n_col = pick_col(summary, ["nSta", "nsta"])
    j_col = pick_col(summary, ["jain", "jainFairness", "jain_fairness"])

    summary[n_col] = pd.to_numeric(summary[n_col], errors="coerce")
    summary[j_col] = pd.to_numeric(summary[j_col], errors="coerce")

    rows = []
    for (t, n), g in summary.groupby([transport_col, n_col]):
        m, s, cnt = mean_std(g[j_col])
        if pd.isna(n):
            continue
        rows.append({"transport": t, "nSta": int(n), "mean": m, "std": s, "count": cnt})
    agg = pd.DataFrame(rows).sort_values(["transport", "nSta"])

    fig = go.Figure()
    for t, g in agg.groupby("transport"):
        g = g.sort_values("nSta")
        fig.add_trace(
            go.Scatter(
                x=g["nSta"],
                y=g["mean"],
                mode="lines+markers",
                name=str(t),
                error_y=dict(type="data", array=g["std"].fillna(0.0)),
                hovertemplate="N=%{x}<br>Jain=%{y:.4f}<extra></extra>",
            )
        )

    fig.update_layout(
        title="P5 - Jain Fairness vs N (UDP vs TCP)",
        xaxis_title="Number of STAs (N)",
        yaxis_title="Jain fairness index",
        template="plotly_white",
        hovermode="x unified",
        yaxis=dict(range=[0, 1.05]),
    )
    save_fig_safe(fig, out_dir, "jain_vs_n", strict_png)


def plot_sumgoodput_vs_n(summary: pd.DataFrame, out_dir: Path, strict_png: bool) -> None:
    transport_col = pick_col(summary, ["transport"])
    n_col = pick_col(summary, ["nSta", "nsta"])
    sum_col = pick_col(summary, ["sumGoodputbps", "sumGoodputBps", "sum_goodput_bps"])

    summary[n_col] = pd.to_numeric(summary[n_col], errors="coerce")
    summary[sum_col] = pd.to_numeric(summary[sum_col], errors="coerce")
    summary["sumGoodput_Mbps"] = summary[sum_col] / 1e6

    rows = []
    for (t, n), g in summary.groupby([transport_col, n_col]):
        m, s, cnt = mean_std(g["sumGoodput_Mbps"])
        if pd.isna(n):
            continue
        rows.append({"transport": t, "nSta": int(n), "mean": m, "std": s, "count": cnt})
    agg = pd.DataFrame(rows).sort_values(["transport", "nSta"])

    fig = go.Figure()
    for t, g in agg.groupby("transport"):
        g = g.sort_values("nSta")
        fig.add_trace(
            go.Scatter(
                x=g["nSta"],
                y=g["mean"],
                mode="lines+markers",
                name=str(t),
                error_y=dict(type="data", array=g["std"].fillna(0.0)),
                hovertemplate="N=%{x}<br>Sum goodput=%{y:.3f} Mbps<extra></extra>",
            )
        )

    fig.update_layout(
        title="P5 - Sum of goodputs vs N (UDP vs TCP)",
        xaxis_title="Number of STAs (N)",
        yaxis_title="Sum goodput (Mbps)",
        template="plotly_white",
        hovermode="x unified",
    )
    save_fig_safe(fig, out_dir, "sumgoodput_vs_n", strict_png)


def plot_timeseries_optional(ts: pd.DataFrame | None, out_dir: Path, strict_png: bool) -> None:
    if ts is None or ts.empty:
        return

    transport_col = pick_col(ts, ["transport"])
    n_col = pick_col(ts, ["nSta", "nsta"])
    time_col = pick_col(ts, ["time_s", "time"])
    thr_col = pick_col(ts, ["throughput_bps", "throughputBps"])

    for c in [n_col, time_col, thr_col]:
        ts[c] = pd.to_numeric(ts[c], errors="coerce")

    ts["throughput_Mbps"] = ts[thr_col] / 1e6

    agg = (
        ts.groupby([transport_col, n_col, time_col])["throughput_Mbps"]
        .mean()
        .reset_index()
        .rename(columns={"throughput_Mbps": "mean_Mbps"})
    )

    for t, gT in agg.groupby(transport_col):
        fig = go.Figure()
        for n, g in gT.groupby(n_col):
            if pd.isna(n):
                continue
            g = g.sort_values(time_col)
            fig.add_trace(
                go.Scatter(
                    x=g[time_col],
                    y=g["mean_Mbps"],
                    mode="lines",
                    name=f"N={int(n)}",
                    hovertemplate="t=%{x:.2f}s<br>thr=%{y:.3f} Mbps<extra></extra>",
                )
            )
        fig.update_layout(
            title=f"P5 (Bonus) - Aggregated throughput vs time ({t})",
            xaxis_title="Time (s)",
            yaxis_title="Throughput (Mbps)",
            template="plotly_white",
            hovermode="x unified",
            legend_title="Group",
        )
        save_fig_safe(fig, out_dir, f"throughput_vs_time_{t}", strict_png)


def main() -> int:
    ap = argparse.ArgumentParser(description="Plot Project 5 (multi-STA fairness) results.")
    ap.add_argument("--results", type=str, default=None, help="Path to results/p5 (default: <repo>/results/p5)")
    ap.add_argument("--strict-png", action="store_true", help="Fail if PNG export fails (requires kaleido).")
    args = ap.parse_args()

    res_dir = resolve_results_dir(args.results, "p5")
    raw_dir = res_dir / "raw"
    plot_dir = res_dir / "plots"
    ensure_dir(plot_dir)

    summary_path = raw_dir / "p5_summary.csv"
    persta_path = raw_dir / "persta_all.csv"
    ts_path = raw_dir / "ts_all.csv"

    if not summary_path.exists():
        print(f"[ERROR] Missing: {summary_path}", file=sys.stderr)
        print("Run: ./scripts/run_p5.sh", file=sys.stderr)
        return 2
    if not persta_path.exists():
        print(f"[ERROR] Missing: {persta_path}", file=sys.stderr)
        print("Run: ./scripts/run_p5.sh", file=sys.stderr)
        return 3

    summary = pd.read_csv(summary_path)
    persta = pd.read_csv(persta_path)

    ts = None
    if ts_path.exists():
        try:
            ts = pd.read_csv(ts_path)
        except Exception:
            ts = None

    plot_persta_bars(persta, plot_dir, args.strict_png)
    plot_jain_vs_n(summary, plot_dir, args.strict_png)
    plot_sumgoodput_vs_n(summary, plot_dir, args.strict_png)
    plot_timeseries_optional(ts, plot_dir, args.strict_png)

    print("[OK] P5 plots saved to:", plot_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
