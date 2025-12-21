#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import os
import sys
import glob
import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns

# -----------------------------------------------------------------------------
# 1. Configuration and Paths
# -----------------------------------------------------------------------------
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.abspath(os.path.join(SCRIPT_DIR, ".."))
RESULTS_DIR = os.path.join(REPO_ROOT, "results", "p3")
PLOTS_DIR = os.path.join(RESULTS_DIR, "plots")

# Set general plot style
sns.set_style("whitegrid")
sns.set_context("paper", font_scale=1.4)

def plot_sweep_graphs(df):
    """
    Plots the main project graphs (Goodput and RTT vs Distance).
    """
    print(">>> Plotting main sweep graphs...")
    
    # Sort by distance
    df.sort_values(by='distance_m', inplace=True)
    distances = sorted(df['distance_m'].unique())

    # --- 1. Goodput Plot ---
    plt.figure(figsize=(10, 6))
    sns.lineplot(
        data=df,
        x='distance_m',
        y='goodput_Mbps',
        hue='transport',
        style='transport',
        markers=True,
        dashes=False,
        errorbar='sd', # Show shadow for standard deviation
        linewidth=2.5,
        markersize=8
    )
    plt.title("Goodput vs Distance (With Shadowing & Fading)", fontweight='bold')
    plt.xlabel("Distance (m)")
    plt.ylabel("Goodput (Mbps)")
    plt.legend(title="Transport")
    plt.xticks(distances)
    plt.tight_layout()
    plt.savefig(os.path.join(PLOTS_DIR, "goodput_vs_distance.png"), dpi=300)
    print(" -> Saved: goodput_vs_distance.png")
    plt.close()

    # --- 2. RTT Plot ---
    plt.figure(figsize=(10, 6))
    sns.lineplot(
        data=df,
        x='distance_m',
        y='rtt_mean_ms',
        hue='transport',
        style='transport',
        markers=True,
        dashes=False,
        errorbar='sd',
        linewidth=2.5,
        markersize=8,
        palette="deep"
    )
    plt.title("RTT vs Distance (Mean Delay)", fontweight='bold')
    plt.xlabel("Distance (m)")
    plt.ylabel("RTT (ms)")
    plt.legend(title="Transport")
    plt.xticks(distances)
    plt.tight_layout()
    plt.savefig(os.path.join(PLOTS_DIR, "rtt_vs_distance.png"), dpi=300)
    print(" -> Saved: rtt_vs_distance.png")
    plt.close()


def plot_timeseries_for_distance(distance_tag, title_suffix):
    """
    Plots the time series (throughput vs time) for a specific distance.
    """
    # Search for ts_dXXm_*.csv files in all subdirectories
    pattern = os.path.join(RESULTS_DIR, "**", f"ts_{distance_tag}_*.csv")
    files = glob.glob(pattern, recursive=True)
    
    # Filter for UDP files (since stability analysis is more interesting for UDP)
    udp_files = [f for f in files if "udp" in f.lower()]
    
    if not udp_files:
        # If no UDP found, accept any file
        target_files = files
    else:
        target_files = udp_files

    if not target_files:
        print(f"[Warning] No time series file found for distance {distance_tag}.")
        return

    # Select the first file (Run 1) to keep the plot clean
    f = target_files[0]
    
    try:
        df = pd.read_csv(f)
        df.columns = df.columns.str.strip()
        
        plt.figure(figsize=(10, 4))
        sns.lineplot(data=df, x='time_s', y='throughput_Mbps', linewidth=2)
        
        plt.title(f"Throughput Stability: {title_suffix} ({distance_tag})", fontweight='bold')
        plt.xlabel("Time (s)")
        plt.ylabel("Throughput (Mbps)")
        plt.ylim(bottom=0) # Start Y axis from zero
        
        filename = f"timeseries_{distance_tag}.png"
        plt.tight_layout()
        plt.savefig(os.path.join(PLOTS_DIR, filename), dpi=300)
        print(f" -> Saved time series: {filename}")
        plt.close()
        
    except Exception as e:
        print(f"[Error] plotting {f}: {e}")


def main():
    print("--- Starting Comprehensive Plotting Script ---")
    os.makedirs(PLOTS_DIR, exist_ok=True)

    # ---------------------------------------------------------
    # Part 1: Main Sweep Graphs (from p3_sweep.csv)
    # ---------------------------------------------------------
    search_pattern = os.path.join(RESULTS_DIR, "**", "p3_sweep.csv")
    csv_files = glob.glob(search_pattern, recursive=True)

    if csv_files:
        print(f"Found {len(csv_files)} p3_sweep.csv files. Aggregating...")
        df_list = []
        for f in csv_files:
            try:
                temp = pd.read_csv(f)
                temp.columns = temp.columns.str.strip()
                df_list.append(temp)
            except:
                pass
        
        if df_list:
            full_df = pd.concat(df_list, ignore_index=True)
            # Standardize transport names
            if 'transport' in full_df.columns:
                full_df['transport'] = full_df['transport'].str.upper()
            
            # Plot main graphs
            plot_sweep_graphs(full_df)
        else:
            print("[Error] CSV files are empty or corrupted.")
    else:
        print("[Warning] No p3_sweep.csv files found. Did you run the simulation?")

    # ---------------------------------------------------------
    # Part 2: Analytical Time Series Graphs
    # ---------------------------------------------------------
    print(">>> Plotting time stability graphs...")
    
    # Plot for close range (Stable)
    plot_timeseries_for_distance("d1m", "Close Range / Stable")
    
    # Plot for far range (Unstable - Cell Edge)
    plot_timeseries_for_distance("d30m", "Far Range / Edge")

    print("--- Done. All plots are available in results/p3/plots/ ---")

if __name__ == "__main__":
    main()