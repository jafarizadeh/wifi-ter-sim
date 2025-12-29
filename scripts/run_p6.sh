#!/usr/bin/env bash
# scripts/run_p6.sh
# Project 6 runner (parallel, tmp outDir per run, safe merging)
# Scenario: p6_roaming_multi_ap (Multi-AP roaming under traffic)
# Workflow:
#  1) Clean outputs
#  2) Stage scenario into ns-3 scratch/
#  3) Build ns-3
#  4) Execute runs in parallel (bounded by JOBS), each with a private outDir: results/p6/tmp_runs/<tag>
#  5) Merge per-run CSV/TXT into final consolidated files under results/p6/raw/
#  6) Copy per-run trees under results/p6/raw/per_run/
#  7) Remove tmp_runs
#  8) Run Python plot script via venv (like Project 2) and print plot summary logs

set -euo pipefail
IFS=$'\n\t'

# ---------------------------- Paths ----------------------------
NS3_DIR="${NS3_DIR:-$HOME/ns-3}"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

SCEN_SRC="${SCEN_SRC:-$ROOT_DIR/scenarios/p6_roaming_multi_ap.cc}"
SCEN_NAME="${SCEN_NAME:-p6_roaming_multi_ap}"
SCENARIO="scratch/${SCEN_NAME}"

OUTDIR="${OUTDIR:-$ROOT_DIR/results/p6}"
RAW_DIR="$OUTDIR/raw"
LOG_DIR="$OUTDIR/logs"
PLOT_DIR="$OUTDIR/plots"
TMP_ROOT="$OUTDIR/tmp_runs"

# Python venv (no installs here) + plot script
VENV_DIR="${VENV_DIR:-$ROOT_DIR/.venv}"
PY="$VENV_DIR/bin/python"
PLOT_SCRIPT="${PLOT_SCRIPT:-$ROOT_DIR/scripts/plot_p6.py}"

# ---------------------------- Parallelism ----------------------------
JOBS="${JOBS:-6}"
MAX_JOBS="${MAX_JOBS:-8}"

# ---------------------------- Experiment matrix ----------------------------
SPEEDS="${SPEEDS:-0.5,1.0,2.0}"
AP_DISTANCE="${AP_DISTANCE:-30}"
MOVE_START="${MOVE_START:-5}"
SIMTIME="${SIMTIME:-30}"
APPSTART="${APPSTART:-2}"

# Traffic
PKTSIZE="${PKTSIZE:-1200}"
UDPRATE="${UDPRATE:-20Mbps}"
THR_INTERVAL="${THR_INTERVAL:-0.5}"

# PHY/channel
TX_STA_DBM="${TX_STA_DBM:-16}"
TX_AP1_DBM="${TX_AP1_DBM:-20}"
TX_AP2_DBM="${TX_AP2_DBM:-16}"
NOISEFIG_DB="${NOISEFIG_DB:-7}"
LOGEXP="${LOGEXP:-3.0}"
SHADOW_SIGMA_DB="${SHADOW_SIGMA_DB:-4.0}"
ENABLE_FADING="${ENABLE_FADING:-true}"

# MAC/roaming
WIFI_STD="${WIFI_STD:-ax}"         # ax|ac|n
USE_MINSTREL="${USE_MINSTREL:-true}"
ACTIVE_PROBING="${ACTIVE_PROBING:-false}"
BEST_ROAM="${BEST_ROAM:-true}"
ROAM_CHECK="${ROAM_CHECK:-0.2}"
ROAM_HYST_DB="${ROAM_HYST_DB:-4.0}"
ROAM_DWELL="${ROAM_DWELL:-1.0}"
ROAM_MINGAP="${ROAM_MINGAP:-2.0}"

# Probes
ENABLE_RTT_PROBE="${ENABLE_RTT_PROBE:-true}"
RTT_HZ="${RTT_HZ:-5.0}"
RTT_PORT="${RTT_PORT:-9000}"
ROAM_POLL="${ROAM_POLL:-0.2}"
POS_POLL="${POS_POLL:-0.2}"
LINK_ID="${LINK_ID:-0}"

# Reproducibility / options
SEED="${SEED:-1}"
RUN_BASE="${RUN_BASE:-1}"
PCAP="${PCAP:-false}"
FLOWMON="${FLOWMON:-true}"

# ---------------------------- Helpers ----------------------------
ts(){ date +"%Y-%m-%d %H:%M:%S"; }
log(){ echo "[$(ts)] $*"; }
warn(){ echo "[$(ts)] WARN: $*" >&2; }
die(){ echo "[$(ts)] ERROR: $*" >&2; exit 1; }

need_cmd(){ command -v "$1" >/dev/null 2>&1 || die "Missing command: $1"; }
ensure_dirs(){ mkdir -p "$RAW_DIR" "$LOG_DIR" "$PLOT_DIR" "$TMP_ROOT"; }

step_log(){
  # step_log <idx> <total> <PHASE> <status> <name>
  # status: start|ok|FAILED
  local i="$1" n="$2" phase="$3" status="$4" name="$5"
  log "[P6] [$i/$n] ${phase} ${status} ${name}"
}

clean_outputs(){
  log "[P6] Cleaning previous outputs under: $OUTDIR"
  rm -rf "$RAW_DIR" "$LOG_DIR" "$PLOT_DIR" "$TMP_ROOT" || true
  ensure_dirs
}

stage_scenario(){
  [[ -d "$NS3_DIR" ]] || die "NS3_DIR not found: $NS3_DIR"
  [[ -f "$SCEN_SRC" ]] || die "Scenario source not found: $SCEN_SRC"
  local dst="$NS3_DIR/scratch/${SCEN_NAME}.cc"
  mkdir -p "$NS3_DIR/scratch"
  cp -f "$SCEN_SRC" "$dst" || die "Copy to ns-3 scratch failed: $dst"
  log "[P6] Staged scenario: $SCEN_SRC -> $dst"
}

build_ns3(){
  log "[P6] Building ns-3"
  local build_log="$LOG_DIR/build.log"
  ( cd "$NS3_DIR" && ./ns3 build --jobs="$MAX_JOBS" ) >"$build_log" 2>&1 || {
    tail -n 200 "$build_log" || true
    die "ns-3 build failed (see $build_log)"
  }
  log "[P6] Build OK"
}

safe_tag(){
  local s="$1"
  s="${s// /_}"; s="${s//:/_}"; s="${s//,/_}"
  echo "$s"
}

common_args(){
  local out="$1" run="$2" speed="$3"
  echo "\
--simTime=${SIMTIME} --appStart=${APPSTART} --moveStart=${MOVE_START} \
--apDistance=${AP_DISTANCE} --staSpeed=${speed} \
--ssid=wifi6-ter --outDir=${out} \
--pcap=${PCAP} --flowmon=${FLOWMON} \
--seed=${SEED} --run=${run} \
--pktSize=${PKTSIZE} --udpRate=${UDPRATE} --interval=${THR_INTERVAL} \
--txPowerStaDbm=${TX_STA_DBM} --txPowerAp1Dbm=${TX_AP1_DBM} --txPowerAp2Dbm=${TX_AP2_DBM} \
--noiseFigureDb=${NOISEFIG_DB} \
--logExp=${LOGEXP} --shadowingSigmaDb=${SHADOW_SIGMA_DB} --enableFading=${ENABLE_FADING} \
--useMinstrel=${USE_MINSTREL} --wifiStd=${WIFI_STD} \
--roamPoll=${ROAM_POLL} --linkId=${LINK_ID} --posPoll=${POS_POLL} \
--enableRttProbe=${ENABLE_RTT_PROBE} --rttHz=${RTT_HZ} --rttPort=${RTT_PORT} \
--activeProbing=${ACTIVE_PROBING} --bestRoam=${BEST_ROAM} \
--roamCheck=${ROAM_CHECK} --roamHystDb=${ROAM_HYST_DB} --roamDwell=${ROAM_DWELL} --roamMinGap=${ROAM_MINGAP}"
}

run_one(){
  local tag="$1" args="$2" seq="$3" total="$4"
  local log_path="$LOG_DIR/${tag}.log"
  step_log "$seq" "$total" "RUN" "start" "$tag"
  if ( cd "$NS3_DIR" && ./ns3 run "${SCENARIO} ${args}" ) >"$log_path" 2>&1; then
    step_log "$seq" "$total" "RUN" "ok" "$tag"
    return 0
  else
    warn "[P6] RUN FAILED: ${tag} (see $log_path)"
    tail -n 120 "$log_path" || true
    step_log "$seq" "$total" "RUN" "FAILED" "$tag"
    return 1
  fi
}

# Pick header from first matching non-empty file, else use default.
init_master_from_first(){
  local master="$1" pattern="$2" default_header="$3"
  local first
  first="$(find "$TMP_ROOT" -type f -path "$pattern" -size +0c 2>/dev/null | sort | head -n 1 || true)"
  if [[ -n "${first:-}" ]]; then
    head -n 1 "$first" > "$master"
  else
    echo "$default_header" > "$master"
  fi
}

merge_csv(){
  local master="$1" pattern="$2" default_header="$3"
  init_master_from_first "$master" "$pattern" "$default_header"
  local f
  while IFS= read -r f; do
    [[ -s "$f" ]] || continue
    tail -n +2 "$f" >> "$master" || true
  done < <(find "$TMP_ROOT" -type f -path "$pattern" 2>/dev/null | sort)
}

merge_csv_prefix_run(){
  local master="$1" pattern="$2" header="$3" rx='run([0-9]+)'
  echo "$header" > "$master"
  local f
  while IFS= read -r f; do
    [[ -s "$f" ]] || continue
    local bn run
    bn="$(basename "$f")"
    if [[ "$bn" =~ $rx ]]; then run="${BASH_REMATCH[1]}"; else run="0"; fi
    awk -v r="$run" 'NR==1{next} {print r "," $0}' "$f" >> "$master" || true
  done < <(find "$TMP_ROOT" -type f -path "$pattern" 2>/dev/null | sort)
}

merge_txt_prefix_run(){
  local master="$1" pattern="$2" header="$3" rx='run([0-9]+)'
  echo "$header" > "$master"
  local f
  while IFS= read -r f; do
    [[ -s "$f" ]] || continue
    local bn run
    bn="$(basename "$f")"
    if [[ "$bn" =~ $rx ]]; then run="${BASH_REMATCH[1]}"; else run="0"; fi
    awk -v r="$run" 'NR==1{next} {print r "," $0}' "$f" >> "$master" || true
  done < <(find "$TMP_ROOT" -type f -path "$pattern" 2>/dev/null | sort)
}

copy_per_run_tree(){
  local dest_root="$RAW_DIR/per_run"
  mkdir -p "$dest_root"
  local d
  for d in "$TMP_ROOT"/*; do
    [[ -d "$d" ]] || continue
    local tag; tag="$(basename "$d")"
    mkdir -p "$dest_root/$tag"
    [[ -d "$d/raw"   ]] && cp -a "$d/raw"   "$dest_root/$tag/" || true
    [[ -d "$d/logs"  ]] && cp -a "$d/logs"  "$dest_root/$tag/" || true
    [[ -d "$d/plots" ]] && cp -a "$d/plots" "$dest_root/$tag/" || true
  done
}

do_merges(){
  local steps=(
    "plain|$RAW_DIR/p6_summary.csv|*/raw/p6_summary.csv|apDistance,staSpeed,moveStart,udpRate,pktSize,seed,run,rxBytes,goodputbps,roamTime"
    "csvrun|$RAW_DIR/throughput_timeseries_all.csv|*/raw/throughput_timeseries_run*.csv|run,time_s,throughput_bps"
    "txtrun|$RAW_DIR/roaming_events_all.csv|*/raw/roaming_events_run*.txt|run,time_s,event,bssid"
    "csvrun|$RAW_DIR/sta_pos_all.csv|*/raw/sta_pos_run*.csv|run,time_s,x,y,z"
    "csvrun|$RAW_DIR/rtt_probe_all.csv|*/raw/rtt_probe_run*.csv|run,time_s,seq,rtt_ms"
  )

  local total="${#steps[@]}"
  local i=0

  for s in "${steps[@]}"; do
    i=$((i+1))
    IFS='|' read -r type out pattern header <<< "$s"
    step_log "$i" "$total" "MERGE" "start" "$(basename "$out")"

    case "$type" in
      plain)  merge_csv "$out" "$pattern" "$header" ;;
      csvrun) merge_csv_prefix_run "$out" "$pattern" "$header" ;;
      txtrun) merge_txt_prefix_run "$out" "$pattern" "$header" ;;
      *) die "Unknown merge type: $type" ;;
    esac

    step_log "$i" "$total" "MERGE" "ok" "$(basename "$out")"
  done
}

# ---------------------------- Plot helpers (like P2) ----------------------------
count_png(){
  local dir="$1"
  [[ -d "$dir" ]] || { echo 0; return; }
  find "$dir" -maxdepth 1 -type f -name '*.png' 2>/dev/null | wc -l | tr -d ' '
}

ensure_venv(){
  if [[ ! -x "$PY" ]]; then
    need_cmd python3
    log "[P6] Creating venv at: $VENV_DIR (no package install performed)"
    python3 -m venv "$VENV_DIR" || die "Failed to create venv at: $VENV_DIR"
  fi
}

check_plot_deps_or_die(){
  "$PY" - <<'PY'
import importlib, sys
required = ["numpy", "pandas", "plotly", "kaleido"]
missing = []
for name in required:
    try:
        importlib.import_module(name)
    except Exception:
        missing.append(name)

if missing:
    print("[P6] ERROR: Missing Python packages in venv: " + ", ".join(missing), file=sys.stderr)
    print("", file=sys.stderr)
    print("[P6] Install them like this:", file=sys.stderr)
    print("  source .venv/bin/activate", file=sys.stderr)
    print("  python -m pip install -r requirements.txt", file=sys.stderr)
    sys.exit(2)
PY
}

run_plots(){
  [[ -f "$PLOT_SCRIPT" ]] || die "Plot script not found: $PLOT_SCRIPT"
  local plot_log="$LOG_DIR/plots.log"
  local before after new
  before="$(count_png "$PLOT_DIR")"

  log "[P6] [PLOT] start p6_plots"
  if "$PY" "$PLOT_SCRIPT" --results "$OUTDIR" --strict-png >"$plot_log" 2>&1; then
    after="$(count_png "$PLOT_DIR")"
    new=$((after - before)); (( new < 0 )) && new=0
    log "[P6] [PLOT] ok    p6_plots (+${new} png, total=${after})"
  else
    warn "[P6] [PLOT] FAILED p6_plots (see $plot_log)"
    tail -n 120 "$plot_log" || true
    return 1
  fi
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

  IFS=',' read -r -a speeds_arr <<< "$SPEEDS"
  local total="${#speeds_arr[@]}"
  local idx=0 running=0 fail=0

  for v in "${speeds_arr[@]}"; do
    idx=$((idx+1))
    local runnum=$((RUN_BASE + idx - 1))
    local tag="v${v}_run${runnum}"
    local out_run="$TMP_ROOT/$(safe_tag "$tag")"
    mkdir -p "$out_run/raw" "$out_run/logs" "$out_run/plots"

    local args; args="$(common_args "$out_run" "$runnum" "$v")"
    run_one "$(safe_tag "$tag")" "$args" "$idx" "$total" &
    running=$((running+1))

    if (( running >= JOBS )); then
      if wait -n; then running=$((running-1)); else fail=$((fail+1)); running=$((running-1)); fi
    fi
  done

  while (( running > 0 )); do
    if wait -n; then running=$((running-1)); else fail=$((fail+1)); running=$((running-1)); fi
  done

  # Consolidate outputs (with P2-style step logs)
  do_merges

  # Preserve per-run trees (optional but useful for debugging)
  copy_per_run_tree

  rm -rf "$TMP_ROOT" || true

  log "[P6] Completed."

  # Generate plots (like P2)
  ensure_venv
  check_plot_deps_or_die
  run_plots

  # Fail after plots so you still get plots even if some runs failed
  (( fail == 0 )) || die "[P6] One or more runs failed (see $LOG_DIR)"
}

main "$@"
