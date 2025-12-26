#!/usr/bin/env bash
# Projet 9 runner (ns-3.41) - Safe parallel runs (no CSV corruption)
# Machine: 8 cores (1 thread/core), RAM ~15GiB
# Strategy:
#  - Run simulations in parallel (default JOBS=6)
#  - Each run writes into a private outDir: results/.../tmp_runs/<tag>
#  - Merge tmp grid.csv + heatmap.csv into final results/.../(raw,heatmaps)
#  - Keep only: heatmaps/ raw/ logs/ plots/

set -euo pipefail
IFS=$'\n\t'

# -------------------- paths --------------------
NS3_DIR="${NS3_DIR:-$HOME/ns-3}"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

SCEN_SRC="${SCEN_SRC:-$ROOT_DIR/scenarios/p9_heatmap.cc}"
SCEN_NAME="${SCEN_NAME:-p9_heatmap}"
SCENARIO="${SCENARIO:-scratch/${SCEN_NAME}}"

OUTDIR="${OUTDIR:-$ROOT_DIR/results/p9}"   # (اگر خواستی دقیقاً مثل PDF باشد: results/p9_heatmap) :contentReference[oaicite:2]{index=2}

# -------------------- parallelism (tuned) --------------------
# 8 cores → JOBS=6 usually safe (3-4x faster vs serial)
JOBS="${JOBS:-6}"
MAX_JOBS="${MAX_JOBS:-8}"   # hard cap

# -------------------- behavior --------------------
CLEAN="${CLEAN:-true}"
STOP_ON_FIRST_FAIL="${STOP_ON_FIRST_FAIL:-false}"
TAIL_LINES="${TAIL_LINES:-120}"

# -------------------- grid (your 16x16 = 256 points) --------------------
X_MIN="${X_MIN:-0}"
X_MAX="${X_MAX:-30}"
Y_MIN="${Y_MIN:--15}"
Y_MAX="${Y_MAX:-15}"
STEP="${STEP:-2}"
POINTS="${POINTS:-}"   # optional: "x:y,x:y,..."

# -------------------- sim args (must match C++ CLI) --------------------
PCAP="${PCAP:-false}"
FLOWMON="${FLOWMON:-false}"
SEED="${SEED:-1}"
RUN_BASE="${RUN_BASE:-1}"

SIMTIME="${SIMTIME:-7.0}"
APPSTART="${APPSTART:-2.0}"
MEASURETIME="${MEASURETIME:-3.0}"

TRANSPORT="${TRANSPORT:-udp}"
PKTSIZE="${PKTSIZE:-1200}"
UDPRATE="${UDPRATE:-10}"         # keep sane for heatmap (avoid saturation)
TCPMAXBYTES="${TCPMAXBYTES:-0}"

STANDARD="${STANDARD:-ax}"
RATECONTROL="${RATECONTROL:-adaptive}"
DATAMODE="${DATAMODE:-HeMcs7}"

PROPMODEL="${PROPMODEL:-logdistance}"
EXPONENT="${EXPONENT:-3.0}"

TXPWR="${TXPWR:-20.0}"
CHWIDTH="${CHWIDTH:-20}"
APX="${APX:-0.0}"
APY="${APY:-0.0}"

PLOT="${PLOT:-false}"
PLOT_SCRIPT="${PLOT_SCRIPT:-$ROOT_DIR/scripts/plot_p9.py}"

# -------------------- final output dirs --------------------
HEAT_DIR="$OUTDIR/heatmaps"
RAW_DIR="$OUTDIR/raw"
LOG_DIR="$OUTDIR/logs"
PLOT_DIR="$OUTDIR/plots"
TMP_ROOT="$OUTDIR/tmp_runs"

ts() { date +"%Y-%m-%d %H:%M:%S"; }
log() { echo "[$(ts)] $*"; }
warn() { echo "[$(ts)] WARN: $*" >&2; }
die() { echo "[$(ts)] ERROR: $*" >&2; exit 1; }

ensure_dirs() {
  mkdir -p "$HEAT_DIR" "$RAW_DIR" "$LOG_DIR" "$PLOT_DIR"
}

clean_outputs() {
  log "[P9] Cleaning previous outputs under: $OUTDIR"
  rm -rf "$HEAT_DIR" "$RAW_DIR" "$LOG_DIR" "$PLOT_DIR" "$TMP_ROOT" || true
  ensure_dirs
}

stage_scenario() {
  [[ -d "$NS3_DIR" ]] || die "NS3_DIR not found: $NS3_DIR"
  [[ -f "$SCEN_SRC" ]] || die "Scenario source not found: $SCEN_SRC"
  local dst="$NS3_DIR/scratch/${SCEN_NAME}.cc"
  cp -f "$SCEN_SRC" "$dst" || die "Copy to ns-3 scratch failed: $dst"
  log "[P9] Staged scenario: $SCEN_SRC -> $dst"
}

build_ns3() {
  log "[P9] Building ns-3 in: $NS3_DIR"
  local build_log="$LOG_DIR/build.log"
  ( cd "$NS3_DIR" && NINJAFLAGS="-j${MAX_JOBS}" ./ns3 build ) >"$build_log" 2>&1 || {
    warn "[P9] BUILD FAILED. Last ${TAIL_LINES} lines:"
    tail -n "$TAIL_LINES" "$build_log" || true
    die "ns-3 build failed (full log: $build_log)"
  }
  log "[P9] Build OK"
}

safe_tag() {
  local x="$1" y="$2"
  local sx="${x//-/_m}"; local sy="${y//-/_m}"
  sx="${sx//./p}"; sy="${sy//./p}"
  echo "x${sx}_y${sy}"
}

common_args() {
  local out="$1" runnum="$2" x="$3" y="$4"
  echo "\
--outDir=${out} --pcap=${PCAP} --flowmon=${FLOWMON} \
--seed=${SEED} --run=${runnum} \
--simTime=${SIMTIME} --appStart=${APPSTART} --measureTime=${MEASURETIME} \
--transport=${TRANSPORT} --pktSize=${PKTSIZE} --udpRateMbps=${UDPRATE} --tcpMaxBytes=${TCPMAXBYTES} \
--standard=${STANDARD} --rateControl=${RATECONTROL} --dataMode=${DATAMODE} \
--propModel=${PROPMODEL} --exponent=${EXPONENT} \
--txPowerDbm=${TXPWR} --channelWidth=${CHWIDTH} \
--apX=${APX} --apY=${APY} \
--x=${x} --y=${y}"
}

# Concurrency gate
wait_for_slot() {
  while (( $(jobs -rp | wc -l) >= JOBS )); do
    wait -n || true
  done
}

run_one() {
  local tag="$1" args="$2" seq="$3" total="$4"
  local run_log="$LOG_DIR/${tag}.log"

  log "[P9] [${seq}/${total}] RUN start  ${tag}"
  ( cd "$NS3_DIR" && ./ns3 run "${SCENARIO} ${args}" ) >"$run_log" 2>&1 || {
    warn "[P9] RUN FAILED: $tag"
    warn "[P9] Log file: $run_log"
    tail -n "$TAIL_LINES" "$run_log" || true
    return 1
  }
  log "[P9] [${seq}/${total}] RUN ok     ${tag}"
  return 0
}

init_master_from_first() {
  local master="$1" pattern="$2" default_header="$3"
  local first
  first="$(find "$TMP_ROOT" -type f -path "$pattern" | sort | head -n 1 || true)"
  if [[ -n "${first:-}" && -s "$first" ]]; then
    head -n 1 "$first" > "$master"
  else
    echo "$default_header" > "$master"
  fi
}

merge_csv_family() {
  local master="$1" pattern="$2" default_header="$3"
  init_master_from_first "$master" "$pattern" "$default_header"

  local f
  while IFS= read -r f; do
    [[ -s "$f" ]] || continue
    tail -n +2 "$f" >> "$master" || true
  done < <(find "$TMP_ROOT" -type f -path "$pattern" | sort)

  log "[P9] Merge done -> $master"
}

maybe_plot() {
  [[ "$PLOT" == "true" ]] || return 0
  [[ -f "$PLOT_SCRIPT" ]] || { warn "[P9] PLOT=true but plot script not found: $PLOT_SCRIPT"; return 0; }
  log "[P9] Plotting with $PLOT_SCRIPT"
  python3 "$PLOT_SCRIPT" "$RAW_DIR/grid.csv" "$PLOT_DIR" >"$LOG_DIR/plot_p9.log" 2>&1 || \
    warn "[P9] Plot failed (see $LOG_DIR/plot_p9.log)"
}

main() {
  # Safe cap
  if (( JOBS > MAX_JOBS )); then JOBS="$MAX_JOBS"; fi
  if (( JOBS < 1 )); then JOBS=1; fi

  ensure_dirs
  if [[ "$CLEAN" == "true" ]]; then
    clean_outputs
  else
    log "[P9] CLEAN disabled"
  fi
  mkdir -p "$TMP_ROOT"

  stage_scenario
  build_ns3

  log "[P9] Parallel runs: JOBS=$JOBS (8 cores tuned)."
  log "[P9] UDPRATE=$UDPRATE Mbps"

  # Total points
  local total idx fail
  fail=0; idx=0
  if [[ -n "$POINTS" ]]; then
    IFS=',' read -r -a pts <<< "$POINTS"
    total="${#pts[@]}"
  else
    local nx=$(( (X_MAX - X_MIN) / STEP + 1 ))
    local ny=$(( (Y_MAX - Y_MIN) / STEP + 1 ))
    total=$(( nx * ny ))
  fi

  if [[ -n "$POINTS" ]]; then
    for p in "${pts[@]}"; do
      local px="${p%%:*}"
      local py="${p#*:}"
      local runnum=$((RUN_BASE + idx))
      local seq=$((idx + 1))
      local tag="p9_$(safe_tag "$px" "$py")_run${runnum}"

      local out_run="$TMP_ROOT/$tag"
      mkdir -p "$out_run/heatmaps" "$out_run/raw"

      local args; args="$(common_args "$out_run" "$runnum" "$px" "$py")"
      run_one "$tag" "$args" "$seq" "$total" || { fail=$((fail + 1)); [[ "$STOP_ON_FIRST_FAIL" == "true" ]] && break; } &
      wait_for_slot
      idx=$((idx + 1))
    done
  else
    for ((xx=X_MIN; xx<=X_MAX; xx+=STEP)); do
      for ((yy=Y_MIN; yy<=Y_MAX; yy+=STEP)); do
        local runnum=$((RUN_BASE + idx))
        local seq=$((idx + 1))
        local tag="p9_$(safe_tag "$xx" "$yy")_run${runnum}"

        local out_run="$TMP_ROOT/$tag"
        mkdir -p "$out_run/heatmaps" "$out_run/raw"

        local args; args="$(common_args "$out_run" "$runnum" "$xx" "$yy")"
        run_one "$tag" "$args" "$seq" "$total" || { fail=$((fail + 1)); [[ "$STOP_ON_FIRST_FAIL" == "true" ]] && break; } &
        wait_for_slot
        idx=$((idx + 1))
      done
      [[ "$STOP_ON_FIRST_FAIL" == "true" && "$fail" -ne 0 ]] && break
    done
  fi

  wait || true

  # Merge heatmap.csv (optional extra file)
  merge_csv_family \
    "$HEAT_DIR/heatmap.csv" \
    "*/heatmaps/heatmap.csv" \
    "x,y,associated,offered_mbps,goodput_mbps,avg_rtt_ms,rtt_replies,tx_bytes,rx_bytes,loss_ratio,rssi_est_dbm,snr_est_db,seed,run,standard,transport,rateControl,channelWidth"

  # Merge REQUIRED grid.csv for the project :contentReference[oaicite:3]{index=3}
  merge_csv_family \
    "$RAW_DIR/grid.csv" \
    "*/raw/grid.csv" \
    "x,y,seed,run,rssi_dbm,snr_db,goodput_mbps,rtt_ms,delay_ms,loss"

  maybe_plot

  # Keep only required dirs
  rm -rf "$TMP_ROOT" || true

  log "[P9] Outputs kept:"
  log "  heatmaps: $HEAT_DIR"
  log "  raw     : $RAW_DIR"
  log "  logs    : $LOG_DIR"
  log "  plots   : $PLOT_DIR"

  if [[ "$fail" -ne 0 ]]; then
    die "[P9] Completed with failures: $fail (see $LOG_DIR)"
  fi
  log "[P9] All points completed successfully."
}

main "$@"
