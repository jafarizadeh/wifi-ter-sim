#!/usr/bin/env bash
set -u
set -o pipefail

# -----------------------------------------------------------------------------
# Scientific Wi-Fi 6 Simulation Runner (Hierarchical Data)
# -----------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
NS3_DIR="${NS3_DIR:-$HOME/ns-3}"

BASE_OUT="${REPO_ROOT}/results/p4"
DATA_ROOT="${BASE_OUT}/data"
GLOBAL_CSV="${BASE_OUT}/p4_matrix_global.csv"

# Experiment Parameters
DISTANCES=(5 20)
WIDTHS=(20 40 80)
POWERS=(10 20)
MCSS=(0 7)

# Optional: offered load (change if you want to saturate)
UDP_RATE="${UDP_RATE:-600Mbps}"
PKT_SIZE="${PKT_SIZE:-1200}"
SIM_TIME="${SIM_TIME:-20}"
APP_START="${APP_START:-2}"

SEED="${SEED:-1}"

echo "=================================================================="
echo "   Scientific Wi-Fi 6 Simulation Runner (Hierarchical Data)       "
echo "=================================================================="
echo "Base Output: $BASE_OUT"
echo "NS-3 Dir:    $NS3_DIR"
echo "UDP_RATE:    $UDP_RATE"
echo

# -----------------------------------------------------------------------------
# 0) Clean previous data (IMPORTANT)
# -----------------------------------------------------------------------------
echo "[Init] Cleaning previous run data..."
rm -rf "$DATA_ROOT"
mkdir -p "$DATA_ROOT"
rm -f "$GLOBAL_CSV"

# -----------------------------------------------------------------------------
# 1) Build ns-3 once
# -----------------------------------------------------------------------------
echo "[Init] Building ns-3 project..."
( cd "$NS3_DIR" && ./ns3 build ) >/dev/null
echo "[Init] Build OK."
echo

# -----------------------------------------------------------------------------
# 2) Execution Loop
# -----------------------------------------------------------------------------
RUN_COUNT=1
TOTAL_RUNS=$(( ${#DISTANCES[@]} * ${#WIDTHS[@]} * ${#POWERS[@]} * ( ${#MCSS[@]} + 1 ) ))

FAIL_COUNT=0

run_simulation() {
  local d=$1
  local w=$2
  local p=$3
  local mode=$4
  local mcs=$5
  local folder_name=$6

  local run_dir="${DATA_ROOT}/d${d}m/w${w}MHz/p${p}dBm/${folder_name}"
  mkdir -p "$run_dir"

  echo "[${RUN_COUNT}/${TOTAL_RUNS}] Running: D=$d, W=$w, P=$p, ${folder_name}"

  # Each run gets unique RNG run number
  local RNG_RUN="$RUN_COUNT"

  # Run ns-3 (do NOT stop entire script on failure)
  set +e
  ( cd "$NS3_DIR" && ./ns3 run "scratch/p4_phy_mac_sweep \
      --distance=$d \
      --channelWidth=$w \
      --txPowerDbm=$p \
      --rateMode=$mode \
      --mcs=$mcs \
      --udpRate=$UDP_RATE \
      --pktSize=$PKT_SIZE \
      --simTime=$SIM_TIME \
      --appStart=$APP_START \
      --ssid=wifi6-ter \
      --outDir=$run_dir \
      --pcap=false \
      --flowmon=true \
      --seed=$SEED \
      --run=$RNG_RUN" ) > "$run_dir/console.log" 2>&1
  local status=$?
  set -e

  if [ $status -ne 0 ]; then
    echo "  -> [FAIL] Exit=$status (see $run_dir/console.log)"
    echo "FAIL" > "$run_dir/FAILED"
    FAIL_COUNT=$((FAIL_COUNT+1))
  else
    echo "  -> [OK]"
  fi

  RUN_COUNT=$((RUN_COUNT+1))
}

set -e

for d in "${DISTANCES[@]}"; do
  for w in "${WIDTHS[@]}"; do
    for p in "${POWERS[@]}"; do

      # Constant rate
      for m in "${MCSS[@]}"; do
        run_simulation "$d" "$w" "$p" "constant" "$m" "constant_mcs${m}"
      done

      # Adaptive
      run_simulation "$d" "$w" "$p" "adaptive" "0" "adaptive"

    done
  done
done

echo
echo "=================================================================="
echo "   Aggregating Distributed Data...                                "
echo "=================================================================="

FOUND_CSVS=$(find "$DATA_ROOT" -name "p4_matrix.csv" | sort)

if [ -z "$FOUND_CSVS" ]; then
  echo "[Error] No result CSVs found. Check logs in: $DATA_ROOT"
  exit 1
fi

# Header from first file
FIRST_FILE=$(echo "$FOUND_CSVS" | head -n 1)
head -n 1 "$FIRST_FILE" > "$GLOBAL_CSV"

# Append all (skip headers)
for f in $FOUND_CSVS; do
  tail -n +2 "$f" >> "$GLOBAL_CSV"
done

echo "Success!"
echo "Global Matrix saved to: $GLOBAL_CSV"
echo "Detailed Data tree:     $DATA_ROOT"
echo "Failed runs:            $FAIL_COUNT"
