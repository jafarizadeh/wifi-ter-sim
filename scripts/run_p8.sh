#!/usr/bin/env bash
# scripts/run_p8.sh
#  Part 8 runner (parallel-safe with per-run outDir + merge)
# Shows run progress like: [1/6]

set -euo pipefail
IFS=$'\n\t'

# -------------------- configuration --------------------
NS3_DIR="${NS3_DIR:-$HOME/ns-3}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

SCEN_NAME="${SCEN_NAME:-p8_qos_wmm}"
SCEN_SRC="${SCEN_SRC:-$ROOT_DIR/scenarios/${SCEN_NAME}.cc}"
SCENARIO="${SCENARIO:-scratch/${SCEN_NAME}}"

# مهم: پوشه خروجی باید p8 باشد
OUTDIR="${OUTDIR:-$ROOT_DIR/results/p8}"
RAW_DIR="$OUTDIR/raw"
LOG_DIR="$OUTDIR/logs"
PLOT_DIR="$OUTDIR/plots"
TMP_ROOT="$OUTDIR/tmp_runs"

CSV_OUT="$RAW_DIR/p8_summary.csv"
CSV_HEADER="mode,beRateMbps,seed,run,goodputBE,goodputVO,goodputVI,delayVO_ms,jitterVO_ms,lossVO,delayVI_ms,jitterVI_ms,lossVI"

CLEAN="${CLEAN:-true}"
KEEP_TMP="${KEEP_TMP:-false}"

# ---- build/parallel knobs ----
BUILD_JOBS="${BUILD_JOBS:-2}"
TAIL_LINES="${TAIL_LINES:-120}"

JOBS="${JOBS:-6}"
MAX_JOBS="${MAX_JOBS:-6}"

LAUNCH_STAGGER_MS="${LAUNCH_STAGGER_MS:-250}"

NICE_LEVEL="${NICE_LEVEL:-15}"
IONICE_CLASS="${IONICE_CLASS:-2}"
IONICE_LEVEL="${IONICE_LEVEL:-7}"
CPULIMIT_PCT="${CPULIMIT_PCT:-10}"

# -------------------- sweep parameters --------------------
BE_RATES_STR="${BE_RATES:-0 10 20 40 60}"
MODES_STR="${MODES:-OFF ON}"

DURATION="${DURATION:-30}"
APP_START="${APP_START:-1}"

SEED="${SEED:-1}"
RUN_BASE="${RUN_BASE:-0}"

FLOWMON="${FLOWMON:-true}"
PCAP="${PCAP:-false}"
VERBOSE_APP="${VERBOSE_APP:-false}"

# Wi-Fi knobs
STANDARD="${STANDARD:-ax}"
DATAMODE="${DATAMODE:-HeMcs7}"
CTRLMODE="${CTRLMODE:-HeMcs0}"
TXPOWER_DBM="${TXPOWER_DBM:-16}"
CHANNEL_WIDTH="${CHANNEL_WIDTH:-20}"
CHANNEL_NUMBER="${CHANNEL_NUMBER:-36}"

# VO/VI/BE knobs
VO_PKT="${VO_PKT_SIZE:-160}"
VO_PPS="${VO_PPS:-50}"
VI_PKT="${VI_PKT_SIZE:-1200}"
VI_RATE="${VI_RATE_MBPS:-6}"
BE_PKT="${BE_PKT_SIZE:-1200}"

EXTRA_ARGS="${EXTRA_ARGS:-}"

# -------------------- helpers --------------------
ts()   { date +"%Y-%m-%d %H:%M:%S"; }
log()  { echo "[$(ts)] $*"; }
warn() { echo "[$(ts)] WARN: $*" >&2; }
die()  { echo "[$(ts)] ERROR: $*" >&2; exit 1; }

have_cmd() { command -v "$1" >/dev/null 2>&1; }

ensure_dirs() { mkdir -p "$RAW_DIR" "$LOG_DIR" "$PLOT_DIR" "$TMP_ROOT"; }

clean_outputs() {
  log "[P8] Cleaning outputs under: $OUTDIR"
  rm -rf "$RAW_DIR" "$LOG_DIR" "$PLOT_DIR" "$TMP_ROOT" || true
  ensure_dirs
}

stage_scenario() {
  [[ -d "$NS3_DIR" ]] || die "NS3_DIR not found: $NS3_DIR"
  [[ -f "$SCEN_SRC" ]] || die "Scenario source not found: $SCEN_SRC"
  mkdir -p "$NS3_DIR/scratch"
  cp -f "$SCEN_SRC" "$NS3_DIR/scratch/${SCEN_NAME}.cc" || die "Copy to ns-3 scratch failed"
  log "[P8] Staged scenario: $SCEN_SRC -> $NS3_DIR/scratch/${SCEN_NAME}.cc"
}

build_ns3_low_heat() {
  log "[P8] Building ns-3 (low-heat): -j${BUILD_JOBS}"
  local build_log="$LOG_DIR/build.log"
  ( cd "$NS3_DIR" && NINJAFLAGS="-j${BUILD_JOBS}" ./ns3 build ) >"$build_log" 2>&1 || {
    warn "[P8] BUILD FAILED. Last ${TAIL_LINES} lines:"
    tail -n "$TAIL_LINES" "$build_log" || true
    die "ns-3 build failed (full log: $build_log)"
  }
  log "[P8] Build OK"
}

to_array() {
  local s="$1"
  local -n out="$2"
  local IFS=' '
  read -r -a out <<< "$s"
}

run_cmd_wrapped() {
  local run_log="$1"; shift
  local -a cmd=( "$@" )

  if have_cmd ionice; then
    cmd=( ionice -c "$IONICE_CLASS" -n "$IONICE_LEVEL" "${cmd[@]}" )
  fi
  if have_cmd nice; then
    cmd=( nice -n "$NICE_LEVEL" "${cmd[@]}" )
  fi

  if have_cmd cpulimit; then
    cpulimit -l "$CPULIMIT_PCT" -- "${cmd[@]}" >"$run_log" 2>&1
  else
    "${cmd[@]}" >"$run_log" 2>&1
  fi
}

wait_for_slot() {
  while (( $(jobs -rp | wc -l) >= JOBS )); do
    wait -n || true
  done
}

safe_tag() {
  local mode="$1" be="$2" run="$3"
  echo "p8_${mode}_be${be}_s${SEED}_r${run}"
}

init_master_csv() {
  mkdir -p "$(dirname "$CSV_OUT")"
  echo "$CSV_HEADER" > "$CSV_OUT"
}

merge_summary_csv() {
  init_master_csv
  local f first
  while IFS= read -r f; do
    [[ -s "$f" ]] || continue
    first="$(head -n 1 "$f" || true)"
    if [[ "$first" == "$CSV_HEADER" ]] || [[ "$first" == mode,beRateMbps,* ]]; then
      tail -n +2 "$f" >> "$CSV_OUT" || true
    else
      cat "$f" >> "$CSV_OUT" || true
    fi
  done < <(find "$TMP_ROOT" -type f -path "*/raw/p8_summary.csv" | sort)
  log "[P8] Merge -> $CSV_OUT"
}

run_one() {
  local tag="$1" args="$2" out_run="$3" seq="$4" total="$5"
  local run_log="$LOG_DIR/${tag}.log"

  log "[P8] [${seq}/${total}] RUN start  ${tag}"

  mkdir -p "$out_run/raw" "$out_run/logs" "$out_run/plots" "$out_run/pcap"
  echo "$CSV_HEADER" > "$out_run/raw/p8_summary.csv"

  if ( cd "$NS3_DIR" && ./ns3 run "${SCENARIO} ${args}" --no-build ) >/dev/null 2>&1; then
    run_cmd_wrapped "$run_log" bash -lc "cd \"$NS3_DIR\" && ./ns3 run \"${SCENARIO} ${args}\" --no-build"
  else
    run_cmd_wrapped "$run_log" bash -lc "cd \"$NS3_DIR\" && ./ns3 run \"${SCENARIO} ${args}\""
  fi

  local rc=$?
  if (( rc != 0 )); then
    warn "[P8] [${seq}/${total}] RUN FAILED: $tag (rc=$rc)"
    warn "[P8] Log file: $run_log"
    tail -n "$TAIL_LINES" "$run_log" || true
    return "$rc"
  fi

  log "[P8] [${seq}/${total}] RUN ok     ${tag}"
  return 0
}

# -------------------- main --------------------
main() {
  if (( JOBS > MAX_JOBS )); then JOBS="$MAX_JOBS"; fi
  if (( JOBS < 1 )); then JOBS=1; fi
  if (( BUILD_JOBS < 1 )); then BUILD_JOBS=1; fi

  ensure_dirs
  if [[ "$CLEAN" == "true" ]]; then
    clean_outputs
  fi
  mkdir -p "$TMP_ROOT"

  stage_scenario
  build_ns3_low_heat

  local -a BE_RATES MODES
  to_array "$BE_RATES_STR" BE_RATES
  to_array "$MODES_STR" MODES

  local -a jobs_list=()
  local be mode
  for be in "${BE_RATES[@]}"; do
    for mode in "${MODES[@]}"; do
      jobs_list+=( "${mode}|${be}" )
    done
  done

  local total="${#jobs_list[@]}"
  (( total > 0 )) || die "[P8] No runs to execute."
  log "[P8] Runs: total=${total}, max parallel JOBS=${JOBS}"

  local idx=0 run_num tag out_run args item
  for item in "${jobs_list[@]}"; do
    IFS='|' read -r mode be <<< "$item"
    idx=$((idx + 1))
    run_num=$((RUN_BASE + idx - 1))

    tag="$(safe_tag "$mode" "$be" "$run_num")"
    out_run="$TMP_ROOT/$tag"
    mkdir -p "$out_run"

    # IMPORTANT: outDir per run to avoid overwrite/races
    args="--mode=${mode} --beRateMbps=${be} --duration=${DURATION} --appStart=${APP_START}"
    args+=" --seed=${SEED} --run=${run_num} --outDir=${out_run}"
    args+=" --flowmon=${FLOWMON} --pcap=${PCAP} --verboseApp=${VERBOSE_APP}"
    args+=" --standard=${STANDARD} --dataMode=${DATAMODE} --ctrlMode=${CTRLMODE}"
    args+=" --txPowerDbm=${TXPOWER_DBM} --channelWidth=${CHANNEL_WIDTH} --channelNumber=${CHANNEL_NUMBER}"
    args+=" --voPktSize=${VO_PKT} --voPps=${VO_PPS}"
    args+=" --viPktSize=${VI_PKT} --viRateMbps=${VI_RATE}"
    args+=" --bePktSize=${BE_PKT}"

    if [[ -n "$EXTRA_ARGS" ]]; then
      args+=" ${EXTRA_ARGS}"
    fi

    wait_for_slot
    ( run_one "$tag" "$args" "$out_run" "$idx" "$total" ) &

    if (( LAUNCH_STAGGER_MS > 0 )); then
      python3 - <<PY >/dev/null 2>&1 || true
import time
time.sleep(${LAUNCH_STAGGER_MS}/1000.0)
PY
    fi
  done

  wait || true

  merge_summary_csv

  # Copy per-run artifacts for inspection
  mkdir -p "$RAW_DIR/per_run_raw"
  if have_cmd rsync; then
    rsync -a --delete "$TMP_ROOT/" "$RAW_DIR/per_run_raw/" >/dev/null 2>&1 || true
  else
    rm -rf "$RAW_DIR/per_run_raw" || true
    mkdir -p "$RAW_DIR/per_run_raw"
    cp -a "$TMP_ROOT/." "$RAW_DIR/per_run_raw/" 2>/dev/null || true
  fi

  if [[ "$KEEP_TMP" != "true" ]]; then
    rm -rf "$TMP_ROOT" || true
  else
    log "[P8] KEEP_TMP=true (tmp runs kept at: $TMP_ROOT)"
  fi

  cat <<EOF

====================== [P8] Summary ======================
NS3_DIR : $NS3_DIR
Scenario: $SCENARIO
Source  : $SCEN_SRC
OutDir  : $OUTDIR
Raw     : $RAW_DIR
Logs    : $LOG_DIR
Plots   : $PLOT_DIR
----------------------------------------------------------
Key outputs:
  - $CSV_OUT
  - per-run artifacts under $RAW_DIR/per_run_raw/
==========================================================

EOF

  log "[P8] Done."
}

main "$@"
