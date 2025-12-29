#!/usr/bin/env python3
# -*- coding: utf-8 -*-

from __future__ import annotations

import argparse
from pathlib import Path
import sys
import numpy as np
import pandas as pd
import plotly.graph_objects as go


# ---------------------------- Paths ----------------------------
def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def resolve_results_dir(user_path: str | None, project: str = "p6") -> Path:
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


# ---------------------------- Safe figure save ----------------------------
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


# ---------------------------- CSV helpers ----------------------------
def read_csv_optional(path: Path) -> pd.DataFrame | None:
    if not path.exists() or path.stat().st_size == 0:
        return None
    try:
        return pd.read_csv(path)
    except Exception:
        # Fallback: python engine can be more forgiving
        try:
            return pd.read_csv(path, engine="python")
        except Exception:
            return None


def coerce_numeric(df: pd.DataFrame, cols: list[str]) -> pd.DataFrame:
    for c in cols:
        if c in df.columns:
            df[c] = pd.to_numeric(df[c], errors="coerce")
    return df


def normalize_throughput_df(thr: pd.DataFrame) -> pd.DataFrame:
    """
    Make throughput columns consistent:
    - time: time_s (if time exists)
    - throughput: throughput_bps (if throughputBps exists)
    """
    thr = thr.copy()
    if "time_s" not in thr.columns and "time" in thr.columns:
        thr["time_s"] = thr["time"]
    if "throughput_bps" not in thr.columns:
        for cand in ["throughputBps", "throughput_bps", "throughput"]:
            if cand in thr.columns:
                thr["throughput_bps"] = thr[cand]
                break

    thr = coerce_numeric(thr, ["run", "time_s", "throughput_bps"])
    return thr


def normalize_rtt_df(rtt: pd.DataFrame) -> pd.DataFrame:
    rtt = rtt.copy()
    if "time_s" not in rtt.columns and "time" in rtt.columns:
        rtt["time_s"] = rtt["time"]
    if "rtt_ms" not in rtt.columns:
        for cand in ["rttMs", "rtt_ms", "rtt"]:
            if cand in rtt.columns:
                rtt["rtt_ms"] = rtt[cand]
                break
    rtt = coerce_numeric(rtt, ["run", "time_s", "seq", "rtt_ms"])
    return rtt


def normalize_pos_df(pos: pd.DataFrame) -> pd.DataFrame:
    pos = pos.copy()
    if "time_s" not in pos.columns and "time" in pos.columns:
        pos["time_s"] = pos["time"]
    pos = coerce_numeric(pos, ["run", "time_s", "x", "y", "z"])
    return pos


def normalize_events_df(ev: pd.DataFrame) -> pd.DataFrame:
    ev = ev.copy()
    if "time_s" not in ev.columns and "time" in ev.columns:
        ev["time_s"] = ev["time"]
    ev = coerce_numeric(ev, ["run", "time_s"])
    return ev


# ---------------------------- Domain helpers ----------------------------
def add_vline(fig: go.Figure, x: float | None, label: str, dash: str = "dash") -> None:
    if x is None or not np.isfinite(x):
        return
    fig.add_vline(
        x=float(x),
        line_width=2,
        line_dash=dash,
        annotation_text=label,
        annotation_position="top left",
    )


def find_roam_time(run: int, summary: pd.DataFrame | None, events: pd.DataFrame | None) -> float | None:
    """
    Priority:
      1) p6_summary.csv roamTime (per run)
      2) roaming_events_all.csv first "re-assoc"/"reassoc"/"re-association" event time
    """
    if summary is not None and not summary.empty:
        if "run" in summary.columns and "roamTime" in summary.columns:
            s = summary.loc[summary["run"] == run, "roamTime"]
            if len(s) > 0:
                rt = pd.to_numeric(s.iloc[0], errors="coerce")
                if np.isfinite(rt):
                    return float(rt)

    if events is not None and not events.empty:
        if "run" in events.columns and "time_s" in events.columns and "event" in events.columns:
            e = events[events["run"] == run].copy()
            if not e.empty:
                ev = e["event"].astype(str).str.lower()
                cand = e[ev.str.contains("re-assoc") | ev.str.contains("reassoc") | ev.str.contains("re-association")]
                if cand.empty:
                    cand = e[ev.str.contains("assoc")]
                if not cand.empty:
                    t = pd.to_numeric(cand["time_s"], errors="coerce").dropna()
                    if len(t) > 0:
                        return float(t.min())
    return None


def run_speed_map(summary: pd.DataFrame | None) -> dict[int, float]:
    m: dict[int, float] = {}
    if summary is None or summary.empty:
        return m
    if "run" in summary.columns and "staSpeed" in summary.columns:
        tmp = summary.dropna(subset=["run"]).copy()
        tmp = coerce_numeric(tmp, ["run", "staSpeed"])
        for _, row in tmp.iterrows():
            r = row.get("run")
            s = row.get("staSpeed")
            if pd.notna(r) and pd.notna(s):
                m[int(r)] = float(s)
    return m


# ---------------------------- Plots ----------------------------
def plot_throughput_per_run(
    thr: pd.DataFrame,
    summary: pd.DataFrame | None,
    events: pd.DataFrame | None,
    plot_dir: Path,
    strict_png: bool,
) -> list[int]:
    runs = sorted(set(int(r) for r in thr["run"].dropna().unique().tolist())) if "run" in thr.columns else []
    speed_map = run_speed_map(summary)

    for run in runs:
        g = thr[thr["run"] == run].dropna(subset=["time_s", "throughput_bps"]).copy()
        if g.empty:
            continue
        g = g.sort_values("time_s")
        g["throughput_Mbps"] = g["throughput_bps"] / 1e6

        roam_t = find_roam_time(run, summary, events)
        sp = speed_map.get(run, np.nan)

        title = f"P6 - Throughput vs time (run={run}"
        if np.isfinite(sp):
            title += f", staSpeed={sp} m/s"
        title += ")"

        fig = go.Figure()
        fig.add_trace(
            go.Scatter(
                x=g["time_s"],
                y=g["throughput_Mbps"],
                mode="lines",
                name="throughput",
                hovertemplate="t=%{x:.2f}s<br>thr=%{y:.3f} Mbps<extra></extra>",
            )
        )
        add_vline(fig, roam_t, "roamTime", dash="dash")

        # Optional: moveStart marker
        if summary is not None and "moveStart" in summary.columns and "run" in summary.columns:
            ms = summary.loc[summary["run"] == run, "moveStart"]
            if len(ms) > 0:
                mv = pd.to_numeric(ms.iloc[0], errors="coerce")
                if np.isfinite(mv):
                    add_vline(fig, float(mv), "moveStart", dash="dot")

        fig.update_layout(
            title=title,
            xaxis_title="Time (s)",
            yaxis_title="Throughput (Mbps)",
            template="plotly_white",
            hovermode="x unified",
        )
        save_fig_safe(fig, plot_dir, f"p6_throughput_vs_time_run{run}", strict_png)

    return runs


def plot_rtt_per_run(
    rtt: pd.DataFrame,
    runs: list[int],
    summary: pd.DataFrame | None,
    events: pd.DataFrame | None,
    plot_dir: Path,
    strict_png: bool,
) -> None:
    speed_map = run_speed_map(summary)

    for run in runs:
        g = rtt[rtt["run"] == run].dropna(subset=["time_s", "rtt_ms"]).copy()
        if g.empty:
            continue
        g = g.sort_values("time_s")
        roam_t = find_roam_time(run, summary, events)

        sp = speed_map.get(run, np.nan)
        title = f"P6 - RTT vs time (run={run}"
        if np.isfinite(sp):
            title += f", staSpeed={sp} m/s"
        title += ")"

        fig = go.Figure()
        fig.add_trace(
            go.Scatter(
                x=g["time_s"],
                y=g["rtt_ms"],
                mode="lines+markers",
                marker=dict(size=4),
                name="RTT",
                hovertemplate="t=%{x:.2f}s<br>rtt=%{y:.3f} ms<extra></extra>",
            )
        )
        add_vline(fig, roam_t, "roamTime", dash="dash")

        fig.update_layout(
            title=title,
            xaxis_title="Time (s)",
            yaxis_title="RTT (ms)",
            template="plotly_white",
            hovermode="x unified",
        )
        save_fig_safe(fig, plot_dir, f"p6_rtt_vs_time_run{run}", strict_png)

        # Optional: RTT CDF
        x = np.sort(pd.to_numeric(g["rtt_ms"], errors="coerce").dropna().to_numpy())
        if x.size > 5:
            y = np.arange(1, x.size + 1) / x.size
            fig_cdf = go.Figure()
            fig_cdf.add_trace(go.Scatter(x=x, y=y, mode="lines", name="ECDF"))
            fig_cdf.update_layout(
                title=f"P6 - RTT CDF (run={run})",
                xaxis_title="RTT (ms)",
                yaxis_title="ECDF",
                template="plotly_white",
                hovermode="x unified",
            )
            save_fig_safe(fig_cdf, plot_dir, f"p6_rtt_cdf_run{run}", strict_png)


def plot_position_per_run(
    pos: pd.DataFrame,
    runs: list[int],
    summary: pd.DataFrame | None,
    events: pd.DataFrame | None,
    plot_dir: Path,
    strict_png: bool,
) -> None:
    speed_map = run_speed_map(summary)

    for run in runs:
        g = pos[pos["run"] == run].dropna(subset=["time_s", "x"]).copy()
        if g.empty:
            continue
        g = g.sort_values("time_s")
        roam_t = find_roam_time(run, summary, events)

        sp = speed_map.get(run, np.nan)
        title = f"P6 - STA position x(t) (run={run}"
        if np.isfinite(sp):
            title += f", staSpeed={sp} m/s"
        title += ")"

        fig = go.Figure()
        fig.add_trace(
            go.Scatter(
                x=g["time_s"],
                y=g["x"],
                mode="lines",
                name="x(t)",
                hovertemplate="t=%{x:.2f}s<br>x=%{y:.2f} m<extra></extra>",
            )
        )
        add_vline(fig, roam_t, "roamTime", dash="dash")

        # Optional: AP2 at distance D (if apDistance exists)
        if summary is not None and "apDistance" in summary.columns and "run" in summary.columns:
            ad = summary.loc[summary["run"] == run, "apDistance"]
            if len(ad) > 0:
                D = pd.to_numeric(ad.iloc[0], errors="coerce")
                if np.isfinite(D):
                    fig.add_hline(y=0.0, line_dash="dot", annotation_text="AP1 x=0", annotation_position="bottom left")
                    fig.add_hline(y=float(D), line_dash="dot", annotation_text="AP2 x=D", annotation_position="top left")

        fig.update_layout(
            title=title,
            xaxis_title="Time (s)",
            yaxis_title="Position x (m)",
            template="plotly_white",
            hovermode="x unified",
        )
        save_fig_safe(fig, plot_dir, f"p6_sta_pos_x_vs_time_run{run}", strict_png)


def plot_summary_recommended(
    summary: pd.DataFrame,
    plot_dir: Path,
    strict_png: bool,
) -> None:
    # roamTime vs speed
    if "staSpeed" in summary.columns and "roamTime" in summary.columns:
        tmp = summary.dropna(subset=["staSpeed", "roamTime"]).copy()
        tmp = coerce_numeric(tmp, ["staSpeed", "roamTime"])
        if not tmp.empty and tmp["roamTime"].notna().any():
            fig = go.Figure()
            fig.add_trace(
                go.Scatter(
                    x=tmp["staSpeed"],
                    y=tmp["roamTime"],
                    mode="markers+lines",
                    name="roamTime",
                    hovertemplate="speed=%{x} m/s<br>roamTime=%{y:.2f} s<extra></extra>",
                )
            )
            fig.update_layout(
                title="P6 - Roam time vs STA speed",
                xaxis_title="STA speed (m/s)",
                yaxis_title="Roam time (s)",
                template="plotly_white",
                hovermode="x unified",
            )
            save_fig_safe(fig, plot_dir, "p6_roamTime_vs_speed", strict_png)

    # goodput vs speed (support goodputbps or goodput_bps)
    gb_col = None
    for cand in ["goodputbps", "goodput_bps", "goodputBps"]:
        if cand in summary.columns:
            gb_col = cand
            break
    if gb_col and "staSpeed" in summary.columns:
        tmp = summary.dropna(subset=["staSpeed", gb_col]).copy()
        tmp = coerce_numeric(tmp, ["staSpeed", gb_col])
        if not tmp.empty and tmp[gb_col].notna().any():
            tmp["goodput_Mbps"] = tmp[gb_col] / 1e6
            fig = go.Figure()
            fig.add_trace(
                go.Scatter(
                    x=tmp["staSpeed"],
                    y=tmp["goodput_Mbps"],
                    mode="markers+lines",
                    name="goodput",
                    hovertemplate="speed=%{x} m/s<br>goodput=%{y:.3f} Mbps<extra></extra>",
                )
            )
            fig.update_layout(
                title="P6 - Overall goodput vs STA speed",
                xaxis_title="STA speed (m/s)",
                yaxis_title="Goodput (Mbps)",
                template="plotly_white",
                hovermode="x unified",
            )
            save_fig_safe(fig, plot_dir, "p6_goodput_vs_speed", strict_png)


# ---------------------------- Main ----------------------------
def main() -> int:
    ap = argparse.ArgumentParser(description="Plot Project 6 (roaming multi-AP) results.")
    ap.add_argument("--results", type=str, default=None, help="Path to results/p6 (default: <repo>/results/p6)")
    ap.add_argument("--strict-png", action="store_true", help="Fail if PNG export fails (requires kaleido).")
    args = ap.parse_args()

    res_dir = resolve_results_dir(args.results, "p6")
    raw_dir = res_dir / "raw"
    plot_dir = res_dir / "plots"
    ensure_dir(plot_dir)

    summary_path = raw_dir / "p6_summary.csv"
    thr_path = raw_dir / "throughput_timeseries_all.csv"

    if not summary_path.exists() or not thr_path.exists():
        print("[P6] ERROR: Missing required raw files:", file=sys.stderr)
        print(" -", summary_path, file=sys.stderr)
        print(" -", thr_path, file=sys.stderr)
        print("Run: ./scripts/run_p6.sh", file=sys.stderr)
        return 2

    summary = pd.read_csv(summary_path)
    thr = pd.read_csv(thr_path)

    # Optional files
    rtt_path = raw_dir / "rtt_probe_all.csv"
    pos_path = raw_dir / "sta_pos_all.csv"
    ev_path = raw_dir / "roaming_events_all.csv"

    rtt = read_csv_optional(rtt_path)
    pos = read_csv_optional(pos_path)
    ev = read_csv_optional(ev_path)

    # Normalize / coerce
    summary = coerce_numeric(summary, ["run", "staSpeed", "moveStart", "apDistance", "goodputbps", "rxBytes", "roamTime"])
    thr = normalize_throughput_df(thr)
    if rtt is not None:
        rtt = normalize_rtt_df(rtt)
    if pos is not None:
        pos = normalize_pos_df(pos)
    if ev is not None:
        ev = normalize_events_df(ev)

    # Plot 1: throughput per run (mandatory)
    runs = plot_throughput_per_run(thr, summary, ev, plot_dir, args.strict_png)

    # Plot 2: RTT per run (mandatory if exists)
    if rtt is not None and not rtt.empty:
        plot_rtt_per_run(rtt, runs, summary, ev, plot_dir, args.strict_png)

    # Plot 3: position x(t) (recommended if exists)
    if pos is not None and not pos.empty:
        plot_position_per_run(pos, runs, summary, ev, plot_dir, args.strict_png)

    # Extra summary plots (recommended)
    if summary is not None and not summary.empty:
        plot_summary_recommended(summary, plot_dir, args.strict_png)

    print("[OK] P6 plots saved to:", plot_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
