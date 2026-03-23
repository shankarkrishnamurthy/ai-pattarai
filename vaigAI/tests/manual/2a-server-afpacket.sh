#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# ═══════════════════════════════════════════════════════════════════════════════
#  2A — Server Mode (vaigai as server, Linux clients over AF_PACKET)
# ═══════════════════════════════════════════════════════════════════════════════
#  Topology: vaigai --server (DPDK af_packet on veth-vsrv2a)
#                ↔ veth pair ↔ native Linux clients (veth-vcli2a)
#  Network:  192.168.203.1 (vaigai server) ↔ 192.168.203.2 (clients)
#  No physical NIC, no VM, no container needed.
#
#  This is the inverse of 1E: vaigai is the SERVER, standard Linux
#  tools are the CLIENTS.  Tests all six server handlers against
#  real-world tools: socat, curl, openssl.
#
# ┌────────────────────────────── Host ────────────────────────────────────┐
# │                                                                       │
# │  vaigAI (--server)                      Native Linux Clients          │
# │    └─► DPDK af_packet                                                │
# │          │                              socat    → tcp:5000  (echo)   │
# │   ┌──────┴──────┐   ┌──────────────┐   socat    → tcp:5001  (discard)│
# │   │ veth-vsrv2a │◄═►│ veth-vcli2a  │   socat    → tcp:5002  (chargen)│
# │   └─────────────┘   └──────────────┘   curl     → http:8080         │
# │   192.168.203.1       192.168.203.2     curl -k  → https:8443       │
# │   (no kernel IP)      (kernel IP)       openssl  → tls:4433  (echo)  │
# │                                                                       │
# └───────────────────────────────────────────────────────────────────────┘
#
#  Clients used (standard Linux packages):
#  ┌──────────┬─────────────┬───────────────────────────────────────────┐
#  │ Tool     │ Package     │ What it tests                             │
#  ├──────────┼─────────────┼───────────────────────────────────────────┤
#  │ socat    │ socat       │ TCP echo, discard, chargen verification   │
#  │ curl     │ curl        │ HTTP/1.1 and HTTPS (TLS+HTTP) requests   │
#  │ openssl  │ openssl     │ Raw TLS handshake + encrypted echo       │
#  │ nc       │ nmap-ncat   │ Quick TCP connectivity check (optional)  │
#  └──────────┴─────────────┴───────────────────────────────────────────┘
#
#  Usage:
#    sudo bash tests/manual/2a-server-afpacket.sh              # all-in-one
#    sudo bash tests/manual/2a-server-afpacket.sh --server     # terminal 1
#    sudo bash tests/manual/2a-server-afpacket.sh --client     # terminal 2
#    sudo bash tests/manual/2a-server-afpacket.sh --cleanup    # clean up
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
Usage: $(basename "$0") [--server | --client | --cleanup]
  (no args)   Run everything: veth + vaigai server (interactive)
  --server    Start veth + vaigai in server mode (terminal 1)
  --client    Print client test commands (terminal 2)
  --cleanup   Clean up all resources
EOF
}

# ── Arguments ────────────────────────────────────────────────────────────────
MODE=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --server)  MODE="server" ;;
        --client)  MODE="client" ;;
        --cleanup) MODE="cleanup" ;;
        -h|--help) usage; exit 0 ;;
        *) err "Unknown option: $1"; usage; exit 1 ;;
    esac
    shift
done

# ── Pre-flight ───────────────────────────────────────────────────────────────
[[ $EUID -ne 0 ]] && { err "Must run as root"; exit 1; }
if [[ "$MODE" == "server" || -z "$MODE" ]]; then
    [[ ! -x "$VAIGAI_BIN" ]] && { err "vaigai not built — run: ninja -C $VAIGAI_DIR/build"; exit 1; }
fi

# ── Constants ────────────────────────────────────────────────────────────────
VAIGAI_IP=192.168.203.1
CLIENT_IP=192.168.203.2
VETH_SRV=veth-vsrv2a
VETH_CLI=veth-vcli2a
STATE_DIR=/tmp/vaigai-2a
TLS_DIR=/tmp/vaigai-2a-tls

# Listener ports
PORT_ECHO=5000
PORT_DISCARD=5001
PORT_CHARGEN=5002
PORT_HTTP=8080
PORT_HTTPS=8443
PORT_TLS_ECHO=4433

# ── Print client test commands ───────────────────────────────────────────────
print_client_commands() {
    cat <<CMDS

${BOLD}═══ Linux Client Commands (run from any terminal as root) ═══${NC}

${CYAN}── TCP Echo (socat) ──────────────────────────────────────────${NC}
  echo "hello" | socat - TCP:${VAIGAI_IP}:${PORT_ECHO}
  # Expected: "hello" echoed back

${CYAN}── TCP Discard (socat) ──────────────────────────────────────${NC}
  echo "discard this" | socat -t 1 - TCP:${VAIGAI_IP}:${PORT_DISCARD}
  # Expected: connection succeeds, no data returned

${CYAN}── TCP Chargen (socat + timeout) ────────────────────────────${NC}
  timeout 2 socat - TCP:${VAIGAI_IP}:${PORT_CHARGEN} | wc -c
  # Expected: thousands of bytes received

${CYAN}── HTTP GET (curl) ─────────────────────────────────────────${NC}
  curl -v http://${VAIGAI_IP}:${PORT_HTTP}/
  # Expected: HTTP/1.1 200 OK with body

${CYAN}── HTTPS GET (curl, self-signed cert) ──────────────────────${NC}
  curl -vk https://${VAIGAI_IP}:${PORT_HTTPS}/
  # Expected: HTTP/1.1 200 OK over TLS

${CYAN}── TLS Echo (openssl s_client) ─────────────────────────────${NC}
  echo "tls-test" | openssl s_client -connect ${VAIGAI_IP}:${PORT_TLS_ECHO} -quiet
  # Expected: "tls-test" echoed back over TLS

${CYAN}── Quick connectivity check (nc) ───────────────────────────${NC}
  echo "test" | nc -w 2 ${VAIGAI_IP} ${PORT_ECHO}
  # Expected: "test" echoed back

${CYAN}── Bulk TCP throughput (socat, 1 MB) ────────────────────────${NC}
  dd if=/dev/zero bs=1024 count=1024 2>/dev/null | socat -t 5 - TCP:${VAIGAI_IP}:${PORT_ECHO} | wc -c
  # Expected: ~1048576 bytes echoed back

${BOLD}═══ vaigai Server CLI (at vaigai(server)> prompt) ═══${NC}

  show listeners                    # list active listeners with stats
  show connections                  # per-worker active TCB count
  stat net                          # aggregate protocol counters
  stat net --rate                   # per-second rates
  stat cpu                          # CPU utilization per lcore
  stop tcp:${PORT_DISCARD}:discard  # stop a single listener
  stop                              # stop all listeners
  reset                             # RST all TCBs, reset counters
  serve --listen tcp:${PORT_ECHO}:echo --listen http:${PORT_HTTP}   # re-serve

CMDS
}

# ── Hugepages ────────────────────────────────────────────────────────────────
setup_hugepages() {
    local cur
    cur=$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages)
    if (( cur < 256 )); then
        echo 256 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
        info "Hugepages set to 256 x 2 MB"
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
    info "Cleaning up 2A..."
    ip link del "$VETH_SRV" 2>/dev/null || true
    rm -rf "$TLS_DIR" "$STATE_DIR"
    ok "2A cleanup done"
}

# ── Server: veth + TLS certs + vaigai --server ───────────────────────────────
start_server() {
    # Pre-clean
    ip link del "$VETH_SRV" 2>/dev/null || true
    rm -rf "$TLS_DIR" "$STATE_DIR"
    mkdir -p "$STATE_DIR" "$TLS_DIR"

    # Create veth pair
    ip link add "$VETH_SRV" type veth peer name "$VETH_CLI"
    ip link set "$VETH_SRV" up
    ip link set "$VETH_CLI" up
    ip addr add "$CLIENT_IP/24" dev "$VETH_CLI"

    # Disable TX checksum offload (critical for af_packet)
    ethtool -K "$VETH_SRV" tx off 2>/dev/null || true
    ethtool -K "$VETH_CLI" tx off 2>/dev/null || true
    ok "veth pair: $VETH_SRV <-> $VETH_CLI"

    # Generate TLS certs for HTTPS/TLS handlers
    if command -v openssl &>/dev/null; then
        openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
            -keyout "$TLS_DIR/server.key" -out "$TLS_DIR/server.crt" \
            -subj "/CN=vaigai-test" 2>/dev/null
        ok "TLS cert: $TLS_DIR/server.crt"
    else
        warn "openssl not found — HTTPS/TLS listeners will fail"
    fi

    # Determine serve command
    local SERVE_ARGS=""
    SERVE_ARGS+=" --listen tcp:${PORT_ECHO}:echo"
    SERVE_ARGS+=" --listen tcp:${PORT_DISCARD}:discard"
    SERVE_ARGS+=" --listen tcp:${PORT_CHARGEN}:chargen"
    SERVE_ARGS+=" --listen http:${PORT_HTTP}"
    if [[ -f "$TLS_DIR/server.crt" ]]; then
        SERVE_ARGS+=" --listen https:${PORT_HTTPS}"
        SERVE_ARGS+=" --listen tls:${PORT_TLS_ECHO}:echo"
        SERVE_ARGS+=" --tls-cert $TLS_DIR/server.crt --tls-key $TLS_DIR/server.key"
    fi
    echo "$SERVE_ARGS" > "$STATE_DIR/serve-args.txt"

    info "Starting vaigai in server mode (interactive)..."
    info "After the vaigai(server)> prompt appears, run:"
    echo ""
    echo -e "  ${CYAN}serve${SERVE_ARGS}${NC}"
    echo ""
    info "Then open a second terminal and run:"
    echo -e "  ${CYAN}sudo bash $0 --client${NC}"
    echo ""

    "$VAIGAI_BIN" -l 0-1 --no-pci --vdev "net_af_packet0,iface=$VETH_SRV" \
        -- --server --src-ip "$VAIGAI_IP"
}

# ── Client: print test commands ──────────────────────────────────────────────
start_client() {
    # Verify veth exists
    if ! ip link show "$VETH_CLI" >/dev/null 2>&1; then
        err "veth interface $VETH_CLI not found — run --server first"
        exit 1
    fi

    print_client_commands
}

# ── Main ─────────────────────────────────────────────────────────────────────
case "$MODE" in
    server)
        setup_hugepages
        trap do_cleanup EXIT
        start_server
        ;;
    client)
        start_client
        ;;
    cleanup)
        do_cleanup
        ;;
    "")
        setup_hugepages
        trap do_cleanup EXIT
        start_server
        ;;
esac
