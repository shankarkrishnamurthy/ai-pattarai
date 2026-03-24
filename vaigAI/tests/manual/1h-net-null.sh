#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# ═══════════════════════════════════════════════════════════════════════════════
#  1H — Null Benchmark (net_null)
# ═══════════════════════════════════════════════════════════════════════════════
#  Topology: vaigai TX → net_null0 → /dev/null  (all packets silently dropped)
#            vaigai RX ← net_null0 ← (empty)    (RX always returns zero bursts)
#  Network:  192.168.211.1 (vaigai)
#  No physical NIC, no kernel, no external peer.
#
#  Purpose: measure CPU overhead and code-path latency with no I/O bottleneck.
#  ARP will NOT resolve (requests are dropped; no replies come back).
#  TX counters count ARP requests, not data packets.
#  Key metrics: `stat cpu` (lcore utilization) and `stat net --rate` (TX rate).
#  For functional loopback testing use 1G (net_ring) instead.
#
# ┌─────────────────────── vaigai process ──────────────────────────────────┐
# │                                                                         │
# │   lcore 0: mgmt                                                         │
# │   lcore 1: worker                                                       │
# │                                                                         │
# │   ┌─────────────────────────────────────────────┐                      │
# │   │  net_null0   192.168.211.1                  │                      │
# │   │                                             │                      │
# │   │   TX worker ──► /dev/null  (all dropped)    │                      │
# │   │   RX worker ◄── (always empty)              │                      │
# │   └─────────────────────────────────────────────┘                      │
# │                                                                         │
# │   CPU baseline: no I/O cost, pure code-path overhead.                  │
# │   Key metrics: stat cpu  /  stat net --rate                            │
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
  (no args)   Run vaigai with net_null benchmark
  --server    No-op: net_null needs no external server
  --vaigai    Start vaigai with net_null (interactive)
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
VAIGAI_IP=192.168.211.1
PEER_IP=192.168.211.2

# ── Print traffic & debug commands ───────────────────────────────────────────
print_traffic_commands() {
    local VIP="$1" PIP="$2"
    cat <<CMDS

${BOLD}═══ Benchmark Commands (ARP will not resolve — TX = ARP retries) ═══${NC}

  ${CYAN}# Trigger TX code path — ARP requests flood the null device${NC}
  start --ip $PIP --port 9 --proto udp --size 64 --duration 10 --rate 0
  start --ip $PIP --port 9 --proto udp --size 1400 --duration 10 --rate 0

  ${CYAN}# Control${NC}
  stop
  reset

${BOLD}═══ Key Metrics ═══${NC}
  stat cpu                      # lcore utilization (primary benchmark metric)
  stat net --rate               # TX rate (ARP request throughput)
  stat net                      # raw counters
  show interface                # verify null device is up
  quit

${BOLD}Note:${NC} ARP never resolves on net_null (no RX). Traffic generation enqueues
  packets but they are silently dropped. Use net_ring (1G) for functional tests.
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
    info "Cleaning up 1H..."
    rm -f /dev/hugepages/vaigai_* 2>/dev/null || true
    ok "1H cleanup done"
}

# ── Server ───────────────────────────────────────────────────────────────────
start_server() {
    ok "net_null needs no external server — all TX is silently discarded."
    info "Run --vaigai to start the benchmark."
}

# ── Tgen ─────────────────────────────────────────────────────────────────────
start_tgen() {
    info "Starting vaigai with net_null (interactive — Ctrl+C or 'quit' to exit)..."
    echo ""
    print_traffic_commands "$VAIGAI_IP" "$PEER_IP"
    echo ""
    "$VAIGAI_BIN" -l 0-1 --no-pci --vdev "net_null0" -- -I "$VAIGAI_IP"
}

# ── Dryrun ─────────────────────────────────────────────────────────────────────
dryrun_server() {
    info "[DRYRUN] --server would run:"
    echo "  # net_null needs no external server — this is a no-op"
}

dryrun_tgen() {
    info "[DRYRUN] --vaigai would run:"
    cat <<EOF
  # Hugepages
  echo 256 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages

  # Start vaigai with net_null benchmark
  $VAIGAI_BIN -l 0-1 --no-pci --vdev "net_null0" -- -I $VAIGAI_IP
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
