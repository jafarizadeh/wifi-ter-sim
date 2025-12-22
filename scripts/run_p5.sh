#!/usr/bin/env bash
# scripts/run_p5.sh
# Projet 5 — Run UDP/TCP fairness experiments for N={2,5,10}
# Compatible with the CommandLine parameters in your current p5_multi_sta_fairness.cc

set -u
set -o pipefail

# -------------------- config --------------------
NS3_DIR="${NS3_DIR:-$HOME/ns-3}"

# For scratch programs, this form is reliable across ns-3 setups:
SCENARIO="${SCENARIO:-scratch/p5_multi_sta_fairness}"

OUTDIR="${OUTDIR:-$HOME/wifi-ter-sim/results/p5}"

SIMTIME="${SIMTIME:-20}"
APPSTART="${APPSTART:-3}"
DISTANCE="${DISTANCE:-10}"
RADIUS="${RADIUS:-10}"

PKTSIZE="${PKTSIZE:-1200}"
UDP_RATE_PER_STA="${UDP_RATE_PER_STA:-10Mbps}"
TCP_MAXBYTES="${TCP_MAXBYTES:-0}"

INTERVAL="${INTERVAL:-0.5}"

PCAP="${PCAP:-false}"
FLOWMON="${FLOWMON:-true}"

SEED="${SEED:-1}"
RUN_BASE="${RUN_BASE:-1}"

NSTAS=(2 5 10)

# -------------------- helpers --------------------
die() { echo "ERROR: $*" >&2; exit 1; }

# -------------------- checks --------------------
[[ -d "$NS3_DIR" ]] || die "NS3_DIR not found: $NS3_DIR"

mkdir -p "$OUTDIR/raw" "$OUTDIR/logs" "$OUTDIR/plots"

# -------------------- build once --------------------
echo "[P5] Building ns-3 in: $NS3_DIR"
( cd "$NS3_DIR" && ./ns3 build ) || die "ns-3 build failed"

timestamp="$(date +%Y%m%d_%H%M%S)"
FAIL=0
IDX=0

run_one () {
  local transport="$1"
  local nsta="$2"
  local runnum="$3"

  local tag="${transport}_n${nsta}_run${runnum}_${timestamp}"
  local log="$OUTDIR/logs/${tag}.log"

  echo "[P5] Running transport=$transport nSta=$nsta run=$runnum (log: $log)"

  # IMPORTANT: Only pass arguments that exist in your C++ CommandLine.
  ( cd "$NS3_DIR" && \
    ./ns3 run "${SCENARIO} \
--simTime=${SIMTIME} \
--appStart=${APPSTART} \
--nSta=${nsta} \
--distance=${DISTANCE} \
--radius=${RADIUS} \
--ssid=wifi6-ter \
--outDir=${OUTDIR} \
--pcap=${PCAP} \
--flowmon=${FLOWMON} \
--seed=${SEED} \
--run=${runnum} \
--transport=${transport} \
--pktSize=${PKTSIZE} \
--udpRatePerSta=${UDP_RATE_PER_STA} \
--interval=${INTERVAL} \
" \
  ) >"$log" 2>&1


  if [[ $? -ne 0 ]]; then
    echo "[P5] FAILED: transport=$transport nSta=$nsta run=$runnum"
    ((FAIL++)) || true
    touch "$OUTDIR/raw/.failed_${tag}"
  fi
}

# UDP series (3 runs)
for nsta in "${NSTAS[@]}"; do
  runnum=$((RUN_BASE + IDX))
  run_one "udp" "$nsta" "$runnum"
  IDX=$((IDX + 1))
done

# TCP series (3 runs)
for nsta in "${NSTAS[@]}"; do
  runnum=$((RUN_BASE + IDX))
  run_one "tcp" "$nsta" "$runnum"
  IDX=$((IDX + 1))
done

# -------------------- sanity checks --------------------
echo "[P5] Sanity checks..."

SUMMARY="$OUTDIR/raw/p5_summary.csv"
if [[ ! -f "$SUMMARY" ]]; then
  echo "[P5] WARNING: summary not found: $SUMMARY"
else
  echo "[P5] Summary path: $SUMMARY"
  echo "[P5] Summary lines (including header): $(wc -l < "$SUMMARY")"
fi

if [[ $FAIL -ne 0 ]]; then
  echo "[P5] Completed with $FAIL failures. Check logs in $OUTDIR/logs/"
  exit 1
fi

echo "[P5] All runs completed successfully."
echo "[P5] Outputs:"
echo "  - $OUTDIR/raw/persta_*.csv"
echo "  - $OUTDIR/raw/p5_summary.csv"
echo "  - $OUTDIR/logs/*.log"
echo "  - $OUTDIR/raw/flowmon_*.xml (if flowmon=true)"
echo "  - $OUTDIR/raw/ts_*.csv (if enabled by scenario)"
