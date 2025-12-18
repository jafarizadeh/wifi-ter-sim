#!/usr/bin/env python3
import argparse
import csv
import os
from pathlib import Path
from typing import List, Tuple

import numpy as np
import matplotlib.pyplot as plt


# Lecture CSV simple (sans pandas) pour rester portable
def read_throughput_csv(path: Path) -> Tuple[np.ndarray, np.ndarray]:
    # Format attendu: time_s,throughput_bps
    times: List[float] = []
    thr_bps: List[float] = []
    with path.open("r", newline="") as f:
        r = csv.DictReader(f)
        for row in r:
            times.append(float(row["time_s"]))
            thr_bps.append(float(row["throughput_bps"]))
    return np.array(times), np.array(thr_bps)


def read_rtt_csv(path: Path) -> Tuple[np.ndarray, np.ndarray, np.ndarray]:
    # Format attendu: time_s,seq,rtt_ms
    times: List[float] = []
    seqs: List[int] = []
    rtt_ms: List[float] = []
    with path.open("r", newline="") as f:
        r = csv.DictReader(f)
        for row in r:
            times.append(float(row["time_s"]))
            seqs.append(int(row["seq"]))
            rtt_ms.append(float(row["rtt_ms"]))
    return np.array(times), np.array(seqs), np.array(rtt_ms)


def safe_label_from_run_dir(run_dir: Path) -> str:
    # Exemple: .../results/p2/udp/run1 -> udp_run1
    parts = run_dir.parts
    if len(parts) >= 2:
        return f"{parts[-2]}_{parts[-1]}"
    return run_dir.name


def ensure_dir(p: Path) -> None:
    p.mkdir(parents=True, exist_ok=True)


def stats_str(x: np.ndarray, unit: str) -> str:
    if x.size == 0:
        return f"(vide)"
    p50 = np.percentile(x, 50)
    p95 = np.percentile(x, 95)
    p99 = np.percentile(x, 99)
    return (
        f"n={x.size}, mean={x.mean():.3f} {unit}, median={p50:.3f} {unit}, "
        f"p95={p95:.3f} {unit}, p99={p99:.3f} {unit}, min={x.min():.3f} {unit}, max={x.max():.3f} {unit}"
    )


def plot_throughput(times: np.ndarray, thr_bps: np.ndarray, out_path: Path, title: str) -> None:
    # Débit utile (goodput instantané) en Mbps
    thr_mbps = thr_bps / 1e6
    plt.figure()
    plt.plot(times, thr_mbps)
    plt.xlabel("Temps (s)")
    plt.ylabel("Goodput instantané (Mb/s)")
    plt.title(title)
    plt.tight_layout()
    plt.savefig(out_path)
    plt.close()


def plot_rtt(times: np.ndarray, rtt_ms: np.ndarray, out_path: Path, title: str) -> None:
    plt.figure()
    plt.plot(times, rtt_ms)
    plt.xlabel("Temps (s)")
    plt.ylabel("RTT (ms)")
    plt.title(title)
    plt.tight_layout()
    plt.savefig(out_path)
    plt.close()


def plot_rtt_cdf(rtt_ms: np.ndarray, out_path: Path, title: str) -> None:
    # CDF: utile pour comparer la dispersion (latence sous charge)
    x = np.sort(rtt_ms)
    y = np.linspace(0.0, 1.0, num=x.size, endpoint=True)
    plt.figure()
    plt.plot(x, y)
    plt.xlabel("RTT (ms)")
    plt.ylabel("CDF")
    plt.title(title)
    plt.tight_layout()
    plt.savefig(out_path)
    plt.close()


def process_run(run_dir: Path, out_format: str, make_cdf: bool) -> None:
    raw_dir = run_dir / "raw"
    plots_dir = run_dir / "plots"
    ensure_dir(plots_dir)

    thr_csv = raw_dir / "throughput_timeseries.csv"
    rtt_csv = raw_dir / "rtt_timeseries.csv"

    if not thr_csv.exists():
        raise FileNotFoundError(f"Fichier manquant: {thr_csv}")
    if not rtt_csv.exists():
        raise FileNotFoundError(f"Fichier manquant: {rtt_csv}")

    label = safe_label_from_run_dir(run_dir)

    t_thr, thr_bps = read_throughput_csv(thr_csv)
    t_rtt, _, rtt_ms = read_rtt_csv(rtt_csv)

    # Figures
    thr_fig = plots_dir / f"throughput_vs_time_{label}.{out_format}"
    rtt_fig = plots_dir / f"rtt_vs_time_{label}.{out_format}"
    plot_throughput(t_thr, thr_bps, thr_fig, f"Part 2 - Goodput vs Temps ({label})")
    plot_rtt(t_rtt, rtt_ms, rtt_fig, f"Part 2 - RTT vs Temps ({label})")

    if make_cdf:
        cdf_fig = plots_dir / f"rtt_cdf_{label}.{out_format}"
        plot_rtt_cdf(rtt_ms, cdf_fig, f"Part 2 - RTT CDF ({label})")

    # Stats (texte) pour aider le rapport
    thr_mbps = thr_bps / 1e6
    stats_path = plots_dir / f"stats_{label}.txt"
    with stats_path.open("w") as f:
        f.write(f"=== Statistiques ({label}) ===\n")
        f.write(f"Goodput instantané (Mb/s): {stats_str(thr_mbps, 'Mb/s')}\n")
        f.write(f"RTT (ms): {stats_str(rtt_ms, 'ms')}\n")

    print(f"[OK] {label}")
    print(f"  - {thr_fig}")
    print(f"  - {rtt_fig}")
    if make_cdf:
        print(f"  - {plots_dir / f'rtt_cdf_{label}.{out_format}'}")
    print(f"  - {stats_path}")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Part 2: generation de courbes Goodput/RTT a partir des CSV."
    )
    parser.add_argument(
        "--runs",
        nargs="+",
        default=[
            str(Path.home() / "wifi-ter-sim" / "results" / "p2" / "udp" / "run1"),
            str(Path.home() / "wifi-ter-sim" / "results" / "p2" / "tcp" / "run2"),
        ],
        help="Liste des repertoires de run (ex: .../results/p2/udp/run1 .../results/p2/tcp/run2)",
    )
    parser.add_argument(
        "--format",
        choices=["png", "pdf"],
        default="png",
        help="Format des figures (png ou pdf)",
    )
    parser.add_argument(
        "--cdf",
        action="store_true",
        help="Generer aussi la CDF du RTT",
    )

    args = parser.parse_args()

    for run in args.runs:
        run_dir = Path(run).expanduser().resolve()
        if not run_dir.exists():
            print(f"[WARN] Repertoire introuvable: {run_dir}")
            continue
        try:
            process_run(run_dir, args.format, args.cdf)
        except Exception as e:
            print(f"[ERREUR] {run_dir}: {e}")


if __name__ == "__main__":
    main()
