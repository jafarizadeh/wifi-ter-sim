#!/usr/bin/env bash
set -euo pipefail
IFS=$'\n\t'

# ============================================================
# Part 4 runner (equivalent structure to Script 9)
# Scenario: p4_phy_mac_sweep
#
# Key properties (مثل اسکریپت 9):
#  - tmp_runs برای هر اجرا (عدم آسیب به داده‌ها)
#  - اجرای موازی JOBS (پیش‌فرض 6)
#  - Merge خروجی‌های CSV در یک فایل نهایی
#  - Build فقط یک بار (و کم‌فشار) — در هر Run بیلد موازی انجام نمی‌شود
#  - کنترل فشار/دما: nice + ionice + (در صورت وجود) cpulimit
#
# IMPORTANT:
#  - طبق خروجی شما، گزینه --useMinstrelHe در کد فعلی وجود ندارد
#    پس این اسکریپت آن را پاس نمی‌دهد (مثل خطای شما).
# ============================================================

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

CSV_OUT="${CSV_OUT:-$RAW_DIR/p4_matrix.csv}"
CSV_HEADER="distance,channelWidth,txPowerDbm,rateMode,mcs,udpRate,pktSize,seed,run,rxBytes,goodputMbps,rttMeanMs"

CLEAN="${CLEAN:-true}"
KEEP_TMP="${KEEP_TMP:-false}"
STOP_ON_FIRST_FAIL="${STOP_ON_FIRST_FAIL:-false}"

# -------------------- parallelism --------------------
JOBS="${JOBS:-6}"
MAX_JOBS="${MAX_JOBS:-8}"
TAIL_LINES="${TAIL_LINES:-120}"
START_STAGGER_S="${START_STAGGER_S:-0.35}"   # برای جلوگیری از پیک همزمان

# -------------------- build control (کم فشار) --------------------
DO_BUILD="${DO_BUILD:-true}"
BUILD_JOBS="${BUILD_JOBS:-2}"                # برای کاهش دما/فشار، بیلد کم‌پردازشی
NO_BUILD_FLAG="${NO_BUILD_FLAG:-auto}"       # auto|true|false  (auto: اگر ns3 run --no-build داشت)

# -------------------- thermal/load limiting --------------------
NICE_LVL="${NICE_LVL:-10}"
IONICE_CLASS="${IONICE_CLASS:-2}"
IONICE_PRIO="${IONICE_PRIO:-7}"
CPU_LIMIT_PER_JOB="${CPU_LIMIT_PER_JOB:-10}"  # فقط اگر cpulimit نصب باشد (درصد از یک CPU)

# -------------------- sweep parameters (override with env) --------------------
SIM_TIME="${SIM_TIME:-20}"
APP_START="${APP_START:-2}"

SEED="${SEED:-1}"
RUN_BASE="${RUN_BASE:-1}"   # run = RUN_BASE + idx

# Grid sets (با env هم می‌توانید override کنید)
DISTANCES=(${DISTANCES_OVERRIDE:-5 10 20 30})
WIDTHS=(${WIDTHS_OVERRIDE:-20 40 80})
TXPOWERS=(${TXPOWERS_OVERRIDE:-12 16 20})
RATEMODES=(${RATEMODES_OVERRIDE:-adaptive constant})
MCS_LIST=(${MCS_LIST_OVERRIDE:-0 3 7 9})     # فقط برای constant استفاده می‌شود

# Extra args for scenario
SSID="${SSID:-wifi6-ter}"
UDPRATE="${UDPRATE:-600Mbps}"
PKTSIZE="${PKTSIZE:-1200}"

LOGEXP="${LOGEXP:-3.0}"
REFDIST="${REFDIST:-1.0}"
REFLOSS="${REFLOSS:-46.6777}"
NOISEFIG="${NOISEFIG:-7.0}"

SHADOWING="${SHADOWING:-false}"
SHADOWSIGMA="${SHADOWSIGMA:-5.0}"
SHADOWUPDATE="${SHADOWUPDATE:-1.0}"
FADING="${FADING:-false}"

RTTHZ="${RTTHZ:-2.0}"
RTTPAYLOAD="${RTTPAYLOAD:-32}"
RTTVERBOSE="${RTTVERBOSE:-false}"

PCAP="${PCAP:-false}"
FLOWMON="${FLOWMON:-true}"

USE_MINSTREL="${USE_MINSTREL:-true}"

# Optional manual cases:
# CASES="d:w:p:mode:mcs,d:w:p:mode:mcs,..."  (اگر ست می‌خواهید دقیق کنترل کنید)
CASES="${CASES:-}"

EXTRA_ARGS="${EXTRA_ARGS:-}"  # هر چیز اضافی که کد می‌پذیرد

# -------------------- helpers --------------------
ts() { date +"%Y-%m-%d %H:%M:%S"; }
log() { echo "[$(ts)] $*"; }
warn() { echo "[$(ts)] WARN: $*" >&2; }
die() { echo "[$(ts)] ERROR: $*" >&2; exit 1; }

have_cpulimit() { command -v cpulimit >/dev/null 2>&1; }

ensure_dirs() {
  mkdir -p "$RAW_DIR" "$LOG_DIR" "$PLOT_DIR"
}

clean_outputs() {
  log "[P4] Cleaning previous outputs under: $OUTDIR"
  rm -rf "$RAW_DIR" "$LOG_DIR" "$PLOT_DIR" "$TMP_ROOT" || true
  ensure_dirs
}

stage_scenario() {
  [[ -d "$NS3_DIR" ]] || die "NS3_DIR not found: $NS3_DIR"
  [[ -f "$SCEN_SRC" ]] || die "Scenario source not found: $SCEN_SRC"
  cp -f "$SCEN_SRC" "$NS3_DIR/scratch/${SCEN_NAME}.cc" || die "Copy to ns-3 scratch failed"
  log "[P4] Staged scenario: $SCEN_SRC -> $NS3_DIR/scratch/${SCEN_NAME}.cc"
}

build_ns3() {
  [[ "$DO_BUILD" == "true" ]] || { log "[P4] DO_BUILD=false (skipping build)"; return 0; }
  log "[P4] Building ns-3 (low pressure) in: $NS3_DIR  (BUILD_JOBS=${BUILD_JOBS})"
  local build_log="$LOG_DIR/build.log"
  ( cd "$NS3_DIR" && NINJAFLAGS="-j${BUILD_JOBS}" ./ns3 build ) >"$build_log" 2>&1 || {
    warn "[P4] BUILD FAILED. Last ${TAIL_LINES} lines:"
    tail -n "$TAIL_LINES" "$build_log" || true
    die "ns-3 build failed (full log: $build_log)"
  }
  log "[P4] Build OK"
}

ns3_run_supports_no_build() {
  ( cd "$NS3_DIR" && ./ns3 run --help 2>/dev/null | grep -q -- "--no-build" ) || return 1
  return 0
}

use_no_build() {
  if [[ "$NO_BUILD_FLAG" == "true" ]]; then
    return 0
  elif [[ "$NO_BUILD_FLAG" == "false" ]]; then
    return 1
  else
    ns3_run_supports_no_build
  fi
}

safe_tag() {
  # similar style to p9 tags
  local d="$1" w="$2" p="$3" mode="$4" mcs="$5" seed="$6" run="$7"
  local sd="${d//-/_m}"; sd="${sd//./p}"
  local sw="${w//-/_m}"; sw="${sw//./p}"
  local sp="${p//-/_m}"; sp="${sp//./p}"
  echo "p4_d${sd}_w${sw}_p${sp}_${mode}_mcs${mcs}_s${seed}_r${run}"
}

common_args() {
  local out="$1" d="$2" w="$3" p="$4" mode="$5" mcs="$6" run="$7"

  local args="--simTime=${SIM_TIME} --appStart=${APP_START} \
--distance=${d} --channelWidth=${w} --txPowerDbm=${p} \
--rateMode=${mode} --mcs=${mcs} --useMinstrel=${USE_MINSTREL} \
--ssid=${SSID} --udpRate=${UDPRATE} --pktSize=${PKTSIZE} \
--logExp=${LOGEXP} --refDist=${REFDIST} --refLoss=${REFLOSS} --noiseFigureDb=${NOISEFIG} \
--enableShadowing=${SHADOWING} --shadowSigmaDb=${SHADOWSIGMA} --shadowUpdateS=${SHADOWUPDATE} \
--enableFading=${FADING} \
--rttHz=${RTTHZ} --rttPayloadSize=${RTTPAYLOAD} --rttVerbose=${RTTVERBOSE} \
--pcap=${PCAP} --flowmon=${FLOWMON} \
--seed=${SEED} --run=${run} \
--outDir=${out} --tag="
  if [[ -n "${EXTRA_ARGS}" ]]; then
    args+=" ${EXTRA_ARGS}"
  fi
  echo "$args"
}

wait_for_slot() {
  while (( $(jobs -rp | wc -l) >= JOBS )); do
    wait -n || true
  done
}

run_one() {
  local tag="$1" args="$2" seq="$3" total="$4"
  local run_log="$LOG_DIR/${tag}.log"

  log "[P4] [${seq}/${total}] RUN start  ${tag}"

  (
    cd "$NS3_DIR"

    local -a cmd
    if use_no_build; then
      cmd=( ./ns3 run --no-build "${SCENARIO} ${args}" )
    else
      cmd=( ./ns3 run "${SCENARIO} ${args}" )
    fi

    if have_cpulimit; then
      cpulimit -l "$CPU_LIMIT_PER_JOB" -- \
        nice -n "$NICE_LVL" ionice -c"$IONICE_CLASS" -n"$IONICE_PRIO" \
        "${cmd[@]}"
    else
      nice -n "$NICE_LVL" ionice -c"$IONICE_CLASS" -n"$IONICE_PRIO" \
        "${cmd[@]}"
    fi
  ) >"$run_log" 2>&1 || {
    warn "[P4] RUN FAILED: $tag"
    warn "[P4] Log file: $run_log"
    tail -n "$TAIL_LINES" "$run_log" || true
    return 1
  }

  log "[P4] [${seq}/${total}] RUN ok     ${tag}"
  return 0
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
  done < <(find "$TMP_ROOT" -type f -path "*/raw/p4_matrix.csv" | sort)

  log "[P4] Merge done -> $CSV_OUT"
}

copy_per_run_raw() {
  local dst="$RAW_DIR/per_run_raw"
  mkdir -p "$dst"

  local d
  while IFS= read -r d; do
    local tag
    tag="$(basename "$d")"
    mkdir -p "$dst/$tag"
    if [[ -d "$d/raw" ]]; then
      cp -a "$d/raw/." "$dst/$tag/" 2>/dev/null || true
    fi
  done < <(find "$TMP_ROOT" -mindepth 1 -maxdepth 1 -type d | sort)
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
  build_ns3

  log "[P4] simTime=${SIM_TIME}s appStart=${APP_START}s seed=${SEED} jobs=${JOBS} buildJobs=${BUILD_JOBS}"
  if have_cpulimit; then
    log "[P4] cpulimit enabled: per-job=${CPU_LIMIT_PER_JOB}% (reduce to cool down)"
  else
    log "[P4] cpulimit not found (optional). nice/ionice enabled."
  fi
  [[ -n "${EXTRA_ARGS}" ]] && log "[P4] EXTRA_ARGS=${EXTRA_ARGS}"
  [[ -n "${CASES}" ]] && log "[P4] CASES provided (manual list)"

  # Build run list
  local -a runs=()
  if [[ -n "${CASES}" ]]; then
    IFS=',' read -r -a runs <<< "$CASES"
  else
    local d w p mode mcs
    for d in "${DISTANCES[@]}"; do
      for w in "${WIDTHS[@]}"; do
        for p in "${TXPOWERS[@]}"; do
          for mode in "${RATEMODES[@]}"; do
            if [[ "$mode" == "adaptive" ]]; then
              runs+=( "${d}:${w}:${p}:${mode}:0" )
            else
              for mcs in "${MCS_LIST[@]}"; do
                runs+=( "${d}:${w}:${p}:${mode}:${mcs}" )
              done
            fi
          done
        done
      done
    done
  fi

  local total="${#runs[@]}"
  log "[P4] Total runs=${total}"

  local idx=0 fail=0
  for item in "${runs[@]}"; do
    local d="${item%%:*}"
    local rest="${item#*:}"
    local w="${rest%%:*}"; rest="${rest#*:}"
    local p="${rest%%:*}"; rest="${rest#*:}"
    local mode="${rest%%:*}"
    local mcs="${rest##*:}"

    local runnum=$((RUN_BASE + idx))
    local seq=$((idx + 1))

    local tag
    tag="$(safe_tag "$d" "$w" "$p" "$mode" "$mcs" "$SEED" "$runnum")"

    local out_run="$TMP_ROOT/$tag"
    mkdir -p "$out_run/raw" "$out_run/logs" "$out_run/plots"

    local args
    args="$(common_args "$out_run" "$d" "$w" "$p" "$mode" "$mcs" "$runnum")"

    run_one "$tag" "$args" "$seq" "$total" || {
      fail=$((fail + 1))
      [[ "$STOP_ON_FIRST_FAIL" == "true" ]] && break
    } &

    sleep "$START_STAGGER_S"
    wait_for_slot
    idx=$((idx + 1))
  done

  wait || true

  merge_matrix_csv
  copy_per_run_raw

  if [[ "$KEEP_TMP" != "true" ]]; then
    rm -rf "$TMP_ROOT" || true
  else
    log "[P4] KEEP_TMP=true (tmp runs kept at: $TMP_ROOT)"
  fi

  log "[P4] Outputs:"
  log "  raw   : $RAW_DIR"
  log "  logs  : $LOG_DIR"
  log "  plots : $PLOT_DIR"
  log "  merged: $CSV_OUT"
  log "  per-run raw: $RAW_DIR/per_run_raw/"

  if [[ "$fail" -ne 0 ]]; then
    die "[P4] Completed with failures: $fail (see $LOG_DIR)"
  fi
  log "[P4] All runs completed successfully."
}

main "$@"
