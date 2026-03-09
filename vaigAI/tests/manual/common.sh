#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# vaigAI — Common helpers for manual test scripts.
# Sourced by each test script; not meant to be run directly.

set -euo pipefail

VAIGAI_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
VAIGAI_BIN="$VAIGAI_DIR/build/vaigai"

# ── Colours ──────────────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[0;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'

info()  { echo -e "${CYAN}[INFO]${NC}  $*"; }
ok()    { echo -e "${GREEN}[OK]${NC}    $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
err()   { echo -e "${RED}[ERR]${NC}   $*"; }

# ── Pre-flight checks ───────────────────────────────────────────────────────
preflight() {
    if [[ $EUID -ne 0 ]]; then
        err "Must run as root"; exit 1
    fi
    if [[ ! -x "$VAIGAI_BIN" ]]; then
        err "vaigai not built — run: ninja -C $VAIGAI_DIR/build"; exit 1
    fi
}

# ── Hugepages ────────────────────────────────────────────────────────────────
setup_hugepages() {
    local cur
    cur=$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages)
    if (( cur < 256 )); then
        echo 256 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
        info "Hugepages set to 256 × 2 MB"
    fi
    # Ensure each NUMA node has enough hugepages for DPDK workers
    local node
    for node in /sys/devices/system/node/node*/hugepages/hugepages-2048kB/free_hugepages; do
        [[ -f "$node" ]] || continue
        local free ndir
        free=$(cat "$node")
        ndir=$(dirname "$node")
        if (( free < 64 )); then
            echo 128 > "$ndir/nr_hugepages"
            local nname
            nname=$(basename "$(dirname "$(dirname "$ndir")")")
            info "Hugepages on $nname increased to 128"
        fi
    done
    # Clean up stale DPDK hugepage files from previous runs
    rm -f /dev/hugepages/vaigai_* 2>/dev/null || true
}

# ── Send command to vaigai via remote CLI ────────────────────────────────────
vaigai_cmd() {
    echo "$1" | "$VAIGAI_BIN" --attach 2>/dev/null
}

# ── Print traffic & debug commands ───────────────────────────────────────────
print_traffic_commands() {
    local SIP="$1"   # SERVER_IP
    local HTTP_PORT="${2:-80}"
    local HTTPS_PORT="${3:-443}"
    local TLS_PORT="${4:-4433}"
    local TCP_PORT="${5:-5000}"
    local UDP_PORT="${6:-5001}"

    cat <<EOF

${BOLD}═══ Traffic Commands (at vaigai> prompt or via --attach) ═══${NC}

  ${CYAN}# Single-request tests (--one)${NC}
  start --ip $SIP --port $TCP_PORT --proto tcp --one
  start --ip $SIP --port $HTTP_PORT --proto http --one --url /
  start --ip $SIP --port $HTTPS_PORT --proto https --one --url /
  start --ip $SIP --port $TLS_PORT --proto tls --one

  ${CYAN}# Duration / rate tests${NC}
  start --ip $SIP --port $TCP_PORT --proto tcp --duration 30
  start --ip $SIP --port $TCP_PORT --proto tcp --duration 30 --rate 1000
  start --ip $SIP --port $HTTP_PORT --proto http --duration 30 --url /
  start --ip $SIP --port $HTTP_PORT --proto http --duration 30 --rate 500 --url /index.html
  start --ip $SIP --port $HTTPS_PORT --proto https --duration 30 --url /
  start --ip $SIP --port $HTTPS_PORT --proto https --duration 30 --rate 200 --url /
  start --ip $SIP --port $TLS_PORT --proto tls --duration 30
  start --ip $SIP --port $TLS_PORT --proto tls --duration 30 --rate 100
  start --ip $SIP --port $UDP_PORT --proto udp --size 1024 --duration 30
  start --ip $SIP --port $UDP_PORT --proto udp --size 512 --duration 30 --rate 10000
  start --ip $SIP --port 0 --proto icmp --duration 10

  ${CYAN}# Throughput (connection reuse)${NC}
  start --ip $SIP --port $HTTP_PORT --proto http --duration 30 --reuse --streams 4 --url /100k.bin
  start --ip $SIP --port $HTTPS_PORT --proto https --duration 30 --reuse --streams 4 --url /100k.bin
  start --ip $SIP --port $TLS_PORT --proto tls --duration 30 --reuse --streams 4

  ${CYAN}# Control${NC}
  stop                          # stop active traffic
  reset                         # reset TCP state between tests

${BOLD}═══ Monitoring & Debug ═══${NC}
  ${CYAN}# Stats (work concurrently with running traffic via --attach)${NC}
  stat net                      # snapshot of all counters
  stat net --rate               # per-second rates (pps, Mbps)
  stat net --rate --watch       # live dashboard (Ctrl+C to stop)
  stat net --core 0             # single worker core stats
  stat cpu                      # CPU utilization per lcore
  stat cpu --rate --watch       # live CPU monitor
  stat mem                      # mempool usage
  stat port                     # NIC port counters
  stat port --rate              # NIC port rates

  ${CYAN}# Packet trace${NC}
  trace start /tmp/capture.pcapng
  trace stop

  ${CYAN}# Interface info${NC}
  show interface

  ${CYAN}# ICMP ping${NC}
  ping $SIP
  ping $SIP 10                  # 10 pings
  ping $SIP 1 1400              # 1 ping, 1400-byte payload

  ${CYAN}# Quit${NC}
  quit
EOF
}
