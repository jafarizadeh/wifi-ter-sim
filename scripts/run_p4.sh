#!/usr/bin/env bash
# scripts/run_p4.sh
# Part 4 runner (parallel, per-run raw only, safe merge)
#
# Output layout:
#   results/p4/
#     logs/   -> build.log, prog_help.log, <tag>.log, plots.log
#     plots/  -> plot_p4 outputs
#     raw/
#       p4_matrix.csv
#       per_run/<tag>/   (RAW FILES ONLY)

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

PER_RUN_DIR="$RAW_DIR/per_run"

CSV_OUT="$RAW_DIR/p4_matrix.csv"
CSV_HEADER="distance,channelWidth,txPowerDbm,rateMode,mcs,udpRate,pktSize,seed,run,rxBytes,goodputMbps,rttMeanMs"

# Clean policy:
# - "all": wipe raw/logs/plots
# - "soft": wipe raw/logs/plots (same behavior here; kept for compatibility)
CLEAN_MODE="${CLEAN_MODE:-soft}"          # soft|all
STOP_ON_FIRST_FAIL="${STOP_ON_FIRST_FAIL:-false}"
TAIL_LINES="${TAIL_LINES:-120}"

# Python venv (no installs here)
VENV_DIR="${VENV_DIR:-$ROOT_DIR/.venv}"
PY="$VENV_DIR/bin/python"
STRICT_PNG="${STRICT_PNG:-false}"

# ---- CPU knobs ----
BUILD_JOBS="${BUILD_JOBS:-2}"
JOBS="${JOBS:-}"              # if empty => auto: min(MAX_JOBS, max(1, nproc/2))
MAX_JOBS="${MAX_JOBS:-6}"
LOAD_MAX="${LOAD_MAX:-}"      # empty => ~0.75*cores
BACKOFF_SLEEP_SEC="${BACKOFF_SLEEP_SEC:-1}"
LAUNCH_STAGGER_MS="${LAUNCH_STAGGER_MS:-200}"

NICE_LEVEL="${NICE_LEVEL:-15}"
IONICE_CLASS="${IONICE_CLASS:-2}"
IONICE_LEVEL="${IONICE_LEVEL:-7}"
CPULIMIT_PCT="${CPULIMIT_PCT:-}"          # empty => disabled

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
need_cmd() { command -v "$1" >/dev/null 2>&1 || die "Missing command: $1"; }

ensure_dirs() { mkdir -p "$RAW_DIR" "$LOG_DIR" "$PLOT_DIR" "$PER_RUN_DIR"; }

clean_outputs() {
  log "[P4] Cleaning previous outputs under: $OUTDIR (mode=$CLEAN_MODE)"
  rm -rf "$RAW_DIR" "$LOG_DIR" "$PLOT_DIR" || true
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
  log "[P4] Building ns-3"
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
      warn "[P4] Could not fetch program help (see $help_log)."
      : > "$help_log"
    }
  fi
  echo "$help_log"
}

arg_supported() {
  local help_file="$1" argname="$2"
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
  done < <(find "$PER_RUN_DIR" -type f -name "p4_matrix.csv" | sort)
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
    if (( running >= JOBS )); then
      if wait -n; then
        running=$((running-1))
      else
        fail=1
        running=$((running-1))
        if [[ "$STOP_ON_FIRST_FAIL" == "true" ]]; then
          warn "[P4] STOP_ON_FIRST_FAIL=true -> stopping remaining jobs."
          kill_children
          break
        fi
      fi
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

  if have_cmd cpulimit && [[ -n "${CPULIMIT_PCT:-}" ]]; then
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

  # Filter EXTRA_ARGS against --PrintHelp (best-effort)
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
    local IFS=' '
    # shellcheck disable=SC2206
    args+=( $EXTRA_ARGS )
  fi

  echo "${args[*]}"
}

normalize_per_run_raw_only() {
  # Goal: inside $out_run/ keep ONLY raw files (no raw/, logs/, plots/)
  local out_run="$1"

  # If scenario created out_run/raw, move its contents to out_run/
  if [[ -d "$out_run/raw" ]]; then
    shopt -s dotglob nullglob
    mv "$out_run/raw"/* "$out_run/" 2>/dev/null || true
    shopt -u dotglob nullglob
    rm -rf "$out_run/raw" 2>/dev/null || true
  fi

  # If scenario created logs/plots under out_run, DO NOT keep them in per_run.
  rm -rf "$out_run/logs" "$out_run/plots" 2>/dev/null || true
}

run_one() {
  local tag="$1" args="$2" seq="$3" total="$4" out_run="$5"
  local run_log="$LOG_DIR/${tag}.log"

  log "[P4] [${seq}/${total}] RUN start ${tag}"

  if ( cd "$NS3_DIR" && ./ns3 run "${SCENARIO} ${args}" --no-build ) >/dev/null 2>&1; then
    run_cmd_wrapped "$run_log" bash -lc "cd \"$NS3_DIR\" && ./ns3 run \"${SCENARIO} ${args}\" --no-build"
  else
    run_cmd_wrapped "$run_log" bash -lc "cd \"$NS3_DIR\" && ./ns3 run \"${SCENARIO} ${args}\""
  fi

  local rc=$?
  if (( rc != 0 )); then
    warn "[P4] RUN FAILED: $tag (rc=$rc) (see $run_log)"
    tail -n "$TAIL_LINES" "$run_log" || true
    return "$rc"
  fi

  normalize_per_run_raw_only "$out_run"
  log "[P4] [${seq}/${total}] RUN ok    ${tag}"
  return 0
}

kill_children() {
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
    log "[P4] Creating venv at: $VENV_DIR (no package install performed)"
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
    print("[P4] ERROR: Missing Python packages in venv: " + ", ".join(missing), file=sys.stderr)
    print("[P4] Install:", file=sys.stderr)
    print("  source .venv/bin/activate", file=sys.stderr)
    print("  python -m pip install -r requirements.txt", file=sys.stderr)
    sys.exit(2)
PY
}

run_plots(){
  [[ -f "$ROOT_DIR/scripts/plot_p4.py" ]] || die "Plot script not found: $ROOT_DIR/scripts/plot_p4.py"
  local plot_log="$LOG_DIR/plots.log"

  log "[P4] [1/1] PLOT start p4_plots"
  if [[ "$STRICT_PNG" == "true" ]]; then
    if "$PY" "$ROOT_DIR/scripts/plot_p4.py" --results "$OUTDIR" --strict-png >"$plot_log" 2>&1; then
      log "[P4] [1/1] PLOT ok    p4_plots"
    else
      warn "[P4] PLOT FAILED (see $plot_log)"
      tail -n "$TAIL_LINES" "$plot_log" || true
      return 1
    fi
  else
    if "$PY" "$ROOT_DIR/scripts/plot_p4.py" --results "$OUTDIR" >"$plot_log" 2>&1; then
      log "[P4] [1/1] PLOT ok    p4_plots"
    else
      warn "[P4] PLOT FAILED (see $plot_log)"
      tail -n "$TAIL_LINES" "$plot_log" || true
      return 1
    fi
  fi
}

# -------------------- main --------------------
main() {
  need_cmd find
  need_cmd tail
  need_cmd grep
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
  (( BUILD_JOBS < 1 )) && BUILD_JOBS=1

  stage_scenario
  build_ns3_low_heat

  local help_file
  help_file="$(get_prog_help)"

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

  # NOTE: these are used inside wait_for_slot_and_load()
  running=0
  fail=0

  local idx=0 run_num tag out_run args item

  for item in "${jobs_list[@]}"; do
    IFS='|' read -r d w p rm m <<< "$item"
    idx=$((idx + 1))
    run_num=$((RUN_BASE + idx - 1))

    tag="$(safe_tag "$d" "$w" "$p" "$rm" "$m" "$SEED" "$run_num")"
    out_run="$PER_RUN_DIR/$tag"
    mkdir -p "$out_run"

    args="$(build_args_for_run "$help_file" "$out_run" "$d" "$w" "$p" "$rm" "$m" "$run_num")"

    wait_for_slot_and_load
    [[ "$STOP_ON_FIRST_FAIL" == "true" && "$fail" -ne 0 ]] && break

    ( run_one "$tag" "$args" "$idx" "$total" "$out_run" ) &
    running=$((running+1))

    if (( LAUNCH_STAGGER_MS > 0 )); then
      python3 - <<PY >/dev/null 2>&1 || true
import time
time.sleep(${LAUNCH_STAGGER_MS}/1000.0)
PY
    fi
  done

  # Drain remaining jobs
  while (( running > 0 )); do
    if wait -n; then
      running=$((running-1))
    else
      fail=1
      running=$((running-1))
      if [[ "$STOP_ON_FIRST_FAIL" == "true" ]]; then
        warn "[P4] STOP_ON_FIRST_FAIL=true -> stopping remaining jobs."
        kill_children
        break
      fi
    fi
  done

  merge_matrix_csv

  ensure_venv
  check_plot_deps_or_die
  run_plots

  (( fail == 0 )) || die "[P4] One or more runs failed (see $LOG_DIR/*.log)."
  log "[P4] Completed. Final CSV: $CSV_OUT"
}

main "$@"
