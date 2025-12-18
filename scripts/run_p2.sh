#!/usr/bin/env bash
set -euo pipefail

# Chemins (adaptez si nécessaire)
NS3_DIR="$HOME/ns-3"
REPO_DIR="$HOME/wifi-ter-sim"

# Programme ns-3 dans scratch
SCRATCH_PROG="scratch/p2_baseline"

# Paramètres communs
SIMTIME=20
APPSTART=2
DIST=5
SSID="wifi-demo"
PKTSIZE=1200

PCAP=true
FLOWMON=true

SEED=1

# Paramètres UDP
UDP_RATE="50Mbps"
UDP_RUN=1

# Paramètres TCP
TCP_MAXBYTES=0
TCP_RUN=2

run_one () {
  local TRANSPORT="$1"   # udp/tcp
  local RUN="$2"         # numéro de run RNG
  local OUTDIR="$3"      # répertoire de sortie (absolu conseillé)

  mkdir -p "$OUTDIR/raw" "$OUTDIR/logs" "$OUTDIR/plots"

  echo "============================================================"
  echo "Execution: transport=$TRANSPORT run=$RUN"
  echo "Sortie   : $OUTDIR"
  echo "============================================================"

  (
    cd "$NS3_DIR"
    ./ns3 run "$SCRATCH_PROG \
      --transport=$TRANSPORT \
      --ssid=$SSID \
      --distance=$DIST \
      --simTime=$SIMTIME \
      --appStart=$APPSTART \
      --pktSize=$PKTSIZE \
      --udpRate=$UDP_RATE \
      --tcpMaxBytes=$TCP_MAXBYTES \
      --pcap=$PCAP \
      --flowmon=$FLOWMON \
      --seed=$SEED \
      --run=$RUN \
      --outDir=$OUTDIR"
  ) | tee "$OUTDIR/logs/console_${TRANSPORT}_run${RUN}.txt"
}

# Separation des sorties pour eviter tout ecrasement (CSV/PCAP)
run_one "udp" "$UDP_RUN" "$REPO_DIR/results/p2/udp/run${UDP_RUN}"
run_one "tcp" "$TCP_RUN" "$REPO_DIR/results/p2/tcp/run${TCP_RUN}"

echo "DONE."
echo "Verifier:"
echo "  $REPO_DIR/results/p2/udp/run${UDP_RUN}"
echo "  $REPO_DIR/results/p2/tcp/run${TCP_RUN}"
