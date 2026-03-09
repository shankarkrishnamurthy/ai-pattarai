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
