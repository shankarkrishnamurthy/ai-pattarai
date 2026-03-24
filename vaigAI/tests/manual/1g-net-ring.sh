#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# ═══════════════════════════════════════════════════════════════════════════════
#  1G — Software Loopback (net_ring)
# ═══════════════════════════════════════════════════════════════════════════════
#  Topology: vaigai TX → [rte_ring] → vaigai RX  (same process, zero-copy)
#  Network:  192.168.210.1 (vaigai self-loopback)
#  No physical NIC, no kernel, no external peer.
#
#  Packets transmitted by vaigai loop back immediately to its own RX path.
#  ARP resolves against itself: vaigai answers its own ARP broadcast.
#  Use `ping 192.168.210.1` first to bootstrap ARP, then run start commands.
#
# ┌─────────────────────── vaigai process ──────────────────────────────────┐
# │                                                                         │
# │   lcore 0: mgmt                                                         │
# │   lcore 1: worker                                                       │
# │                                                                         │
# │   ┌─────────────────────────────────────────────┐                      │
# │   │  net_ring0   192.168.210.1                  │                      │
# │   │                                             │                      │
# │   │   TX worker ──[rte_ring]──► RX worker       │                      │
# │   │              (zero-copy)                    │                      │
# │   └─────────────────────────────────────────────┘                      │
# │                                                                         │
# │   No NIC, no kernel, no external peer.                                  │
# │   TX pps == RX pps.  Use `stat net` to confirm.                        │
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
  (no args)   Run vaigai with net_ring loopback
  --server    No-op: net_ring needs no external server
  --vaigai    Start vaigai with net_ring loopback (interactive)
  --cleanup   Remove hugepage files
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

# ── Constants ────────────────────────────────────────────────────────────────
VAIGAI_IP=192.168.210.1

# ── Print traffic & debug commands ───────────────────────────────────────────
print_traffic_commands() {
    local SIP="$1"
    cat <<CMDS

${BOLD}═══ Traffic Commands (loopback — TX and RX counters both increment) ═══${NC}

  ${CYAN}# Bootstrap ARP first (vaigai answers its own ARP broadcast)${NC}
  ping $SIP

  ${CYAN}# UDP flood — TX pps == RX pps on loopback${NC}
  start --ip $SIP --port 9 --proto udp --size 64 --duration 5
  start --ip $SIP --port 9 --proto udp --size 1400 --duration 5 --rate 100000

  ${CYAN}# TCP — SYN loops back; vaigai sees incoming SYN on same IP${NC}
  start --ip $SIP --port 5000 --proto tcp --duration 5

  ${CYAN}# Control${NC}
  stop
  reset

${BOLD}═══ Monitoring & Debug ═══${NC}
  stat net                      # TX == RX counters confirm loopback
  stat net --rate               # per-second rates
  stat cpu                      # CPU utilization per lcore
  show interface
  trace start /tmp/capture.pcapng
  trace stop
  quit
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
    info "Cleaning up 1G..."
    rm -f /dev/hugepages/vaigai_* 2>/dev/null || true
    ok "1G cleanup done"
}

# ── Server ───────────────────────────────────────────────────────────────────
start_server() {
    ok "net_ring needs no external server — TX rings loop back to RX rings."
    info "Run --vaigai to start testing."
}

# ── Tgen ─────────────────────────────────────────────────────────────────────
start_tgen() {
    info "Starting vaigai with net_ring loopback (interactive — Ctrl+C or 'quit' to exit)..."
    echo ""
    print_traffic_commands "$VAIGAI_IP"
    echo ""
    "$VAIGAI_BIN" -l 0-1 --no-pci --vdev "net_ring0" -- -I "$VAIGAI_IP"
}

# ── Dryrun ─────────────────────────────────────────────────────────────────────
dryrun_server() {
    info "[DRYRUN] --server would run:"
    echo "  # net_ring needs no external server — this is a no-op"
}

dryrun_tgen() {
    info "[DRYRUN] --vaigai would run:"
    cat <<EOF
  # Hugepages
  echo 256 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages

  # Start vaigai with net_ring loopback
  $VAIGAI_BIN -l 0-1 --no-pci --vdev "net_ring0" -- -I $VAIGAI_IP
EOF
}

dryrun_cleanup() {
    info "[DRYRUN] --cleanup would run:"
    echo "  rm -f /dev/hugepages/vaigai_*"
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
