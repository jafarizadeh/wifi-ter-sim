#!/usr/bin/env bash
# Projet 7 runner (parallel, safe merging)
# ns-3 scenario: p7_channel_planning (co-channel vs separate)
# Strategy:
#  - Clean outputs
#  - Stage scenario into ns-3 scratch/
#  - Build ns-3
#  - Execute runs in parallel (JOBS)
#  - Each run writes into a private tmp outDir to avoid concurrent writes
#  - Merge per-run CSVs into final results (summary + per-flow)

set -euo pipefail
IFS=$'\n\t'

# ---------------------------- Paths ----------------------------
NS3_DIR="${NS3_DIR:-$HOME/ns-3}"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

SCEN_SRC="${SCEN_SRC:-$ROOT_DIR/scenarios/p7_channel_planning.cc}"
SCEN_NAME="${SCEN_NAME:-p7_channel_planning}"
SCENARIO="scratch/${SCEN_NAME}"

OUTDIR="${OUTDIR:-$ROOT_DIR/results/p7}"
RAW_DIR="$OUTDIR/raw"
LOG_DIR="$OUTDIR/logs"
PLOT_DIR="$OUTDIR/plots"
TMP_ROOT="$OUTDIR/tmp_runs"

# ---------------------------- Parallelism ----------------------------
JOBS="${JOBS:-6}"
MAX_JOBS="${MAX_JOBS:-8}"

# ---------------------------- Matrix ----------------------------
NSTAS="${NSTAS:-2,5,10}"
CHAN1="${CHAN1:-36}"
CHAN2="${CHAN2:-40}"
WIDTH="${WIDTH:-20}"

SEED="${SEED:-1}"
RUN_BASE="${RUN_BASE:-1}"

# ---------------------------- Defaults (match C++ CLI) ----------------------------
PCAP="${PCAP:-false}"
FLOWMON="${FLOWMON:-true}"

SIMTIME="${SIMTIME:-25}"
APPSTART="${APPSTART:-2}"

AP_SEP="${AP_SEP:-15}"
R_STA="${R_STA:-5}"

PKTSIZE="${PKTSIZE:-1200}"
UDP_RATE_PER_STA="${UDP_RATE_PER_STA:-10Mbps}"

TXPWR="${TXPWR:-16}"
NOISEFIG="${NOISEFIG:-7}"
LOGEXP="${LOGEXP:-3.0}"
SHADOW_SIGMA="${SHADOW_SIGMA:-4.0}"
ENABLE_FADING="${ENABLE_FADING:-true}"

ENABLE_WIFI_LOGS="${ENABLE_WIFI_LOGS:-false}"
WIFI_LOG_LEVEL="${WIFI_LOG_LEVEL:-INFO}"
ENABLE_PREFIXES="${ENABLE_PREFIXES:-true}"
ENABLE_ASSOC_LOGS="${ENABLE_ASSOC_LOGS:-false}"

# ---------------------------- Helpers ----------------------------
ts(){ date +"%Y-%m-%d %H:%M:%S"; }
log(){ echo "[$(ts)] $*"; }
warn(){ echo "[$(ts)] WARN: $*" >&2; }
die(){ echo "[$(ts)] ERROR: $*" >&2; exit 1; }

need_cmd(){ command -v "$1" >/dev/null 2>&1 || die "Missing command: $1"; }

ensure_dirs(){ mkdir -p "$RAW_DIR" "$LOG_DIR" "$PLOT_DIR" "$TMP_ROOT"; }

clean_outputs(){
  log "[P7] Cleaning previous outputs under: $OUTDIR"
  rm -rf "$RAW_DIR" "$LOG_DIR" "$PLOT_DIR" "$TMP_ROOT" || true
  ensure_dirs
}

stage_scenario(){
  [[ -d "$NS3_DIR" ]] || die "NS3_DIR not found: $NS3_DIR"
  [[ -f "$SCEN_SRC" ]] || die "Scenario source not found: $SCEN_SRC"
  local dst="$NS3_DIR/scratch/${SCEN_NAME}.cc"
  mkdir -p "$NS3_DIR/scratch"
  cp -f "$SCEN_SRC" "$dst" || die "Copy to ns-3 scratch failed: $dst"
  log "[P7] Staged scenario: $SCEN_SRC -> $dst"
}

build_ns3(){
  log "[P7] Building ns-3"
  local build_log="$LOG_DIR/build.log"
  ( cd "$NS3_DIR" && ./ns3 build --jobs="$MAX_JOBS" ) >"$build_log" 2>&1 || {
    tail -n 200 "$build_log" || true
    die "ns-3 build failed (see $build_log)"
  }
  log "[P7] Build OK"
}

safe_tag(){
  local s="$1"
  s="${s// /_}"; s="${s//:/_}"; s="${s//,/_}"
  echo "$s"
}

common_args(){
  local out="$1" run="$2" plan="$3" n="$4" c1="$5" c2="$6" w="$7"
  echo "\
--simTime=${SIMTIME} --appStart=${APPSTART} \
--nStaPerCell=${n} \
--ssid1=cell1 --ssid2=cell2 \
--outDir=${out} --pcap=${PCAP} --flowmon=${FLOWMON} \
--apSeparation=${AP_SEP} --rSta=${R_STA} \
--pktSize=${PKTSIZE} --udpRatePerSta=${UDP_RATE_PER_STA} \
--channelPlan=${plan} --chan1=${c1} --chan2=${c2} --channelWidth=${w} \
--txPowerDbm=${TXPWR} --noiseFigureDb=${NOISEFIG} \
--logExp=${LOGEXP} --shadowingSigmaDb=${SHADOW_SIGMA} --enableFading=${ENABLE_FADING} \
--seed=${SEED} --run=${run} \
--enableWifiLogs=${ENABLE_WIFI_LOGS} --wifiLogLevel=${WIFI_LOG_LEVEL} \
--enableLogPrefixes=${ENABLE_PREFIXES} --enableAssocManagerLogs=${ENABLE_ASSOC_LOGS}"
}

run_one(){
  local tag="$1" args="$2" seq="$3" total="$4"
  local run_log="$LOG_DIR/${tag}.log"
  log "[P7] [${seq}/${total}] RUN start  ${tag}"
  ( cd "$NS3_DIR" && ./ns3 run "${SCENARIO} ${args}" ) >"$run_log" 2>&1 || {
    warn "[P7] RUN FAILED: ${tag} (see $run_log)"
    return 1
  }
  log "[P7] [${seq}/${total}] RUN ok     ${tag}"
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

merge_csv(){
  local master="$1" pattern="$2" default_header="$3"
  init_master_from_first "$master" "$pattern" "$default_header"
  local f
  while IFS= read -r f; do
    [[ -s "$f" ]] || continue
    tail -n +2 "$f" >> "$master" || true
  done < <(find "$TMP_ROOT" -type f -path "$pattern" | sort)
  log "[P7] Merge -> $master"
}

copy_perflow_files(){
  mkdir -p "$RAW_DIR/perflow"
  find "$TMP_ROOT" -type f -name "perflow_*.csv" -exec cp -f {} "$RAW_DIR/perflow/" \; || true
}

# ---------------------------- Main ----------------------------
main(){
  need_cmd find
  need_cmd tail

  # concurrency bounds
  (( JOBS > MAX_JOBS )) && JOBS="$MAX_JOBS"
  (( JOBS < 1 )) && JOBS=1

  clean_outputs
  stage_scenario
  build_ns3

  IFS=',' read -r -a nstas_arr <<< "$NSTAS"
  plans=( "cocanal" "separe" )

  # total runs for progress
  local total=$(( ${#nstas_arr[@]} * ${#plans[@]} ))
  local idx=0
  local running=0
  local fail=0

  for n in "${nstas_arr[@]}"; do
    for plan in "${plans[@]}"; do
      idx=$((idx+1))
      local runnum=$((RUN_BASE + idx - 1))
      local c1="$CHAN1"
      local c2="$CHAN1"
      [[ "$plan" == "separe" ]] && c2="$CHAN2"

      local tag="p7_${plan}_n${n}_c${c1}-${c2}_w${WIDTH}_run${runnum}"
      local out_run="$TMP_ROOT/$tag"
      mkdir -p "$out_run/raw" "$out_run/logs" "$out_run/plots"

      local args; args="$(common_args "$out_run" "$runnum" "$plan" "$n" "$c1" "$c2" "$WIDTH")"

      run_one "$(safe_tag "$tag")" "$args" "$idx" "$total" &
      running=$((running+1))

      if (( running >= JOBS )); then
        if wait -n; then
          running=$((running-1))
        else
          fail=$((fail+1))
          running=$((running-1))
        fi
      fi
    done
  done

  # wait remaining
  while (( running > 0 )); do
    if wait -n; then
      running=$((running-1))
    else
      fail=$((fail+1))
      running=$((running-1))
    fi
  done

  # merge summary and per-flow
  merge_csv \
    "$RAW_DIR/p7_summary.csv" \
    "*/raw/p7_summary.csv" \
    "channelPlan,nStaPerCell,chan1,chan2,seed,run,goodputCell1,goodputCell2,goodputTotal,jainCells"

  # optional consolidated perflow_all.csv
  merge_csv \
    "$RAW_DIR/perflow_all.csv" \
    "*/raw/perflow_*.csv" \
    "cellId,staId,rxBytes,goodputbps"

  copy_perflow_files

  # cleanup tmp
  rm -rf "$TMP_ROOT" || true

  log "[P7] Completed. Summary: $RAW_DIR/p7_summary.csv"
  (( fail == 0 )) || die "[P7] One or more runs failed (see $LOG_DIR)"
}

main "$@"
