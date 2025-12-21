#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import os
import sys
import re
import numpy as np
import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.abspath(os.path.join(SCRIPT_DIR, ".."))
RESULTS_DIR = os.path.join(REPO_ROOT, "results", "p4")
CSV_FILE = os.path.join(RESULTS_DIR, "p4_matrix_global.csv")
PLOTS_DIR = os.path.join(RESULTS_DIR, "plots")

sns.set_style("whitegrid")
sns.set_context("paper", font_scale=1.4)
plt.rcParams.update({"figure.autolayout": True})


def parse_mbps(rate_str: str) -> float:
    """Parses strings like '50Mbps' into float Mbps."""
    if pd.isna(rate_str):
        return np.nan
    s = str(rate_str).strip()
    m = re.match(r"^\s*([0-9]*\.?[0-9]+)\s*Mbps\s*$", s, re.IGNORECASE)
    if m:
        return float(m.group(1))
    return np.nan


def main():
    print("--- Starting Scientific Plotting ---")
    os.makedirs(PLOTS_DIR, exist_ok=True)

    if not os.path.exists(CSV_FILE):
        print(f"[Error] Global CSV not found: {CSV_FILE}")
        print("Did you run 'run_p4.sh' successfully to aggregate data?")
        sys.exit(1)

    df = pd.read_csv(CSV_FILE)
    df.columns = df.columns.str.strip()

    if df.empty:
        print("[Error] CSV is empty.")
        sys.exit(1)

    # Basic required columns check (helpful error messages)
    required = ["distance", "channelWidth", "txPowerDbm", "mcs", "rxBytes", "rateMode", "udpRate"]
    missing = [c for c in required if c not in df.columns]
    if missing:
        raise KeyError(f"Missing required columns in CSV: {missing}\nAvailable columns: {list(df.columns)}")

    # Ensure numeric types (only if present)
    for col in ["distance", "channelWidth", "txPowerDbm", "mcs", "rxBytes"]:
        df[col] = pd.to_numeric(df[col], errors="coerce")

    # RTT column compatibility
    if "rtt_mean_ms" in df.columns:
        df["rtt_mean_ms"] = pd.to_numeric(df["rtt_mean_ms"], errors="coerce")
    elif "rttMeanMs" in df.columns:
        df["rtt_mean_ms"] = pd.to_numeric(df["rttMeanMs"], errors="coerce")
    else:
        df["rtt_mean_ms"] = np.nan

    # Drop failed runs (no received bytes)
    df = df[df["rxBytes"].fillna(0) > 0].copy()

    # -------------------- Goodput column compatibility --------------------
    # Supports any of: goodput_Mbps, goodputMbps, goodputbps, goodput_mbps
    if "goodput_mbps" in df.columns:
        df["goodput_mbps"] = pd.to_numeric(df["goodput_mbps"], errors="coerce")
    elif "goodput_Mbps" in df.columns:
        df["goodput_mbps"] = pd.to_numeric(df["goodput_Mbps"], errors="coerce")
    elif "goodputMbps" in df.columns:
        df["goodput_mbps"] = pd.to_numeric(df["goodputMbps"], errors="coerce")
    elif "goodputbps" in df.columns:
        df["goodput_mbps"] = pd.to_numeric(df["goodputbps"], errors="coerce") / 1e6
    else:
        raise KeyError(f"No goodput column found. Columns are: {list(df.columns)}")

    # Safety net: if someone stored bps but labeled as Mbps, convert values that are clearly too big
    # (in your scenarios Mbps won't be > 1000)
    mask = df["goodput_mbps"] > 1000
    df.loc[mask, "goodput_mbps"] = df.loc[mask, "goodput_mbps"] / 1e6

    # Clean RTT invalid values
    df.loc[df["rtt_mean_ms"] < 0, "rtt_mean_ms"] = np.nan

    # Labels
    def make_label(row):
        if str(row["rateMode"]).strip().lower() == "adaptive":
            return "Adaptive (Minstrel)"
        try:
            return f"Fixed MCS {int(row['mcs'])}"
        except Exception:
            return "Fixed MCS"

    df["Label"] = df.apply(make_label, axis=1)

    # Offered load for y-limits
    df["udp_mbps"] = df["udpRate"].apply(parse_mbps)
    offered = float(df["udp_mbps"].dropna().iloc[0]) if df["udp_mbps"].notna().any() else None

    # -------------------------------------------------------------------------
    # Plot 1: Bandwidth Scaling
    # -------------------------------------------------------------------------
    print("Generating Plot 1: Bandwidth Scaling...")
    subset_fig1 = df[df["txPowerDbm"] == 20]

    if not subset_fig1.empty:
        g = sns.catplot(
            data=subset_fig1,
            x="channelWidth",
            y="goodput_mbps",
            hue="Label",
            col="distance",
            kind="bar",
            height=5,
            aspect=1.2,
            palette="viridis",
            edgecolor="black",
            errorbar=None,
        )
        g.fig.subplots_adjust(top=0.85)
        g.fig.suptitle("Impact of Channel Width on Capacity (TxPower=20dBm)", fontweight="bold")
        g.set_axis_labels("Channel Width (MHz)", "Goodput (Mbps)")
        g.set_titles("Distance: {col_name} m")

        out_f1 = os.path.join(PLOTS_DIR, "fig1_bandwidth_scaling.png")
        plt.savefig(out_f1, dpi=300)
        print(f" -> Saved: {out_f1}")
        plt.close()

    # -------------------------------------------------------------------------
    # Plot 2: Power Sensitivity at Edge
    # -------------------------------------------------------------------------
    print("Generating Plot 2: Power Sensitivity at Cell Edge...")
    subset_fig2 = df[(df["channelWidth"] == 80) & (df["distance"] == 20)]

    if not subset_fig2.empty:
        plt.figure(figsize=(9, 6))
        sns.barplot(
            data=subset_fig2,
            x="txPowerDbm",
            y="goodput_mbps",
            hue="Label",
            palette="magma",
            edgecolor="black",
        )
        plt.title("Power Sensitivity at Cell Edge (Dist=20m, BW=80MHz)", fontweight="bold")
        plt.xlabel("Transmission Power (dBm)")
        plt.ylabel("Goodput (Mbps)")
        if offered is not None:
            plt.ylim(0, offered * 1.05)
        plt.legend(title="Rate Control Strategy")

        out_f2 = os.path.join(PLOTS_DIR, "fig2_power_sensitivity.png")
        plt.savefig(out_f2, dpi=300)
        print(f" -> Saved: {out_f2}")
        plt.close()

    # -------------------------------------------------------------------------
    # Plot 3: RTT vs Width
    # -------------------------------------------------------------------------
    print("Generating Plot 3: Latency & Jitter Analysis...")
    subset_fig3 = df[(df["distance"] == 20) & (df["rtt_mean_ms"].notna())]

    if not subset_fig3.empty:
        plt.figure(figsize=(10, 6))
        sns.pointplot(
            data=subset_fig3,
            x="channelWidth",
            y="rtt_mean_ms",
            hue="Label",
            markers=["o", "s", "^"],
            linestyles=["-", "--", "-."],
            scale=1.2,
        )
        plt.title("Latency penalty of wider channels at Edge (20m)", fontweight="bold")
        plt.xlabel("Channel Width (MHz)")
        plt.ylabel("Mean RTT (ms)")
        plt.grid(True, linestyle="--", alpha=0.7)
        plt.legend(title="Configuration")

        out_f3 = os.path.join(PLOTS_DIR, "fig3_latency_analysis.png")
        plt.savefig(out_f3, dpi=300)
        print(f" -> Saved: {out_f3}")
        plt.close()

    print(f"--- Done. Scientific plots saved to {PLOTS_DIR} ---")


if __name__ == "__main__":
    main()
