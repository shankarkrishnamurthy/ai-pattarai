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
# ═══════════════════════════════════════════════════════════════════════════════

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/common.sh"
preflight
setup_hugepages

SERVER_IP=192.168.201.2
VETH_HOST=veth-vaigai
VETH_NATIVE=veth-native

# Use 8443 for HTTPS if port 443 is already taken on this host
HTTPS_PORT=443
if ss -tlnp sport = :443 | grep -q LISTEN 2>/dev/null; then
    HTTPS_PORT=8443
    info "Port 443 in use — using $HTTPS_PORT for HTTPS"
fi

# ── Cleanup function ────────────────────────────────────────────────────────
cleanup() {
    info "Cleaning up 1E..."
    nginx -s stop -c /tmp/vaigai-native-nginx.conf 2>/dev/null || true
    [[ -n "${OPENSSL_PID:-}" ]] && kill "$OPENSSL_PID" 2>/dev/null || true
    [[ -n "${SOCAT1_PID:-}" ]] && kill "$SOCAT1_PID" 2>/dev/null || true
    [[ -n "${SOCAT2_PID:-}" ]] && kill "$SOCAT2_PID" 2>/dev/null || true
    ip link del "$VETH_HOST" 2>/dev/null || true
    rm -rf /tmp/vaigai-native-tls /tmp/vaigai-native-www
    rm -f /tmp/vaigai-native-nginx.conf /tmp/vaigai-native-nginx.pid
    rm -f /tmp/vaigai-native-nginx-error.log /tmp/vaigai-native-nginx-access.log
    ok "1E cleanup done"
}
trap cleanup EXIT

# ── Pre-clean: stop leftovers from previous runs ────────────────────────────
nginx -s stop -c /tmp/vaigai-native-nginx.conf 2>/dev/null || true
ip link del "$VETH_HOST" 2>/dev/null || true

# ── Setup: create veth pair ─────────────────────────────────────────────────
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

# ── TLS certificates ───────────────────────────────────────────────────────
mkdir -p /tmp/vaigai-native-tls
openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
    -keyout /tmp/vaigai-native-tls/server.key \
    -out /tmp/vaigai-native-tls/server.crt \
    -subj "/CN=vaigai-test" 2>/dev/null

# ── nginx config ─────────────────────────────────────────────────────────────
cat > /tmp/vaigai-native-nginx.conf << EOF
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
EOF

mkdir -p /tmp/vaigai-native-www
echo "OK" > /tmp/vaigai-native-www/index.html
dd if=/dev/urandom of=/tmp/vaigai-native-www/100k.bin bs=1024 count=100 2>/dev/null

# ── Start servers ────────────────────────────────────────────────────────────
nginx -c /tmp/vaigai-native-nginx.conf

setsid openssl s_server -cert /tmp/vaigai-native-tls/server.crt \
    -key /tmp/vaigai-native-tls/server.key \
    -accept 4433 -www -quiet </dev/null &>/dev/null &
OPENSSL_PID=$!

setsid socat TCP-LISTEN:5000,bind=$SERVER_IP,fork,reuseaddr SYSTEM:'cat' </dev/null &>/dev/null &
SOCAT1_PID=$!
setsid socat TCP-LISTEN:5001,bind=$SERVER_IP,fork,reuseaddr /dev/null </dev/null &>/dev/null &
SOCAT2_PID=$!

sleep 1

# Verify all ports are listening
info "Listening ports:"
ss -tlnp | grep -E ":(80|${HTTPS_PORT}|4433|5000|5001)\b"
EXPECTED=5
ACTUAL=$(ss -tlnp | grep -cE ":(80|${HTTPS_PORT}|4433|5000|5001)\b")
if (( ACTUAL < EXPECTED )); then
    warn "Only $ACTUAL of $EXPECTED server ports are listening"
else
    ok "All $EXPECTED server ports listening"
fi
echo ""

# ── Start vaigai ─────────────────────────────────────────────────────────────
info "Starting vaigai with af_packet (interactive — Ctrl+C or 'quit' to exit)..."
echo ""
print_traffic_commands "$SERVER_IP" 80 "$HTTPS_PORT" 4433 5000 5001
echo ""

"$VAIGAI_BIN" -l 0-1 --no-pci --vdev "net_af_packet0,iface=$VETH_HOST" -- -I 192.168.201.1
