#!/usr/bin/env python3
# -*- coding: utf-8 -*-

from __future__ import annotations

import argparse
from pathlib import Path
import sys
from typing import Iterable

import numpy as np
import pandas as pd
import plotly.graph_objects as go


# ---------------- Paths ----------------
def repo_root() -> Path:
    # WIFI-TER-SIM/scripts/plot_p8.py -> parents[1] is repo root
    return Path(__file__).resolve().parents[1]


def resolve_results_dir(user_path: str | None, project: str = "p8") -> Path:
    root = repo_root()
    if user_path:
        p = Path(user_path)
        if p.exists():
            return p.resolve()
        p2 = (root / user_path).resolve()
        if p2.exists():
            return p2
        raise FileNotFoundError(f"--results not found: {user_path} (tried CWD and repo root).")
    return (root / "results" / project).resolve()


def ensure_dir(d: Path) -> None:
    d.mkdir(parents=True, exist_ok=True)


# ---------------- Robust I/O ----------------
def read_csv_robust(path: Path) -> pd.DataFrame:
    if not path.exists() or path.stat().st_size == 0:
        raise FileNotFoundError(f"Missing/empty CSV: {path}")
    try:
        return pd.read_csv(path)
    except Exception:
        # fallback
        return pd.read_csv(path, engine="python")


def pick_col(df: pd.DataFrame, candidates: Iterable[str], required: bool = True) -> str | None:
    cols = set(df.columns)
    for c in candidates:
        if c in cols:
            return c
    if required:
        raise ValueError(f"Missing required column. Tried: {list(candidates)}. Existing: {list(df.columns)}")
    return None


def to_num(df: pd.DataFrame, cols: Iterable[str]) -> None:
    for c in cols:
        if c in df.columns:
            df[c] = pd.to_numeric(df[c], errors="coerce")


def normalize_mode(x: str) -> str:
    s = str(x).strip().upper()
    if s in ("0", "OFF", "FALSE", "NO"):
        return "OFF"
    if s in ("1", "ON", "TRUE", "YES"):
        return "ON"
    return s


def mean_std(x: pd.Series) -> tuple[float, float, int]:
    v = pd.to_numeric(x, errors="coerce").dropna().astype(float)
    n = int(len(v))
    if n == 0:
        return np.nan, np.nan, 0
    if n == 1:
        return float(v.iloc[0]), 0.0, 1
    return float(v.mean()), float(v.std(ddof=1)), n


def jain_index(vals: np.ndarray) -> float:
    v = np.asarray(vals, dtype=float)
    v = v[np.isfinite(v)]
    if v.size == 0:
        return np.nan
    s = float(v.sum())
    ss = float((v * v).sum())
    if ss <= 0:
        return np.nan
    return (s * s) / (v.size * ss)


def maybe_bps_to_mbps(series: pd.Series, name_hint: str = "") -> pd.Series:
    """
    Convert to Mbps if likely in bps.
    Heuristics:
      - if column name contains 'bps' or endswith '_bps' => divide by 1e6
      - else if median is huge (>1e4) we assume bps (keeps compatibility with older outputs)
    """
    v = pd.to_numeric(series, errors="coerce")
    nh = name_hint.lower()
    if "bps" in nh or nh.endswith("_bps"):
        return v / 1e6

    arr = v.to_numpy(dtype=float, copy=False)
    med = np.nanmedian(arr) if np.isfinite(arr).any() else np.nan
    if np.isfinite(med) and med > 1e4:
        return v / 1e6
    return v


# ---------------- Plot saving (safe PNG) ----------------
def save_fig_safe(fig: go.Figure, out_dir: Path, stem: str, strict_png: bool) -> None:
    html = out_dir / f"{stem}.html"
    png = out_dir / f"{stem}.png"

    fig.write_html(str(html), include_plotlyjs="cdn")

    # PNG via kaleido
    try:
        fig.write_image(str(png), scale=2)  # kaleido
    except Exception as e:
        if strict_png:
            raise RuntimeError(f"Failed to write PNG for {stem}: {e}") from e
        # else: keep html only


# ---------------- Plot builders ----------------
def grouped_bar_off_on(
    x_labels: list[str],
    off_mean: list[float],
    on_mean: list[float],
    off_std: list[float] | None,
    on_std: list[float] | None,
    title: str,
    y_title: str,
) -> go.Figure:
    fig = go.Figure()

    fig.add_trace(go.Bar(
        x=x_labels,
        y=off_mean,
        name="OFF",
        error_y=dict(type="data", array=(off_std or [0.0] * len(off_mean))),
        hovertemplate="%{x}<br>OFF=%{y:.3f}<extra></extra>",
    ))

    fig.add_trace(go.Bar(
        x=x_labels,
        y=on_mean,
        name="ON",
        error_y=dict(type="data", array=(on_std or [0.0] * len(on_mean))),
        hovertemplate="%{x}<br>ON=%{y:.3f}<extra></extra>",
    ))

    fig.update_layout(
        title=title,
        xaxis_title="Class",
        yaxis_title=y_title,
        barmode="group",
        template="plotly_white",
    )
    return fig


def line_vs_rate(
    agg: pd.DataFrame,
    x_col: str,
    y_col: str,
    y_err: str,
    title: str,
    y_title: str,
) -> go.Figure:
    fig = go.Figure()
    for mode, g in agg.groupby("mode"):
        g = g.sort_values(x_col)
        fig.add_trace(go.Scatter(
            x=g[x_col],
            y=g[y_col],
            mode="lines+markers",
            name=str(mode),
            error_y=dict(type="data", array=g[y_err].fillna(0.0)),
            hovertemplate="rate=%{x}<br>value=%{y:.3f}<extra></extra>",
        ))
    fig.update_layout(
        title=title,
        xaxis_title="BE rate (Mbps)",
        yaxis_title=y_title,
        template="plotly_white",
        hovermode="x unified",
        legend_title="Mode",
    )
    return fig


# ---------------- Main ----------------
def main() -> int:
    ap = argparse.ArgumentParser(description="Plot Project 8 (QoS/WMM) results.")
    ap.add_argument("--results", type=str, default=None, help="Path to results/p8 (default: <repo>/results/p8)")
    ap.add_argument("--strict-png", action="store_true", help="Fail if PNG export fails (requires kaleido).")
    args = ap.parse_args()

    res_dir = resolve_results_dir(args.results, "p8")
    raw_dir = res_dir / "raw"
    plot_dir = res_dir / "plots"
    ensure_dir(plot_dir)

    csv_path = raw_dir / "p8_summary.csv"
    if not csv_path.exists():
        alt = raw_dir / "p8_summary_all.csv"
        if alt.exists():
            csv_path = alt
        else:
            print(f"[ERROR] Missing: {raw_dir/'p8_summary.csv'} (or p8_summary_all.csv)", file=sys.stderr)
            print("Run: ./scripts/run_p8.sh", file=sys.stderr)
            return 2

    df = read_csv_robust(csv_path)

    # --- pick/normalize columns robustly ---
    mode_col = pick_col(df, ["mode", "qosMode", "wmm", "WMM", "qos"])
    be_rate_col = pick_col(df, ["beRateMbps", "beRate", "be_rate_mbps", "be_rate"], required=False)

    goodput_be_col = pick_col(df, ["goodputBE", "goodputBE_bps", "goodputBEbps", "goodputBE_Mbps", "goodputBe"])
    goodput_vo_col = pick_col(df, ["goodputVO", "goodputVO_bps", "goodputVObps", "goodputVO_Mbps", "goodputVo"])
    goodput_vi_col = pick_col(df, ["goodputVI", "goodputVI_bps", "goodputVIbps", "goodputVI_Mbps", "goodputVi"])

    delay_vo_col = pick_col(df, ["delayVO_ms", "delayVOms", "delayVO", "voDelay_ms", "voDelayMs"])
    jitter_vo_col = pick_col(df, ["jitterVO_ms", "jitterVOms", "jitterVO", "voJitter_ms", "voJitterMs"])
    loss_vo_col = pick_col(df, ["lossVO", "lossVO_ratio", "lossVO_pct", "voLoss", "voLossRatio"], required=False)

    delay_vi_col = pick_col(df, ["delayVI_ms", "delayVIms", "delayVI", "viDelay_ms", "viDelayMs"])
    jitter_vi_col = pick_col(df, ["jitterVI_ms", "jitterVIms", "jitterVI", "viJitter_ms", "viJitterMs"])
    loss_vi_col = pick_col(df, ["lossVI", "lossVI_ratio", "lossVI_pct", "viLoss", "viLossRatio"], required=False)

    # numeric conversions
    df[mode_col] = df[mode_col].apply(normalize_mode)
    to_num(df, [be_rate_col] if be_rate_col else [])
    to_num(df, [goodput_be_col, goodput_vo_col, goodput_vi_col,
                delay_vo_col, jitter_vo_col, delay_vi_col, jitter_vi_col])
    if loss_vo_col:
        to_num(df, [loss_vo_col])
    if loss_vi_col:
        to_num(df, [loss_vi_col])

    # derived Mbps goodputs (robust to bps)
    df["goodputBE_Mbps"] = maybe_bps_to_mbps(df[goodput_be_col], goodput_be_col)
    df["goodputVO_Mbps"] = maybe_bps_to_mbps(df[goodput_vo_col], goodput_vo_col)
    df["goodputVI_Mbps"] = maybe_bps_to_mbps(df[goodput_vi_col], goodput_vi_col)

    # Ensure both OFF/ON appear (still plot if one missing)
    modes_present = sorted(set(df[mode_col].dropna().unique().tolist()))
    if not modes_present:
        print("[ERROR] No valid mode values found.", file=sys.stderr)
        return 3

    def mode_stats(series: pd.Series) -> dict[str, tuple[float, float]]:
        out = {}
        for m, g in df.groupby(mode_col):
            mu, sd, _ = mean_std(g[series.name])
            out[str(m)] = (mu, sd)
        return out

    # ---------------- Mandatory: Delay (VO/VI) OFF vs ON ----------------
    rows = []
    for m, g in df.groupby(mode_col):
        m_vo, s_vo, _ = mean_std(g[delay_vo_col])
        m_vi, s_vi, _ = mean_std(g[delay_vi_col])
        rows.append({"mode": str(m), "VO_mean": m_vo, "VO_std": s_vo, "VI_mean": m_vi, "VI_std": s_vi})
    a = pd.DataFrame(rows).set_index("mode")

    def get_or_nan(ix: str, col: str) -> float:
        return float(a.loc[ix, col]) if ix in a.index else float("nan")

    fig = grouped_bar_off_on(
        x_labels=["VO", "VI"],
        off_mean=[get_or_nan("OFF", "VO_mean"), get_or_nan("OFF", "VI_mean")],
        on_mean=[get_or_nan("ON", "VO_mean"), get_or_nan("ON", "VI_mean")],
        off_std=[get_or_nan("OFF", "VO_std"), get_or_nan("OFF", "VI_std")] if "OFF" in a.index else None,
        on_std=[get_or_nan("ON", "VO_std"), get_or_nan("ON", "VI_std")] if "ON" in a.index else None,
        title="P8 - Delay (VO/VI): QoS OFF vs ON",
        y_title="Mean delay (ms)",
    )
    save_fig_safe(fig, plot_dir, "mandatory_delay_vo_vi_off_on", strict_png=args.strict_png)

    # ---------------- Mandatory: Jitter (VO/VI) OFF vs ON ----------------
    rows = []
    for m, g in df.groupby(mode_col):
        m_vo, s_vo, _ = mean_std(g[jitter_vo_col])
        m_vi, s_vi, _ = mean_std(g[jitter_vi_col])
        rows.append({"mode": str(m), "VO_mean": m_vo, "VO_std": s_vo, "VI_mean": m_vi, "VI_std": s_vi})
    a = pd.DataFrame(rows).set_index("mode")

    fig = grouped_bar_off_on(
        x_labels=["VO", "VI"],
        off_mean=[get_or_nan("OFF", "VO_mean"), get_or_nan("OFF", "VI_mean")],
        on_mean=[get_or_nan("ON", "VO_mean"), get_or_nan("ON", "VI_mean")],
        off_std=[get_or_nan("OFF", "VO_std"), get_or_nan("OFF", "VI_std")] if "OFF" in a.index else None,
        on_std=[get_or_nan("ON", "VO_std"), get_or_nan("ON", "VI_std")] if "ON" in a.index else None,
        title="P8 - Jitter (VO/VI): QoS OFF vs ON",
        y_title="Mean jitter (ms)",
    )
    save_fig_safe(fig, plot_dir, "mandatory_jitter_vo_vi_off_on", strict_png=args.strict_png)

    # ---------------- Mandatory: BE goodput OFF vs ON ----------------
    rows = []
    for m, g in df.groupby(mode_col):
        mu, sd, _ = mean_std(g["goodputBE_Mbps"])
        rows.append({"mode": str(m), "mean": mu, "std": sd})
    a = pd.DataFrame(rows).set_index("mode")

    fig = go.Figure()
    fig.add_trace(go.Bar(
        x=["BE"], y=[get_or_nan("OFF", "mean")],
        name="OFF",
        error_y=dict(type="data", array=[get_or_nan("OFF", "std")] if "OFF" in a.index else [0.0]),
        hovertemplate="BE<br>OFF=%{y:.3f} Mbps<extra></extra>",
    ))
    fig.add_trace(go.Bar(
        x=["BE"], y=[get_or_nan("ON", "mean")],
        name="ON",
        error_y=dict(type="data", array=[get_or_nan("ON", "std")] if "ON" in a.index else [0.0]),
        hovertemplate="BE<br>ON=%{y:.3f} Mbps<extra></extra>",
    ))
    fig.update_layout(
        title="P8 - Best Effort goodput: QoS OFF vs ON",
        xaxis_title="Traffic class",
        yaxis_title="Goodput (Mbps)",
        barmode="group",
        template="plotly_white",
    )
    save_fig_safe(fig, plot_dir, "mandatory_goodput_be_off_on", strict_png=args.strict_png)

    # ---------------- Recommended: Loss (VO/VI) OFF vs ON ----------------
    if loss_vo_col and loss_vi_col:
        rows = []
        for m, g in df.groupby(mode_col):
            m_vo, s_vo, _ = mean_std(g[loss_vo_col])
            m_vi, s_vi, _ = mean_std(g[loss_vi_col])
            rows.append({"mode": str(m), "VO_mean": m_vo, "VO_std": s_vo, "VI_mean": m_vi, "VI_std": s_vi})
        a = pd.DataFrame(rows).set_index("mode")

        fig = grouped_bar_off_on(
            x_labels=["VO", "VI"],
            off_mean=[get_or_nan("OFF", "VO_mean"), get_or_nan("OFF", "VI_mean")],
            on_mean=[get_or_nan("ON", "VO_mean"), get_or_nan("ON", "VI_mean")],
            off_std=[get_or_nan("OFF", "VO_std"), get_or_nan("OFF", "VI_std")] if "OFF" in a.index else None,
            on_std=[get_or_nan("ON", "VO_std"), get_or_nan("ON", "VI_std")] if "ON" in a.index else None,
            title="P8 (Recommended) - Loss (VO/VI): QoS OFF vs ON",
            y_title="Loss (as exported)",
        )
        save_fig_safe(fig, plot_dir, "rec_loss_vo_vi_off_on", strict_png=args.strict_png)

    # ---------------- Recommended: Goodput per class (BE/VO/VI) OFF vs ON ----------------
    rows = []
    for m, g in df.groupby(mode_col):
        mbe, sbe, _ = mean_std(g["goodputBE_Mbps"])
        mvo, svo, _ = mean_std(g["goodputVO_Mbps"])
        mvi, svi, _ = mean_std(g["goodputVI_Mbps"])
        rows.append({"mode": str(m),
                     "BE_mean": mbe, "BE_std": sbe,
                     "VO_mean": mvo, "VO_std": svo,
                     "VI_mean": mvi, "VI_std": svi})
    a = pd.DataFrame(rows).set_index("mode")

    fig = grouped_bar_off_on(
        x_labels=["BE", "VO", "VI"],
        off_mean=[get_or_nan("OFF", "BE_mean"), get_or_nan("OFF", "VO_mean"), get_or_nan("OFF", "VI_mean")],
        on_mean=[get_or_nan("ON", "BE_mean"), get_or_nan("ON", "VO_mean"), get_or_nan("ON", "VI_mean")],
        off_std=[get_or_nan("OFF", "BE_std"), get_or_nan("OFF", "VO_std"), get_or_nan("OFF", "VI_std")] if "OFF" in a.index else None,
        on_std=[get_or_nan("ON", "BE_std"), get_or_nan("ON", "VO_std"), get_or_nan("ON", "VI_std")] if "ON" in a.index else None,
        title="P8 (Recommended) - Goodput per class: QoS OFF vs ON",
        y_title="Goodput (Mbps)",
    )
    save_fig_safe(fig, plot_dir, "rec_goodput_all_classes_off_on", strict_png=args.strict_png)

    # ---------------- Recommended: Jain fairness across classes ----------------
    df["jain_classes"] = df.apply(
        lambda r: jain_index([r["goodputBE_Mbps"], r["goodputVO_Mbps"], r["goodputVI_Mbps"]]),
        axis=1
    )
    rows = []
    for m, g in df.groupby(mode_col):
        mu, sd, _ = mean_std(g["jain_classes"])
        rows.append({"mode": str(m), "mean": mu, "std": sd})
    a = pd.DataFrame(rows).set_index("mode")

    fig = go.Figure()
    for m in ["OFF", "ON"]:
        if m in a.index:
            fig.add_trace(go.Bar(
                x=[m],
                y=[a.loc[m, "mean"]],
                error_y=dict(type="data", array=[a.loc[m, "std"]]),
                name=m,
                hovertemplate="mode=%{x}<br>Jain=%{y:.4f}<extra></extra>",
            ))
    fig.update_layout(
        title="P8 (Recommended) - Jain fairness across classes (BE/VO/VI)",
        xaxis_title="QoS mode",
        yaxis_title="Jain index (0..1)",
        template="plotly_white",
        yaxis=dict(range=[0, 1.05]),
    )
    save_fig_safe(fig, plot_dir, "rec_jain_fairness_across_classes", strict_png=args.strict_png)

    # ---------------- Recommended: Trends vs BE rate (if beRate exists) ----------------
    if be_rate_col:
        tmp = df.dropna(subset=[be_rate_col, mode_col]).copy()
        tmp["beRateMbps"] = pd.to_numeric(tmp[be_rate_col], errors="coerce")
        tmp = tmp.dropna(subset=["beRateMbps"])

        def agg_vs_rate(series: str) -> pd.DataFrame:
            rows2 = []
            for (mode, rate), g in tmp.groupby([mode_col, "beRateMbps"]):
                mu, sd, _ = mean_std(g[series])
                rows2.append({"mode": str(mode), "beRateMbps": float(rate), "mean": mu, "std": sd})
            return pd.DataFrame(rows2)

        # VO delay vs rate
        agg = agg_vs_rate(delay_vo_col)
        if not agg.empty:
            fig = line_vs_rate(
                agg=agg,
                x_col="beRateMbps",
                y_col="mean",
                y_err="std",
                title="P8 (Recommended) - VO delay vs BE rate (OFF vs ON)",
                y_title="VO mean delay (ms)",
            )
            save_fig_safe(fig, plot_dir, "rec_vo_delay_vs_be_rate", strict_png=args.strict_png)

        # VI delay vs rate
        agg = agg_vs_rate(delay_vi_col)
        if not agg.empty:
            fig = line_vs_rate(
                agg=agg,
                x_col="beRateMbps",
                y_col="mean",
                y_err="std",
                title="P8 (Recommended) - VI delay vs BE rate (OFF vs ON)",
                y_title="VI mean delay (ms)",
            )
            save_fig_safe(fig, plot_dir, "rec_vi_delay_vs_be_rate", strict_png=args.strict_png)

        # BE goodput vs rate
        agg = agg_vs_rate("goodputBE_Mbps")
        if not agg.empty:
            fig = line_vs_rate(
                agg=agg,
                x_col="beRateMbps",
                y_col="mean",
                y_err="std",
                title="P8 (Recommended) - BE goodput vs BE rate (OFF vs ON)",
                y_title="BE goodput (Mbps)",
            )
            save_fig_safe(fig, plot_dir, "rec_be_goodput_vs_be_rate", strict_png=args.strict_png)

    print("[OK] P8 plots saved to:", plot_dir)
    print("Mandatory:")
    print(" - mandatory_delay_vo_vi_off_on.(html/png)")
    print(" - mandatory_jitter_vo_vi_off_on.(html/png)")
    print(" - mandatory_goodput_be_off_on.(html/png)")
    print("Recommended:")
    print(" - rec_loss_vo_vi_off_on.(html/png) [if loss columns exist]")
    print(" - rec_goodput_all_classes_off_on.(html/png)")
    print(" - rec_jain_fairness_across_classes.(html/png)")
    if be_rate_col:
        print(" - rec_vo_delay_vs_be_rate.(html/png)")
        print(" - rec_vi_delay_vs_be_rate.(html/png)")
        print(" - rec_be_goodput_vs_be_rate.(html/png)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
