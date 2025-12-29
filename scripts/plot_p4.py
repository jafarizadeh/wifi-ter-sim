#!/usr/bin/env python3
# -*- coding: utf-8 -*-

from __future__ import annotations

import argparse
from pathlib import Path
import sys

import numpy as np
import pandas as pd
import plotly.graph_objects as go


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def resolve_results_dir(user_path: str | None, project: str = "p4") -> Path:
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


def ensure_dir(p: Path) -> None:
    p.mkdir(parents=True, exist_ok=True)


def save_fig_safe(fig: go.Figure, out_dir: Path, stem: str, strict_png: bool) -> None:
    """
    Always writes HTML. Writes PNG via kaleido if available.
    If strict_png=True, failure to write PNG is fatal.
    """
    ensure_dir(out_dir)
    html = out_dir / f"{stem}.html"
    fig.write_html(str(html), include_plotlyjs="cdn")

    png = out_dir / f"{stem}.png"
    try:
        # do not pass engine=... to avoid future deprecation behavior changes
        fig.write_image(str(png), scale=2)
    except Exception as e:
        if strict_png:
            raise RuntimeError(f"PNG export failed for '{stem}': {e}") from e
        # non-strict: ignore PNG errors (HTML still produced)


def mean_ci95(x: pd.Series) -> tuple[float, float, float, int]:
    x = pd.to_numeric(x, errors="coerce").dropna().astype(float)
    n = int(len(x))
    if n == 0:
        return np.nan, np.nan, np.nan, 0
    m = float(x.mean())
    if n == 1:
        return m, m, m, 1
    s = float(x.std(ddof=1))
    se = s / np.sqrt(n)
    lo = m - 1.96 * se
    hi = m + 1.96 * se
    return m, lo, hi, n


def normalize_columns(df: pd.DataFrame) -> pd.DataFrame:
    # Goodput -> goodputMbps
    if "goodputMbps" in df.columns:
        df["goodputMbps"] = pd.to_numeric(df["goodputMbps"], errors="coerce")
    elif "goodputbps" in df.columns:
        df["goodputMbps"] = pd.to_numeric(df["goodputbps"], errors="coerce") / 1e6
    elif "goodput_Mbps" in df.columns:
        df["goodputMbps"] = pd.to_numeric(df["goodput_Mbps"], errors="coerce")
    else:
        raise ValueError("No goodput column found (expected one of: goodputMbps, goodputbps, goodput_Mbps)")

    # RTT mean -> rttMeanMs (best-effort)
    if "rttMeanMs" in df.columns:
        df["rttMeanMs"] = pd.to_numeric(df["rttMeanMs"], errors="coerce")
    elif "rtt_mean_ms" in df.columns:
        df["rttMeanMs"] = pd.to_numeric(df["rtt_mean_ms"], errors="coerce")
    else:
        df["rttMeanMs"] = np.nan

    # distance column -> distance
    if "distance" not in df.columns and "distance_m" in df.columns:
        df["distance"] = df["distance_m"]

    # numeric coercions
    for c in [
        "distance", "distance_m", "channelWidth", "txPowerDbm", "mcs",
        "run", "seed", "rxBytes", "rttMeanMs", "goodputMbps"
    ]:
        if c in df.columns:
            df[c] = pd.to_numeric(df[c], errors="coerce")

    # rate mode label
    def mode_label(r: pd.Series) -> str:
        rm = str(r.get("rateMode", "")).strip().lower()
        if rm == "adaptive":
            return "adaptive"
        if rm == "constant":
            try:
                return f"constant MCS{int(r.get('mcs', 0))}"
            except Exception:
                return "constant"
        return rm if rm else "unknown"

    df["modeLabel"] = df.apply(mode_label, axis=1)
    return df


def line_ci_figure(agg: pd.DataFrame, x: str, title: str, y_title: str, series_col: str) -> go.Figure:
    fig = go.Figure()

    for name, g in agg.groupby(series_col):
        g = g.sort_values(x)
        xs = g[x].to_numpy()
        ys = g["mean"].to_numpy()
        lo = g["ci_lo"].to_numpy()
        hi = g["ci_hi"].to_numpy()

        fig.add_trace(go.Scatter(
            x=np.concatenate([xs, xs[::-1]]),
            y=np.concatenate([hi, lo[::-1]]),
            fill="toself",
            line=dict(width=0),
            hoverinfo="skip",
            showlegend=False,
            opacity=0.18,
        ))

        fig.add_trace(go.Scatter(
            x=xs,
            y=ys,
            mode="lines+markers",
            name=str(name),
            hovertemplate=f"{x}=%{{x}}<br>{y_title}=%{{y:.3f}}<extra></extra>",
        ))

    fig.update_layout(
        title=title,
        xaxis_title=x,
        yaxis_title=y_title,
        template="plotly_white",
        hovermode="x unified",
        legend_title=series_col,
    )
    return fig


def heatmap_goodput(df: pd.DataFrame, distance: float, mode: str) -> go.Figure:
    g = df[(df["distance"] == distance) & (df["modeLabel"] == mode)].copy()
    if g.empty:
        raise ValueError("No data for heatmap selection")

    pivot = g.groupby(["txPowerDbm", "channelWidth"])["goodputMbps"].mean().reset_index()
    z = pivot.pivot(index="txPowerDbm", columns="channelWidth", values="goodputMbps").sort_index()

    fig = go.Figure(data=go.Heatmap(
        x=z.columns.astype(float),
        y=z.index.astype(float),
        z=z.values,
        hovertemplate="width=%{x} MHz<br>power=%{y} dBm<br>goodput=%{z:.3f} Mbps<extra></extra>",
    ))
    fig.update_layout(
        title=f"P4 - Heatmap Goodput (distance={distance} m, mode={mode})",
        xaxis_title="Channel width (MHz)",
        yaxis_title="Tx power (dBm)",
        template="plotly_white",
    )
    return fig


def main() -> int:
    ap = argparse.ArgumentParser(description="Plot Project 4 (PHY/MAC sweep) results.")
    ap.add_argument("--results", type=str, default=None, help="Path to results/p4 (default: <repo>/results/p4)")
    ap.add_argument("--strict-png", action="store_true", help="Fail if PNG export fails (requires kaleido).")
    args = ap.parse_args()

    res_dir = resolve_results_dir(args.results, "p4")
    raw_dir = res_dir / "raw"
    plot_dir = res_dir / "plots"
    ensure_dir(plot_dir)

    csv_path = raw_dir / "p4_matrix.csv"
    if not csv_path.exists():
        print(f"[ERROR] Missing: {csv_path}", file=sys.stderr)
        print("Run: ./scripts/run_p4.sh", file=sys.stderr)
        return 2

    df = pd.read_csv(csv_path)
    df = normalize_columns(df)

    # Pick-or-closest helper (robust if grid differs)
    def pick_or_closest(col: str, target: float) -> float:
        vals = sorted(v for v in df[col].dropna().unique().tolist())
        if not vals:
            return target
        return min(vals, key=lambda v: abs(v - target))

    power20 = pick_or_closest("txPowerDbm", 20.0)
    d5 = pick_or_closest("distance", 5.0)
    d20 = pick_or_closest("distance", 20.0)
    w80 = pick_or_closest("channelWidth", 80.0)

    # --- Fig 1: Goodput vs width (distance 5 & 20, power 20) ---
    def fig_goodput_vs_width(distance: float, power: float) -> go.Figure:
        sub = df[(df["distance"] == distance) & (df["txPowerDbm"] == power)].copy()
        if sub.empty:
            raise ValueError("No data for required selection (goodput vs width)")
        rows = []
        for (mode, w), g in sub.groupby(["modeLabel", "channelWidth"]):
            m, lo, hi, n = mean_ci95(g["goodputMbps"])
            rows.append({"modeLabel": mode, "channelWidth": w, "mean": m, "ci_lo": lo, "ci_hi": hi, "n": n})
        agg = pd.DataFrame(rows)
        return line_ci_figure(
            agg,
            x="channelWidth",
            title=f"P4 - Goodput vs Channel width (distance={distance} m, power={power} dBm)",
            y_title="Goodput (Mbps)",
            series_col="modeLabel",
        )

    save_fig_safe(fig_goodput_vs_width(d5, power20), plot_dir, f"goodput_vs_width_d{int(d5)}_p{int(power20)}", args.strict_png)
    save_fig_safe(fig_goodput_vs_width(d20, power20), plot_dir, f"goodput_vs_width_d{int(d20)}_p{int(power20)}", args.strict_png)

    # --- Fig 2: Goodput vs power (distance 20, width 80) ---
    sub2 = df[(df["distance"] == d20) & (df["channelWidth"] == w80)].copy()
    if not sub2.empty:
        rows = []
        for (mode, p), g in sub2.groupby(["modeLabel", "txPowerDbm"]):
            m, lo, hi, n = mean_ci95(g["goodputMbps"])
            rows.append({"modeLabel": mode, "txPowerDbm": p, "mean": m, "ci_lo": lo, "ci_hi": hi, "n": n})
        agg2 = pd.DataFrame(rows)
        fig2 = line_ci_figure(
            agg2,
            x="txPowerDbm",
            title=f"P4 - Goodput vs Tx power (distance={d20} m, width={w80} MHz)",
            y_title="Goodput (Mbps)",
            series_col="modeLabel",
        )
        save_fig_safe(fig2, plot_dir, f"goodput_vs_power_d{int(d20)}_w{int(w80)}", args.strict_png)

    # --- Fig 3: RTT vs width (distance 20, power 20) ---
    sub3 = df[(df["distance"] == d20) & (df["txPowerDbm"] == power20)].copy()
    if (not sub3.empty) and sub3["rttMeanMs"].notna().any():
        rows = []
        for (mode, w), g in sub3.groupby(["modeLabel", "channelWidth"]):
            m, lo, hi, n = mean_ci95(g["rttMeanMs"])
            rows.append({"modeLabel": mode, "channelWidth": w, "mean": m, "ci_lo": lo, "ci_hi": hi, "n": n})
        agg3 = pd.DataFrame(rows)
        fig3 = line_ci_figure(
            agg3,
            x="channelWidth",
            title=f"P4 - RTT vs Channel width (distance={d20} m, power={power20} dBm)",
            y_title="RTT mean (ms)",
            series_col="modeLabel",
        )
        save_fig_safe(fig3, plot_dir, f"rtt_vs_width_d{int(d20)}_p{int(power20)}", args.strict_png)
        save_fig_safe(fig3, plot_dir, "rtt_comparison", args.strict_png)

    # --- Optional heatmaps ---
    try:
        for dist in [d5, d20]:
            for mode in ["adaptive", "constant MCS0", "constant MCS7"]:
                if df[(df["distance"] == dist) & (df["modeLabel"] == mode)].shape[0] >= 4:
                    fig_hm = heatmap_goodput(df, dist, mode)
                    save_fig_safe(fig_hm, plot_dir, f"heatmap_goodput_d{int(dist)}_{mode.replace(' ','_')}", args.strict_png)
    except Exception:
        pass

    print("[OK] P4 plots saved to:", plot_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
