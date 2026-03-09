#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# ═══════════════════════════════════════════════════════════════════════════════
#  1D — Container (podman) + veth pair (AF_XDP)
# ═══════════════════════════════════════════════════════════════════════════════
#  Topology: vaigai (DPDK af_xdp on veth-vaigai) ↔ container (veth-peer)
#  Network:  192.168.200.1 (vaigai) ↔ 192.168.200.2 (container)
#  No physical NIC needed. Requires: libbpf, vaigai built with -Daf_xdp=enabled.
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

# ── Pre-flight checks ───────────────────────────────────────────────────────
[[ $EUID -ne 0 ]] && { err "Must run as root"; exit 1; }
[[ ! -x "$VAIGAI_BIN" ]] && { err "vaigai not built — run: ninja -C $VAIGAI_DIR/build"; exit 1; }

# ── Hugepages ────────────────────────────────────────────────────────────────
_cur=$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages)
if (( _cur < 256 )); then
    echo 256 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
    info "Hugepages set to 256 × 2 MB"
fi
for _node in /sys/devices/system/node/node*/hugepages/hugepages-2048kB/free_hugepages; do
    [[ -f "$_node" ]] || continue
    _free=$(cat "$_node"); _ndir=$(dirname "$_node")
    if (( _free < 64 )); then
        echo 128 > "$_ndir/nr_hugepages"
        info "Hugepages on $(basename "$(dirname "$(dirname "$_ndir")")") increased to 128"
    fi
done
rm -f /dev/hugepages/vaigai_* 2>/dev/null || true

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
  start --ip $SIP --port $TCP_PORT --proto tcp --duration 30
  start --ip $SIP --port $TCP_PORT --proto tcp --duration 30 --rate 1000
  start --ip $SIP --port $HTTP_PORT --proto http --duration 30 --url /
  start --ip $SIP --port $HTTPS_PORT --proto https --duration 30 --url /
  start --ip $SIP --port $TLS_PORT --proto tls --duration 30
  start --ip $SIP --port $UDP_PORT --proto udp --size 1024 --duration 30

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

SERVER_IP=192.168.200.2
VETH_HOST=veth-vaigai
VETH_PEER=veth-peer

# ── Cleanup function ────────────────────────────────────────────────────────
cleanup() {
    info "Cleaning up 1D..."
    podman rm -f vaigai-server 2>/dev/null || true
    ip link del "$VETH_HOST" 2>/dev/null || true
    ok "1D cleanup done"
}
trap cleanup EXIT

# ── Pre-clean: stop leftovers from previous runs ────────────────────────────
podman rm -f vaigai-server 2>/dev/null || true
ip link del "$VETH_HOST" 2>/dev/null || true

# ── Phase 1: Create container with default network to install packages ──────
info "Creating container with network for package installation..."
podman run -d --name vaigai-server alpine:latest sleep infinity
podman exec vaigai-server sh -c '
    apk add --no-cache nginx openssl socat iproute2 2>/dev/null
    mkdir -p /etc/nginx/ssl /var/www/html /run/nginx
    openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
        -keyout /etc/nginx/ssl/server.key -out /etc/nginx/ssl/server.crt \
        -subj "/CN=vaigai-test" 2>/dev/null
    echo "OK" > /var/www/html/index.html
    dd if=/dev/urandom of=/var/www/html/100k.bin bs=1024 count=100 2>/dev/null
'
ok "Packages installed"

# ── Phase 2: Recreate container with --network=none + veth ──────────────────
# Commit the container as an image so we can restart with --network=none
podman commit vaigai-server vaigai-server-img >/dev/null
podman rm -f vaigai-server >/dev/null
podman run -d --name vaigai-server --network=none vaigai-server-img sleep infinity
CTR_PID=$(podman inspect -f '{{.State.Pid}}' vaigai-server)
info "Container restarted with --network=none (PID $CTR_PID)"

# ── Phase 3: Create veth pair and move peer into container ──────────────────
ip link add "$VETH_HOST" type veth peer name "$VETH_PEER"
ip link set "$VETH_HOST" up
# Do NOT add an IP address to the host veth — AF_XDP takes full control.
# The kernel stack must not compete with DPDK for packets on this interface.

ip link set "$VETH_PEER" netns "$CTR_PID"
nsenter -t "$CTR_PID" -n ip addr add $SERVER_IP/24 dev "$VETH_PEER"
nsenter -t "$CTR_PID" -n ip link set "$VETH_PEER" up
nsenter -t "$CTR_PID" -n ip link set lo up

# Disable TX/RX checksum offload on both veth ends.
# veth uses CHECKSUM_PARTIAL internally; AF_XDP/XDP intercepts packets
# before the kernel fills in full checksums, causing false bad-cksum drops.
ethtool -K "$VETH_HOST" tx off rx off gso off gro off tso off 2>/dev/null || true
nsenter -t "$CTR_PID" -n ethtool -K "$VETH_PEER" tx off rx off gso off gro off tso off 2>/dev/null || true
ok "Veth pair created (checksum offload disabled)"

# ── Phase 4: Start services inside container ────────────────────────────────
podman exec vaigai-server sh -c '
    # Configure nginx SSL on port 443
    cat > /etc/nginx/http.d/ssl.conf << "SSLEOF"
server {
    listen 443 ssl;
    ssl_certificate /etc/nginx/ssl/server.crt;
    ssl_certificate_key /etc/nginx/ssl/server.key;
    root /var/www/html;
    location / { try_files $uri $uri/ =200; }
}
SSLEOF
    nginx
    openssl s_server -cert /etc/nginx/ssl/server.crt -key /etc/nginx/ssl/server.key \
        -accept 4433 -www -quiet </dev/null >/dev/null 2>&1 &
    socat TCP-LISTEN:5000,fork,reuseaddr SYSTEM:"cat" </dev/null &
    socat TCP-LISTEN:5001,fork,reuseaddr /dev/null </dev/null &
'
sleep 1
info "Server ports:"
podman exec vaigai-server ss -tlnp
echo ""

# ── Phase 5: Start vaigai (AF_XDP) ─────────────────────────────────────────
info "Starting vaigai with AF_XDP (interactive — Ctrl+C or 'quit' to exit)..."
echo ""
print_traffic_commands "$SERVER_IP"
echo ""

"$VAIGAI_BIN" -l 0-1 --no-pci \
    --vdev "net_af_xdp0,iface=$VETH_HOST,start_queue=0,force_copy=1" \
    -- -I 192.168.200.1
