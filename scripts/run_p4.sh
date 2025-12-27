#!/usr/bin/env bash
# scripts/run_p4.sh
#  Part 4 runner (parallel, per-run tmp outDir, safe merge) — FIXED wait/failure handling

set -euo pipefail
IFS=$'\n\t'

# -------------------- configuration --------------------
NS3_DIR="${NS3_DIR:-$HOME/ns-3}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

SCEN_NAME="${SCEN_NAME:-p4_phy_mac_sweep}"
SCEN_SRC="${SCEN_SRC:-$ROOT_DIR/scenarios/${SCEN_NAME}.cc}"
SCENARIO="${SCENARIO:-scratch/${SCEN_NAME}}"

OUTDIR="${OUTDIR:-$ROOT_DIR/results/p4}"
RAW_DIR="$OUTDIR/raw"
LOG_DIR="$OUTDIR/logs"
PLOT_DIR="$OUTDIR/plots"
TMP_ROOT="$OUTDIR/tmp_runs"

CSV_OUT="$RAW_DIR/p4_matrix.csv"
CSV_HEADER="distance,channelWidth,txPowerDbm,rateMode,mcs,udpRate,pktSize,seed,run,rxBytes,goodputMbps,rttMeanMs"

CLEAN="${CLEAN:-true}"
KEEP_TMP="${KEEP_TMP:-false}"
STOP_ON_FIRST_FAIL="${STOP_ON_FIRST_FAIL:-false}"

# ---- CPU/thermal knobs ----
BUILD_JOBS="${BUILD_JOBS:-2}"
TAIL_LINES="${TAIL_LINES:-120}"

JOBS="${JOBS:-6}"
MAX_JOBS="${MAX_JOBS:-6}"

LOAD_MAX="${LOAD_MAX:-}"                 # e.g. 4.0 ; empty => ~0.75*cores
LAUNCH_STAGGER_MS="${LAUNCH_STAGGER_MS:-250}"

NICE_LEVEL="${NICE_LEVEL:-15}"
IONICE_CLASS="${IONICE_CLASS:-2}"
IONICE_LEVEL="${IONICE_LEVEL:-7}"

CPULIMIT_PCT="${CPULIMIT_PCT:-8}"
BACKOFF_SLEEP_SEC="${BACKOFF_SLEEP_SEC:-1}"

# -------------------- Sweep parameters --------------------
DISTANCES_STR="${DISTANCES:-5 10 20 30}"
WIDTHS_STR="${WIDTHS:-20 40 80}"
POWERS_STR="${POWERS:-12 16 20}"
RATE_MODES_STR="${RATE_MODES:-adaptive constant}"
MCS_LIST_STR="${MCS_LIST:-0 3 7 9}"

SIM_TIME="${SIM_TIME:-20}"
APP_START="${APP_START:-2}"
SEED="${SEED:-1}"
RUN_BASE="${RUN_BASE:-1}"

UDP_RATE="${UDP_RATE:-600Mbps}"
PKT_SIZE="${PKT_SIZE:-1200}"
LOG_EXP="${LOG_EXP:-3.0}"
REF_DIST="${REF_DIST:-1.0}"
REF_LOSS="${REF_LOSS:-46.6777}"
NOISE_FIGURE_DB="${NOISE_FIGURE_DB:-7.0}"
ENABLE_SHADOWING="${ENABLE_SHADOWING:-false}"
SHADOW_SIGMA_DB="${SHADOW_SIGMA_DB:-5.0}"
SHADOW_UPDATE_S="${SHADOW_UPDATE_S:-1.0}"
ENABLE_FADING="${ENABLE_FADING:-false}"
RTT_HZ="${RTT_HZ:-2.0}"
RTT_PAYLOAD_SIZE="${RTT_PAYLOAD_SIZE:-32}"
RTT_VERBOSE="${RTT_VERBOSE:-false}"
PCAP="${PCAP:-false}"
FLOWMON="${FLOWMON:-true}"
USE_MINSTREL="${USE_MINSTREL:-true}"

TAG_SUFFIX="${TAG_SUFFIX:-}"
EXTRA_ARGS="${EXTRA_ARGS:-}"

# -------------------- helpers --------------------
ts()   { date +"%Y-%m-%d %H:%M:%S"; }
log()  { echo "[$(ts)] $*"; }
warn() { echo "[$(ts)] WARN: $*" >&2; }
die()  { echo "[$(ts)] ERROR: $*" >&2; exit 1; }

have_cmd() { command -v "$1" >/dev/null 2>&1; }

ensure_dirs() { mkdir -p "$RAW_DIR" "$LOG_DIR" "$PLOT_DIR"; }

clean_outputs() {
  log "[P4] Cleaning outputs under: $OUTDIR"
  rm -rf "$RAW_DIR" "$LOG_DIR" "$PLOT_DIR" "$TMP_ROOT" || true
  ensure_dirs
}

stage_scenario() {
  [[ -d "$NS3_DIR" ]] || die "NS3_DIR not found: $NS3_DIR"
  [[ -f "$SCEN_SRC" ]] || die "Scenario source not found: $SCEN_SRC"
  mkdir -p "$NS3_DIR/scratch"
  cp -f "$SCEN_SRC" "$NS3_DIR/scratch/${SCEN_NAME}.cc" || die "Copy to ns-3 scratch failed"
  log "[P4] Staged scenario: $SCEN_SRC -> $NS3_DIR/scratch/${SCEN_NAME}.cc"
}

build_ns3_low_heat() {
  log "[P4] Building ns-3 (low-heat): -j${BUILD_JOBS}"
  local build_log="$LOG_DIR/build.log"
  ( cd "$NS3_DIR" && NINJAFLAGS="-j${BUILD_JOBS}" ./ns3 build ) >"$build_log" 2>&1 || {
    warn "[P4] BUILD FAILED. Last ${TAIL_LINES} lines:"
    tail -n "$TAIL_LINES" "$build_log" || true
    die "ns-3 build failed (full log: $build_log)"
  }
  log "[P4] Build OK"
}

get_prog_help() {
  local help_log="$LOG_DIR/prog_help.log"
  if ( cd "$NS3_DIR" && ./ns3 run "${SCENARIO} --PrintHelp" --no-build ) >"$help_log" 2>&1; then
    true
  else
    ( cd "$NS3_DIR" && ./ns3 run "${SCENARIO} --PrintHelp" ) >"$help_log" 2>&1 || {
      warn "[P4] Could not fetch program help (see $help_log). Will run without arg-filtering."
      : > "$help_log"
    }
  fi
  echo "$help_log"
}

arg_supported() {
  local help_file="$1"
  local argname="$2"
  [[ -s "$help_file" ]] || return 1
  grep -q -- "--${argname}:" "$help_file"
}

to_array() {
  local s="$1"
  local -n out="$2"
  local IFS=' '
  read -r -a out <<< "$s"
}

safe_tag() {
  local d="$1" w="$2" p="$3" rm="$4" m="$5" s="$6" r="$7"
  echo "p4_d${d}_w${w}_p${p}_${rm}_mcs${m}_s${s}_r${r}"
}

init_master_csv() {
  mkdir -p "$(dirname "$CSV_OUT")"
  echo "$CSV_HEADER" > "$CSV_OUT"
}

merge_matrix_csv() {
  init_master_csv
  local f first
  while IFS= read -r f; do
    [[ -s "$f" ]] || continue
    first="$(head -n 1 "$f" || true)"
    if [[ "$first" == "$CSV_HEADER" ]] || [[ "$first" == distance,channelWidth,* ]]; then
      tail -n +2 "$f" >> "$CSV_OUT" || true
    else
      cat "$f" >> "$CSV_OUT" || true
    fi
  done < <(find "$TMP_ROOT" -type f -name "p4_matrix.csv" | sort)
  log "[P4] Merge -> $CSV_OUT"
}

get_cores() { getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1; }
get_load1() { awk '{print $1}' /proc/loadavg 2>/dev/null || echo 0; }

load_ok() {
  local cores load max
  cores="$(get_cores)"
  load="$(get_load1)"
  if [[ -z "${LOAD_MAX}" ]]; then
    max="$(python3 - <<PY
c=int($cores)
print(max(1.0, 0.75*c))
PY
)"
  else
    max="$LOAD_MAX"
  fi
  python3 - <<PY
load=float("$load"); maxv=float("$max")
print("1" if load <= maxv else "0")
PY
}

wait_for_slot_and_load() {
  while true; do
    if (( $(jobs -rp | wc -l) >= JOBS )); then
      # wait one job to free a slot
      wait -n || true
      continue
    fi
    if [[ "$(load_ok)" == "1" ]]; then
      break
    fi
    sleep "$BACKOFF_SLEEP_SEC"
  done
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

build_args_for_run() {
  local help_file="$1"
  local out_run="$2"
  local distance="$3"
  local width="$4"
  local power="$5"
  local rateMode="$6"
  local mcs="$7"
  local run="$8"

  local args=()
  args+=( "--simTime=${SIM_TIME}" )
  args+=( "--appStart=${APP_START}" )
  args+=( "--distance=${distance}" )
  args+=( "--channelWidth=${width}" )
  args+=( "--txPowerDbm=${power}" )
  args+=( "--rateMode=${rateMode}" )
  args+=( "--mcs=${mcs}" )
  args+=( "--useMinstrel=${USE_MINSTREL}" )
  args+=( "--ssid=wifi6-ter" )
  args+=( "--udpRate=${UDP_RATE}" )
  args+=( "--pktSize=${PKT_SIZE}" )
  args+=( "--logExp=${LOG_EXP}" )
  args+=( "--refDist=${REF_DIST}" )
  args+=( "--refLoss=${REF_LOSS}" )
  args+=( "--noiseFigureDb=${NOISE_FIGURE_DB}" )
  args+=( "--enableShadowing=${ENABLE_SHADOWING}" )
  args+=( "--shadowSigmaDb=${SHADOW_SIGMA_DB}" )
  args+=( "--shadowUpdateS=${SHADOW_UPDATE_S}" )
  args+=( "--enableFading=${ENABLE_FADING}" )
  args+=( "--rttHz=${RTT_HZ}" )
  args+=( "--rttPayloadSize=${RTT_PAYLOAD_SIZE}" )
  args+=( "--rttVerbose=${RTT_VERBOSE}" )
  args+=( "--pcap=${PCAP}" )
  args+=( "--flowmon=${FLOWMON}" )
  args+=( "--seed=${SEED}" )
  args+=( "--run=${run}" )
  args+=( "--outDir=${out_run}" )

  if [[ -n "$TAG_SUFFIX" ]] && arg_supported "$help_file" "tag"; then
    args+=( "--tag=${TAG_SUFFIX}" )
  fi

  # Optional: filter EXTRA_ARGS against --PrintHelp
  if [[ -n "$EXTRA_ARGS" ]] && [[ -s "$help_file" ]]; then
    local IFS=' '
    # shellcheck disable=SC2206
    local extra_tokens=( $EXTRA_ARGS )
    local t key
    for t in "${extra_tokens[@]}"; do
      if [[ "$t" =~ ^--([A-Za-z0-9_]+)= ]]; then
        key="${BASH_REMATCH[1]}"
        if arg_supported "$help_file" "$key"; then
          args+=( "$t" )
        else
          warn "[P4] Skipping unsupported arg from EXTRA_ARGS: $t"
        fi
      else
        args+=( "$t" )
      fi
    done
  elif [[ -n "$EXTRA_ARGS" ]]; then
    # No help available: pass as-is
    local IFS=' '
    # shellcheck disable=SC2206
    args+=( $EXTRA_ARGS )
  fi

  echo "${args[*]}"
}

flatten_outdir() {
  local out_run="$1"
  local sub
  for sub in raw logs plots; do
    if [[ -d "$out_run/$sub" ]]; then
      shopt -s dotglob nullglob
      mv "$out_run/$sub"/* "$out_run/" 2>/dev/null || true
      shopt -u dotglob nullglob
      rmdir "$out_run/$sub" 2>/dev/null || true
    fi
  done
}

run_one() {
  local tag="$1" args="$2" seq="$3" total="$4" out_run="$5"
  local run_log="$LOG_DIR/${tag}.log"

  log "[P4] [${seq}/${total}] RUN start  ${tag}"

  if ( cd "$NS3_DIR" && ./ns3 run "${SCENARIO} ${args}" --no-build ) >/dev/null 2>&1; then
    run_cmd_wrapped "$run_log" bash -lc "cd \"$NS3_DIR\" && ./ns3 run \"${SCENARIO} ${args}\" --no-build"
  else
    run_cmd_wrapped "$run_log" bash -lc "cd \"$NS3_DIR\" && ./ns3 run \"${SCENARIO} ${args}\""
  fi

  local rc=$?
  if (( rc != 0 )); then
    warn "[P4] RUN FAILED: $tag (rc=$rc)"
    warn "[P4] Log file: $run_log"
    tail -n "$TAIL_LINES" "$run_log" || true
    return "$rc"
  fi

  flatten_outdir "$out_run"
  log "[P4] [${seq}/${total}] RUN ok     ${tag}"
  return 0
}

kill_children() {
  # Kill any remaining background jobs when exiting early
  jobs -rp >/dev/null 2>&1 || return 0
  local pids
  pids="$(jobs -rp | tr '\n' ' ' || true)"
  [[ -n "${pids// /}" ]] || return 0
  kill $pids 2>/dev/null || true
}
trap 'kill_children' EXIT INT TERM

# -------------------- main --------------------
main() {
  if (( JOBS > MAX_JOBS )); then JOBS="$MAX_JOBS"; fi
  if (( JOBS < 1 )); then JOBS=1; fi
  if (( BUILD_JOBS < 1 )); then BUILD_JOBS=1; fi

  ensure_dirs
  if [[ "$CLEAN" == "true" ]]; then clean_outputs; fi
  mkdir -p "$TMP_ROOT"

  stage_scenario
  build_ns3_low_heat

  local help_file
  help_file="$(get_prog_help)"
  log "[P4] Program help captured: $help_file"

  local -a DISTANCES WIDTHS POWERS RATE_MODES MCS_LIST
  to_array "$DISTANCES_STR" DISTANCES
  to_array "$WIDTHS_STR" WIDTHS
  to_array "$POWERS_STR" POWERS
  to_array "$RATE_MODES_STR" RATE_MODES
  to_array "$MCS_LIST_STR" MCS_LIST

  local -a jobs_list=()
  local d w p rm m
  for d in "${DISTANCES[@]}"; do
    for w in "${WIDTHS[@]}"; do
      for p in "${POWERS[@]}"; do
        for rm in "${RATE_MODES[@]}"; do
          if [[ "$rm" == "constant" ]]; then
            for m in "${MCS_LIST[@]}"; do
              jobs_list+=( "${d}|${w}|${p}|${rm}|${m}" )
            done
          else
            jobs_list+=( "${d}|${w}|${p}|${rm}|0" )
          fi
        done
      done
    done
  done

  local total="${#jobs_list[@]}"
  (( total > 0 )) || die "[P4] No runs to execute."
  if (( total == 1 )); then JOBS=1; fi

  local cores; cores="$(get_cores)"
  if [[ -z "${LOAD_MAX}" ]]; then
    log "[P4] Load-aware throttle: LOAD_MAX not set -> default ~0.75*cores (cores=$cores)"
  else
    log "[P4] Load-aware throttle: LOAD_MAX=${LOAD_MAX}"
  fi

  log "[P4] Runs: total=${total}, max parallel JOBS=${JOBS}"
  if have_cmd cpulimit; then
    log "[P4] Thermal control: cpulimit per job = ${CPULIMIT_PCT}%"
  else
    log "[P4] Thermal control: cpulimit not found (using nice/ionice + load throttle)"
  fi

  # ---- FIXED: robust failure tracking without "no child processes" false-fails ----
  local idx=0 run_num tag out_run args item
  local fail=0

  for item in "${jobs_list[@]}"; do
    IFS='|' read -r d w p rm m <<< "$item"
    idx=$((idx + 1))
    run_num=$((RUN_BASE + idx - 1))

    tag="$(safe_tag "$d" "$w" "$p" "$rm" "$m" "$SEED" "$run_num")"
    out_run="$TMP_ROOT/$tag"
    mkdir -p "$out_run"

    args="$(build_args_for_run "$help_file" "$out_run" "$d" "$w" "$p" "$rm" "$m" "$run_num")"

    wait_for_slot_and_load

    (
      run_one "$tag" "$args" "$idx" "$total" "$out_run"
    ) &

    if (( LAUNCH_STAGGER_MS > 0 )); then
      python3 - <<PY >/dev/null 2>&1 || true
import time
time.sleep(${LAUNCH_STAGGER_MS}/1000.0)
PY
    fi
  done

  # Collect all remaining jobs; mark fail if any fails.
  while (( $(jobs -rp | wc -l) > 0 )); do
    if ! wait -n; then
      fail=1
      if [[ "$STOP_ON_FIRST_FAIL" == "true" ]]; then
        warn "[P4] STOP_ON_FIRST_FAIL=true -> stopping remaining jobs."
        kill_children
        break
      fi
    fi
  done

  merge_matrix_csv

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
    log "[P4] KEEP_TMP=true (tmp runs kept at: $TMP_ROOT)"
  fi

  cat <<EOF

====================== [P4] Summary ======================
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

  if (( fail != 0 )); then
    die "[P4] One or more runs failed (see $LOG_DIR/*.log)."
  fi

  log "[P4] All runs completed successfully."
}

main "$@"
