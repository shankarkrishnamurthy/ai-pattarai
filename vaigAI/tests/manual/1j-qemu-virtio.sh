#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# ═══════════════════════════════════════════════════════════════════════════════
#  1J — QEMU MicroVM + virtio-net (TAP bridge, vhost-net accelerated)
# ═══════════════════════════════════════════════════════════════════════════════
#  Topology: vaigai (net_tap) ↔ tap-vaigai ↔ br-vaigai ↔ tap-qemu0
#                                                        ↔ QEMU microVM (eth0)
#  Network:  192.168.205.1 (vaigai) ↔ 192.168.205.2 (VM)
#  No physical NIC needed.
#
#  QEMU uses the Firecracker-compatible microvm machine with vhost-net
#  acceleration on tap-qemu0.  The guest sees a standard virtio-net NIC (eth0).
#  Same kernel + rootfs as 1C, just QEMU instead of Firecracker.
#
#  Access VM serial: tail -f /tmp/vaigai-1j/serial.log
#
# ┌─────────────────────────────── Host ──────────────────────────────────┐
# │                                                                       │
# │  vaigAI                                                               │
# │    └─► DPDK net_tap0                                                  │
# │          │                                                            │
# │   ┌──────┴──────┐   ┌──────────────────┐   ┌──────────────┐         │
# │   │ tap-vaigai  │◄═►│  br-vaigai205    │◄═►│ tap-qemu205  │         │
# │   └─────────────┘   └──────────────────┘   └──────┬───────┘         │
# │   192.168.205.1      192.168.205.3/24        vhost-net               │
# │                                           ┌──────▼──────────────┐   │
# │                                           │  QEMU microVM       │   │
# │                                           │  Alpine · virtio-net│   │
# │                                           │  192.168.205.2      │   │
# │                                           │                     │   │
# │                                           │  nginx      :80/443 │   │
# │                                           │  openssl    :4433   │   │
# │                                           │  socat echo :5000   │   │
# │                                           │  socat sink :5001   │   │
# │                                           └─────────────────────┘   │
# └───────────────────────────────────────────────────────────────────────┘
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
  (no args)   Run everything: QEMU VM + vaigai
  --server    Start bridge + QEMU VM with all services (terminal 1)
  --vaigai    Start vaigai traffic generator only (terminal 2)
  --cleanup   Clean up all resources from previous runs
EOF
}

# ── Parse arguments ──────────────────────────────────────────────────────────
MODE=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --server)  MODE="server" ;;
        --vaigai)  MODE="vaigai" ;;
        --cleanup) MODE="cleanup" ;;
        -h|--help) usage; exit 0 ;;
        *) err "Unknown option: $1"; usage; exit 1 ;;
    esac
    shift
done

# ── Pre-flight checks ────────────────────────────────────────────────────────
[[ $EUID -ne 0 ]] && { err "Must run as root"; exit 1; }
if [[ "$MODE" == "vaigai" || -z "$MODE" ]]; then
    [[ ! -x "$VAIGAI_BIN" ]] && { err "vaigai not built — run: ninja -C $VAIGAI_DIR/build"; exit 1; }
fi
if [[ "$MODE" == "server" || -z "$MODE" ]]; then
    command -v qemu-system-x86_64 >/dev/null 2>&1 || { err "qemu-system-x86_64 not found — dnf install -y qemu-kvm"; exit 1; }
    VMLINUX="${VMLINUX:-/work/firecracker/vmlinux}"
    ROOTFS="${ROOTFS:-/work/firecracker/alpine.ext4}"
    [[ ! -f "$VMLINUX" ]] && { err "Kernel not found: $VMLINUX (set \$VMLINUX)"; exit 1; }
    [[ ! -f "$ROOTFS"  ]] && { err "Rootfs not found: $ROOTFS (set \$ROOTFS)"; exit 1; }
fi

# ── Constants ────────────────────────────────────────────────────────────────
SERVER_IP=192.168.205.2
VAIGAI_IP=192.168.205.1
BRIDGE=br-vaigai205
BRIDGE_IP=192.168.205.3
TAP_QEMU=tap-qemu205
ROOTFS_COW=/tmp/vaigai-1j-rootfs.ext4
STATE_DIR=/tmp/vaigai-1j
SERIAL_LOG="$STATE_DIR/serial.log"
VMLINUX="${VMLINUX:-/work/firecracker/vmlinux}"
ROOTFS="${ROOTFS:-/work/firecracker/alpine.ext4}"

# ── Print traffic & debug commands ───────────────────────────────────────────
print_traffic_commands() {
    local SIP="$1"
    cat <<CMDS

${BOLD}═══ Traffic Commands (at vaigai> prompt or via --attach) ═══${NC}

  ${CYAN}# Single-request tests (--one)${NC}
  start --ip $SIP --port 5000 --proto tcp --one
  start --ip $SIP --port 80 --proto http --one --url /
  start --ip $SIP --port 443 --proto https --one --url /
  start --ip $SIP --port 4433 --proto tls --one

  ${CYAN}# Duration / rate tests${NC}
  start --ip $SIP --port 5000 --proto tcp --duration 5
  start --ip $SIP --port 5000 --proto tcp --duration 5 --rate 1000
  start --ip $SIP --port 80 --proto http --duration 5 --url /
  start --ip $SIP --port 443 --proto https --duration 5 --url /
  start --ip $SIP --port 4433 --proto tls --duration 5
  start --ip $SIP --port 5001 --proto udp --size 1024 --duration 5

  ${CYAN}# Control${NC}
  stop
  reset

${BOLD}═══ Monitoring & Debug ═══${NC}
  stat net                      # snapshot of all counters
  stat net --rate               # per-second rates
  stat cpu                      # CPU utilization per lcore
  ping $SIP                     # ICMP ping
  show interface
  trace start /tmp/capture.pcapng
  trace stop
  quit
CMDS
}

# ── Helper: terminate process from PID file ──────────────────────────────────
stop_pidfile() {
    local f="$1"
    if [[ -f "$f" ]]; then
        local pid; pid=$(cat "$f")
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
    info "Cleaning up 1J..."
    echo "quit" | "$VAIGAI_BIN" --attach 2>/dev/null || true
    stop_pidfile "$STATE_DIR/vaigai.pid"
    stop_pidfile "$STATE_DIR/qemu.pid"
    ip link del "$TAP_QEMU" 2>/dev/null || true
    ip link del tap-vaigai 2>/dev/null || true
    ip link del "$BRIDGE" 2>/dev/null || true
    rm -f "$ROOTFS_COW"
    rm -rf "$STATE_DIR"
    ok "1J cleanup done"
}

# ── Server: bridge + QEMU microVM ────────────────────────────────────────────
start_server() {
    # Pre-clean leftovers
    stop_pidfile "$STATE_DIR/qemu.pid" 2>/dev/null || true
    ip link del "$TAP_QEMU" 2>/dev/null || true
    ip link del "$BRIDGE"   2>/dev/null || true
    rm -f "$ROOTFS_COW"
    mkdir -p "$STATE_DIR"

    # Bridge + QEMU TAP (pre-created; vaigai TAP is created by DPDK later)
    ip link add "$BRIDGE" type bridge
    ip link set "$BRIDGE" up
    ip addr add "$BRIDGE_IP/24" dev "$BRIDGE"

    ip tuntap add "$TAP_QEMU" mode tap
    ip link set "$TAP_QEMU" master "$BRIDGE"
    ip link set "$TAP_QEMU" up

    sysctl -q -w net.bridge.bridge-nf-call-iptables=0
    sysctl -q -w net.bridge.bridge-nf-call-ip6tables=0
    sysctl -q -w net.bridge.bridge-nf-call-arptables=0
    ok "Bridge $BRIDGE + $TAP_QEMU created"

    # COW copy of rootfs (guest writes go here, base image untouched)
    cp --reflink=auto "$ROOTFS" "$ROOTFS_COW"

    # Boot QEMU microVM (same machine model as Firecracker, no PCI bus)
    setsid qemu-system-x86_64 \
        -machine microvm,accel=kvm,pic=off,pit=off \
        -cpu host -m 256M -smp 1 -no-reboot -nographic \
        -kernel "$VMLINUX" \
        -drive "id=rootfs,file=$ROOTFS_COW,format=raw,if=none" \
        -device virtio-blk-device,drive=rootfs \
        -netdev "tap,id=nd0,ifname=$TAP_QEMU,vhost=on,script=no,downscript=no" \
        -device "virtio-net-device,netdev=nd0,mac=52:54:00:12:34:56" \
        -append "console=ttyS0 reboot=k panic=1 pci=off root=/dev/vda rw quiet vaigai_mode=all ip=$SERVER_IP::$BRIDGE_IP:255.255.255.0::eth0:off" \
        </dev/null >"$SERIAL_LOG" 2>&1 &
    local QEMU_PID=$!
    echo "$QEMU_PID" > "$STATE_DIR/qemu.pid"
    info "QEMU started (PID $QEMU_PID, serial → $SERIAL_LOG)"

    # Wait for VM to become reachable
    info "Waiting for VM to boot..."
    for i in $(seq 1 40); do
        ping -c 1 -W 1 "$SERVER_IP" >/dev/null 2>&1 && break
        sleep 1
    done
    if ping -c 1 -W 2 "$SERVER_IP" >/dev/null 2>&1; then
        ok "VM reachable at $SERVER_IP"
    else
        warn "VM not reachable after 40s — check: tail -f $SERIAL_LOG"
    fi
}

# ── Tgen: vaigai daemon + bridge attach ──────────────────────────────────────
start_tgen() {
    if ! ip link show "$BRIDGE" >/dev/null 2>&1; then
        err "Bridge $BRIDGE not found — run --server first"
        exit 1
    fi

    echo "quit" | "$VAIGAI_BIN" --attach 2>/dev/null || true
    stop_pidfile "$STATE_DIR/vaigai.pid"
    mkdir -p "$STATE_DIR"

    nohup "$VAIGAI_BIN" -l 0-1 --no-pci --vdev "net_tap0,iface=tap-vaigai" \
        -- -I "$VAIGAI_IP" </dev/null >/tmp/vaigai-1j.log 2>&1 &
    local VAIGAI_PID=$!
    echo "$VAIGAI_PID" > "$STATE_DIR/vaigai.pid"
    info "vaigai started (PID $VAIGAI_PID, log → /tmp/vaigai-1j.log)"

    # Wait for DPDK to create tap-vaigai
    for i in $(seq 1 30); do
        ip link show tap-vaigai >/dev/null 2>&1 && break
        sleep 0.5
    done
    ip link show tap-vaigai >/dev/null 2>&1 || { err "tap-vaigai not created"; exit 1; }

    # Attach to bridge + FDB fix (same as 1C)
    ip link set tap-vaigai master "$BRIDGE"
    ip link set tap-vaigai up
    local VAIGAI_MAC
    VAIGAI_MAC=$(cat /sys/class/net/tap-vaigai/address)
    bridge fdb del "$VAIGAI_MAC" dev tap-vaigai master 2>/dev/null || true
    ok "tap-vaigai attached to $BRIDGE"

    cat <<EOF

┌──────────────────────────────────────────────────────────────┐
│  vaigai running — use remote CLI:                            │
│                                                              │
│  $VAIGAI_BIN --attach                                        │
│                                                              │
│  Serial console: tail -f $SERIAL_LOG
│  vaigai log:     tail -f /tmp/vaigai-1j.log                  │
└──────────────────────────────────────────────────────────────┘
EOF
    print_traffic_commands "$SERVER_IP"
    echo ""

    trap 'stop_pidfile "$STATE_DIR/vaigai.pid"' EXIT
    info "Waiting... Press Ctrl+C to stop vaigai."
    wait "$VAIGAI_PID" 2>/dev/null || true
}

# ── Main ─────────────────────────────────────────────────────────────────────
case "$MODE" in
    server)
        setup_hugepages
        trap do_cleanup EXIT
        start_server
        ok "Server running. Press Ctrl+C to stop and clean up."
        info "Serial console: tail -f $SERIAL_LOG"
        sleep infinity
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
