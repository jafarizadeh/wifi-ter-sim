#!/usr/bin/env bash
# scripts/run_p7.sh
# Project 7 runner — P2-compatible layout + clean logs + plots

set -euo pipefail
IFS=$'\n\t'

# ---------------------------- Paths ----------------------------
NS3_DIR="${NS3_DIR:-$HOME/ns-3}"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

SCEN_NAME="${SCEN_NAME:-p7_channel_planning}"
SCEN_SRC="${SCEN_SRC:-$ROOT_DIR/scenarios/${SCEN_NAME}.cc}"
SCENARIO="scratch/${SCEN_NAME}"

OUTDIR="${OUTDIR:-$ROOT_DIR/results/p7}"
RAW_DIR="$OUTDIR/raw"
LOG_DIR="$OUTDIR/logs"
PLOT_DIR="$OUTDIR/plots"
TMP_ROOT="$OUTDIR/tmp_runs"
PER_RUN_DIR="$RAW_DIR/per_run"

# Python venv
VENV_DIR="${VENV_DIR:-$ROOT_DIR/.venv}"
PY="$VENV_DIR/bin/python"

# ---------------------------- Experiment params ----------------------------
PLANS="${PLANS:-cocanal,separe}"
NSTAS="${NSTAS:-2,5,10}"

SIMTIME="${SIMTIME:-20}"
APPSTART="${APPSTART:-2}"
PKTSIZE="${PKTSIZE:-1200}"
UDPRATE="${UDPRATE:-50Mbps}"

PCAP="${PCAP:-false}"
FLOWMON="${FLOWMON:-true}"

SEED="${SEED:-1}"
RUN_BASE="${RUN_BASE:-1}"

MAX_JOBS="${MAX_JOBS:-8}"
JOBS="${JOBS:-}"

STRICT_PNG="${STRICT_PNG:-true}"

# ---------------------------- Helpers ----------------------------
ts(){ date +"%Y-%m-%d %H:%M:%S"; }
log(){ echo "[$(ts)] $*"; }
warn(){ echo "[$(ts)] WARN: $*" >&2; }
die(){ echo "[$(ts)] ERROR: $*" >&2; exit 1; }

step_log(){
  log "[P7] [$1/$2] $3 $4 $5"
}

ensure_dirs(){
  mkdir -p "$RAW_DIR" "$LOG_DIR" "$PLOT_DIR" "$TMP_ROOT" "$PER_RUN_DIR"
}

clean_outputs(){
  log "[P7] Cleaning previous outputs under: $OUTDIR"
  rm -rf "$RAW_DIR" "$LOG_DIR" "$PLOT_DIR" "$TMP_ROOT" || true
  ensure_dirs
}

stage_scenario(){
  cp -f "$SCEN_SRC" "$NS3_DIR/scratch/${SCEN_NAME}.cc"
  log "[P7] Staged scenario: $SCEN_SRC"
}

build_ns3(){
  log "[P7] Building ns-3"
  (cd "$NS3_DIR" && ./ns3 build --jobs="$MAX_JOBS") >"$LOG_DIR/build.log" 2>&1
  log "[P7] Build OK"
}

run_one(){
  local tag="$1" args="$2" idx="$3" total="$4"
  local run_log="$LOG_DIR/${tag}.log"

  log "[P7] [$idx/$total] RUN start $tag"
  if (cd "$NS3_DIR" && ./ns3 run "${SCENARIO} ${args}") >"$run_log" 2>&1; then
    log "[P7] [$idx/$total] RUN ok    $tag"
    mkdir -p "$TMP_ROOT/$tag/logs"
    cp -f "$run_log" "$TMP_ROOT/$tag/logs/console.log"
    return 0
  else
    warn "[P7] RUN FAILED: $tag (see $run_log)"
    return 1
  fi
}

copy_per_run_tree(){
  for d in "$TMP_ROOT"/*; do
    [[ -d "$d" ]] || continue
    local tag="$(basename "$d")"
    mkdir -p "$PER_RUN_DIR/$tag"
    [[ -d "$d/raw"  ]] && cp -a "$d/raw"  "$PER_RUN_DIR/$tag/"
    [[ -d "$d/logs" ]] && cp -a "$d/logs" "$PER_RUN_DIR/$tag/"
  done
}

ensure_venv(){
  [[ -x "$PY" ]] || python3 -m venv "$VENV_DIR"
}

check_plot_deps_or_die(){
  "$PY" - <<'PY'
import importlib, sys
for p in ["numpy","pandas","plotly","kaleido"]:
    try: importlib.import_module(p)
    except: sys.exit(2)
PY
}

run_plots(){
  log "[P7] [PLOT] start p7_plots"
  local args=()
  [[ "${STRICT_PNG,,}" == "true" ]] && args+=(--strict-png)
  "$PY" "$ROOT_DIR/scripts/plot_p7.py" --results "$OUTDIR" "${args[@]}" \
    >"$LOG_DIR/plots.log" 2>&1
  log "[P7] [PLOT] ok    p7_plots"
}

# ---------------------------- Main ----------------------------
main(){
  [[ -z "$JOBS" ]] && JOBS=$(nproc 2>/dev/null | awk '{print int($1/2)}')
  (( JOBS < 1 )) && JOBS=1
  (( JOBS > MAX_JOBS )) && JOBS="$MAX_JOBS"

  clean_outputs
  stage_scenario
  build_ns3

  IFS=',' read -r -a PLAN_ARR <<< "$PLANS"
  IFS=',' read -r -a N_ARR <<< "$NSTAS"

  local total=$(( ${#PLAN_ARR[@]} * ${#N_ARR[@]} ))
  local idx=0 running=0 fail=0

  for plan in "${PLAN_ARR[@]}"; do
    for n in "${N_ARR[@]}"; do
      idx=$((idx+1))
      local runnum=$((RUN_BASE + idx - 1))
      local tag="p7_${plan}_n${n}_run${runnum}"
      local out="$TMP_ROOT/$tag"
      mkdir -p "$out/raw"

      local args="--channelPlan=${plan} --nStaPerCell=${n} \
--simTime=${SIMTIME} --appStart=${APPSTART} \
--pktSize=${PKTSIZE} --udpRatePerSta=${UDPRATE} \
--outDir=${out} --pcap=${PCAP} --flowmon=${FLOWMON} \
--seed=${SEED} --run=${runnum}"

      run_one "$tag" "$args" "$idx" "$total" &
      running=$((running+1))
      (( running >= JOBS )) && { wait -n || fail=$((fail+1)); running=$((running-1)); }
    done
  done

  while (( running > 0 )); do
    wait -n || fail=$((fail+1))
    running=$((running-1))
  done

  step_log 1 2 MERGE start p7_summary.csv
  cp "$TMP_ROOT"/*/raw/p7_summary.csv "$RAW_DIR/p7_summary.csv" 2>/dev/null || true
  step_log 1 2 MERGE ok    p7_summary.csv

  step_log 2 2 MERGE start perflow_*.csv
  mkdir -p "$RAW_DIR/perflow"
  find "$TMP_ROOT" -type f -path "*/raw/perflow_*.csv" -exec cp {} "$RAW_DIR/perflow/" \;
  step_log 2 2 MERGE ok    perflow_*.csv

  copy_per_run_tree
  rm -rf "$TMP_ROOT"

  log "[P7] Completed."

  ensure_venv
  check_plot_deps_or_die
  run_plots

  (( fail == 0 )) || die "[P7] One or more runs failed"
}

main "$@"
