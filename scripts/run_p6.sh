#!/usr/bin/env bash
# Projet 6 runner (parallel, tmp outDir per run, safe merging)
# Scenario: p6_roaming_multi_ap (Multi-AP roaming under traffic)
# Workflow:
#  1) Clean outputs
#  2) Stage scenario into ns-3 scratch/
#  3) Build ns-3
#  4) Execute runs in parallel (bounded by JOBS), each with a private outDir: results/p6/tmp_runs/<tag>
#  5) Merge per-run CSV/TXT into final consolidated files under results/p6/raw/
#  6) Keep run-scoped artifacts; remove tmp_runs

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
ROAM_POLL="${ROAM_POLL:-0.2}"      # program enforces >=0.2 in bestRoam mode
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
  log "[P6] [${seq}/${total}] RUN start  ${tag}"
  ( cd "$NS3_DIR" && ./ns3 run "${SCENARIO} ${args}" ) >"$log_path" 2>&1 || {
    warn "[P6] RUN FAILED: ${tag} (see $log_path)"
    return 1
  }
  log "[P6] [${seq}/${total}] RUN ok     ${tag}"
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
  log "[P6] Merge -> $master"
}

merge_with_run(){
  local master="$1" pattern="$2" header="$3" rx='run([0-9]+)'
  echo "$header" > "$master"
  local f
  while IFS= read -r f; do
    [[ -s "$f" ]] || continue
    local bn; bn="$(basename "$f")"
    local run
    if [[ "$bn" =~ $rx ]]; then run="${BASH_REMATCH[1]}"; else run="0"; fi
    awk -F'\n' -v r="$run" 'NR==1{next} {print r","$0}' OFS=',' "$f" >> "$master" || true
  done < <(find "$TMP_ROOT" -type f -path "$pattern" | sort)
  log "[P6] Merge -> $master"
}

copy_per_run(){
  local sub="$1"
  local pattern="$2"
  local dest="$RAW_DIR/$sub"
  mkdir -p "$dest"
  find "$TMP_ROOT" -type f -path "$pattern" -exec cp -f {} "$dest/" \; || true
}

# ---------------------------- Main ----------------------------
main(){
  need_cmd find
  need_cmd tail
  ensure_dirs
  clean_outputs
  stage_scenario
  build_ns3

  IFS=',' read -r -a speeds_arr <<< "$SPEEDS"
  (( JOBS > MAX_JOBS )) && JOBS="$MAX_JOBS"
  (( JOBS < 1 )) && JOBS=1

  local total="${#speeds_arr[@]}"
  local idx=0
  local running=0
  local fail=0

  for v in "${speeds_arr[@]}"; do
    idx=$((idx+1))
    local runnum=$((RUN_BASE + idx - 1))
    local tag="v${v}_run${runnum}"
    local out_run="$TMP_ROOT/$tag"
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

  # Consolidate outputs
  merge_plain \
    "$RAW_DIR/p6_summary.csv" \
    "*/raw/p6_summary.csv" \
    "apDistance,staSpeed,moveStart,udpRate,pktSize,seed,run,rxBytes,goodputbps,roamTime"

  merge_with_run \
    "$RAW_DIR/throughput_timeseries_all.csv" \
    "*/raw/throughput_timeseries_run*.csv" \
    "run,time_s,throughput_bps"

  merge_with_run \
    "$RAW_DIR/roaming_events_all.csv" \
    "*/raw/roaming_events_run*.txt" \
    "run,time_s,event,bssid"

  merge_with_run \
    "$RAW_DIR/sta_pos_all.csv" \
    "*/raw/sta_pos_run*.csv" \
    "run,time_s,x,y,z"

  merge_with_run \
    "$RAW_DIR/rtt_probe_all.csv" \
    "*/raw/rtt_probe_run*.csv" \
    "run,time_s,seq,rtt_ms"

  copy_per_run "flowmon" "*/raw/flowmon_run*.xml"
  copy_per_run "per_run_raw" "*/raw/*"

  rm -rf "$TMP_ROOT" || true

  log "[P6] Completed. Summary: $RAW_DIR/p6_summary.csv"
  (( fail == 0 )) || die "[P6] One or more runs failed (see $LOG_DIR)"
}

main "$@"
