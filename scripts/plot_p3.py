#!/usr/bin/env python3
# -*- coding: utf-8 -*-

from __future__ import annotations

import argparse
from pathlib import Path
import sys
import time
from tempfile import NamedTemporaryFile
from typing import Optional

import numpy as np
import pandas as pd

import plotly.graph_objects as go
import plotly.io as pio


EXPECTED_COLS = [
    "distance_m", "transport", "propModel", "logExp", "refDist", "refLoss",
    "simTime", "appStart", "pktSize", "udpRate", "tcpMaxBytes", "seed", "run",
    "rxBytes", "goodput_Mbps", "rtt_mean_ms", "rtt_p95_ms", "rtt_samples"
]

BONUS_RADIO_CANDIDATES = [
    "rssi_dbm", "RSSI_dBm", "rxpower_dbm", "rxPowerDbm", "rx_power_dbm",
    "snr_db", "SNR_dB", "snr", "sinr_db", "SINR_dB"
]


def repo_root_from_this_file() -> Path:
    # WIFI-TER-SIM/scripts/plot_p3.py -> parents[1] is repo root
    return Path(__file__).resolve().parents[1]


def resolve_results_dir(user_path: str | None, project: str) -> Path:
    root = repo_root_from_this_file()
    if user_path:
        p = Path(user_path)
        if p.exists():
            return p.resolve()
        p2 = (root / user_path).resolve()
        if p2.exists():
            return p2
        raise FileNotFoundError(f"--results not found: '{user_path}' (tried CWD and repo root).")
    return (root / "results" / project).resolve()


def ensure_dir(d: Path) -> None:
    d.mkdir(parents=True, exist_ok=True)


# ---------------------------- Safe saving (HTML + PNG) ----------------------------
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
    Save HTML (always) + PNG (via kaleido) safely:
      - Atomic writes (temp -> rename)
      - PNG export with retries
      - strict_png=True => fail if PNG cannot be produced
    """
    ensure_dir(out_dir)

    # HTML (always)
    html_path = out_dir / f"{stem}.html"
    html_str = fig.to_html(include_plotlyjs="cdn", full_html=True)
    _atomic_write_text(html_path, html_str)

    # PNG (requires kaleido)
    png_path = out_dir / f"{stem}.png"
    try:
        import kaleido  # noqa: F401  (presence check)

        last_exc: Optional[Exception] = None
        for _ in range(max_retries):
            try:
                # No engine=... (Plotly warns it's deprecated; kaleido is default)
                png_bytes = pio.to_image(fig, format="png", scale=scale, validate=True)
                _atomic_write_bytes(png_path, png_bytes)
                return
            except Exception as ex:
                last_exc = ex
                time.sleep(retry_sleep_s)

        msg = f"PNG export failed for {png_path.name}: {last_exc}"
        if strict_png:
            raise RuntimeError(msg)
    except ImportError:
        if strict_png:
            raise RuntimeError("kaleido is not installed; cannot export PNG.")


# ---------------------------- CSV reading ----------------------------
def read_sweep_csv(path: Path) -> pd.DataFrame:
    """
    Robust reader:
    - Tries normal read
    - If header is broken (e.g., contains '...') or parsing fails,
      falls back to header=None and assigns EXPECTED_COLS (if counts match).
    """
    if not path.exists() or path.stat().st_size == 0:
        raise FileNotFoundError(f"Missing sweep CSV: {path}")

    first_line = path.open("r", encoding="utf-8", errors="ignore").readline().strip()

    # Fast path (proper header)
    try:
        df = pd.read_csv(path)
        # If header line is suspicious (contains ...) or required cols missing, fallback
        if "..." in first_line or ("distance_m" not in df.columns and df.shape[1] == len(EXPECTED_COLS)):
            raise ValueError("Suspicious header, retrying with fallback.")
        return df
    except Exception:
        pass

    # Fallback: read without header
    df = pd.read_csv(path, header=None, engine="python")
    # If first row is a header-like row, drop it
    if df.shape[1] == len(EXPECTED_COLS):
        row0 = df.iloc[0].astype(str).tolist()
        if any("distance" in s for s in row0):
            df = df.iloc[1:].reset_index(drop=True)
        df.columns = EXPECTED_COLS
        return df

    # Last resort: try python engine with existing header
    return pd.read_csv(path, engine="python")


def coerce_numeric(df: pd.DataFrame, cols: list[str]) -> pd.DataFrame:
    for c in cols:
        if c in df.columns:
            df[c] = pd.to_numeric(df[c], errors="coerce")
    return df


# ---------------------------- Aggregation / labels ----------------------------
def scenario_label(row: pd.Series) -> str:
    parts = []
    if "transport" in row and pd.notna(row["transport"]):
        parts.append(str(row["transport"]))
    if "propModel" in row and pd.notna(row["propModel"]):
        parts.append(str(row["propModel"]))
    # If multiple udpRate/pktSize exist, include them to avoid mixing different configs
    if "udpRate" in row and pd.notna(row["udpRate"]) and str(row["udpRate"]) not in ("0", "0Mbps", "nan"):
        parts.append(f"udpRate={row['udpRate']}")
    if "pktSize" in row and pd.notna(row["pktSize"]):
        try:
            parts.append(f"pkt={int(float(row['pktSize']))}")
        except Exception:
            parts.append(f"pkt={row['pktSize']}")
    return " | ".join(parts) if parts else "scenario"


def mean_ci95(x: pd.Series) -> tuple[float, float, float]:
    x = x.dropna().astype(float)
    if len(x) == 0:
        return np.nan, np.nan, np.nan
    m = float(x.mean())
    if len(x) == 1:
        return m, m, m
    s = float(x.std(ddof=1))
    se = s / np.sqrt(len(x))
    lo = m - 1.96 * se
    hi = m + 1.96 * se
    return m, lo, hi


def aggregate_by_distance(df: pd.DataFrame, metric: str) -> pd.DataFrame:
    """
    Returns per (scenario, distance) mean + CI95 for the metric.
    """
    work = df.copy()
    work["scenario"] = work.apply(scenario_label, axis=1)

    rows = []
    for (sc, dist), g in work.groupby(["scenario", "distance_m"], dropna=True):
        if metric in g.columns:
            m, lo, hi = mean_ci95(g[metric])
            n = int(g[metric].dropna().shape[0])
        else:
            m, lo, hi, n = np.nan, np.nan, np.nan, 0
        rows.append({"scenario": sc, "distance_m": dist, "mean": m, "ci_lo": lo, "ci_hi": hi, "n": n})

    out = pd.DataFrame(rows).sort_values(["scenario", "distance_m"])
    return out


# ---------------------------- Plotting ----------------------------
def plot_line_with_ci(agg: pd.DataFrame, title: str, y_label: str) -> go.Figure:
    fig = go.Figure()
    for sc, g in agg.groupby("scenario"):
        g = g.sort_values("distance_m")
        x = g["distance_m"].to_numpy()
        y = g["mean"].to_numpy()
        lo = g["ci_lo"].to_numpy()
        hi = g["ci_hi"].to_numpy()

        # CI band
        fig.add_trace(go.Scatter(
            x=np.concatenate([x, x[::-1]]),
            y=np.concatenate([hi, lo[::-1]]),
            fill="toself",
            line=dict(width=0),
            hoverinfo="skip",
            name=f"{sc} (CI95%)",
            showlegend=False,
            opacity=0.18,
        ))

        # mean line
        fig.add_trace(go.Scatter(
            x=x,
            y=y,
            mode="lines+markers",
            name=f"{sc} (mean)",
            hovertemplate="distance=%{x} m<br>value=%{y:.3f}<extra></extra>",
        ))

    fig.update_layout(
        title=title,
        xaxis_title="Distance (m)",
        yaxis_title=y_label,
        template="plotly_white",
        hovermode="x unified",
        legend_title="Scenario",
    )
    return fig


def plot_rtt_mean_and_p95(df: pd.DataFrame) -> go.Figure:
    """
    One modern plot: mean RTT (solid) + p95 RTT (dashed) vs distance, with CI on mean.
    """
    df = df.copy()
    df["scenario"] = df.apply(scenario_label, axis=1)

    # mean RTT agg
    agg_mean = aggregate_by_distance(df, "rtt_mean_ms")

    fig = go.Figure()

    # CI + mean line
    for sc, g in agg_mean.groupby("scenario"):
        g = g.sort_values("distance_m")
        x = g["distance_m"].to_numpy()
        y = g["mean"].to_numpy()
        lo = g["ci_lo"].to_numpy()
        hi = g["ci_hi"].to_numpy()

        fig.add_trace(go.Scatter(
            x=np.concatenate([x, x[::-1]]),
            y=np.concatenate([hi, lo[::-1]]),
            fill="toself",
            line=dict(width=0),
            hoverinfo="skip",
            showlegend=False,
            opacity=0.18,
        ))
        fig.add_trace(go.Scatter(
            x=x, y=y, mode="lines+markers",
            name=f"{sc} mean",
            hovertemplate="distance=%{x} m<br>mean RTT=%{y:.3f} ms<extra></extra>",
        ))

    # p95 RTT (no CI, usually enough)
    if "rtt_p95_ms" in df.columns:
        agg_p95 = aggregate_by_distance(df, "rtt_p95_ms")
        for sc, g in agg_p95.groupby("scenario"):
            g = g.sort_values("distance_m")
            fig.add_trace(go.Scatter(
                x=g["distance_m"],
                y=g["mean"],
                mode="lines+markers",
                name=f"{sc} p95",
                line=dict(dash="dash"),
                hovertemplate="distance=%{x} m<br>p95 RTT=%{y:.3f} ms<extra></extra>",
            ))

    fig.update_layout(
        title="P3 - RTT vs Distance (mean + p95)",
        xaxis_title="Distance (m)",
        yaxis_title="RTT (ms)",
        template="plotly_white",
        hovermode="x unified",
        legend_title="Scenario",
    )
    return fig


def find_radio_metric(df: pd.DataFrame) -> str | None:
    cols = set(df.columns)
    for c in BONUS_RADIO_CANDIDATES:
        if c in cols:
            return c
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description="Plot Project 3 (distance sweep) results.")
    ap.add_argument("--results", type=str, default=None, help="Path to results/p3. Default: <repo_root>/results/p3")
    ap.add_argument("--strict-png", action="store_true",
                    help="Fail if PNG cannot be produced (requires kaleido).")
    args = ap.parse_args()

    results_dir = resolve_results_dir(args.results, "p3")
    raw_dir = results_dir / "raw"
    plot_dir = results_dir / "plots"
    ensure_dir(plot_dir)

    sweep_path = raw_dir / "p3_sweep.csv"
    if not sweep_path.exists():
        print(f"[ERROR] Missing: {sweep_path}", file=sys.stderr)
        print("Run the sweep first: ./scripts/run_p3.sh", file=sys.stderr)
        return 2

    df = read_sweep_csv(sweep_path)

    # Numeric coercions
    df = coerce_numeric(df, ["distance_m", "goodput_Mbps", "rtt_mean_ms", "rtt_p95_ms", "rtt_samples", "run"])

    # Drop rows with no distance
    df = df.dropna(subset=["distance_m"]).copy()
    df["distance_m"] = df["distance_m"].astype(float)

    # ---------- Mandatory 1: Goodput vs Distance ----------
    if "goodput_Mbps" not in df.columns:
        print("[ERROR] Column 'goodput_Mbps' not found in p3_sweep.csv", file=sys.stderr)
        return 3
    agg_goodput = aggregate_by_distance(df, "goodput_Mbps")
    fig_goodput = plot_line_with_ci(
        agg_goodput,
        title="P3 - Goodput vs Distance",
        y_label="Goodput (Mbps)",
    )
    save_figure_safe(fig_goodput, plot_dir, "p3_goodput_vs_distance", strict_png=args.strict_png)

    # ---------- Mandatory 2: RTT vs Distance ----------
    if "rtt_mean_ms" in df.columns:
        fig_rtt = plot_rtt_mean_and_p95(df)
        save_figure_safe(fig_rtt, plot_dir, "p3_rtt_vs_distance", strict_png=args.strict_png)
    else:
        # keep warnings minimal (stderr) – runner will redirect to logs anyway
        print("[WARN] Column 'rtt_mean_ms' not found -> skipping RTT plots.", file=sys.stderr)

    # ---------- Recommended: p95 RTT separate ----------
    if "rtt_p95_ms" in df.columns:
        agg_p95 = aggregate_by_distance(df, "rtt_p95_ms")
        fig_p95 = plot_line_with_ci(
            agg_p95,
            title="P3 - RTT p95 vs Distance",
            y_label="RTT p95 (ms)",
        )
        save_figure_safe(fig_p95, plot_dir, "p3_rtt_p95_vs_distance", strict_png=args.strict_png)

    # ---------- Bonus: RSSI/SNR vs Distance (if present) ----------
    radio_col = find_radio_metric(df)
    if radio_col:
        agg_radio = aggregate_by_distance(df, radio_col)
        fig_radio = plot_line_with_ci(
            agg_radio,
            title=f"P3 - {radio_col} vs Distance (Bonus)",
            y_label=radio_col,
        )
        save_figure_safe(fig_radio, plot_dir, f"p3_{radio_col}_vs_distance", strict_png=args.strict_png)

    print("[OK] P3 plots generated in:", plot_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
