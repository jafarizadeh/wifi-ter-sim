#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import os
import glob
import pandas as pd
import matplotlib.pyplot as plt

BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RAW_DIR = os.path.join(BASE_DIR, "results", "p6", "raw")
PLOT_DIR = os.path.join(BASE_DIR, "results", "p6", "plots")

os.makedirs(PLOT_DIR, exist_ok=True)


def discover_runs():
    """Find run ids from available files."""
    runs = set()
    for pat in [
        os.path.join(RAW_DIR, "roaming_events_run*.txt"),
        os.path.join(RAW_DIR, "rtt_probe_run*.csv"),
        os.path.join(RAW_DIR, "sta_pos_run*.csv"),
        os.path.join(RAW_DIR, "throughput_timeseries_run*.csv"),
    ]:
        for p in glob.glob(pat):
            bn = os.path.basename(p)
            # ...run3...
            num = "".join(ch for ch in bn if ch.isdigit())
            # safer parse:
            import re
            m = re.search(r"run(\d+)", bn)
            if m:
                runs.add(int(m.group(1)))
    return sorted(runs)


def read_roam_time(run):
    path = os.path.join(RAW_DIR, f"roaming_events_run{run}.txt")
    if not os.path.exists(path):
        return None

    df = pd.read_csv(path)
    # accept either 'type' or 'event'
    typ_col = "type" if "type" in df.columns else ("event" if "event" in df.columns else None)
    if typ_col is None or "time_s" not in df.columns:
        return None

    roams = df[df[typ_col].astype(str).str.upper() == "ROAM"]
    if len(roams) == 0:
        return None
    return float(roams.iloc[0]["time_s"])


def plot_bssid(run):
    path = os.path.join(RAW_DIR, f"roaming_events_run{run}.txt")
    if not os.path.exists(path):
        return

    df = pd.read_csv(path)
    if "time_s" not in df.columns or "bssid" not in df.columns:
        return

    typ_col = "type" if "type" in df.columns else ("event" if "event" in df.columns else None)
    if typ_col is not None:
        df = df[df[typ_col].astype(str).str.upper().isin(["INIT", "ROAM"])]

    bssids = sorted(df["bssid"].astype(str).unique())
    bssid_map = {b: i for i, b in enumerate(bssids)}
    df["bssid_i"] = df["bssid"].astype(str).map(bssid_map)

    plt.figure(figsize=(8, 4))
    plt.step(df["time_s"], df["bssid_i"], where="post")

    plt.yticks(list(bssid_map.values()), list(bssid_map.keys()))

    roam_t = read_roam_time(run)
    if roam_t is not None:
        plt.axvline(roam_t, linestyle="--", label=f"roam @ {roam_t:.1f}s")
        plt.legend()

    plt.xlabel("Time (s)")
    plt.ylabel("BSSID")
    plt.title(f"P6 BSSID vs Time (run {run})")
    plt.tight_layout()
    plt.savefig(os.path.join(PLOT_DIR, f"bssid_vs_time_run{run}.png"), dpi=150)
    plt.close()


def plot_rtt(run):
    path = os.path.join(RAW_DIR, f"rtt_probe_run{run}.csv")
    if not os.path.exists(path):
        return

    # rtt_probe ممکن است tab-separated باشد، sep=None با engine=python خودش تشخیص می‌دهد
    df = pd.read_csv(path, sep=None, engine="python")
    # normalize columns
    cols = {c.strip(): c for c in df.columns}
    if "time_s" not in cols:
        # اگر بدون هدر باشد:
        if df.shape[1] >= 3:
            df.columns = ["time_s", "seq", "rtt_ms"] + list(df.columns[3:])
        else:
            return

    # rtt column name
    rtt_col = "rtt_ms" if "rtt_ms" in df.columns else (df.columns[2] if df.shape[1] >= 3 else None)
    if rtt_col is None:
        return

    plt.figure(figsize=(8, 4))
    plt.plot(df["time_s"], df[rtt_col], linewidth=1)

    roam_t = read_roam_time(run)
    if roam_t is not None:
        plt.axvline(roam_t, linestyle="--", label=f"roam @ {roam_t:.1f}s")
        plt.legend()

    plt.xlabel("Time (s)")
    plt.ylabel("RTT (ms)")
    plt.title(f"P6 RTT vs Time (run {run})")
    plt.tight_layout()
    plt.savefig(os.path.join(PLOT_DIR, f"rtt_vs_time_run{run}.png"), dpi=150)
    plt.close()


def _pick_position_columns(df: pd.DataFrame):
    """
    Return (time_col, x_col) by trying common names.
    Falls back to first numeric columns.
    """
    # common names
    time_candidates = ["time_s", "time", "t"]
    x_candidates = ["x_m", "x", "posx", "pos_x", "sta_x", "xpos"]

    time_col = next((c for c in time_candidates if c in df.columns), None)
    x_col = next((c for c in x_candidates if c in df.columns), None)

    # fallback: try numeric columns
    if time_col is None or x_col is None:
        numeric_cols = [c for c in df.columns if pd.api.types.is_numeric_dtype(df[c])]
        if time_col is None and len(numeric_cols) >= 1:
            time_col = numeric_cols[0]
        if x_col is None and len(numeric_cols) >= 2:
            x_col = numeric_cols[1]

    return time_col, x_col


def plot_position(run):
    path = os.path.join(RAW_DIR, f"sta_pos_run{run}.csv")
    if not os.path.exists(path):
        return

    # ممکن است comma/tab باشد
    df = pd.read_csv(path, sep=None, engine="python")

    # اگر بدون هدر باشد و ستون‌ها عددی‌اند
    if not any(isinstance(c, str) and ("time" in c.lower() or "x" in c.lower()) for c in df.columns):
        # احتمالاً pandas ستون‌های 0,1,2... گذاشته، مشکلی نیست (fallback عددی داریم)
        pass

    time_col, x_col = _pick_position_columns(df)
    if time_col is None or x_col is None:
        # آخرین fallback: اگر حداقل 2 ستون دارد
        if df.shape[1] >= 2:
            time_col = df.columns[0]
            x_col = df.columns[1]
        else:
            return

    # ensure numeric
    df = df.copy()
    df[time_col] = pd.to_numeric(df[time_col], errors="coerce")
    df[x_col] = pd.to_numeric(df[x_col], errors="coerce")
    df = df.dropna(subset=[time_col, x_col])

    if df.empty:
        return

    roam_t = read_roam_time(run)

    plt.figure(figsize=(8, 4))
    plt.plot(df[time_col], df[x_col], linewidth=2)

    if roam_t is not None:
        # closest x at roam time
        idx = (df[time_col] - roam_t).abs().idxmin()
        x_roam = float(df.loc[idx, x_col])
        plt.axvline(roam_t, linestyle="--")
        plt.text(roam_t, x_roam, f"x≈{x_roam:.1f}m",
                 rotation=90, va="bottom", ha="right")

    plt.xlabel("Time (s)")
    plt.ylabel("STA x-position (m)")
    plt.title(f"P6 STA Position x(t) (run {run})")
    plt.tight_layout()
    plt.savefig(os.path.join(PLOT_DIR, f"sta_x_vs_time_run{run}.png"), dpi=150)
    plt.close()


def main():
    runs = discover_runs()
    if not runs:
        print("[plot_p6] No run files found in results/p6/raw")
        return

    for run in runs:
        plot_bssid(run)
        plot_rtt(run)
        plot_position(run)

    print(f"[plot_p6] Done. Plots are in: {PLOT_DIR}")


if __name__ == "__main__":
    main()
