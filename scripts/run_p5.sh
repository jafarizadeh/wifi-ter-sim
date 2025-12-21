#!/usr/bin/env bash
# scripts/run_p5.sh
# Projet 5 — Run UDP/TCP fairness experiments for N={2,5,10}

set -u
set -o pipefail

NS3_DIR="${NS3_DIR:-$HOME/ns-3}"
SCENARIO="scratch/p5_multi_sta_fairness"

OUTDIR="${OUTDIR:-results/p5}"

SIMTIME="${SIMTIME:-20}"
APPSTART="${APPSTART:-2}"
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

CHANNEL_WIDTH="${CHANNEL_WIDTH:-20}"
TXPOWER_DBM="${TXPOWER_DBM:-20}"
NOISE_FIGURE_DB="${NOISE_FIGURE_DB:-7}"
LOGEXP="${LOGEXP:-3}"
REFDIST="${REFDIST:-1}"
REFLOSS="${REFLOSS:-46.6777}"
RATE_MANAGER="${RATE_MANAGER:-ns3::MinstrelHtWifiManager}"

NSTAS=(2 5 10)

die() { echo "ERROR: $*" >&2; exit 1; }

if [[ ! -d "$NS3_DIR" ]]; then
  die "NS3_DIR not found: $NS3_DIR"
fi

mkdir -p "$OUTDIR/raw" "$OUTDIR/logs" "$OUTDIR/plots"

echo "[P5] Building ns-3 in: $NS3_DIR"
( cd "$NS3_DIR" && ./ns3 build ) || die "ns-3 build failed"

FAIL=0
IDX=0
timestamp="$(date +%Y%m%d_%H%M%S)"

run_one () {
  local transport="$1"
  local nsta="$2"
  local runnum="$3"

  local tag="${transport}_n${nsta}_run${runnum}_${timestamp}"
  local log="$OUTDIR/logs/${tag}.log"

  echo "[P5] Running transport=$transport nSta=$nsta run=$runnum (log: $log)"

  # ✅ مهم: در نسخه‌ی شما نباید جداکننده‌ی "--" بعد از سناریو بیاید
  ( cd "$NS3_DIR" && \
    ./ns3 run "$SCENARIO \
      --simTime=${SIMTIME} \
      --appStart=${APPSTART} \
      --nSta=${nsta} \
      --distance=${DISTANCE} \
      --radius=${RADIUS} \
      --transport=${transport} \
      --pktSize=${PKTSIZE} \
      --udpRatePerSta=${UDP_RATE_PER_STA} \
      --tcpMaxBytes=${TCP_MAXBYTES} \
      --interval=${INTERVAL} \
      --pcap=${PCAP} \
      --flowmon=${FLOWMON} \
      --seed=${SEED} \
      --run=${runnum} \
      --outDir=${OUTDIR} \
      --channelWidth=${CHANNEL_WIDTH} \
      --txPowerDbm=${TXPOWER_DBM} \
      --noiseFigureDb=${NOISE_FIGURE_DB} \
      --logExp=${LOGEXP} \
      --refDist=${REFDIST} \
      --refLoss=${REFLOSS} \
      --rateManager=${RATE_MANAGER} \
    " \
  ) >"$log" 2>&1

  if [[ $? -ne 0 ]]; then
    echo "[P5] FAILED: transport=$transport nSta=$nsta run=$runnum"
    ((FAIL++)) || true
    touch "$OUTDIR/raw/.failed_${tag}"
  fi
}

for nsta in "${NSTAS[@]}"; do
  runnum=$((RUN_BASE + IDX))
  run_one "udp" "$nsta" "$runnum"
  IDX=$((IDX + 1))
done

for nsta in "${NSTAS[@]}"; do
  runnum=$((RUN_BASE + IDX))
  run_one "tcp" "$nsta" "$runnum"
  IDX=$((IDX + 1))
done

echo "[P5] Sanity checks..."
SUMMARY="$OUTDIR/raw/p5_summary.csv"
if [[ -f "$SUMMARY" ]]; then
  echo "[P5] Summary lines (incl header): $(wc -l < "$SUMMARY")"
else
  echo "[P5] WARNING: summary not found: $SUMMARY"
fi

if [[ $FAIL -ne 0 ]]; then
  echo "[P5] Completed with $FAIL failures. Check logs in $OUTDIR/logs/"
  exit 1
fi

echo "[P5] All runs completed successfully."
echo "[P5] Outputs are under: $OUTDIR/"
