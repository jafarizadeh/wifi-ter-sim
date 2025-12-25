#!/usr/bin/env bash
# scripts/run_p8.sh
#
# Projet 8 (ns-3) runner - inspired by run_p7.sh:
#  1) Cleans previous outputs (default).
#  2) Stages scenario source into ns-3 scratch/.
#  3) Builds ns-3 (quiet by default, logs to results/p8/logs/build.log).
#  4) Runs the required scenarios with counters like: [1/38]
#       - baseline (1 run)
#       - qos off + qos on (2 runs)
#       - heatmap single-point grid (35 runs: 7x5)
#  5) Verifies that required output files exist.
#
# Usage:
#   chmod +x scripts/run_p8.sh
#   ./scripts/run_p8.sh
#
# Overrides:
#   NS3_DIR=~/ns-3 OUTDIR=results/p8 ./scripts/run_p8.sh
#   CLEAN=false QUIET_RUN=false ./scripts/run_p8.sh
#   STOP_ON_FIRST_FAIL=true ./scripts/run_p8.sh
#   # If you want to stage your fixed file instead:
#   SCEN_SRC=./scenarios/p8_final_simulator_fixed.cc ./scripts/run_p8.sh

set -euo pipefail
IFS=$'\n\t'

# ---------------------------- Defaults / Config ----------------------------
NS3_DIR="${NS3_DIR:-$HOME/ns-3}"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Scenario source in your repo + target name in ns-3 scratch/
SCEN_SRC="${SCEN_SRC:-$ROOT_DIR/scenarios/p8_final_simulator.cc}"
SCEN_NAME="${SCEN_NAME:-p8_final_simulator}"
SCENARIO="${SCENARIO:-scratch/${SCEN_NAME}}"

# Output folder (inside your repo)
OUTDIR="${OUTDIR:-$ROOT_DIR/results/p8}"

# Common knobs (match your C++ CLI options)
SSID="${SSID:-WIFI-TER-P8}"
PCAP="${PCAP:-false}"
FLOWMON="${FLOWMON:-true}"
SEED="${SEED:-1}"
RUN_BASE="${RUN_BASE:-1}"

TXPWR="${TXPWR:-16}"
LOGEXP="${LOGEXP:-3.0}"

# Baseline settings
BASE_TRANSPORT="${BASE_TRANSPORT:-udp}"      # udp|tcp
BASE_SIMTIME="${BASE_SIMTIME:-25}"
BASE_APPSTART="${BASE_APPSTART:-2}"
BASE_DISTANCE="${BASE_DISTANCE:-10}"
BASE_PKTSIZE="${BASE_PKTSIZE:-1200}"
BASE_UDPRATE="${BASE_UDPRATE:-10Mbps}"
BASE_TCPMAXBYTES="${BASE_TCPMAXBYTES:-0}"   # 0 => unlimited in your code (if implemented)

# QoS settings
QOS_SIMTIME="${QOS_SIMTIME:-25}"
QOS_APPSTART="${QOS_APPSTART:-2}"
VIDEO_RATE="${VIDEO_RATE:-20Mbps}"
BE_RATE="${BE_RATE:-40Mbps}"
VOIP_PKTSIZE="${VOIP_PKTSIZE:-200}"
VOIP_INTERVAL_MS="${VOIP_INTERVAL_MS:-20}"
VIDEO_PKTSIZE="${VIDEO_PKTSIZE:-1200}"
BE_PKTSIZE="${BE_PKTSIZE:-1200}"

# Heatmap settings (single point grid; 7x5 = 35)
HEAT_SIMTIME="${HEAT_SIMTIME:-12}"
HEAT_APPSTART="${HEAT_APPSTART:-2}"
HEAT_PKTSIZE="${HEAT_PKTSIZE:-1200}"
HEAT_UDPRATE="${HEAT_UDPRATE:-10Mbps}"

# Grid points (as specified in project statement)
X_POINTS=(0 5 10 15 20 25 30)
Y_POINTS=(0 5 10 15 20)

# Behavior toggles
CLEAN="${CLEAN:-true}"                      # delete old outputs first
STOP_ON_FIRST_FAIL="${STOP_ON_FIRST_FAIL:-false}"
QUIET_BUILD="${QUIET_BUILD:-true}"
QUIET_RUN="${QUIET_RUN:-true}"
TAIL_LINES="${TAIL_LINES:-180}"

# ---------------------------- Helpers ----------------------------
ts() { date +"%Y-%m-%d %H:%M:%S"; }
log() { echo "[$(ts)] $*"; }
warn() { echo "[$(ts)] WARN: $*" >&2; }
die() { echo "[$(ts)] ERROR: $*" >&2; exit 1; }

need_cmd() { command -v "$1" >/dev/null 2>&1 || die "Missing command: $1"; }

ensure_dirs() {
  mkdir -p "$OUTDIR/raw" "$OUTDIR/logs" "$OUTDIR/plots" "$OUTDIR/heatmaps"
}

clean_outputs() {
  log "[P8] Cleaning previous outputs under: $OUTDIR"
  # Hard clean: remove all files under OUTDIR (but keep directory)
  rm -rf "$OUTDIR/raw" "$OUTDIR/logs" "$OUTDIR/plots" "$OUTDIR/heatmaps" || true
  ensure_dirs
}

stage_scenario() {
  [[ -d "$NS3_DIR" ]] || die "NS3_DIR not found: $NS3_DIR"
  [[ -f "$SCEN_SRC" ]] || die "Scenario source not found: $SCEN_SRC"

  local dst="$NS3_DIR/scratch/${SCEN_NAME}.cc"
  cp -f "$SCEN_SRC" "$dst" || die "Copy to ns-3 scratch failed: $dst"
  log "[P8] Staged scenario: $SCEN_SRC -> $dst"
}

build_ns3() {
  log "[P8] Building ns-3 in: $NS3_DIR"
  local build_log="$OUTDIR/logs/build.log"

  if [[ "$QUIET_BUILD" == "true" ]]; then
    ( cd "$NS3_DIR" && ./ns3 build ) >"$build_log" 2>&1 || {
      echo
      warn "[P8] BUILD FAILED. Showing last ${TAIL_LINES} lines from $build_log"
      tail -n "$TAIL_LINES" "$build_log" || true
      die "ns-3 build failed (full log: $build_log)"
    }
  else
    ( cd "$NS3_DIR" && ./ns3 build ) || die "ns-3 build failed"
    : >"$build_log" || true
  fi

  log "[P8] Build OK"
}

run_ns3() {
  local tag="$1"
  local args="$2"
  local seq="$3"
  local total="$4"

  local log_path="$OUTDIR/logs/${tag}.log"

  log "[P8] [${seq}/${total}] RUN start  ${tag}"

  if [[ "$QUIET_RUN" == "true" ]]; then
    ( cd "$NS3_DIR" && ./ns3 run "${SCENARIO} ${args}" ) >"$log_path" 2>&1 || {
      echo
      warn "[P8] RUN FAILED: $tag"
      warn "[P8] Log file: $log_path"
      warn "[P8] Last ${TAIL_LINES} lines:"
      tail -n "$TAIL_LINES" "$log_path" || true
      return 1
    }
  else
    ( cd "$NS3_DIR" && ./ns3 run "${SCENARIO} ${args}" ) | tee "$log_path" || {
      echo
      warn "[P8] RUN FAILED: $tag"
      warn "[P8] Log file: $log_path"
      return 1
    }
  fi

  log "[P8] [${seq}/${total}] RUN ok     ${tag}"
  return 0
}

exists_ok() {
  local path="$1"
  if [[ -s "$path" ]]; then
    echo "OK   $path"
    return 0
  fi
  echo "MISS $path"
  return 1
}

usage() {
  cat <<EOF
Usage: ./scripts/run_p8.sh

Environment overrides (examples):
  NS3_DIR=~/ns-3 OUTDIR=results/p8 ./scripts/run_p8.sh
  SCEN_SRC=./scenarios/p8_final_simulator_fixed.cc ./scripts/run_p8.sh
  CLEAN=false QUIET_RUN=false ./scripts/run_p8.sh
  STOP_ON_FIRST_FAIL=true ./scripts/run_p8.sh

Key toggles:
  CLEAN=true|false              (default: true)
  QUIET_BUILD=true|false        (default: true)
  QUIET_RUN=true|false          (default: true)
  STOP_ON_FIRST_FAIL=true|false (default: false)
EOF
}

report_outputs() {
  echo
  echo "====================== [P8] Summary ======================"
  echo "OutDir  : $OUTDIR"
  echo "Logs    : $OUTDIR/logs"
  echo "Raw     : $OUTDIR/raw"
  echo "Heatmaps: $OUTDIR/heatmaps"
  echo "----------------------------------------------------------"
  echo "[P8] Required files check (best-effort):"

  # These depend on your C++ implementation (but are expected in the fixed version)
  exists_ok "$OUTDIR/raw/p8_summary.csv" || true
  exists_ok "$OUTDIR/raw/baseline_summary.csv" || true
  exists_ok "$OUTDIR/raw/qos_summary.csv" || true
  exists_ok "$OUTDIR/heatmaps/heatmap.csv" || true

  echo "=========================================================="
}

# ---------------------------- Main ----------------------------
main() {
  need_cmd tail
  need_cmd find
  need_cmd sort

  if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
  fi

  ensure_dirs

  # Always clean by default (per your requirement).
  if [[ "$CLEAN" == "true" ]]; then
    clean_outputs
  else
    log "[P8] CLEAN disabled; existing outputs may be overwritten/appended"
  fi

  stage_scenario
  build_ns3

  # Total runs:
  # baseline(1) + qos(off,on)=2 + heatmap grid = len(X)*len(Y)
  local HEAT_RUNS=$(( ${#X_POINTS[@]} * ${#Y_POINTS[@]} ))
  local TOTAL_RUNS=$(( 1 + 2 + HEAT_RUNS ))

  local FAIL=0
  local IDX=0

  # ---------------- baseline ----------------
  local runnum=$((RUN_BASE + IDX))
  local seq=$((IDX + 1))
  local tag="baseline_${BASE_TRANSPORT}_d${BASE_DISTANCE}_run${runnum}"

  local args="\
--scenario=baseline \
--outDir=${OUTDIR} --ssid=${SSID} \
--pcap=${PCAP} --flowmon=${FLOWMON} \
--seed=${SEED} --run=${runnum} \
--txPowerDbm=${TXPWR} --logExp=${LOGEXP} \
--transport=${BASE_TRANSPORT} \
--simTime=${BASE_SIMTIME} --appStart=${BASE_APPSTART} \
--distance=${BASE_DISTANCE} \
--pktSize=${BASE_PKTSIZE} \
--udpRate=${BASE_UDPRATE} \
--tcpMaxBytes=${BASE_TCPMAXBYTES}"

  if ! run_ns3 "$tag" "$args" "$seq" "$TOTAL_RUNS"; then
    FAIL=$((FAIL + 1))
    [[ "$STOP_ON_FIRST_FAIL" == "true" ]] && report_outputs && die "[P8] Aborting on first failure"
  fi
  IDX=$((IDX + 1))

  # ---------------- qos OFF / ON ----------------
  for mode in off on; do
    runnum=$((RUN_BASE + IDX))
    seq=$((IDX + 1))
    tag="qos_${mode}_run${runnum}"

    args="\
--scenario=qos \
--outDir=${OUTDIR} --ssid=${SSID} \
--pcap=${PCAP} --flowmon=${FLOWMON} \
--seed=${SEED} --run=${runnum} \
--txPowerDbm=${TXPWR} --logExp=${LOGEXP} \
--simTime=${QOS_SIMTIME} --appStart=${QOS_APPSTART} \
--qosMode=${mode} \
--voipPktSize=${VOIP_PKTSIZE} --voipIntervalMs=${VOIP_INTERVAL_MS} \
--videoPktSize=${VIDEO_PKTSIZE} --videoRate=${VIDEO_RATE} \
--bePktSize=${BE_PKTSIZE} --beRate=${BE_RATE}"

    if ! run_ns3 "$tag" "$args" "$seq" "$TOTAL_RUNS"; then
      FAIL=$((FAIL + 1))
      [[ "$STOP_ON_FIRST_FAIL" == "true" ]] && report_outputs && die "[P8] Aborting on first failure"
    fi
    IDX=$((IDX + 1))
  done

  # ---------------- heatmap grid (single point per run) ----------------
  for x in "${X_POINTS[@]}"; do
    for y in "${Y_POINTS[@]}"; do
      runnum=$((RUN_BASE + IDX))
      seq=$((IDX + 1))
      tag="heatmap_x${x}_y${y}_run${runnum}"

      args="\
--scenario=heatmap \
--heatmapMode=single \
--outDir=${OUTDIR} --ssid=${SSID} \
--pcap=${PCAP} --flowmon=${FLOWMON} \
--seed=${SEED} --run=${runnum} \
--txPowerDbm=${TXPWR} --logExp=${LOGEXP} \
--heatSimTime=${HEAT_SIMTIME} --heatAppStart=${HEAT_APPSTART} \
--heatPktSize=${HEAT_PKTSIZE} --heatUdpRate=${HEAT_UDPRATE} \
--x=${x} --y=${y}"

      if ! run_ns3 "$tag" "$args" "$seq" "$TOTAL_RUNS"; then
        FAIL=$((FAIL + 1))
        [[ "$STOP_ON_FIRST_FAIL" == "true" ]] && report_outputs && die "[P8] Aborting on first failure"
      fi
      IDX=$((IDX + 1))
    done
  done

  # Post-run verification + summary
  report_outputs

  if [[ $FAIL -ne 0 ]]; then
    die "[P8] Completed with failures: $FAIL (see $OUTDIR/logs/)"
  fi

  log "[P8] All runs completed successfully."
}

main "$@"
