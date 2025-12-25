#!/usr/bin/env bash
# scripts/run_p7.sh
#
# Projet 7 (ns-3) runner - professional, reproducible, and robust.
#
# What this script does:
#  1) Cleans previous outputs (default).
#  2) Copies (stages) the scenario source into ns-3 scratch/.
#  3) Builds ns-3 (quiet by default, logs to results/p7/logs/build.log).
#  4) Runs the required experiment matrix:
#        channelPlan ∈ {cocanal, separe}
#        nStaPerCell ∈ {2, 5, 10}
#     with deterministic seed/run ids.
#  5) Verifies that required output files exist.
#  6) Prints a concise summary: where files were created.
#  7) Validates the mandatory condition:
#        For N=10, goodputTotal(cocanal) <= goodputTotal(separe)
#
# Usage:
#   chmod +x scripts/run_p7.sh
#   ./scripts/run_p7.sh
#
# Common overrides (examples):
#   NS3_DIR=~/ns-3 OUTDIR=results/p7 ./scripts/run_p7.sh
#   CLEAN=false QUIET_RUN=false ./scripts/run_p7.sh
#   STOP_ON_FIRST_FAIL=true ./scripts/run_p7.sh

set -euo pipefail
IFS=$'\n\t'

# ---------------------------- Defaults / Config ----------------------------
NS3_DIR="${NS3_DIR:-$HOME/ns-3}"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Scenario source in your repo + target name in ns-3 scratch/
SCEN_SRC="${SCEN_SRC:-$ROOT_DIR/scenarios/p7_channel_planning.cc}"
SCEN_NAME="${SCEN_NAME:-p7_channel_planning}"
SCENARIO="${SCENARIO:-scratch/${SCEN_NAME}}"

# Output folder (inside your repo)
OUTDIR="${OUTDIR:-$ROOT_DIR/results/p7}"

# Simulation settings
SIMTIME="${SIMTIME:-25}"
APPSTART="${APPSTART:-2}"
APSEP="${APSEP:-15}"
RSTA="${RSTA:-5}"

# Traffic settings
PKTSIZE="${PKTSIZE:-1200}"
UDPRATE="${UDPRATE:-10Mbps}"

# Channel planning settings
CHAN1="${CHAN1:-36}"
CHAN2="${CHAN2:-40}"
CHWIDTH="${CHWIDTH:-20}"

# Realism knobs (must match your C++ CLI options)
TXPWR="${TXPWR:-16}"
NOISEFIG="${NOISEFIG:-7}"
LOGEXP="${LOGEXP:-3.0}"
SHADOWSIG="${SHADOWSIG:-4.0}"
FADING="${FADING:-true}"

# Extra outputs
PCAP="${PCAP:-false}"
FLOWMON="${FLOWMON:-true}"

# Reproducibility
SEED="${SEED:-1}"
RUN_BASE="${RUN_BASE:-1}"

# Required matrix for Projet 7
NSTAS=(2 5 10)
PLANS=(cocanal separe)

# Behavior toggles
CLEAN="${CLEAN:-true}"                      # Clean old outputs first (default true)
STOP_ON_FIRST_FAIL="${STOP_ON_FIRST_FAIL:-false}"
QUIET_BUILD="${QUIET_BUILD:-true}"          # Build output to build.log only
QUIET_RUN="${QUIET_RUN:-true}"              # Run output to per-run log only
TAIL_LINES="${TAIL_LINES:-180}"             # Tail lines shown on failure
VALIDATE="${VALIDATE:-true}"                # Validate N=10 constraint
SHOW_N10_TOTALS="${SHOW_N10_TOTALS:-true}"  # Print N=10 totals in Mbps

# ---------------------------- Helpers ----------------------------
ts() { date +"%Y-%m-%d %H:%M:%S"; }
log() { echo "[$(ts)] $*"; }
warn() { echo "[$(ts)] WARN: $*" >&2; }
die() { echo "[$(ts)] ERROR: $*" >&2; exit 1; }

need_cmd() { command -v "$1" >/dev/null 2>&1 || die "Missing command: $1"; }

ensure_dirs() {
  mkdir -p "$OUTDIR/raw" "$OUTDIR/logs" "$OUTDIR/plots"
}

clean_outputs() {
  log "[P7] Cleaning previous outputs under: $OUTDIR"
  rm -f "$OUTDIR/raw/p7_summary.csv" || true
  rm -f "$OUTDIR/raw/perflow_"*.csv || true
  rm -f "$OUTDIR/raw/flowmon_"*.xml || true
  rm -f "$OUTDIR/raw/pcap_"*.pcap || true
  rm -f "$OUTDIR/logs/"*.log "$OUTDIR/logs/build.log" || true
}

stage_scenario() {
  [[ -d "$NS3_DIR" ]] || die "NS3_DIR not found: $NS3_DIR"
  [[ -f "$SCEN_SRC" ]] || die "Scenario source not found: $SCEN_SRC"

  local dst="$NS3_DIR/scratch/${SCEN_NAME}.cc"
  cp -f "$SCEN_SRC" "$dst" || die "Copy to ns-3 scratch failed: $dst"
  log "[P7] Staged scenario: $SCEN_SRC -> $dst"
}

build_ns3() {
  log "[P7] Building ns-3 in: $NS3_DIR"
  local build_log="$OUTDIR/logs/build.log"

  if [[ "$QUIET_BUILD" == "true" ]]; then
    ( cd "$NS3_DIR" && ./ns3 build ) >"$build_log" 2>&1 || {
      echo
      warn "[P7] BUILD FAILED. Showing last ${TAIL_LINES} lines from $build_log"
      tail -n "$TAIL_LINES" "$build_log" || true
      die "ns-3 build failed (full log: $build_log)"
    }
  else
    ( cd "$NS3_DIR" && ./ns3 build ) || die "ns-3 build failed"
    : >"$build_log" || true
  fi

  log "[P7] Build OK"
}

run_one() {
  local plan="$1"
  local nsta="$2"
  local runnum="$3"
  local seq="$4"
  local total="$5"


  local tag="${plan}_n${nsta}_run${runnum}"
  local log_path="$OUTDIR/logs/${tag}.log"

  # ✅ FIX: For cocanal, force chan2=chan1 so both cells are truly co-channel (and CSV reflects it)
  local chan2_eff="$CHAN2"
  if [[ "$plan" == "cocanal" ]]; then
    chan2_eff="$CHAN1"
  fi

  local args="\
--outDir=${OUTDIR} \
--simTime=${SIMTIME} --appStart=${APPSTART} \
--nStaPerCell=${nsta} \
--apSeparation=${APSEP} --rSta=${RSTA} \
--pktSize=${PKTSIZE} --udpRatePerSta=${UDPRATE} \
--channelPlan=${plan} --chan1=${CHAN1} --chan2=${chan2_eff} --channelWidth=${CHWIDTH} \
--txPowerDbm=${TXPWR} --noiseFigureDb=${NOISEFIG} \
--logExp=${LOGEXP} --shadowingSigmaDb=${SHADOWSIG} --enableFading=${FADING} \
--pcap=${PCAP} --flowmon=${FLOWMON} \
--seed=${SEED} --run=${runnum}"

log "[P7] [${seq}/${total}] RUN start  plan=${plan}  nStaPerCell=${nsta}  run=${runnum} (chan1=${CHAN1}, chan2=${chan2_eff})"

  if [[ "$QUIET_RUN" == "true" ]]; then
    ( cd "$NS3_DIR" && ./ns3 run "${SCENARIO} ${args}" ) >"$log_path" 2>&1 || {
      echo
      warn "[P7] RUN FAILED: $tag"
      warn "[P7] Log file: $log_path"
      warn "[P7] Last ${TAIL_LINES} lines:"
      tail -n "$TAIL_LINES" "$log_path" || true
      return 1
    }
  else
    ( cd "$NS3_DIR" && ./ns3 run "${SCENARIO} ${args}" ) | tee "$log_path" || {
      echo
      warn "[P7] RUN FAILED: $tag"
      warn "[P7] Log file: $log_path"
      return 1
    }
  fi

log "[P7] [${seq}/${total}] RUN ok     plan=${plan}  nStaPerCell=${nsta}  run=${runnum}"
  return 0
}

exists_ok() {
  local path="$1"
  if [[ -s "$path" ]]; then
    echo "OK   $path"
    return 0
  fi
  echo "MISS $path"
  return 1
}

# Print a sorted list of files matching pattern (relative to OUTDIR)
list_files() {
  local title="$1"
  local pattern="$2"

  echo
  echo "$title"

  local files
  files="$(find "$OUTDIR" -maxdepth 2 -type f -name "$pattern" 2>/dev/null | sort || true)"
  if [[ -n "$files" ]]; then
    printf '%s\n' "$files"
  else
    echo "NONE"
  fi
}

# Extract goodputTotal from p7_summary.csv for (plan, nStaPerCell)
# CSV format:
# channelPlan,nStaPerCell,chan1,chan2,seed,run,goodputCell1,goodputCell2,goodputTotal,jainCells
extract_goodput_total() {
  local plan="$1"
  local nsta="$2"
  local csv="$OUTDIR/raw/p7_summary.csv"
  [[ -f "$csv" ]] || return 1

  awk -F',' -v p="$plan" -v n="$nsta" '
    NR==1 { next }         # skip header
    $1==p && $2==n { val=$9 }
    END {
      if (val=="") exit 1;
      printf "%.10f\n", val;
    }
  ' "$csv"
}

validate_n10_condition() {
  local nsta=10
  local g_cocanal g_separe

  g_cocanal="$(extract_goodput_total "cocanal" "$nsta" || true)"
  g_separe="$(extract_goodput_total "separe" "$nsta" || true)"

  if [[ -z "${g_cocanal:-}" || -z "${g_separe:-}" ]]; then
    warn "[P7] Validation skipped: could not parse goodputTotal for N=10 from p7_summary.csv"
    return 1
  fi

  # Compare floats using awk
  local ok
  ok="$(awk -v a="$g_cocanal" -v b="$g_separe" 'BEGIN{ if (a<=b+1e-9) print "1"; else print "0"; }')"
  if [[ "$ok" != "1" ]]; then
    echo
    warn "[P7] VALIDATION FAILED (N=10): cocanal goodputTotal > separe"
    warn "  cocanal goodputTotal = $g_cocanal bps"
    warn "  separe  goodputTotal = $g_separe bps"
    warn "Possible causes:"
    warn "  - Channels not truly separated (check code: separate must use distinct YansWifiChannel objects)."
    warn "  - Cells too far / weak interference (apSeparation, rSta, TxPower, propagation)."
    warn "  - Different PHY settings between runs."
    return 1
  fi

  log "[P7] Validation OK (N=10): cocanal goodputTotal <= separe"

  if [[ "$SHOW_N10_TOTALS" == "true" ]]; then
    local mc ms ratio
    mc="$(awk -v x="$g_cocanal" 'BEGIN{printf "%.3f", x/1e6}')"
    ms="$(awk -v x="$g_separe"  'BEGIN{printf "%.3f", x/1e6}')"
    ratio="$(awk -v a="$g_cocanal" -v b="$g_separe" 'BEGIN{ if (b>0) printf "%.3f", a/b; else print "nan"; }')"
    log "[P7] N=10 totals: cocanal=${mc} Mbps, separe=${ms} Mbps (ratio=${ratio})"
  fi

  return 0
}

report_outputs() {
  echo
  echo "====================== [P7] Summary ======================"
  echo "OutDir: $OUTDIR"
  echo "Logs : $OUTDIR/logs"
  echo "Raw  : $OUTDIR/raw"
  echo "----------------------------------------------------------"
  echo "[P7] Required files check:"

  exists_ok "$OUTDIR/raw/p7_summary.csv" || true

  # Reconstruct expected file names using the same run numbering logic
  local IDX=0
  for plan in "${PLANS[@]}"; do
    for nsta in "${NSTAS[@]}"; do
      local runnum=$((RUN_BASE + IDX))
      exists_ok "$OUTDIR/raw/perflow_${plan}_n${nsta}_run${runnum}.csv" || true
      if [[ "$FLOWMON" == "true" ]]; then
        exists_ok "$OUTDIR/raw/flowmon_${plan}_n${nsta}_run${runnum}.xml" || true
      fi
      IDX=$((IDX + 1))
    done
  done

  list_files "[P7] Generated per-flow CSVs:" "perflow_*.csv"
  [[ "$FLOWMON" == "true" ]] && list_files "[P7] Generated FlowMonitor XMLs:" "flowmon_*.xml"
  [[ "$PCAP" == "true" ]] && list_files "[P7] Generated PCAPs:" "pcap_*.pcap"
  list_files "[P7] Run logs:" "*.log"

  echo "=========================================================="
}

usage() {
  cat <<EOF
Usage: ./scripts/run_p7.sh

Environment overrides (examples):
  NS3_DIR=~/ns-3 OUTDIR=results/p7 CLEAN=true ./scripts/run_p7.sh
  QUIET_RUN=false ./scripts/run_p7.sh
  STOP_ON_FIRST_FAIL=true ./scripts/run_p7.sh

Key toggles:
  CLEAN=true|false              (default: true)
  QUIET_BUILD=true|false        (default: true)
  QUIET_RUN=true|false          (default: true)
  STOP_ON_FIRST_FAIL=true|false (default: false)
  VALIDATE=true|false           (default: true)
EOF
}

# ---------------------------- Main ----------------------------
main() {
  need_cmd awk
  need_cmd find
  need_cmd tail
  need_cmd sort

  if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
  fi

  ensure_dirs

  # Always clean by default (per your requirement).
  if [[ "$CLEAN" == "true" ]]; then
    clean_outputs
  else
    log "[P7] CLEAN disabled; existing outputs may be overwritten/appended"
  fi

  stage_scenario
  build_ns3

  local FAIL=0
  local IDX=0
    local TOTAL_RUNS=$(( ${#PLANS[@]} * ${#NSTAS[@]} ))


  # Run the experiment matrix
  for plan in "${PLANS[@]}"; do
    for nsta in "${NSTAS[@]}"; do
      local runnum=$((RUN_BASE + IDX))
      local SEQ=$((IDX + 1))
      if ! run_one "$plan" "$nsta" "$runnum" "$SEQ" "$TOTAL_RUNS"; then
        FAIL=$((FAIL + 1))
        if [[ "$STOP_ON_FIRST_FAIL" == "true" ]]; then
          report_outputs
          die "[P7] Aborting on first failure (see logs in $OUTDIR/logs/)"
        fi
      fi
      IDX=$((IDX + 1))
    done
  done

  # Post-run verification + summary
  report_outputs

  # Validate mandatory condition (spec requirement)
  if [[ "$VALIDATE" == "true" ]]; then
    if ! validate_n10_condition; then
      FAIL=$((FAIL + 1))
    fi
  fi

  if [[ $FAIL -ne 0 ]]; then
    die "[P7] Completed with failures: $FAIL (see $OUTDIR/logs/)"
  fi

  log "[P7] All runs completed successfully."
}

main "$@"
