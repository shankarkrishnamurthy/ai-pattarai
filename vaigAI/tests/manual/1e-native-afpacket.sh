#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# ═══════════════════════════════════════════════════════════════════════════════
#  1E — Native Process (no VM, no container — same host)
# ═══════════════════════════════════════════════════════════════════════════════
#  Topology: vaigai (DPDK af_packet on veth-vaigai) ↔ native processes (veth-native)
#  Network:  192.168.201.1 (vaigai) ↔ 192.168.201.2 (native)
#  No physical NIC needed. Simplest topology for quick testing.
#
#  NOTE: TX checksum offload is disabled on both veth interfaces so that the
#  kernel computes checksums in software. DPDK af_packet does not support HW
#  checksum offload, and partial checksums cause vaigai to drop packets.
#
#  Access: Servers run as native processes on the host — no SSH needed.
#          Service ports are bound to 192.168.201.2.
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
  (no args)   Run everything: server + vaigai (original behavior)
  --server    Start server infrastructure only (terminal 1)
  --vaigai      Start vaigai traffic generator only (terminal 2)
  --cleanup   Clean up all resources from previous runs
EOF
}

# ── Parse arguments ──────────────────────────────────────────────────────────
MODE=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --server)  MODE="server" ;;
        --vaigai)    MODE="vaigai" ;;
        --cleanup) MODE="cleanup" ;;
        -h|--help) usage; exit 0 ;;
        *) err "Unknown option: $1"; usage; exit 1 ;;
    esac
    shift
done

# ── Pre-flight checks ───────────────────────────────────────────────────────
[[ $EUID -ne 0 ]] && { err "Must run as root"; exit 1; }
if [[ "$MODE" == "vaigai" || -z "$MODE" ]]; then
    [[ ! -x "$VAIGAI_BIN" ]] && { err "vaigai not built — run: ninja -C $VAIGAI_DIR/build"; exit 1; }
fi

# ── Constants ────────────────────────────────────────────────────────────────
SERVER_IP=192.168.201.2
VETH_HOST=veth-vaigai
VETH_NATIVE=veth-native
STATE_DIR=/tmp/vaigai-1e

# Use 8443 for HTTPS if port 443 is already taken on this host
HTTPS_PORT=443
if ss -tlnp sport = :443 | grep -q LISTEN 2>/dev/null; then
    HTTPS_PORT=8443
    info "Port 443 in use — using $HTTPS_PORT for HTTPS"
fi

# ── Print traffic & debug commands ───────────────────────────────────────────
print_traffic_commands() {
    local SIP="$1"
    local HTTP_PORT="${2:-80}" HTTPS_PORT="${3:-443}" TLS_PORT="${4:-4433}"
    local TCP_PORT="${5:-5000}" UDP_PORT="${6:-5001}"
    cat <<CMDS

${BOLD}═══ Traffic Commands (at vaigai> prompt or via --attach) ═══${NC}

  ${CYAN}# Single-request tests (--one)${NC}
  start --ip $SIP --port $TCP_PORT --proto tcp --one
  start --ip $SIP --port $HTTP_PORT --proto http --one --url /
  start --ip $SIP --port $HTTPS_PORT --proto https --one --url /
  start --ip $SIP --port $TLS_PORT --proto tls --one

  ${CYAN}# Duration / rate tests${NC}
  start --ip $SIP --port $TCP_PORT --proto tcp --duration 5
  start --ip $SIP --port $TCP_PORT --proto tcp --duration 5 --rate 1000
  start --ip $SIP --port $HTTP_PORT --proto http --duration 5 --url /
  start --ip $SIP --port $HTTPS_PORT --proto https --duration 5 --url /
  start --ip $SIP --port $TLS_PORT --proto tls --duration 5
  start --ip $SIP --port $UDP_PORT --proto udp --size 1024 --duration 5

  ${CYAN}# Control${NC}
  stop                          # stop active traffic
  reset                         # reset TCP state between tests

${BOLD}═══ Monitoring & Debug ═══${NC}
  stat net                      # snapshot of all counters
  stat net --rate               # per-second rates
  stat cpu                      # CPU utilization per lcore
  ping $SIP                     # ICMP ping
  show interface                # interface info
  trace start /tmp/capture.pcapng   # packet capture
  trace stop
  quit
CMDS
}

# ── Helper: terminate process from PID file ──────────────────────────────────
stop_pidfile() {
    local f="$1"
    if [[ -f "$f" ]]; then
        local pid
        pid=$(cat "$f")
        /bin/kill "$pid" 2>/dev/null || true
        rm -f "$f"
    fi
}

# ── Hugepages ────────────────────────────────────────────────────────────────
setup_hugepages() {
    local _cur
    _cur=$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages)
    if (( _cur < 256 )); then
        echo 256 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
        info "Hugepages set to 256 × 2 MB"
    fi
    for _node in /sys/devices/system/node/node*/hugepages/hugepages-2048kB/free_hugepages; do
        [[ -f "$_node" ]] || continue
        local _free _ndir
        _free=$(cat "$_node"); _ndir=$(dirname "$_node")
        if (( _free < 64 )); then
            echo 128 > "$_ndir/nr_hugepages"
            info "Hugepages on $(basename "$(dirname "$(dirname "$_ndir")")") increased to 128"
        fi
    done
    rm -f /dev/hugepages/vaigai_* 2>/dev/null || true
}

# ── Cleanup ──────────────────────────────────────────────────────────────────
do_cleanup() {
    info "Cleaning up 1E..."
    nginx -s stop -c /tmp/vaigai-native-nginx.conf 2>/dev/null || true
    stop_pidfile "$STATE_DIR/openssl.pid"
    stop_pidfile "$STATE_DIR/socat1.pid"
    stop_pidfile "$STATE_DIR/socat2.pid"
    ip link del "$VETH_HOST" 2>/dev/null || true
    rm -rf /tmp/vaigai-native-tls /tmp/vaigai-native-www
    rm -f /tmp/vaigai-native-nginx.conf /tmp/vaigai-native-nginx.pid
    rm -f /tmp/vaigai-native-nginx-error.log /tmp/vaigai-native-nginx-access.log
    rm -rf "$STATE_DIR"
    ok "1E cleanup done"
}

# ── Server: veth + TLS + nginx + socat ───────────────────────────────────────
start_server() {
    # Pre-clean leftovers
    nginx -s stop -c /tmp/vaigai-native-nginx.conf 2>/dev/null || true
    stop_pidfile "$STATE_DIR/openssl.pid"
    stop_pidfile "$STATE_DIR/socat1.pid"
    stop_pidfile "$STATE_DIR/socat2.pid"
    ip link del "$VETH_HOST" 2>/dev/null || true
    mkdir -p "$STATE_DIR"

    # Create veth pair
    ip link add "$VETH_HOST" type veth peer name "$VETH_NATIVE"
    ip link set "$VETH_HOST" up
    ip link set "$VETH_NATIVE" up
    ip addr add "$SERVER_IP/24" dev "$VETH_NATIVE"

    # Disable TX checksum offload (critical for af_packet)
    ethtool -K "$VETH_NATIVE" tx off 2>/dev/null || true
    ethtool -K "$VETH_HOST" tx off 2>/dev/null || true

    # Wait for IP to be ready
    for i in $(seq 1 20); do
        ip addr show "$VETH_NATIVE" | grep -q "$SERVER_IP" && break
        sleep 0.25
    done
    ok "veth pair created ($VETH_HOST ↔ $VETH_NATIVE)"

    # TLS certificates
    mkdir -p /tmp/vaigai-native-tls
    openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
        -keyout /tmp/vaigai-native-tls/server.key \
        -out /tmp/vaigai-native-tls/server.crt \
        -subj "/CN=vaigai-test" 2>/dev/null

    # nginx config
    cat > /tmp/vaigai-native-nginx.conf << NGINXEOF
worker_processes 1;
pid /tmp/vaigai-native-nginx.pid;
error_log /tmp/vaigai-native-nginx-error.log;
events { worker_connections 4096; }
http {
    access_log /tmp/vaigai-native-nginx-access.log;
    server {
        listen ${SERVER_IP}:80;
        location / { root /tmp/vaigai-native-www; }
    }
    server {
        listen ${SERVER_IP}:${HTTPS_PORT} ssl;
        ssl_certificate     /tmp/vaigai-native-tls/server.crt;
        ssl_certificate_key /tmp/vaigai-native-tls/server.key;
        location / { root /tmp/vaigai-native-www; }
    }
}
NGINXEOF

    mkdir -p /tmp/vaigai-native-www
    echo "OK" > /tmp/vaigai-native-www/index.html
    dd if=/dev/urandom of=/tmp/vaigai-native-www/100k.bin bs=1024 count=100 2>/dev/null

    # Start servers (disown so they survive shell exit when run non-interactively)
    nginx -c /tmp/vaigai-native-nginx.conf

    # Use socat OPENSSL-LISTEN to bind TLS server to $SERVER_IP explicitly
    socat OPENSSL-LISTEN:4433,cert=/tmp/vaigai-native-tls/server.crt,key=/tmp/vaigai-native-tls/server.key,verify=0,fork,bind=$SERVER_IP,reuseaddr PIPE </dev/null >/dev/null 2>&1 &
    echo $! > "$STATE_DIR/openssl.pid"
    disown $!

    socat TCP-LISTEN:5000,bind=$SERVER_IP,fork,reuseaddr SYSTEM:'cat' </dev/null >/dev/null 2>&1 &
    echo $! > "$STATE_DIR/socat1.pid"
    disown $!
    socat TCP-LISTEN:5001,bind=$SERVER_IP,fork,reuseaddr /dev/null </dev/null >/dev/null 2>&1 &
    echo $! > "$STATE_DIR/socat2.pid"
    disown $!

    sleep 1

    # Verify all ports are listening
    info "Listening ports:"
    ss -tlnp | grep -E ":(80|${HTTPS_PORT}|4433|5000|5001)\b" || true
    local EXPECTED=5
    local ACTUAL
    ACTUAL=$(ss -tlnp | grep -cE ":(80|${HTTPS_PORT}|4433|5000|5001)\b" || true)
    if (( ACTUAL < EXPECTED )); then
        warn "Only $ACTUAL of $EXPECTED server ports are listening"
    else
        ok "All $EXPECTED server ports listening"
    fi
}

# ── Tgen: vaigai with af_packet ──────────────────────────────────────────────
start_tgen() {
    # Verify server is running
    if ! ip link show "$VETH_HOST" >/dev/null 2>&1; then
        err "veth interface $VETH_HOST not found — run --server first"
        exit 1
    fi

    info "Starting vaigai with af_packet (interactive — Ctrl+C or 'quit' to exit)..."
    echo ""
    print_traffic_commands "$SERVER_IP" 80 "$HTTPS_PORT" 4433 5000 5001
    echo ""

    "$VAIGAI_BIN" -l 0-1 --no-pci --vdev "net_af_packet0,iface=$VETH_HOST" -- -I 192.168.201.1
}

# ── Main ─────────────────────────────────────────────────────────────────────
case "$MODE" in
    server)
        setup_hugepages
        start_server
        ok "Server running. Use '--cleanup' to stop."
        ;;
    vaigai)
        setup_hugepages
        start_tgen
        ;;
    cleanup)
        do_cleanup
        ;;
    "")
        setup_hugepages
        trap do_cleanup EXIT
        start_server
        start_tgen
        ;;
esac
