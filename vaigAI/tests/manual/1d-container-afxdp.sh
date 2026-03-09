#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# ═══════════════════════════════════════════════════════════════════════════════
#  1D — Container (podman) + veth pair (AF_XDP)
# ═══════════════════════════════════════════════════════════════════════════════
#  Topology: vaigai (DPDK af_xdp on veth-vaigai) ↔ container (veth-peer)
#  Network:  192.168.200.1 (vaigai) ↔ 192.168.200.2 (container)
#  No physical NIC needed. Requires: libbpf, vaigai built with -Daf_xdp=enabled.
# ═══════════════════════════════════════════════════════════════════════════════

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/common.sh"
preflight
setup_hugepages

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

# ── Setup: create container + veth pair ─────────────────────────────────────
podman run -d --name vaigai-server --network=none alpine:latest sleep infinity
CTR_PID=$(podman inspect -f '{{.State.Pid}}' vaigai-server)
info "Container started (PID $CTR_PID)"

ip link add "$VETH_HOST" type veth peer name "$VETH_PEER"
ip link set "$VETH_HOST" up
ip addr add 192.168.200.1/24 dev "$VETH_HOST"

# Move veth peer into the container's network namespace
ip link set "$VETH_PEER" netns "$CTR_PID"
nsenter -t "$CTR_PID" -n ip addr add 192.168.200.2/24 dev "$VETH_PEER"
nsenter -t "$CTR_PID" -n ip link set "$VETH_PEER" up
nsenter -t "$CTR_PID" -n ip link set lo up

# Verify connectivity
if ping -c 1 -W 2 "$SERVER_IP" >/dev/null 2>&1; then
    ok "Container reachable at $SERVER_IP"
else
    warn "Container not reachable at $SERVER_IP"
fi

# ── Server: install and start services inside container ─────────────────────
podman exec vaigai-server sh -c '
    apk add --no-cache nginx openssl socat
    mkdir -p /etc/nginx/ssl /var/www/html /run/nginx
    openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
        -keyout /etc/nginx/ssl/server.key -out /etc/nginx/ssl/server.crt \
        -subj "/CN=vaigai-test" 2>/dev/null
    echo "OK" > /var/www/html/index.html
    dd if=/dev/urandom of=/var/www/html/100k.bin bs=1024 count=100 2>/dev/null
    nginx
    openssl s_server -cert /etc/nginx/ssl/server.crt -key /etc/nginx/ssl/server.key \
        -accept 4433 -www -quiet &
    socat TCP-LISTEN:5000,fork,reuseaddr PIPE &
    socat TCP-LISTEN:5001,fork,reuseaddr /dev/null &
'
info "Server ports:"
podman exec vaigai-server ss -tlnp
echo ""

# ── Start vaigai (AF_XDP) ───────────────────────────────────────────────────
info "Starting vaigai with AF_XDP (interactive — Ctrl+C or 'quit' to exit)..."
echo ""
print_traffic_commands "$SERVER_IP"
echo ""

"$VAIGAI_BIN" -l 0-1 --no-pci \
    --vdev "net_af_xdp0,iface=$VETH_HOST,start_queue=0,force_copy=1" \
    -- -I 192.168.200.1
