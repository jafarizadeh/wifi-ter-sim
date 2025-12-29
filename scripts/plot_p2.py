#!/usr/bin/env python3
# -*- coding: utf-8 -*-

from __future__ import annotations

import argparse
from pathlib import Path
import sys
import time
from typing import Optional
from tempfile import NamedTemporaryFile

import numpy as np
import pandas as pd

import plotly.express as px
import plotly.graph_objects as go
import plotly.io as pio


# ---------------------------- Path helpers ----------------------------
def repo_root_from_this_file() -> Path:
    """
    WIFI-TER-SIM/
      scripts/plot_p2.py  -> parents[1] == WIFI-TER-SIM
    """
    return Path(__file__).resolve().parents[1]


def resolve_results_dir(user_path: str | None, project: str) -> Path:
    """
    Priority:
      1) If user passed --results and it exists (absolute or relative to CWD), use it
      2) If user passed --results but relative and doesn't exist, try relative to repo root
      3) Default: <repo_root>/results/<project>
    """
    root = repo_root_from_this_file()

    if user_path:
        p = Path(user_path)
        if p.exists():
            return p.resolve()
        p2 = (root / user_path).resolve()
        if p2.exists():
            return p2

        raise FileNotFoundError(f"--results path not found: '{user_path}' (tried CWD and repo root).")

    return (root / "results" / project).resolve()


# ---------------------------- IO / data ----------------------------
def _read_csv_optional(path: Path) -> pd.DataFrame | None:
    if path.exists() and path.stat().st_size > 0:
        return pd.read_csv(path)
    return None


def load_p2_data(raw_dir: Path) -> tuple[pd.DataFrame, pd.DataFrame, pd.DataFrame]:
    thr = _read_csv_optional(raw_dir / "throughput_timeseries_all.csv")
    rtt = _read_csv_optional(raw_dir / "rtt_timeseries_all.csv")
    summ = _read_csv_optional(raw_dir / "p2_summary_all.csv")

    # Fallbacks if consolidated not present
    if thr is None:
        thr_udp = _read_csv_optional(raw_dir / "throughput_timeseries_udp.csv")
        thr_tcp = _read_csv_optional(raw_dir / "throughput_timeseries_tcp.csv")
        frames = []
        if thr_udp is not None:
            t = thr_udp.copy()
            t["transport"] = "udp"
            t["run"] = 1
            frames.append(t)
        if thr_tcp is not None:
            t = thr_tcp.copy()
            t["transport"] = "tcp"
            t["run"] = 2
            frames.append(t)
        thr = pd.concat(frames, ignore_index=True) if frames else pd.DataFrame()

    if rtt is None:
        rtt_udp = _read_csv_optional(raw_dir / "rtt_timeseries_udp.csv")
        rtt_tcp = _read_csv_optional(raw_dir / "rtt_timeseries_tcp.csv")
        frames = []
        if rtt_udp is not None:
            t = rtt_udp.copy()
            t["transport"] = "udp"
            t["run"] = 1
            frames.append(t)
        if rtt_tcp is not None:
            t = rtt_tcp.copy()
            t["transport"] = "tcp"
            t["run"] = 2
            frames.append(t)
        rtt = pd.concat(frames, ignore_index=True) if frames else pd.DataFrame()

    if summ is None:
        summ = _read_csv_optional(raw_dir / "p2_summary.csv")
        if summ is None:
            summ = pd.DataFrame()

    # Normalize / validate expected columns
    if not thr.empty:
        if "time_s" not in thr.columns or "throughput_bps" not in thr.columns:
            raise ValueError("Throughput CSV must contain columns: time_s, throughput_bps")
        if "transport" not in thr.columns:
            thr["transport"] = "unknown"
        if "run" not in thr.columns:
            thr["run"] = 1
        thr["transport"] = thr["transport"].astype(str)
        thr["run"] = pd.to_numeric(thr["run"], errors="coerce").fillna(1).astype(int)


    if not rtt.empty:
        if "time_s" not in rtt.columns or "rtt_ms" not in rtt.columns:
            raise ValueError("RTT CSV must contain columns: time_s, rtt_ms")
        if "transport" not in rtt.columns:
            rtt["transport"] = "unknown"
        if "run" not in rtt.columns:
            rtt["run"] = 1
        rtt["transport"] = rtt["transport"].astype(str)
        rtt["run"] = pd.to_numeric(rtt["run"], errors="coerce").fillna(1).astype(int)

    return thr, rtt, summ


def ensure_dir(d: Path) -> None:
    d.mkdir(parents=True, exist_ok=True)


# ---------------------------- Safe saving ----------------------------
def _atomic_write_bytes(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with NamedTemporaryFile(dir=str(path.parent), delete=False) as tmp:
        tmp.write(data)
    Path(tmp.name).replace(path)


def _atomic_write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with NamedTemporaryFile(dir=str(path.parent), delete=False, mode="w", encoding="utf-8") as tmp:
        tmp.write(text)
    Path(tmp.name).replace(path)


def save_figure_safe(
    fig: go.Figure,
    out_dir: Path,
    stem: str,
    *,
    scale: int = 2,
    strict_png: bool = False,
    max_retries: int = 3,
    retry_sleep_s: float = 0.4,
) -> None:
    """
    Save HTML (always) + PNG (via kaleido) safely.
      - Atomic writes (temp -> rename)
      - PNG export with retries
      - strict_png: raise if PNG cannot be produced
    """
    # HTML (always)
    html_path = out_dir / f"{stem}.html"
    try:
        html_str = fig.to_html(include_plotlyjs="cdn", full_html=True)
        _atomic_write_text(html_path, html_str)
    except Exception as e:
        print(f"[WARN] HTML save failed for {html_path.name}: {e}", file=sys.stderr)

    # PNG via kaleido
    png_path = out_dir / f"{stem}.png"
    try:
        # Fail-fast if kaleido is missing
        import kaleido  # noqa: F401

        last_exc: Optional[Exception] = None
        for attempt in range(1, max_retries + 1):
            try:
                png_bytes = pio.to_image(fig, format="png", scale=scale, validate=True)

                _atomic_write_bytes(png_path, png_bytes)
                break
            except Exception as ex:
                last_exc = ex
                time.sleep(retry_sleep_s)
        else:
            msg = f"[ERROR] PNG export failed for {png_path.name} after {max_retries} attempts: {last_exc}"
            if strict_png:
                raise RuntimeError(msg)
            print(msg, file=sys.stderr)
    except ImportError:
        msg = (
            "[WARN] PNG export skipped: 'kaleido' is not installed. "
            "Install it inside your venv: python -m pip install kaleido"
        )
        if strict_png:
            raise RuntimeError(msg)
        print(msg, file=sys.stderr)


# ---------------------------- Plot helpers ----------------------------
def add_appstart_vline(fig: go.Figure, app_start: float | None, label: str = "appStart") -> None:
    if app_start is None:
        return
    fig.add_vline(
        x=app_start,
        line_width=2,
        line_dash="dash",
        annotation_text=label,
        annotation_position="top left",
    )


def infer_appstart(summary: pd.DataFrame) -> float | None:
    if summary.empty or "appStart" not in summary.columns:
        return None
    try:
        return float(summary["appStart"].mode().iloc[0])
    except Exception:
        return None


# ---------------------------- Plots (mandatory) ----------------------------
def plot_throughput_timeseries(thr: pd.DataFrame, app_start: float | None) -> go.Figure:
    df = thr.copy()
    df["throughput_mbps"] = df["throughput_bps"] / 1e6

    mean_df = (
        df.groupby(["transport", "time_s"], as_index=False)["throughput_mbps"]
        .mean()
        .rename(columns={"throughput_mbps": "mean_throughput_mbps"})
    )

    fig = go.Figure()

    # faint per-run
    for (transport, run), g in df.groupby(["transport", "run"]):
        fig.add_trace(
            go.Scatter(
                x=g["time_s"],
                y=g["throughput_mbps"],
                mode="lines",
                name=f"{transport} (run {run})",
                opacity=0.35,
            )
        )

    # bold mean
    for transport, g in mean_df.groupby("transport"):
        fig.add_trace(
            go.Scatter(
                x=g["time_s"],
                y=g["mean_throughput_mbps"],
                mode="lines",
                name=f"{transport} (mean)",
                line=dict(width=4),
            )
        )

    fig.update_layout(
        title="P2 - Throughput Time Series (UDP vs TCP)",
        xaxis_title="Time (s)",
        yaxis_title="Throughput (Mbps)",
        template="plotly_white",
        hovermode="x unified",
        legend_title="Transport",
    )
    add_appstart_vline(fig, app_start)
    return fig


def plot_rtt_timeseries(rtt: pd.DataFrame, app_start: float | None) -> go.Figure:
    fig = px.line(
        rtt,
        x="time_s",
        y="rtt_ms",
        color="transport",
        line_group="run",
        title="P2 - RTT Time Series (UDP vs TCP)",
        template="plotly_white",
    )
    fig.update_layout(
        xaxis_title="Time (s)",
        yaxis_title="RTT (ms)",
        hovermode="x unified",
        legend_title="Transport",
    )
    add_appstart_vline(fig, app_start)
    return fig


def plot_goodput_bar(summary: pd.DataFrame) -> go.Figure:
    if summary.empty:
        fig = go.Figure()
        fig.update_layout(title="P2 - Goodput Summary (summary CSV not found)", template="plotly_white")
        return fig

    df = summary.copy()
    if "goodputbps" not in df.columns or "transport" not in df.columns:
        fig = go.Figure()
        fig.update_layout(title="P2 - Goodput Summary (missing columns)", template="plotly_white")
        return fig

    df["goodput_mbps"] = pd.to_numeric(df["goodputbps"], errors="coerce") / 1e6
    df = df.dropna(subset=["goodput_mbps"])

    agg = (
        df.groupby("transport", as_index=False)
          .agg(mean_goodput_mbps=("goodput_mbps", "mean"),
               std_goodput_mbps=("goodput_mbps", "std"))
    )

    fig = go.Figure(
        go.Bar(
            x=agg["transport"],
            y=agg["mean_goodput_mbps"],
            error_y=dict(type="data", array=agg["std_goodput_mbps"].fillna(0.0)),
            name="Goodput (mean ± std)",
        )
    )
    fig.update_layout(
        title="P2 - Goodput Comparison (UDP vs TCP)",
        xaxis_title="Transport",
        yaxis_title="Goodput (Mbps)",
        template="plotly_white",
    )
    return fig


# ---------------------------- Plots (recommended) ----------------------------
def plot_rtt_ecdf(rtt: pd.DataFrame) -> go.Figure:
    df = rtt.copy()
    df = df[np.isfinite(df["rtt_ms"])]

    fig = go.Figure()
    for transport, g in df.groupby("transport"):
        x = np.sort(g["rtt_ms"].to_numpy())
        if x.size == 0:
            continue
        y = np.arange(1, x.size + 1) / x.size
        fig.add_trace(go.Scatter(x=x, y=y, mode="lines", name=str(transport)))

    fig.update_layout(
        title="P2 - RTT ECDF",
        xaxis_title="RTT (ms)",
        yaxis_title="ECDF",
        template="plotly_white",
        hovermode="x unified",
        legend_title="Transport",
    )
    return fig


def plot_rtt_violin(rtt: pd.DataFrame) -> go.Figure:
    fig = px.violin(
        rtt,
        x="transport",
        y="rtt_ms",
        box=True,
        points="outliers",
        title="P2 - RTT Distribution (Violin + Box)",
        template="plotly_white",
    )
    fig.update_layout(xaxis_title="Transport", yaxis_title="RTT (ms)")
    return fig


# ---------------------------- Main ----------------------------
def main() -> int:
    ap = argparse.ArgumentParser(description="Plot Project 2 results (reads from ../results/p2 by default).")
    ap.add_argument(
        "--results",
        type=str,
        default=None,
        help="Path to results/p2 directory. If omitted, uses <repo_root>/results/p2.",
    )
    ap.add_argument(
        "--strict-png",
        action="store_true",
        help="Fail the run if PNG export cannot be produced (requires 'kaleido').",
    )
    args = ap.parse_args()

    results_dir = resolve_results_dir(args.results, project="p2")
    raw_dir = results_dir / "raw"
    plot_dir = results_dir / "plots"

    if not raw_dir.exists():
        print(f"[ERROR] raw directory not found: {raw_dir}", file=sys.stderr)
        print("Expected: <repo_root>/results/p2/raw", file=sys.stderr)
        return 2

    ensure_dir(plot_dir)

    thr, rtt, summ = load_p2_data(raw_dir)
    if thr.empty:
        print("[ERROR] No throughput data found in raw/.", file=sys.stderr)
        return 3
    if rtt.empty:
        print("[ERROR] No RTT data found in raw/.", file=sys.stderr)
        return 4

    app_start = infer_appstart(summ)

    # Mandatory
    save_figure_safe(plot_throughput_timeseries(thr, app_start), plot_dir, "p2_throughput_timeseries", strict_png=args.strict_png)
    save_figure_safe(plot_rtt_timeseries(rtt, app_start),       plot_dir, "p2_rtt_timeseries",       strict_png=args.strict_png)
    save_figure_safe(plot_goodput_bar(summ),                     plot_dir, "p2_goodput_bar",          strict_png=args.strict_png)

    # Recommended
    save_figure_safe(plot_rtt_ecdf(rtt),   plot_dir, "p2_rtt_ecdf",   strict_png=args.strict_png)
    save_figure_safe(plot_rtt_violin(rtt), plot_dir, "p2_rtt_violin", strict_png=args.strict_png)

    print("[OK] Plots generated in:", plot_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
