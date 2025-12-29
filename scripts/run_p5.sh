#!/usr/bin/env bash
# scripts/run_p5.sh
# Part 5 runner (parallel, per-run RAW only, safe merge)
#
# Output layout:
#   results/p5/
#     logs/   -> build.log, <tag>.log, plots.log
#     plots/  -> plot_p5 outputs
#     raw/
#       p5_summary.csv
#       persta_all.csv
#       ts_all.csv
#       per_run/<tag>/   (RAW FILES ONLY)

set -euo pipefail
IFS=$'\n\t'

# ---------------------------- Paths ----------------------------
NS3_DIR="${NS3_DIR:-$HOME/ns-3}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

SCEN_NAME="${SCEN_NAME:-p5_multi_sta_fairness}"
SCEN_SRC="${SCEN_SRC:-$ROOT_DIR/scenarios/${SCEN_NAME}.cc}"
SCENARIO="scratch/${SCEN_NAME}"

OUTDIR="${OUTDIR:-$ROOT_DIR/results/p5}"
RAW_DIR="$OUTDIR/raw"
LOG_DIR="$OUTDIR/logs"
PLOT_DIR="$OUTDIR/plots"
PER_RUN_DIR="$RAW_DIR/per_run"

# Clean policy:
# - "all": wipe raw/logs/plots
# - "soft": wipe raw/logs/plots (same behavior; kept for compatibility)
CLEAN_MODE="${CLEAN_MODE:-soft}"

# Python venv (no installs here)
VENV_DIR="${VENV_DIR:-$ROOT_DIR/.venv}"
PY="$VENV_DIR/bin/python"
STRICT_PNG="${STRICT_PNG:-false}"

# ---------------------------- Parallelism ----------------------------
MAX_JOBS="${MAX_JOBS:-8}"
JOBS="${JOBS:-}"   # if empty -> auto: min(MAX_JOBS, max(1, nproc/2))

TAIL_LINES="${TAIL_LINES:-160}"
STOP_ON_FIRST_FAIL="${STOP_ON_FIRST_FAIL:-false}"

# ---------------------------- Experiment parameters ----------------------------
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

TAG_SUFFIX="${TAG_SUFFIX:-}"
EXTRA_ARGS="${EXTRA_ARGS:-}"

# ---------------------------- Helpers ----------------------------
ts(){ date +"%Y-%m-%d %H:%M:%S"; }
log(){ echo "[$(ts)] $*"; }
warn(){ echo "[$(ts)] WARN: $*" >&2; }
die(){ echo "[$(ts)] ERROR: $*" >&2; exit 1; }
need_cmd(){ command -v "$1" >/dev/null 2>&1 || die "Missing command: $1"; }
have_cmd(){ command -v "$1" >/dev/null 2>&1; }

ensure_dirs(){
  mkdir -p "$RAW_DIR" "$LOG_DIR" "$PLOT_DIR" "$PER_RUN_DIR"
}

clean_outputs(){
  log "[P5] Cleaning previous outputs under: $OUTDIR (mode=$CLEAN_MODE)"
  rm -rf "$RAW_DIR" "$LOG_DIR" "$PLOT_DIR" || true
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
  log "[P5] Building ns-3"
  local build_log="$LOG_DIR/build.log"
  ( cd "$NS3_DIR" && ./ns3 build --jobs="$MAX_JOBS" ) >"$build_log" 2>&1 || {
    warn "[P5] BUILD FAILED. Last ${TAIL_LINES} lines:"
    tail -n "$TAIL_LINES" "$build_log" || true
    die "ns-3 build failed (see $build_log)"
  }
  log "[P5] Build OK"
}

safe_tag(){
  local transport="$1" n="$2" run="$3"
  echo "p5_${transport}_n${n}_run${run}${TAG_SUFFIX}"
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
--tcpMaxBytes=${TCP_MAX_BYTES} \
${EXTRA_ARGS}"
}

normalize_per_run_raw_only(){
  # Goal: inside $out_run/ keep ONLY raw files (no raw/, logs/, plots/)
  local out_run="$1"

  if [[ -d "$out_run/raw" ]]; then
    shopt -s dotglob nullglob
    mv "$out_run/raw"/* "$out_run/" 2>/dev/null || true
    shopt -u dotglob nullglob
    rm -rf "$out_run/raw" 2>/dev/null || true
  fi

  rm -rf "$out_run/logs" "$out_run/plots" 2>/dev/null || true
}

run_one(){
  local tag="$1" args="$2" seq="$3" total="$4" out_run="$5"
  local run_log="$LOG_DIR/${tag}.log"

  log "[P5] [${seq}/${total}] RUN start ${tag}"
  ( cd "$NS3_DIR" && ./ns3 run "${SCENARIO} ${args}" ) >"$run_log" 2>&1 || {
    warn "[P5] RUN FAILED: $tag (see $run_log)"
    tail -n "$TAIL_LINES" "$run_log" || true
    return 1
  }

  normalize_per_run_raw_only "$out_run"
  log "[P5] [${seq}/${total}] RUN ok    ${tag}"
  return 0
}

init_master_csv(){
  local master="$1" header="$2"
  mkdir -p "$(dirname "$master")"
  echo "$header" > "$master"
}

merge_plain(){
  local master="$1" header="$2" pattern="$3"
  init_master_csv "$master" "$header"

  local f
  local files=0
  local rows=0

  while IFS= read -r f; do
    [[ -s "$f" ]] || continue
    files=$((files+1))

    local add
    add="$(($(wc -l < "$f") - 1))"
    if (( add > 0 )); then
      tail -n +2 "$f" >> "$master" || true
      rows=$((rows+add))
    fi
  done < <(find "$PER_RUN_DIR" -type f -path "$pattern" | sort)

  log "[P5] Merge -> $master | files=${files} | rows_added=${rows}"
}


merge_persta(){
  local master="$1"
  init_master_csv "$master" "transport,nSta,run,staId,rxBytes,goodputbps"
  local f bn tr nn rr
  while IFS= read -r f; do
    [[ -s "$f" ]] || continue
    bn="$(basename "$f")"
    if [[ "$bn" =~ ^persta_([a-z]+)_n([0-9]+)_run([0-9]+)\.csv$ ]]; then
      tr="${BASH_REMATCH[1]}"; nn="${BASH_REMATCH[2]}"; rr="${BASH_REMATCH[3]}"
      tail -n +2 "$f" | awk -v t="$tr" -v n="$nn" -v r="$rr" -F',' \
        'NF>=3 { printf "%s,%s,%s,%s,%s,%s\n", t,n,r,$1,$2,$3 }' >> "$master"
    fi
  done < <(find "$PER_RUN_DIR" -type f -name "persta_*.csv" | sort)
  log "[P5] Merge -> $master"
}

merge_timeseries(){
  local master="$1"
  init_master_csv "$master" "transport,nSta,run,time_s,throughput_bps,sumRxBytes"
  local f bn tr nn rr
  while IFS= read -r f; do
    [[ -s "$f" ]] || continue
    bn="$(basename "$f")"
    if [[ "$bn" =~ ^ts_([a-z]+)_n([0-9]+)_run([0-9]+)\.csv$ ]]; then
      tr="${BASH_REMATCH[1]}"; nn="${BASH_REMATCH[2]}"; rr="${BASH_REMATCH[3]}"
      tail -n +2 "$f" | awk -v t="$tr" -v n="$nn" -v r="$rr" -F',' \
        'NF>=3 { printf "%s,%s,%s,%s,%s,%s\n", t,n,r,$1,$2,$3 }' >> "$master"
    fi
  done < <(find "$PER_RUN_DIR" -type f -name "ts_*.csv" | sort)
  log "[P5] Merge -> $master"
}

kill_children(){
  jobs -rp >/dev/null 2>&1 || return 0
  local pids
  pids="$(jobs -rp | tr '\n' ' ' || true)"
  [[ -n "${pids// /}" ]] || return 0
  kill $pids 2>/dev/null || true
}
trap 'kill_children' EXIT INT TERM

# ---------------------------- Plot helpers ----------------------------
ensure_venv(){
  if [[ ! -x "$PY" ]]; then
    need_cmd python3
    log "[P5] Creating venv at: $VENV_DIR (no package install performed)"
    python3 -m venv "$VENV_DIR" || die "Failed to create venv: $VENV_DIR"
  fi
}

check_plot_deps_or_die(){
  "$PY" - <<'PY'
import importlib, sys
required = ["numpy","pandas","plotly","kaleido"]
missing=[]
for name in required:
    try:
        importlib.import_module(name)
    except Exception:
        missing.append(name)
if missing:
    print("[P5] ERROR: Missing Python packages in venv: " + ", ".join(missing), file=sys.stderr)
    print("[P5] Install:", file=sys.stderr)
    print("  source .venv/bin/activate", file=sys.stderr)
    print("  python -m pip install -r requirements.txt", file=sys.stderr)
    sys.exit(2)
PY
}

run_plots(){
  [[ -f "$ROOT_DIR/scripts/plot_p5.py" ]] || die "Plot script not found: $ROOT_DIR/scripts/plot_p5.py"
  local plot_log="$LOG_DIR/plots.log"

  log "[P5] [PLOT] start p5_plots"
  if [[ "$STRICT_PNG" == "true" ]]; then
    "$PY" "$ROOT_DIR/scripts/plot_p5.py" --results "$OUTDIR" --strict-png >"$plot_log" 2>&1 || {
      warn "[P5] PLOT FAILED (see $plot_log)"
      tail -n 80 "$plot_log" || true
      return 1
    }
  else
    "$PY" "$ROOT_DIR/scripts/plot_p5.py" --results "$OUTDIR" >"$plot_log" 2>&1 || {
      warn "[P5] PLOT FAILED (see $plot_log)"
      tail -n 80 "$plot_log" || true
      return 1
    }
  fi
  log "[P5] [PLOT] ok    p5_plots"
}

# ---------------------------- Main ----------------------------
main(){
  need_cmd find
  need_cmd tail
  need_cmd awk
  need_cmd wc

  if [[ "$CLEAN_MODE" == "soft" || "$CLEAN_MODE" == "all" ]]; then
    clean_outputs
  else
    ensure_dirs
  fi

  # Default JOBS if not set: min(MAX_JOBS, max(1, nproc/2))
  if [[ -z "${JOBS:-}" ]]; then
    local n=1
    if command -v nproc >/dev/null 2>&1; then n="$(nproc)"; fi
    local half=$(( n / 2 )); (( half < 1 )) && half=1
    JOBS="$half"
  fi
  (( JOBS > MAX_JOBS )) && JOBS="$MAX_JOBS"
  (( JOBS < 1 )) && JOBS=1

  stage_scenario
  build_ns3

  IFS=',' read -r -a T_ARR <<< "$TRANSPORTS"
  IFS=',' read -r -a N_ARR <<< "$NSTAS"

  local total=$(( ${#T_ARR[@]} * ${#N_ARR[@]} ))
  local idx=0
  local running=0
  local fail=0

  for t in "${T_ARR[@]}"; do
    for n in "${N_ARR[@]}"; do
      idx=$((idx+1))
      local runnum=$((RUN_BASE + idx - 1))
      local tag; tag="$(safe_tag "$t" "$n" "$runnum")"
      local out_run="$PER_RUN_DIR/$tag"
      mkdir -p "$out_run"

      local args; args="$(common_args "$out_run" "$runnum" "$t" "$n")"

      ( run_one "$tag" "$args" "$idx" "$total" "$out_run" ) &
      running=$((running+1))

      if (( running >= JOBS )); then
        if wait -n; then
          running=$((running-1))
        else
          fail=$((fail+1))
          running=$((running-1))
        fi
        if [[ "$STOP_ON_FIRST_FAIL" == "true" && "$fail" -gt 0 ]]; then
          die "[P5] Stopping on first failure (see $LOG_DIR)"
        fi
      fi
    done
  done

  while (( running > 0 )); do
    if wait -n; then
      running=$((running-1))
    else
      fail=$((fail+1))
      running=$((running-1))
    fi
    if [[ "$STOP_ON_FIRST_FAIL" == "true" && "$fail" -gt 0 ]]; then
      die "[P5] Stopping on first failure (see $LOG_DIR)"
    fi
  done

  # ----------- Merges (from raw/per_run/*) -----------
  merge_plain \
    "$RAW_DIR/p5_summary.csv" \
    "transport,nSta,run,seed,distance,pktSize,udpRatePerSta,tcpMaxBytes,appStart,simTime,sumGoodputbps,meanGoodputbps,jain" \
    "*/p5_summary.csv"

  merge_persta "$RAW_DIR/persta_all.csv"
  merge_timeseries "$RAW_DIR/ts_all.csv"

  ensure_venv
  check_plot_deps_or_die
  run_plots

  (( fail == 0 )) || die "[P5] One or more runs failed (see $LOG_DIR)"
}

main "$@"
