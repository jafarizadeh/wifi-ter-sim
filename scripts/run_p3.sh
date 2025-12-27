#!/usr/bin/env bash
# scripts/run_p3.sh
#  Part 3 runner (tmp outDir per distance, safe merge, logs per run)

set -euo pipefail
IFS=$'\n\t'

# ---------------------------- Paths ----------------------------
NS3_DIR="${NS3_DIR:-$HOME/ns-3}"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

SCEN_NAME="${SCEN_NAME:-p3_distance_sweep}"
SCEN_SRC="${SCEN_SRC:-$ROOT_DIR/scenarios/${SCEN_NAME}.cc}"
SCENARIO="scratch/${SCEN_NAME}"

OUTDIR="${OUTDIR:-$ROOT_DIR/results/p3}"
RAW_DIR="$OUTDIR/raw"
LOG_DIR="$OUTDIR/logs"
PLOT_DIR="$OUTDIR/plots"
TMP_ROOT="$OUTDIR/tmp_runs"

# ---------------------------- Experiment defaults (per PDF) ----------------------------
# Distances imposed by the  Part
DISTANCES="${DISTANCES:-1,5,10,15,20,25,30}"   # :contentReference[oaicite:7]{index=7}

SIMTIME="${SIMTIME:-20}"
APPSTART="${APPSTART:-2}"

TRANSPORT="${TRANSPORT:-udp}"                  # minimum required: UDP sweep :contentReference[oaicite:8]{index=8}
UDPRATE="${UDPRATE:-50Mbps}"
PKTSIZE="${PKTSIZE:-1200}"

PROPMODEL="${PROPMODEL:-logdistance}"          # minimum required: logdistance :contentReference[oaicite:9]{index=9}
LOGEXP="${LOGEXP:-3.0}"
REFDIST="${REFDIST:-1.0}"
REFLOSS="${REFLOSS:-46.6777}"

# The PDF calls it "interval"; your code uses "thrInterval".
THR_INTERVAL="${THR_INTERVAL:-0.5}"

PCAP="${PCAP:-false}"
FLOWMON="${FLOWMON:-true}"

SEED="${SEED:-1}"
RUN_BASE="${RUN_BASE:-1}"

# Parallelism (keep 1 by default to stay simple; tmp outDir allows parallel safely too)
JOBS="${JOBS:-1}"
MAX_JOBS="${MAX_JOBS:-8}"

# ---------------------------- Helpers ----------------------------
ts(){ date +"%Y-%m-%d %H:%M:%S"; }
log(){ echo "[$(ts)] $*"; }
warn(){ echo "[$(ts)] WARN: $*" >&2; }
die(){ echo "[$(ts)] ERROR: $*" >&2; exit 1; }
need_cmd(){ command -v "$1" >/dev/null 2>&1 || die "Missing command: $1"; }

ensure_dirs(){ mkdir -p "$RAW_DIR" "$LOG_DIR" "$PLOT_DIR" "$TMP_ROOT"; }

clean_outputs(){
  log "[P3] Cleaning previous outputs under: $OUTDIR"
  rm -rf "$RAW_DIR" "$LOG_DIR" "$PLOT_DIR" "$TMP_ROOT" || true
  ensure_dirs
}

stage_scenario(){
  [[ -d "$NS3_DIR" ]] || die "NS3_DIR not found: $NS3_DIR"
  [[ -f "$SCEN_SRC" ]] || die "Scenario source not found: $SCEN_SRC"
  mkdir -p "$NS3_DIR/scratch"
  cp -f "$SCEN_SRC" "$NS3_DIR/scratch/${SCEN_NAME}.cc" || die "Copy to ns-3 scratch failed"
  log "[P3] Staged scenario: $SCEN_SRC -> $NS3_DIR/scratch/${SCEN_NAME}.cc"
}

build_ns3(){
  log "[P3] Building ns-3"
  local build_log="$LOG_DIR/build.log"
  ( cd "$NS3_DIR" && ./ns3 build --jobs="$MAX_JOBS" ) >"$build_log" 2>&1 || {
    tail -n 200 "$build_log" || true
    die "ns-3 build failed (see $build_log)"
  }
  log "[P3] Build OK"
}

get_help(){
  local help_log="$LOG_DIR/prog_help.log"
  if ( cd "$NS3_DIR" && ./ns3 run "${SCENARIO} --PrintHelp" --no-build ) >"$help_log" 2>&1; then
    :
  else
    ( cd "$NS3_DIR" && ./ns3 run "${SCENARIO} --PrintHelp" ) >"$help_log" 2>&1 || : >"$help_log"
  fi
  echo "$help_log"
}

arg_supported(){
  local help_file="$1" arg="$2"
  [[ -s "$help_file" ]] || return 1
  grep -q -- "--${arg}:" "$help_file"
}

run_one(){
  local tag="$1" args="$2"
  local log_path="$LOG_DIR/${tag}.log"
  log "[P3] RUN start  $tag"
  ( cd "$NS3_DIR" && ./ns3 run "${SCENARIO} ${args}" ) >"$log_path" 2>&1 || {
    warn "[P3] RUN FAILED: $tag (see $log_path)"
    return 1
  }
  log "[P3] RUN ok     $tag"
  return 0
}

init_master_from_first(){
  local master="$1" pattern="$2" default_header="$3"
  local first
  first="$(find "$TMP_ROOT" -type f -path "$pattern" | sort | head -n 1 || true)"
  if [[ -n "${first:-}" && -s "$first" ]]; then
    head -n 1 "$first" > "$master"
  else
    echo "$default_header" > "$master"
  fi
}

merge_plain(){
  local master="$1" pattern="$2" default_header="$3"
  init_master_from_first "$master" "$pattern" "$default_header"
  local f
  while IFS= read -r f; do
    [[ -s "$f" ]] || continue
    tail -n +2 "$f" >> "$master" || true
  done < <(find "$TMP_ROOT" -type f -path "$pattern" | sort)
  log "[P3] Merge -> $master"
}

# ---------------------------- Main ----------------------------
main(){
  need_cmd find
  need_cmd tail
  need_cmd grep
  need_cmd wc

  (( JOBS > MAX_JOBS )) && JOBS="$MAX_JOBS"
  (( JOBS < 1 )) && JOBS=1

  clean_outputs
  stage_scenario
  build_ns3

  local help_file
  help_file="$(get_help)"
  log "[P3] Help captured: $help_file"

  # Pick correct CLI arg name: interval (PDF) vs thrInterval (your code)
  local interval_arg="thrInterval"
  if arg_supported "$help_file" "interval"; then
    interval_arg="interval"
  elif arg_supported "$help_file" "thrInterval"; then
    interval_arg="thrInterval"
  else
    warn "[P3] Could not detect interval arg in --PrintHelp; defaulting to thrInterval"
    interval_arg="thrInterval"
  fi

  IFS=',' read -r -a D_ARR <<< "$DISTANCES"
  local total="${#D_ARR[@]}"
  (( total > 0 )) || die "[P3] No distances to run."

  local idx=0 running=0 fail=0

  for d in "${D_ARR[@]}"; do
    idx=$((idx+1))
    local runnum=$((RUN_BASE + idx - 1))
    local tag="d${d}m_run${runnum}"
    local out_run="$TMP_ROOT/$tag"
    mkdir -p "$out_run"

    # IMPORTANT: per-run outDir prevents concurrent append races on p3_sweep.csv
    local args="--distance=${d} --transport=${TRANSPORT} --udpRate=${UDPRATE} --pktSize=${PKTSIZE} \
--simTime=${SIMTIME} --appStart=${APPSTART} --${interval_arg}=${THR_INTERVAL} \
--ssid=wifi6-ter --propModel=${PROPMODEL} --logExp=${LOGEXP} --refDist=${REFDIST} --refLoss=${REFLOSS} \
--outDir=${out_run} --pcap=${PCAP} --flowmon=${FLOWMON} \
--seed=${SEED} --run=${runnum}"

    run_one "$tag" "$args" &
    running=$((running+1))

    # bound parallelism
    if (( running >= JOBS )); then
      if wait -n; then running=$((running-1)); else fail=$((fail+1)); running=$((running-1)); fi
    fi
  done

  while (( running > 0 )); do
    if wait -n; then running=$((running-1)); else fail=$((fail+1)); running=$((running-1)); fi
  done

  # Merge per-run sweep CSVs into final results/p3/raw/p3_sweep.csv
  #  Part expects at least columns like distance,udpRate,pktSize,seed,run,... (extra cols ok). :contentReference[oaicite:10]{index=10}
  merge_plain \
    "$RAW_DIR/p3_sweep.csv" \
    "*/raw/p3_sweep.csv" \
    "distance_m,transport,propModel,logExp,refDist,refLoss,simTime,appStart,pktSize,udpRate,tcpMaxBytes,seed,run,rxBytes,goodput_Mbps,rtt_mean_ms,rtt_p95_ms,rtt_samples"

  # Keep all per-run artifacts for plotting/debug
  mkdir -p "$RAW_DIR/per_run_raw"
  if command -v rsync >/dev/null 2>&1; then
    rsync -a --delete "$TMP_ROOT/" "$RAW_DIR/per_run_raw/" >/dev/null 2>&1 || true
  else
    cp -a "$TMP_ROOT/." "$RAW_DIR/per_run_raw/" 2>/dev/null || true
  fi

  rm -rf "$TMP_ROOT" || true

  log "[P3] Completed. Final CSV: $RAW_DIR/p3_sweep.csv"
  (( fail == 0 )) || die "[P3] One or more runs failed (see $LOG_DIR)"
}

main "$@"
