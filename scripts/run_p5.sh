#!/usr/bin/env bash
# scripts/run_p5.sh
#  Part 5 runner (parallel, tmp outDir per run, safe merging)

set -euo pipefail
IFS=$'\n\t'

NS3_DIR="${NS3_DIR:-$HOME/ns-3}"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

SCEN_NAME="${SCEN_NAME:-p5_multi_sta_fairness}"
SCEN_SRC="${SCEN_SRC:-$ROOT_DIR/scenarios/${SCEN_NAME}.cc}"
SCENARIO="scratch/${SCEN_NAME}"

OUTDIR="${OUTDIR:-$ROOT_DIR/results/p5}"
RAW_DIR="$OUTDIR/raw"
LOG_DIR="$OUTDIR/logs"
PLOT_DIR="$OUTDIR/plots"
TMP_ROOT="$OUTDIR/tmp_runs"

JOBS="${JOBS:-6}"
MAX_JOBS="${MAX_JOBS:-8}"

TRANSPORTS="${TRANSPORTS:-udp,tcp}"
NSTAS="${NSTAS:-2,5,10}"

RADIUS="${RADIUS:-10}"
DISTANCE="${DISTANCE:-10}"

SIMTIME="${SIMTIME:-20}"
APPSTART="${APPSTART:-3}"
PKTSIZE="${PKTSIZE:-1200}"

UDP_RATE_PER_STA="${UDP_RATE_PER_STA:-6Mbps}"
TCP_RATE_PER_STA="${TCP_RATE_PER_STA:-6Mbps}"
TCP_MAX_BYTES="${TCP_MAX_BYTES:-0}"

TS_INTERVAL="${TS_INTERVAL:-0.1}"

TXPWR_DBM="${TXPWR_DBM:-20}"
NOISEFIG_DB="${NOISEFIG_DB:-7}"
LOGEXP="${LOGEXP:-3.0}"
REFDIST_M="${REFDIST_M:-1.0}"
REFLOSS_DB="${REFLOSS_DB:-46.6777}"

RATE_MANAGER="${RATE_MANAGER:-ns3::MinstrelHtWifiManager}"

PCAP="${PCAP:-false}"
FLOWMON="${FLOWMON:-true}"

SEED="${SEED:-1}"
RUN_BASE="${RUN_BASE:-1}"

TAIL_LINES="${TAIL_LINES:-160}"
STOP_ON_FIRST_FAIL="${STOP_ON_FIRST_FAIL:-false}"

ts(){ date +"%Y-%m-%d %H:%M:%S"; }
log(){ echo "[$(ts)] $*"; }
warn(){ echo "[$(ts)] WARN: $*" >&2; }
die(){ echo "[$(ts)] ERROR: $*" >&2; exit 1; }
need_cmd(){ command -v "$1" >/dev/null 2>&1 || die "Missing command: $1"; }

ensure_dirs(){
  mkdir -p "$RAW_DIR" "$LOG_DIR" "$PLOT_DIR" "$TMP_ROOT"
}

clean_outputs(){
  log "[P5] Cleaning previous outputs under: $OUTDIR"
  rm -rf "$RAW_DIR" "$LOG_DIR" "$PLOT_DIR" "$TMP_ROOT" || true
  ensure_dirs
}

stage_scenario(){
  [[ -d "$NS3_DIR" ]] || die "NS3_DIR not found: $NS3_DIR"
  [[ -f "$SCEN_SRC" ]] || die "Scenario source not found: $SCEN_SRC"

  mkdir -p "$NS3_DIR/scratch"
  local dst="$NS3_DIR/scratch/${SCEN_NAME}.cc"
  cp -f "$SCEN_SRC" "$dst" || die "Copy to ns-3 scratch failed: $dst"
  log "[P5] Staged scenario: $SCEN_SRC -> $dst"
}

build_ns3(){
  log "[P5] Building ns-3 in: $NS3_DIR"
  local build_log="$LOG_DIR/build.log"
  ( cd "$NS3_DIR" && ./ns3 build --jobs="$MAX_JOBS" ) >"$build_log" 2>&1 || {
    echo
    warn "[P5] BUILD FAILED. Last ${TAIL_LINES} lines:"
    tail -n "$TAIL_LINES" "$build_log" || true
    die "ns-3 build failed (full log: $build_log)"
  }
  log "[P5] Build OK"
}

safe_tag(){
  local s="$1"
  s="${s// /_}"; s="${s//:/_}"; s="${s//,/_}"
  echo "$s"
}

common_args(){
  local out="$1" run="$2" transport="$3" n="$4"
  echo "\
--simTime=${SIMTIME} --appStart=${APPSTART} \
--nSta=${n} --distance=${DISTANCE} --radius=${RADIUS} \
--ssid=wifi6-ter --outDir=${out} \
--pcap=${PCAP} --flowmon=${FLOWMON} \
--seed=${SEED} --run=${run} \
--transport=${transport} \
--udpRatePerSta=${UDP_RATE_PER_STA} --tcpRatePerSta=${TCP_RATE_PER_STA} --pktSize=${PKTSIZE} \
--interval=${TS_INTERVAL} \
--txPowerDbm=${TXPWR_DBM} --noiseFigureDb=${NOISEFIG_DB} \
--logExp=${LOGEXP} --refDist=${REFDIST_M} --refLoss=${REFLOSS_DB} \
--rateManager=${RATE_MANAGER} \
--tcpMaxBytes=${TCP_MAX_BYTES}"
}

wait_for_slot(){
  while (( $(jobs -rp | wc -l) >= JOBS )); do
    wait -n || true
  done
}

run_one(){
  local tag="$1" args="$2" seq="$3" total="$4"
  local run_log="$LOG_DIR/${tag}.log"

  log "[P5] [${seq}/${total}] RUN start  ${tag}"
  ( cd "$NS3_DIR" && ./ns3 run "${SCENARIO} ${args}" ) >"$run_log" 2>&1 || {
    warn "[P5] RUN FAILED: $tag"
    warn "[P5] Log file: $run_log"
    tail -n "$TAIL_LINES" "$run_log" || true
    return 1
  }
  log "[P5] [${seq}/${total}] RUN ok     ${tag}"
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
  log "[P5] Merge -> $master"
}

merge_persta(){
  local master="$1"
  echo "transport,nSta,run,staId,rxBytes,goodputbps" > "$master"
  local f
  while IFS= read -r f; do
    [[ -s "$f" ]] || continue
    local bn; bn="$(basename "$f")"
    if [[ "$bn" =~ ^persta_([a-z]+)_n([0-9]+)_run([0-9]+)\.csv$ ]]; then
      local tr="${BASH_REMATCH[1]}" nn="${BASH_REMATCH[2]}" rr="${BASH_REMATCH[3]}"
      tail -n +2 "$f" | awk -v t="$tr" -v n="$nn" -v r="$rr" -F',' \
        'NF>=3 { printf "%s,%s,%s,%s,%s,%s\n", t,n,r,$1,$2,$3 }' >> "$master"
    fi
  done < <(find "$TMP_ROOT" -type f -name "persta_*.csv" | sort)
  log "[P5] Merge -> $master"
}

merge_timeseries(){
  local master="$1"
  echo "transport,nSta,run,time_s,throughput_bps,sumRxBytes" > "$master"
  local f
  while IFS= read -r f; do
    [[ -s "$f" ]] || continue
    local bn; bn="$(basename "$f")"
    if [[ "$bn" =~ ^ts_([a-z]+)_n([0-9]+)_run([0-9]+)\.csv$ ]]; then
      local tr="${BASH_REMATCH[1]}" nn="${BASH_REMATCH[2]}" rr="${BASH_REMATCH[3]}"
      tail -n +2 "$f" | awk -v t="$tr" -v n="$nn" -v r="$rr" -F',' \
        'NF>=3 { printf "%s,%s,%s,%s,%s,%s\n", t,n,r,$1,$2,$3 }' >> "$master"
    fi
  done < <(find "$TMP_ROOT" -type f -name "ts_*.csv" | sort)
  log "[P5] Merge -> $master"
}

copy_family(){
  local sub="$1" pattern="$2"
  local dest="$RAW_DIR/$sub"
  mkdir -p "$dest"
  find "$TMP_ROOT" -type f -path "$pattern" -exec cp -f {} "$dest/" \; 2>/dev/null || true
}

report_outputs(){
  echo
  echo "====================== [P5] Summary ======================"
  echo "NS3_DIR : $NS3_DIR"
  echo "Scenario: $SCENARIO"
  echo "Source  : $SCEN_SRC"
  echo "OutDir  : $OUTDIR"
  echo "Raw     : $RAW_DIR"
  echo "Logs    : $LOG_DIR"
  echo "Plots   : $PLOT_DIR"
  echo "----------------------------------------------------------"
  echo "Key outputs:"
  echo "  - $RAW_DIR/p5_summary.csv"
  echo "  - $RAW_DIR/persta_all.csv"
  echo "  - $RAW_DIR/ts_all.csv"
  echo "=========================================================="
  echo
}

main(){
  need_cmd find
  need_cmd tail
  need_cmd awk
  need_cmd wc

  if (( JOBS > MAX_JOBS )); then JOBS="$MAX_JOBS"; fi
  if (( JOBS < 1 )); then JOBS=1; fi

  ensure_dirs
  clean_outputs
  stage_scenario
  build_ns3

  IFS=',' read -r -a T_ARR <<< "$TRANSPORTS"
  IFS=',' read -r -a N_ARR <<< "$NSTAS"

  local total=$(( ${#T_ARR[@]} * ${#N_ARR[@]} ))
  local idx=0
  local fail=0

  log "[P5] Parallel runs: JOBS=$JOBS"
  log "[P5] Grid: TRANSPORTS=$TRANSPORTS | NSTAS=$NSTAS"

  for t in "${T_ARR[@]}"; do
    for n in "${N_ARR[@]}"; do
      idx=$((idx+1))
      local runnum=$((RUN_BASE + idx - 1))
      local tag="p5_${t}_n${n}_run${runnum}"
      local out_run="$TMP_ROOT/$tag"

      mkdir -p "$out_run/raw" "$out_run/logs" "$out_run/plots"

      local args; args="$(common_args "$out_run" "$runnum" "$t" "$n")"
      run_one "$(safe_tag "$tag")" "$args" "$idx" "$total" &

      wait_for_slot
    done
  done

  if ! wait; then
    fail=$((fail + 1))
  fi

  merge_plain \
    "$RAW_DIR/p5_summary.csv" \
    "*/raw/p5_summary.csv" \
    "transport,nSta,run,seed,distance,pktSize,udpRatePerSta,tcpMaxBytes,appStart,simTime,sumGoodputbps,meanGoodputbps,jain"

  merge_persta "$RAW_DIR/persta_all.csv"
  merge_timeseries "$RAW_DIR/ts_all.csv"

  copy_family "flowmon" "*/raw/flowmon_*.xml"
  copy_family "per_run_raw" "*/raw/*"
  copy_family "per_run_logs" "*/logs/*"

  rm -rf "$TMP_ROOT" || true

  report_outputs

  if [[ "$STOP_ON_FIRST_FAIL" == "true" && "$fail" -ne 0 ]]; then
    die "[P5] Completed with failures (see $LOG_DIR)"
  fi

  if [[ "$fail" -ne 0 ]]; then
    die "[P5] Completed with failures (see $LOG_DIR)"
  fi

  log "[P5] All runs completed successfully."
}

main "$@"
