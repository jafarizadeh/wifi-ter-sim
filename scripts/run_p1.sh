#!/usr/bin/env bash
# scripts/run_p1.sh
# Project 1 runner (clean, stage scenario, build, run, collect outputs)

set -euo pipefail
IFS=$'\n\t'

# ---------------------------- Paths ----------------------------
NS3_DIR="${NS3_DIR:-$HOME/ns-3}"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

SCEN_NAME="${SCEN_NAME:-p1_minimal_wifi}"
SCEN_SRC="${SCEN_SRC:-$ROOT_DIR/scenarios/${SCEN_NAME}.cc}"
SCENARIO="scratch/${SCEN_NAME}"

OUTDIR="${OUTDIR:-$ROOT_DIR/results/p1}"
RAW_DIR="$OUTDIR/raw"
LOG_DIR="$OUTDIR/logs"
PLOT_DIR="$OUTDIR/plots"

# ---------------------------- Parameters ----------------------------
SSID="${SSID:-wifi-demo}"
SIMTIME="${SIMTIME:-10}"
DISTANCE="${DISTANCE:-5}"
PCAP="${PCAP:-true}"

# Build parallelism
MAX_JOBS="${MAX_JOBS:-8}"
TAIL_LINES="${TAIL_LINES:-160}"

# Clean toggle
CLEAN="${CLEAN:-true}"

# ---------------------------- Helpers ----------------------------
ts(){ date +"%Y-%m-%d %H:%M:%S"; }
log(){ echo "[$(ts)] $*"; }
warn(){ echo "[$(ts)] WARN: $*" >&2; }
die(){ echo "[$(ts)] ERROR: $*" >&2; exit 1; }
need_cmd(){ command -v "$1" >/dev/null 2>&1 || die "Missing command: $1"; }

ensure_dirs(){ mkdir -p "$RAW_DIR" "$LOG_DIR" "$PLOT_DIR"; }

clean_outputs(){
  log "[P1] Cleaning previous outputs under: $OUTDIR"
  rm -rf "$RAW_DIR" "$LOG_DIR" "$PLOT_DIR" || true
  ensure_dirs
}

stage_scenario(){
  [[ -d "$NS3_DIR" ]] || die "NS3_DIR not found: $NS3_DIR"
  [[ -f "$SCEN_SRC" ]] || die "Scenario source not found: $SCEN_SRC"
  mkdir -p "$NS3_DIR/scratch"
  cp -f "$SCEN_SRC" "$NS3_DIR/scratch/${SCEN_NAME}.cc" || die "Copy to ns-3 scratch failed"
  log "[P1] Staged scenario: $SCEN_SRC -> $NS3_DIR/scratch/${SCEN_NAME}.cc"
}

build_ns3(){
  log "[P1] Building ns-3"
  local build_log="$LOG_DIR/build.log"
  ( cd "$NS3_DIR" && ./ns3 build --jobs="$MAX_JOBS" ) >"$build_log" 2>&1 || {
    echo
    warn "[P1] BUILD FAILED. Last ${TAIL_LINES} lines:"
    tail -n "$TAIL_LINES" "$build_log" || true
    die "ns-3 build failed (full log: $build_log)"
  }
  log "[P1] Build OK"
}

run_one(){
  local run_log="$LOG_DIR/run.log"
  log "[P1] RUN start"
  ( cd "$NS3_DIR" && ./ns3 run "${SCENARIO} --ssid=${SSID} --simTime=${SIMTIME} --distance=${DISTANCE} --pcap=${PCAP} --outDir=${OUTDIR}" ) >"$run_log" 2>&1 || {
    warn "[P1] RUN FAILED (see $run_log)"
    tail -n "$TAIL_LINES" "$run_log" || true
    return 1
  }
  log "[P1] RUN ok"
  return 0
}

report_outputs(){
  echo
  echo "====================== [P1] Summary ======================"
  echo "NS3_DIR : $NS3_DIR"
  echo "Scenario: $SCENARIO"
  echo "Source  : $SCEN_SRC"
  echo "OutDir  : $OUTDIR"
  echo "Raw     : $RAW_DIR"
  echo "Logs    : $LOG_DIR"
  echo "Plots   : $PLOT_DIR"
  echo "----------------------------------------------------------"
  echo "Expected outputs:"
  echo "  - $LOG_DIR/summary.txt"
  echo "  - $RAW_DIR/*.pcap (if PCAP=true)"
  echo "  - $LOG_DIR/run.log (stdout/stderr)"
  echo "=========================================================="
  echo
}

main(){
  need_cmd tail

  ensure_dirs
  if [[ "$CLEAN" == "true" ]]; then
    clean_outputs
  fi

  stage_scenario
  build_ns3
  run_one

  report_outputs
  log "[P1] Completed."
}

main "$@"
