#!/usr/bin/env bash
# scripts/run_p6.sh
# Projet 6 — Multi-AP roaming experiments (speeds sweep)

set -u
set -o pipefail

NS3_DIR="${NS3_DIR:-$HOME/ns-3}"
SCENARIO="${SCENARIO:-scratch/p6_roaming_multi_ap}"
OUTDIR="${OUTDIR:-$HOME/wifi-ter-sim/results/p6}"

SIMTIME="${SIMTIME:-30}"
APPSTART="${APPSTART:-2}"
MOVESTART="${MOVESTART:-5}"
APDIST="${APDIST:-30}"

UDPRATE="${UDPRATE:-20Mbps}"
PKTSIZE="${PKTSIZE:-1200}"
INTERVAL="${INTERVAL:-0.5}"
SSID="${SSID:-wifi6-ter}"

PCAP="${PCAP:-false}"
FLOWMON="${FLOWMON:-true}"

SEED="${SEED:-1}"
RUN_BASE="${RUN_BASE:-1}"

SPEEDS=(0.5 1.0 2.0)

# Optional: only pass roamPoll if you are sure your C++ supports it
PASS_ROAMPOLL="${PASS_ROAMPOLL:-false}"
ROAMPOLL="${ROAMPOLL:-0.05}"

die() { echo "ERROR: $*" >&2; exit 1; }

[[ -d "$NS3_DIR" ]] || die "NS3_DIR not found: $NS3_DIR"
mkdir -p "$OUTDIR/raw" "$OUTDIR/logs" "$OUTDIR/plots"

echo "[P6] Building ns-3 in: $NS3_DIR"
( cd "$NS3_DIR" && ./ns3 build ) || die "ns-3 build failed"

timestamp="$(date +%Y%m%d_%H%M%S)"
FAIL=0
IDX=0

run_one () {
  local speed="$1"
  local runnum="$2"

  local tag="v${speed}_run${runnum}_${timestamp}"
  local log="$OUTDIR/logs/console_${tag}.log"

  echo "[P6] Running staSpeed=$speed run=$runnum (log: $log)"

  local args="\
--simTime=${SIMTIME} --appStart=${APPSTART} --moveStart=${MOVESTART} \
--apDistance=${APDIST} --staSpeed=${speed} \
--udpRate=${UDPRATE} --pktSize=${PKTSIZE} --interval=${INTERVAL} \
--ssid=${SSID} --outDir=${OUTDIR} --pcap=${PCAP} --flowmon=${FLOWMON} \
--seed=${SEED} --run=${runnum}"

  if [[ "$PASS_ROAMPOLL" == "true" ]]; then
    args="${args} --roamPoll=${ROAMPOLL}"
  fi

  set +e
  ( cd "$NS3_DIR" && ./ns3 run "${SCENARIO} ${args}" ) >"$log" 2>&1
  local status=$?
  set -e

  if [[ $status -ne 0 ]]; then
    echo "[P6] FAILED: staSpeed=$speed run=$runnum (exit=$status)"
    echo "[P6] ---- last 80 lines of log ----"
    tail -n 80 "$log" || true
    echo "[P6] ------------------------------"
    ((FAIL++)) || true
    touch "$OUTDIR/raw/.failed_${tag}"
  else
    echo "[P6] OK: staSpeed=$speed run=$runnum"
  fi
}

for v in "${SPEEDS[@]}"; do
  runnum=$((RUN_BASE + IDX))
  run_one "$v" "$runnum"
  IDX=$((IDX + 1))
done

echo "[P6] Sanity checks..."
for f in "$OUTDIR/raw/p6_summary.csv" "$OUTDIR/raw/throughput_timeseries.csv" "$OUTDIR/raw/roaming_events.txt"; do
  [[ -f "$f" ]] || echo "[P6] WARNING: missing $f"
done

if [[ $FAIL -ne 0 ]]; then
  echo "[P6] Completed with $FAIL failures. Check logs in $OUTDIR/logs/"
  exit 1
fi

echo "[P6] All runs completed successfully."
