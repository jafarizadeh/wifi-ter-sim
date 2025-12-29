#!/usr/bin/env bash
# scripts/run_p3.sh
# Part 3 runner (tmp outDir per distance, safe merge, logs per run)
# + Plot generation via scripts/plot_p3.py using local venv (no installs in shell)

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

# Store per-run artifacts UNDER raw (no artifacts/ directory)
PER_RUN_DIR="$RAW_DIR/per_run"

# Python venv (no installs here)
VENV_DIR="${VENV_DIR:-$ROOT_DIR/.venv}"
PY="$VENV_DIR/bin/python"

# ---------------------------- Experiment defaults ----------------------------
DISTANCES="${DISTANCES:-1,5,10,15,20,25,30}"

SIMTIME="${SIMTIME:-20}"
APPSTART="${APPSTART:-2}"

TRANSPORT="${TRANSPORT:-udp}"
UDPRATE="${UDPRATE:-50Mbps}"
PKTSIZE="${PKTSIZE:-1200}"

PROPMODEL="${PROPMODEL:-logdistance}"
LOGEXP="${LOGEXP:-3.0}"
REFDIST="${REFDIST:-1.0}"
REFLOSS="${REFLOSS:-46.6777}"

THR_INTERVAL="${THR_INTERVAL:-0.5}"

PCAP="${PCAP:-false}"
FLOWMON="${FLOWMON:-true}"

SEED="${SEED:-1}"
RUN_BASE="${RUN_BASE:-1}"

# Parallelism
MAX_JOBS="${MAX_JOBS:-8}"
JOBS="${JOBS:-}"

# ---------------------------- Helpers ----------------------------
ts(){ date +"%Y-%m-%d %H:%M:%S"; }
log(){ echo "[$(ts)] $*"; }
warn(){ echo "[$(ts)] WARN: $*" >&2; }
die(){ echo "[$(ts)] ERROR: $*" >&2; exit 1; }
need_cmd(){ command -v "$1" >/dev/null 2>&1 || die "Missing command: $1"; }

ensure_dirs(){ mkdir -p "$RAW_DIR" "$LOG_DIR" "$PLOT_DIR" "$TMP_ROOT" "$PER_RUN_DIR"; }

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
  local tag="$1" args="$2" seq="$3" total="$4"
  local log_path="$LOG_DIR/${tag}.log"
  log "[P3] [${seq}/${total}] RUN start $tag"
  ( cd "$NS3_DIR" && ./ns3 run "${SCENARIO} ${args}" ) >"$log_path" 2>&1 || {
    warn "[P3] RUN FAILED: $tag (see $log_path)"
    return 1
  }
  log "[P3] [${seq}/${total}] RUN ok    $tag"
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

# ---------------------------- Plot helpers ----------------------------
ensure_venv(){
  if [[ ! -x "$PY" ]]; then
    need_cmd python3
    log "[P3] Creating venv at: $VENV_DIR (no package install performed)"
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
    print("[P3] ERROR: Missing Python packages in venv: " + ", ".join(missing), file=sys.stderr)
    print("[P3] Install:", file=sys.stderr)
    print("  source .venv/bin/activate", file=sys.stderr)
    print("  python -m pip install -r requirements.txt", file=sys.stderr)
    sys.exit(2)
PY
}

run_plots(){
  [[ -f "$ROOT_DIR/scripts/plot_p3.py" ]] || die "Plot script not found: $ROOT_DIR/scripts/plot_p3.py"
  local plot_log="$LOG_DIR/plots.log"

  log "[P3] [1/1] PLOT start p3_plots"
  if "$PY" "$ROOT_DIR/scripts/plot_p3.py" --results "$OUTDIR" --strict-png >"$plot_log" 2>&1; then
    log "[P3] [1/1] PLOT ok    p3_plots"
  else
    warn "[P3] PLOT FAILED: p3_plots (see $plot_log)"
    tail -n 80 "$plot_log" || true
    return 1
  fi
}

# ---------------------------- Main ----------------------------
main(){
  need_cmd find
  need_cmd tail
  need_cmd grep

  # Default JOBS if user didn't set it
  if [[ -z "${JOBS:-}" ]]; then
    local n=1
    if command -v nproc >/dev/null 2>&1; then
      n="$(nproc)"
    fi
    local half=$(( n / 2 ))
    (( half < 1 )) && half=1
    JOBS="$half"
  fi

  # Clamp
  (( JOBS > MAX_JOBS )) && JOBS="$MAX_JOBS"
  (( JOBS < 1 )) && JOBS=1

  clean_outputs
  stage_scenario
  build_ns3

  local help_file
  help_file="$(get_help)"

  # Pick correct CLI arg name: interval vs thrInterval
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

    local args="--distance=${d} --transport=${TRANSPORT} --udpRate=${UDPRATE} --pktSize=${PKTSIZE} \
--simTime=${SIMTIME} --appStart=${APPSTART} --${interval_arg}=${THR_INTERVAL} \
--ssid=wifi6-ter --propModel=${PROPMODEL} --logExp=${LOGEXP} --refDist=${REFDIST} --refLoss=${REFLOSS} \
--outDir=${out_run} --pcap=${PCAP} --flowmon=${FLOWMON} \
--seed=${SEED} --run=${runnum}"

    run_one "$tag" "$args" "$idx" "$total" &
    running=$((running+1))

    if (( running >= JOBS )); then
      if wait -n; then running=$((running-1)); else fail=$((fail+1)); running=$((running-1)); fi
    fi
  done

  while (( running > 0 )); do
    if wait -n; then running=$((running-1)); else fail=$((fail+1)); running=$((running-1)); fi
  done

  # Merge per-run sweep CSVs into final results/p3/raw/p3_sweep.csv
  merge_plain \
    "$RAW_DIR/p3_sweep.csv" \
    "*/raw/p3_sweep.csv" \
    "distance_m,transport,propModel,logExp,refDist,refLoss,simTime,appStart,pktSize,udpRate,tcpMaxBytes,seed,run,rxBytes,goodput_Mbps,rtt_mean_ms,rtt_p95_ms,rtt_samples"

  # Keep all per-run artifacts under raw/per_run (not artifacts/)
 # Keep all per-run artifacts under raw/per_run/<tag>/ (flatten: copy files from TMP_ROOT/<tag>/raw/)
rm -rf "$PER_RUN_DIR" || true
mkdir -p "$PER_RUN_DIR"

for d in "$TMP_ROOT"/*; do
  [[ -d "$d" ]] || continue
  tag="$(basename "$d")"
  mkdir -p "$PER_RUN_DIR/$tag"

  # Prefer copying only the real outputs (usually under <tag>/raw/)
  if [[ -d "$d/raw" ]]; then
    if command -v rsync >/dev/null 2>&1; then
      rsync -a "$d/raw/" "$PER_RUN_DIR/$tag/" >/dev/null 2>&1 || true
    else
      cp -a "$d/raw/." "$PER_RUN_DIR/$tag/" 2>/dev/null || true
    fi
  else
    # fallback: copy anything we find (shouldn't happen normally)
    if command -v rsync >/dev/null 2>&1; then
      rsync -a "$d/" "$PER_RUN_DIR/$tag/" >/dev/null 2>&1 || true
    else
      cp -a "$d/." "$PER_RUN_DIR/$tag/" 2>/dev/null || true
    fi
  fi
done


  rm -rf "$TMP_ROOT" || true

  ensure_venv
  check_plot_deps_or_die
  run_plots

  (( fail == 0 )) || die "[P3] One or more runs failed (see $LOG_DIR)"
}

main "$@"
