#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# ═══════════════════════════════════════════════════════════════════════════════
#  1C — Firecracker MicroVM + TAP bridge
# ═══════════════════════════════════════════════════════════════════════════════
#  Topology: vaigai (DPDK net_tap) ↔ tap-vaigai ↔ br-vaigai ↔ tap-fc0 ↔ Firecracker VM
#  Network:  192.168.204.1 (vaigai) ↔ 192.168.204.2 (VM) via 192.168.204.0/24
#  No physical NIC needed.
#
#  Phase 1: Infrastructure setup (bridge, Firecracker VM, vaigai daemon)
#  Phase 2: Manual testing via remote CLI (vaigai --attach)
#
#  Serial console output goes to /tmp/vaigai-serial.log.
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

SERVER_IP=192.168.204.2
BRIDGE=br-vaigai
TAP_FC=tap-fc0
BRIDGE_IP=192.168.204.3
ROOTFS_COW=/tmp/vaigai-fc-rootfs.ext4
FC_SOCKET=/tmp/vaigai-fc.sock
FC_SERIAL=/tmp/vaigai-serial.log

# ── Cleanup function ────────────────────────────────────────────────────────
cleanup() {
    info "Cleaning up 1C..."
    [[ -n "${VAIGAI_PID:-}" ]] && kill "$VAIGAI_PID" 2>/dev/null
    [[ -n "${FC_PID:-}" ]] && kill "$FC_PID" 2>/dev/null
    rm -f "$FC_SOCKET" "$ROOTFS_COW" "$FC_SERIAL"
    ip link del "$TAP_FC" 2>/dev/null || true
    ip link del "$BRIDGE" 2>/dev/null || true
    ok "1C cleanup done"
}
trap cleanup EXIT

# ── Pre-clean: stop leftovers from previous runs ────────────────────────────
echo "quit" | "$VAIGAI_BIN" --attach 2>/dev/null || true
ip link del "$TAP_FC" 2>/dev/null || true
ip link del "$BRIDGE" 2>/dev/null || true
rm -f "$FC_SOCKET" "$ROOTFS_COW" "$FC_SERIAL"

# ── Phase 1a: create bridge + TAP for Firecracker ───────────────────────────
ip link add "$BRIDGE" type bridge
ip link set "$BRIDGE" up
ip addr add "$BRIDGE_IP/24" dev "$BRIDGE"

ip tuntap add "$TAP_FC" mode tap
ip link set "$TAP_FC" master "$BRIDGE"
ip link set "$TAP_FC" up

# Disable netfilter on bridge
sysctl -q -w net.bridge.bridge-nf-call-iptables=0
sysctl -q -w net.bridge.bridge-nf-call-ip6tables=0
sysctl -q -w net.bridge.bridge-nf-call-arptables=0
ok "Bridge $BRIDGE + $TAP_FC created"

# ── Phase 1b: COW copy of rootfs ────────────────────────────────────────────
cp --reflink=auto /work/firecracker/alpine.ext4 "$ROOTFS_COW"

# ── Phase 1c: Start Firecracker ─────────────────────────────────────────────
rm -f "$FC_SOCKET" "$FC_SERIAL"
# setsid creates a new session so Firecracker survives parent shell exit
setsid firecracker --api-sock "$FC_SOCKET" </dev/null >"$FC_SERIAL" 2>&1 &
sleep 1
FC_PID=$(pgrep -f "firecracker.*$FC_SOCKET" | head -1)
info "Firecracker started (PID $FC_PID, serial → $FC_SERIAL)"

# ── Phase 1d: Configure + boot VM via API ────────────────────────────────────
curl -sf --unix-socket "$FC_SOCKET" -X PUT http://localhost/boot-source \
    -H 'Content-Type: application/json' \
    -d '{"kernel_image_path":"/work/firecracker/vmlinux","boot_args":"console=ttyS0 reboot=k panic=1 pci=off root=/dev/vda rw quiet vaigai_mode=all ip=192.168.204.2::192.168.204.3:255.255.255.0::eth0:off"}'

curl -sf --unix-socket "$FC_SOCKET" -X PUT http://localhost/drives/rootfs \
    -H 'Content-Type: application/json' \
    -d "{\"drive_id\":\"rootfs\",\"path_on_host\":\"$ROOTFS_COW\",\"is_root_device\":true,\"is_read_only\":false}"

curl -sf --unix-socket "$FC_SOCKET" -X PUT http://localhost/network-interfaces/eth0 \
    -H 'Content-Type: application/json' \
    -d "{\"iface_id\":\"eth0\",\"host_dev_name\":\"$TAP_FC\"}"

curl -sf --unix-socket "$FC_SOCKET" -X PUT http://localhost/machine-config \
    -H 'Content-Type: application/json' \
    -d '{"vcpu_count":1,"mem_size_mib":256}'

curl -sf --unix-socket "$FC_SOCKET" -X PUT http://localhost/actions \
    -H 'Content-Type: application/json' \
    -d '{"action_type":"InstanceStart"}'
info "VM booting (serial log: tail -f $FC_SERIAL)"

# Wait for VM
info "Waiting for VM to become reachable..."
for i in $(seq 1 20); do
    ping -c 1 -W 1 "$SERVER_IP" >/dev/null 2>&1 && break
    sleep 1
done
if ping -c 1 -W 2 "$SERVER_IP" >/dev/null 2>&1; then
    ok "VM reachable at $SERVER_IP"
else
    warn "VM not reachable at $SERVER_IP after 20s — check $FC_SERIAL"
fi

# ── Phase 1e: Start vaigai daemon ────────────────────────────────────────────
nohup "$VAIGAI_BIN" -l 0-1 --no-pci --vdev "net_tap0,iface=tap-vaigai" \
    -- -I 192.168.204.1 </dev/null >/tmp/vaigai-fc.log 2>&1 &
VAIGAI_PID=$!
info "vaigai started (PID $VAIGAI_PID, log → /tmp/vaigai-fc.log)"

# Wait for DPDK to create tap-vaigai
for i in $(seq 1 30); do
    ip link show tap-vaigai >/dev/null 2>&1 && break
    sleep 0.5
done
if ! ip link show tap-vaigai >/dev/null 2>&1; then
    err "tap-vaigai not created after 15 seconds"; exit 1
fi

# Attach tap-vaigai to the bridge
ip link set tap-vaigai master "$BRIDGE"
ip link set tap-vaigai up

# Delete the bridge's permanent FDB entry for tap-vaigai's MAC.
# Without this, the bridge treats unicast frames destined for tap-vaigai's
# own MAC as "local" and delivers them to the kernel protocol stack instead
# of forwarding them to the TAP fd where DPDK can read them.
VAIGAI_MAC=$(cat /sys/class/net/tap-vaigai/address)
bridge fdb del "$VAIGAI_MAC" dev tap-vaigai master 2>/dev/null || true

ok "tap-vaigai attached to $BRIDGE (FDB fix applied)"

echo ""
bridge link show master "$BRIDGE"
echo ""

# ── Phase 2: Manual testing ─────────────────────────────────────────────────
cat <<EOF

┌──────────────────────────────────────────────────────────────┐
│  1C Setup Complete — Use remote CLI for testing:             │
│                                                              │
│  $VAIGAI_BIN --attach                                        │
│                                                              │
│  Monitor:                                                    │
│    tail -f /tmp/vaigai-serial.log  (VM serial console)       │
│    tail -f /tmp/vaigai-fc.log      (vaigai log)              │
│                                                              │
│  Press Ctrl+C to tear down everything.                       │
└──────────────────────────────────────────────────────────────┘
EOF

print_traffic_commands "$SERVER_IP" 80 443 4433 5000 5001
echo ""

# Block until user presses Ctrl+C (cleanup runs via trap)
info "Waiting... Press Ctrl+C to clean up and exit."
wait "$VAIGAI_PID" 2>/dev/null || true
