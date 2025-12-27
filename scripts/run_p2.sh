#!/usr/bin/env bash
# scripts/run_p2.sh
# Part 2 runner (safe per-run outDir + merge)
# Runs UDP and TCP (optionally parallel) without overwriting fixed output filenames.

set -euo pipefail
IFS=$'\n\t'

# ---------------------------- Paths ----------------------------
NS3_DIR="${NS3_DIR:-$HOME/ns-3}"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

SCEN_NAME="${SCEN_NAME:-p2_baseline}"
SCEN_SRC="${SCEN_SRC:-$ROOT_DIR/scenarios/${SCEN_NAME}.cc}"
SCENARIO="scratch/${SCEN_NAME}"

OUTDIR="${OUTDIR:-$ROOT_DIR/results/p2}"
RAW_DIR="$OUTDIR/raw"
LOG_DIR="$OUTDIR/logs"
PLOT_DIR="$OUTDIR/plots"
TMP_ROOT="$OUTDIR/tmp_runs"

# ---------------------------- Parallelism ----------------------------
# Only 2 runs (udp/tcp). Parallelism is optional.
JOBS="${JOBS:-2}"
MAX_JOBS="${MAX_JOBS:-8}"

# ---------------------------- Experiment parameters ----------------------------
TRANSPORTS="${TRANSPORTS:-udp,tcp}"

SIMTIME="${SIMTIME:-20}"
APPSTART="${APPSTART:-2}"
DISTANCE="${DISTANCE:-5}"

PKTSIZE="${PKTSIZE:-1200}"
UDPRATE="${UDPRATE:-50Mbps}"
TCP_MAX_BYTES="${TCP_MAX_BYTES:-0}"

THR_INTERVAL="${THR_INTERVAL:-0.5}"
RTT_HZ="${RTT_HZ:-5.0}"
RTT_VERBOSE="${RTT_VERBOSE:-false}"

PCAP="${PCAP:-true}"
FLOWMON="${FLOWMON:-true}"

SEED="${SEED:-1}"
RUN_BASE="${RUN_BASE:-1}"

# ---------------------------- Helpers ----------------------------
ts(){ date +"%Y-%m-%d %H:%M:%S"; }
log(){ echo "[$(ts)] $*"; }
warn(){ echo "[$(ts)] WARN: $*" >&2; }
die(){ echo "[$(ts)] ERROR: $*" >&2; exit 1; }
need_cmd(){ command -v "$1" >/dev/null 2>&1 || die "Missing command: $1"; }

ensure_dirs(){ mkdir -p "$RAW_DIR" "$LOG_DIR" "$PLOT_DIR" "$TMP_ROOT"; }

clean_outputs(){
  log "[P2] Cleaning previous outputs under: $OUTDIR"
  rm -rf "$RAW_DIR" "$LOG_DIR" "$PLOT_DIR" "$TMP_ROOT" || true
  ensure_dirs
}

stage_scenario(){
  [[ -d "$NS3_DIR" ]] || die "NS3_DIR not found: $NS3_DIR"
  [[ -f "$SCEN_SRC" ]] || die "Scenario source not found: $SCEN_SRC"
  mkdir -p "$NS3_DIR/scratch"
  cp -f "$SCEN_SRC" "$NS3_DIR/scratch/${SCEN_NAME}.cc" || die "Copy to ns-3 scratch failed"
  log "[P2] Staged scenario: $SCEN_SRC -> $NS3_DIR/scratch/${SCEN_NAME}.cc"
}

build_ns3(){
  log "[P2] Building ns-3"
  local build_log="$LOG_DIR/build.log"
  ( cd "$NS3_DIR" && ./ns3 build --jobs="$MAX_JOBS" ) >"$build_log" 2>&1 || {
    tail -n 160 "$build_log" || true
    die "ns-3 build failed (see $build_log)"
  }
  log "[P2] Build OK"
}

safe_tag(){
  local s="$1"
  s="${s// /_}"; s="${s//:/_}"; s="${s//,/_}"
  echo "$s"
}

common_args(){
  local out="$1" run="$2" transport="$3"
  local args=()
  args+=( "--transport=${transport}" )
  args+=( "--pktSize=${PKTSIZE}" )
  args+=( "--distance=${DISTANCE}" )
  args+=( "--simTime=${SIMTIME}" )
  args+=( "--appStart=${APPSTART}" )
  args+=( "--outDir=${out}" )
  args+=( "--pcap=${PCAP}" )
  args+=( "--flowmon=${FLOWMON}" )
  args+=( "--seed=${SEED}" )
  args+=( "--run=${run}" )
  args+=( "--thrInterval=${THR_INTERVAL}" )
  args+=( "--rttHz=${RTT_HZ}" )
  args+=( "--rttVerbose=${RTT_VERBOSE}" )

  if [[ "$transport" == "udp" ]]; then
    args+=( "--udpRate=${UDPRATE}" )
  else
    args+=( "--tcpMaxBytes=${TCP_MAX_BYTES}" )
  fi

  echo "${args[*]}"
}

run_one(){
  local tag="$1" args="$2" seq="$3" total="$4"
  local run_log="$LOG_DIR/${tag}.log"
  log "[P2] [${seq}/${total}] RUN start  ${tag}"
  ( cd "$NS3_DIR" && ./ns3 run "${SCENARIO} ${args}" ) >"$run_log" 2>&1 || {
    warn "[P2] RUN FAILED: ${tag} (see $run_log)"
    tail -n 120 "$run_log" || true
    return 1
  }
  log "[P2] [${seq}/${total}] RUN ok     ${tag}"
  return 0
}

# Merge helpers (same spirit as your P4/P5/P6)
merge_plain(){
  local master="$1" pattern="$2" default_header="$3"
  mkdir -p "$(dirname "$master")"
  echo "$default_header" > "$master"
  local f
  while IFS= read -r f; do
    [[ -s "$f" ]] || continue
    tail -n +2 "$f" >> "$master" || true
  done < <(find "$TMP_ROOT" -type f -path "$pattern" | sort)
  log "[P2] Merge -> $master"
}

merge_timeseries_with_keys(){
  local master="$1" header="$2" file_rel="$3"
  echo "$header" > "$master"
  local d
  for d in "$TMP_ROOT"/*; do
    [[ -d "$d" ]] || continue
    local tag; tag="$(basename "$d")"

    # tag format: p2_<transport>_run<id>
    local transport="unknown" run="0"
    if [[ "$tag" =~ ^p2_([a-z]+)_run([0-9]+)$ ]]; then
      transport="${BASH_REMATCH[1]}"
      run="${BASH_REMATCH[2]}"
    fi

    local f="$d/raw/$file_rel"
    [[ -s "$f" ]] || continue
    tail -n +2 "$f" | awk -v t="$transport" -v r="$run" -F',' 'NF>=2 {print t","r","$0}' >> "$master" || true
  done
  log "[P2] Merge -> $master"
}

copy_per_run(){
  local dest_sub="$1" pattern="$2"
  local dest="$RAW_DIR/$dest_sub"
  mkdir -p "$dest"
  find "$TMP_ROOT" -type f -path "$pattern" -exec cp -f {} "$dest/" \; 2>/dev/null || true
}

# ---------------------------- Main ----------------------------
main(){
  need_cmd find
  need_cmd tail
  need_cmd awk

  (( JOBS > MAX_JOBS )) && JOBS="$MAX_JOBS"
  (( JOBS < 1 )) && JOBS=1

  ensure_dirs
  clean_outputs
  stage_scenario
  build_ns3

  IFS=',' read -r -a T_ARR <<< "$TRANSPORTS"
  local total="${#T_ARR[@]}"
  local idx=0
  local running=0
  local fail=0

  for t in "${T_ARR[@]}"; do
    idx=$((idx+1))
    local runnum=$((RUN_BASE + idx - 1))
    local tag="p2_${t}_run${runnum}"
    local out_run="$TMP_ROOT/$tag"
    mkdir -p "$out_run/raw" "$out_run/logs" "$out_run/plots"

    local args; args="$(common_args "$out_run" "$runnum" "$t")"
    run_one "$(safe_tag "$tag")" "$args" "$idx" "$total" &
    running=$((running+1))

    if (( running >= JOBS )); then
      if wait -n; then running=$((running-1)); else fail=$((fail+1)); running=$((running-1)); fi
    fi
  done

  while (( running > 0 )); do
    if wait -n; then running=$((running-1)); else fail=$((fail+1)); running=$((running-1)); fi
  done

  # --- Consolidate outputs (no overwrite) ---
  # Summary: concatenate all p2_summary.csv lines
  merge_plain \
    "$RAW_DIR/p2_summary_all.csv" \
    "*/raw/p2_summary.csv" \
    "transport,simTime,appStart,distance,pktSize,udpRate,tcpMaxBytes,seed,run,rxBytes,goodputbps"

  # Timeseries (add transport,run columns)
  merge_timeseries_with_keys \
    "$RAW_DIR/throughput_timeseries_all.csv" \
    "transport,run,time_s,throughput_bps" \
    "throughput_timeseries.csv"

  merge_timeseries_with_keys \
    "$RAW_DIR/rtt_timeseries_all.csv" \
    "transport,run,time_s,seq,rtt_ms" \
    "rtt_timeseries.csv"

  # Keep per-transport copies (closer to PDF expected names, but without clobbering)
  for d in "$TMP_ROOT"/p2_udp_run*; do
    [[ -d "$d" ]] || continue
    [[ -f "$d/raw/throughput_timeseries.csv" ]] && cp -f "$d/raw/throughput_timeseries.csv" "$RAW_DIR/throughput_timeseries_udp.csv" || true
    [[ -f "$d/raw/rtt_timeseries.csv" ]] && cp -f "$d/raw/rtt_timeseries.csv" "$RAW_DIR/rtt_timeseries_udp.csv" || true
    [[ -f "$d/raw/flowmon.xml" ]] && cp -f "$d/raw/flowmon.xml" "$RAW_DIR/flowmon_udp.xml" || true
  done
  for d in "$TMP_ROOT"/p2_tcp_run*; do
    [[ -d "$d" ]] || continue
    [[ -f "$d/raw/throughput_timeseries.csv" ]] && cp -f "$d/raw/throughput_timeseries.csv" "$RAW_DIR/throughput_timeseries_tcp.csv" || true
    [[ -f "$d/raw/rtt_timeseries.csv" ]] && cp -f "$d/raw/rtt_timeseries.csv" "$RAW_DIR/rtt_timeseries_tcp.csv" || true
    [[ -f "$d/raw/flowmon.xml" ]] && cp -f "$d/raw/flowmon.xml" "$RAW_DIR/flowmon_tcp.xml" || true
  done

  # Copy all pcaps / flowmons etc per-run for debugging
  copy_per_run "per_run_raw" "*/raw/*"
  copy_per_run "per_run_logs" "*/logs/*"   # (in case your C++ writes logs later)

  rm -rf "$TMP_ROOT" || true

  log "[P2] Completed."
  log "[P2] Key outputs:"
  log "     - $RAW_DIR/p2_summary_all.csv"
  log "     - $RAW_DIR/throughput_timeseries_all.csv"
  log "     - $RAW_DIR/rtt_timeseries_all.csv"
  log "     - $RAW_DIR/throughput_timeseries_udp.csv / _tcp.csv"
  log "     - $RAW_DIR/rtt_timeseries_udp.csv / _tcp.csv"
  log "     - $RAW_DIR/flowmon_udp.xml / _tcp.xml (if enabled)"

  (( fail == 0 )) || die "[P2] One or more runs failed (see $LOG_DIR)"
}

main "$@"
