#!/usr/bin/env bash
set -euo pipefail

# --------------------------
# Paths
# --------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

NS3_DIR="${NS3_DIR:-$HOME/ns-3}"

# --------------------------
# Experiment defaults
# --------------------------
PROP_MODEL="${PROP_MODEL:-logdistance}"
LOG_EXP="${LOG_EXP:-3.0}"
REF_DIST="${REF_DIST:-1.0}"
REF_LOSS="${REF_LOSS:-46.6777}"

TX_POWER_DBM="${TX_POWER_DBM:-16}"
NOISE_FIGURE_DB="${NOISE_FIGURE_DB:-7}"
SHADOWING_SIGMA_DB="${SHADOWING_SIGMA_DB:-6}"
ENABLE_FADING="${ENABLE_FADING:-true}"
USE_MINSTREL="${USE_MINSTREL:-true}"

SIM_TIME="${SIM_TIME:-20}"
APP_START="${APP_START:-2}"
PKT_SIZE="${PKT_SIZE:-1200}"

UDP_RATE="${UDP_RATE:-10Mbps}"
TCP_MAX_BYTES="${TCP_MAX_BYTES:-0}"

WIFI_MAC_QUEUE_MAX_SIZE="${WIFI_MAC_QUEUE_MAX_SIZE:-50p}"
WIFI_MAC_QUEUE_MAX_DELAY_MS="${WIFI_MAC_QUEUE_MAX_DELAY_MS:-50}"

PCAP="${PCAP:-false}"
FLOWMON="${FLOWMON:-true}"

SEED="${SEED:-1}"
RUNS="${RUNS:-5}"

DISTANCES=(${DISTANCES:-1 5 10 15 20 25 30})
TRANSPORTS=(${TRANSPORTS:-udp tcp})

# Output base
OUT_BASE="${OUT_BASE:-$REPO_ROOT/results/p3}"

# --------------------------
# AQM options
# --------------------------
ENABLE_AQM="${ENABLE_AQM:-true}"
AQM_QUEUE_DISC="${AQM_QUEUE_DISC:-fq_codel}"
AQM_MAX_SIZE="${AQM_MAX_SIZE:-200p}"

resolve_qdisc_typeid() {
  local name="${1}"
  shopt -s nocasematch
  case "${name}" in
    fq_codel|fq-codel|fqcodel|fq_codel_qdisc) echo "ns3::FqCoDelQueueDisc" ;;
    codel|codel_qdisc) echo "ns3::CoDelQueueDisc" ;;
    pie|pie_qdisc) echo "ns3::PieQueueDisc" ;;
    red|red_qdisc) echo "ns3::RedQueueDisc" ;;
    pfifo_fast|pfifo-fast) echo "ns3::PfifoFastQueueDisc" ;;
    *) echo "" ; return 1 ;;
  esac
}

AQM_TYPEID=""
if [[ "${ENABLE_AQM}" == "true" ]]; then
  if AQM_TYPEID="$(resolve_qdisc_typeid "${AQM_QUEUE_DISC}")"; then
    :
  else
    echo "[P3][WARN] Unknown aqmQueueDisc='${AQM_QUEUE_DISC}'. Disabling AQM."
    ENABLE_AQM="false"
  fi
fi

# --------------------------
# Build once
# --------------------------
echo "[INFO] Building ns-3 once..."
( cd "${NS3_DIR}" && ./ns3 build ) >/dev/null

# --------------------------
# Runner Function
# --------------------------
run_one() {
  local transport="$1"
  local run="$2"
  local distance="$3"

  # Base directory for this Transport/Model combination
  local out_dir="${OUT_BASE}/${transport}/${PROP_MODEL}"
  mkdir -p "${out_dir}"

  # Define the specific subfolder for this distance (e.g., raw/d1m)
  # We use "d${distance}m" to match the C++ naming convention
  local dist_tag="d${distance}m"
  local dist_raw_dir="${out_dir}/raw/${dist_tag}"
  local dist_logs_dir="${out_dir}/logs/${dist_tag}"

  # Create subdirectories immediately
  mkdir -p "${dist_raw_dir}" "${dist_logs_dir}"

  echo "============================================================"
  echo "Execution: transport=${transport} run=${run} distance=${distance}m"
  echo "Output mapped to: ${dist_raw_dir}"
  echo "============================================================"

  local args
  args="scratch/p3_distance_sweep"
  args+=" --transport=${transport}"
  args+=" --ssid=wifi-demo"
  args+=" --distance=${distance}"
  args+=" --simTime=${SIM_TIME}"
  args+=" --appStart=${APP_START}"
  args+=" --pktSize=${PKT_SIZE}"
  args+=" --udpRate=${UDP_RATE}"
  args+=" --tcpMaxBytes=${TCP_MAX_BYTES}"
  args+=" --propModel=${PROP_MODEL}"
  args+=" --logExp=${LOG_EXP}"
  args+=" --refDist=${REF_DIST}"
  args+=" --refLoss=${REF_LOSS}"
  args+=" --txPowerDbm=${TX_POWER_DBM}"
  args+=" --noiseFigureDb=${NOISE_FIGURE_DB}"
  args+=" --shadowingSigmaDb=${SHADOWING_SIGMA_DB}"
  args+=" --enableFading=${ENABLE_FADING}"
  args+=" --useMinstrel=${USE_MINSTREL}"
  args+=" --wifiMacQueueMaxSize=${WIFI_MAC_QUEUE_MAX_SIZE}"
  args+=" --wifiMacQueueMaxDelayMs=${WIFI_MAC_QUEUE_MAX_DELAY_MS}"
  args+=" --pcap=${PCAP}"
  args+=" --flowmon=${FLOWMON}"
  args+=" --seed=${SEED}"
  args+=" --run=${run}"
  args+=" --tag=run${run}"
  # Note: We still pass the parent outDir to C++. 
  # C++ puts everything in outDir/raw/..., then we move it below.
  args+=" --outDir=${out_dir}"

  if [[ "${ENABLE_AQM}" == "true" ]]; then
    args+=" --enableAqm=true"
    args+=" --aqmQueueDisc=${AQM_TYPEID}"
    args+=" --aqmMaxSize=${AQM_MAX_SIZE}"
  else
    args+=" --enableAqm=false"
  fi

  # 1. Execute Simulation
  # Capture console log directly to the specific log folder
  ( cd "${NS3_DIR}" && ./ns3 run "${args}" ) > "${dist_logs_dir}/console_run${run}.txt" 2>&1

  # 2. Organize Files (The "Cleanup" Step)
  # Move files containing the distance tag (e.g., _d1m_) from the main raw folder to the subfolder
  # We exclude p3_sweep.csv because it doesn't have the tag and should stay in root
  
  echo "Moving files for ${dist_tag}..."
  
  # Move trace files (ts_*.csv, rtt_*.csv, flowmon_*.xml, *.pcap)
  # 2>/dev/null suppresses errors if no PCAP files are generated
  mv "${out_dir}/raw/"*"_${dist_tag}_"* "${dist_raw_dir}/" 2>/dev/null || true

  # Ensure the sweep CSV stays in the parent raw folder (no action needed, just confirmation)
}

# --------------------------
# Main Loop
# --------------------------
for transport in "${TRANSPORTS[@]}"; do
  for run in $(seq 1 "${RUNS}"); do
    for d in "${DISTANCES[@]}"; do
      run_one "${transport}" "${run}" "${d}"
    done
  done
done

echo "DONE."
echo "Results organized in: ${OUT_BASE}"
echo "  - Summary: raw/p3_sweep.csv"
echo "  - Details: raw/d{distance}m/ (e.g., raw/d1m/)"