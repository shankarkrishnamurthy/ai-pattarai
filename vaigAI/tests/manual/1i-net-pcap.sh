#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# ═══════════════════════════════════════════════════════════════════════════════
#  1I — PCAP Replay / Capture (net_pcap)
# ═══════════════════════════════════════════════════════════════════════════════
#  Topology: vaigai RX ← /tmp/vaigai-1i/input.pcap   (synthetic ARP requests)
#            vaigai TX → /tmp/vaigai-1i/output.pcap   (captures generated traffic)
#  Network:  192.168.220.1 (vaigai)
#  No physical NIC, no external peer.
#
#  --server  generates a synthetic input pcap with ARP + ICMP packets aimed at
#            vaigai's IP; vaigai will reply to them and the replies land in the
#            output pcap.  Run `--vaigai`, generate traffic, then inspect
#            output.pcap with `tcpdump -r /tmp/vaigai-1i/output.pcap`.
#
#  Requires: python3 (bundled with DPDK build env), tcpdump (for inspection)
#            DPDK built with net/pcap driver + libpcap installed.
#
# ┌─────────────────────── vaigai process ──────────────────────────────────┐
# │                                                                         │
# │   lcore 0: mgmt                                                         │
# │   lcore 1: worker                                                       │
# │                                                                         │
# │   ┌──────────────────────────────────────────────────────────┐         │
# │   │  net_pcap0   192.168.220.1                               │         │
# │   │                                                          │         │
# │   │   RX ◄─── input.pcap   (ARP requests, replayed once)    │         │
# │   │   TX ────► output.pcap (vaigai ARP replies + traffic)   │         │
# │   └──────────────────────────────────────────────────────────┘         │
# │                                                                         │
# │   File-based: no kernel, no NIC.  Replay captured traffic and          │
# │   capture generated traffic for offline inspection.                    │
# └─────────────────────────────────────────────────────────────────────────┘
# ═══════════════════════════════════════════════════════════════════════════════

set -euo pipefail

VAIGAI_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
VAIGAI_BIN="$VAIGAI_DIR/build/vaigai"

# ── Colours & logging ────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[0;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'
info()  { echo -e "${CYAN}[INFO]${NC}  $*"; }
ok()    { echo -e "${GREEN}[OK]${NC}    $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
err()   { echo -e "${RED}[ERR]${NC}   $*"; }

usage() {
    cat <<EOF
Usage: $(basename "$0") [--server | --vaigai | --cleanup]
  (no args)   Generate pcap then run vaigai
  --server    Generate synthetic input.pcap (required before --vaigai)
  --vaigai    Run vaigai replaying input.pcap; TX captured to output.pcap
  --cleanup   Remove pcap files and state directory
  --dryrun    Show commands without executing them (combine with --server/--vaigai/--cleanup)
EOF
}

# ── Parse arguments ──────────────────────────────────────────────────────────
MODE=""
DRYRUN=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --server)  MODE="server" ;;
        --vaigai)  MODE="vaigai" ;;
        --cleanup) MODE="cleanup" ;;
        --dryrun)  DRYRUN=1 ;;
        -h|--help) usage; exit 0 ;;
        *) err "Unknown option: $1"; usage; exit 1 ;;
    esac
    shift
done

# ── Pre-flight checks ────────────────────────────────────────────────────────
[[ $EUID -ne 0 ]] && (( ! DRYRUN )) && { err "Must run as root"; exit 1; }
if [[ "$MODE" == "vaigai" || -z "$MODE" ]] && (( ! DRYRUN )); then
    [[ ! -x "$VAIGAI_BIN" ]] && { err "vaigai not built — run: ninja -C $VAIGAI_DIR/build"; exit 1; }
fi
if [[ "$MODE" == "server" || -z "$MODE" ]] && (( ! DRYRUN )); then
    command -v python3 >/dev/null 2>&1 || { err "python3 not found"; exit 1; }
fi
if [[ "$MODE" == "server" || "$MODE" == "vaigai" || -z "$MODE" ]] && (( ! DRYRUN )); then
    if ! find /usr/local/lib* -name "librte_net_pcap.so*" 2>/dev/null | grep -q .; then
        err "net_pcap PMD not found — ensure libpcap-devel is installed and DPDK includes net/pcap:"
        err "  dnf install -y libpcap-devel"
        err "  meson setup --wipe build && ninja -C build"
        exit 1
    fi
fi

# ── Constants ────────────────────────────────────────────────────────────────
VAIGAI_IP=192.168.220.1
PEER_IP=192.168.220.2
STATE_DIR=/tmp/vaigai-1i
PCAP_IN="$STATE_DIR/input.pcap"
PCAP_OUT="$STATE_DIR/output.pcap"

# ── Print traffic & debug commands ───────────────────────────────────────────
print_traffic_commands() {
    local SIP="$1"
    cat <<CMDS

${BOLD}═══ Traffic Commands (TX is captured to $PCAP_OUT) ═══${NC}

  ${CYAN}# vaigai receives ARP+ICMP from input.pcap and replies to output.pcap${NC}
  ${CYAN}# Send additional traffic — goes to output.pcap${NC}
  ping $SIP
  start --ip $SIP --port 9 --proto udp --size 64 --duration 5
  start --ip $SIP --port 9 --proto udp --size 1400 --duration 5

  ${CYAN}# Control${NC}
  stop
  reset

${BOLD}═══ Monitoring & Inspection ═══${NC}
  stat net
  stat net --rate
  show interface
  quit

${BOLD}After quitting, inspect captured output:${NC}
  tcpdump -r $PCAP_OUT -n -c 20
CMDS
}

# ── Hugepages ────────────────────────────────────────────────────────────────
setup_hugepages() {
    local _cur
    _cur=$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages)
    if (( _cur < 256 )); then
        echo 256 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
        info "Hugepages set to 256 × 2 MB"
    fi
    rm -f /dev/hugepages/vaigai_* 2>/dev/null || true
}

# ── Cleanup ──────────────────────────────────────────────────────────────────
do_cleanup() {
    info "Cleaning up 1I..."
    rm -rf "$STATE_DIR"
    rm -f /dev/hugepages/vaigai_* 2>/dev/null || true
    ok "1I cleanup done"
}

# ── Server: generate synthetic input pcap ────────────────────────────────────
start_server() {
    mkdir -p "$STATE_DIR"
    rm -f "$PCAP_IN" "$PCAP_OUT"

    info "Generating synthetic input pcap ($PCAP_IN)..."
    python3 <<PYEOF
import struct

VAIGAI_IP = bytes([192, 168, 220, 1])
PEER_IP   = bytes([192, 168, 220, 2])
PEER_MAC  = bytes.fromhex('020000000002')
BCAST_MAC = b'\xff' * 6
VAIGAI_MAC= bytes.fromhex('020000000001')

def pcap_global_hdr():
    # magic, ver_major, ver_minor, thiszone, sigfigs, snaplen, linktype=Ethernet
    return struct.pack('<IHHiIII', 0xa1b2c3d4, 2, 4, 0, 0, 65535, 1)

def pcap_pkt_hdr(pkt):
    return struct.pack('<IIII', 0, 0, len(pkt), len(pkt))

# ARP request: who has VAIGAI_IP? tell PEER_IP
arp = (BCAST_MAC + PEER_MAC + b'\x08\x06'
       + b'\x00\x01\x08\x00\x06\x04\x00\x01'
       + PEER_MAC + PEER_IP
       + b'\x00\x00\x00\x00\x00\x00' + VAIGAI_IP)

with open('$PCAP_IN', 'wb') as f:
    f.write(pcap_global_hdr())
    # 30 ARP requests — vaigai will reply to each (captured in output.pcap)
    for _ in range(30):
        f.write(pcap_pkt_hdr(arp) + arp)

print('Created $PCAP_IN: 30 ARP requests → vaigai at $VAIGAI_IP')
PYEOF

    ok "Input pcap ready: $PCAP_IN"
    info "Run --vaigai to start replaying. TX responses go to $PCAP_OUT"
}

# ── Tgen ─────────────────────────────────────────────────────────────────────
start_tgen() {
    if [[ ! -f "$PCAP_IN" ]]; then
        err "Input pcap $PCAP_IN not found — run --server first"
        exit 1
    fi

    info "Starting vaigai with net_pcap (interactive — Ctrl+C or 'quit' to exit)..."
    info "RX replays: $PCAP_IN"
    info "TX captured to: $PCAP_OUT"
    echo ""
    print_traffic_commands "$PEER_IP"
    echo ""

    "$VAIGAI_BIN" -l 0-1 --no-pci \
        --vdev "net_pcap0,rx_pcap=$PCAP_IN,tx_pcap=$PCAP_OUT" \
        -- -I "$VAIGAI_IP"

    if [[ -f "$PCAP_OUT" ]]; then
        echo ""
        info "Captured output ($PCAP_OUT):"
        tcpdump -r "$PCAP_OUT" -n -c 20 2>/dev/null || true
    fi
}

# ── Dryrun ─────────────────────────────────────────────────────────────────────
dryrun_server() {
    info "[DRYRUN] --server would run:"
    cat <<EOF
  # Generate synthetic ARP pcap -> $PCAP_IN
  mkdir -p $STATE_DIR
  python3 <<PYEOF  # writes 30 ARP requests targeting $VAIGAI_IP
  PYEOF
EOF
}

dryrun_tgen() {
    info "[DRYRUN] --vaigai would run:"
    cat <<EOF
  # Hugepages
  echo 256 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages

  # Start vaigai with net_pcap replay/capture
  $VAIGAI_BIN -l 0-1 --no-pci \\
      --vdev "net_pcap0,rx_pcap=$PCAP_IN,tx_pcap=$PCAP_OUT" \\
      -- -I $VAIGAI_IP

  # Inspect captured output
  tcpdump -r $PCAP_OUT -n -c 20
EOF
}

dryrun_cleanup() {
    info "[DRYRUN] --cleanup would run:"
    cat <<EOF
  rm -rf $STATE_DIR
  rm -f /dev/hugepages/vaigai_*
EOF
}

# ── Main ─────────────────────────────────────────────────────────────────────
case "$MODE" in
    server)
        if (( DRYRUN )); then dryrun_server; exit 0; fi
        setup_hugepages
        start_server
        ;;
    vaigai)
        if (( DRYRUN )); then dryrun_tgen; exit 0; fi
        setup_hugepages
        start_tgen
        ;;
    cleanup)
        if (( DRYRUN )); then dryrun_cleanup; exit 0; fi
        do_cleanup
        ;;
    "")
        if (( DRYRUN )); then dryrun_server; echo ""; dryrun_tgen; exit 0; fi
        setup_hugepages
        start_server
        start_tgen
        ;;
esac
