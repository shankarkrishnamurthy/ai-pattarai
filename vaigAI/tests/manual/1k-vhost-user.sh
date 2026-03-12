#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# ═══════════════════════════════════════════════════════════════════════════════
#  1K — vhost-user (DPDK net_vhost ↔ QEMU guest VM)
# ═══════════════════════════════════════════════════════════════════════════════
#  Topology: vaigai (net_vhost, server) ↔ /tmp/vaigai-1k/vhost.sock
#                                        ↔ QEMU microVM (virtio-net eth0)
#  Network:  192.168.206.1 (vaigai) ↔ 192.168.206.2 (VM)
#  No physical NIC, no kernel in the data path.
#
#  ⚠  REVERSED STARTUP ORDER — vaigai creates the socket; QEMU connects to it:
#       Terminal 1:  sudo bash 1k-vhost-user.sh --vaigai   ← start FIRST
#       Terminal 2:  sudo bash 1k-vhost-user.sh --server   ← start SECOND
#
#  Requires: qemu-system-x86_64, DPDK built with net/vhost driver.
#  VM memory is backed by memfd shared memory (required by vhost-user protocol).
#  Access VM serial: tail -f /tmp/vaigai-1k/serial.log
#
# ┌─────────────────────────────── Host ──────────────────────────────────┐
# │                                                                       │
# │  vaigAI (DPDK net_vhost, server — creates socket)                    │
# │    └─► net_vhost0                                                     │
# │          │                                                            │
# │          │  vhost-user protocol (shared memory rings, zero-copy)      │
# │          │  /tmp/vaigai-1k/vhost.sock                                 │
# │          │                                                            │
# │   ┌──────▼──────────────────────────────────────────────────┐        │
# │   │  QEMU microVM  (vhost-user client)                      │        │
# │   │  Alpine · virtio-net eth0  (memory-backend-memfd)       │        │
# │   │  192.168.206.2                                          │        │
# │   │                                                         │        │
# │   │  nginx      :80/:443    openssl  :4433                  │        │
# │   │  socat echo :5000       socat sink :5001                │        │
# │   └─────────────────────────────────────────────────────────┘        │
# │                                                                       │
# │  vaigAI IP = 192.168.206.1                                           │
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
  ⚠  Start --vaigai FIRST (creates socket), then --server (starts QEMU).
  --vaigai    Start vaigai net_vhost server (interactive, creates socket)
  --server    Start QEMU VM (connects to vaigai's socket — run after --vaigai)
  --cleanup   Clean up all resources
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
    if ! find /usr/local/lib* -name "librte_net_vhost.so*" 2>/dev/null | grep -q .; then
        err "net_vhost PMD not found — rebuild DPDK with net/vhost driver:"
        err "  cd /work/dpdk-stable-24.11.1"
        err "  meson setup --reconfigure builddir -Denable_drivers=...,net/vhost"
        err "  ninja -C builddir && sudo ninja -C builddir install && sudo ldconfig"
        exit 1
    fi
fi
if [[ "$MODE" == "server" || -z "$MODE" ]]; then
    command -v qemu-system-x86_64 >/dev/null 2>&1 || { err "qemu-system-x86_64 not found — dnf install -y qemu-kvm"; exit 1; }
    VMLINUX="${VMLINUX:-/work/firecracker/vmlinux}"
    ROOTFS="${ROOTFS:-/work/firecracker/alpine.ext4}"
    [[ ! -f "$VMLINUX" ]] && { err "Kernel not found: $VMLINUX (set \$VMLINUX)"; exit 1; }
    [[ ! -f "$ROOTFS"  ]] && { err "Rootfs not found: $ROOTFS (set \$ROOTFS)"; exit 1; }
fi

# ── Constants ────────────────────────────────────────────────────────────────
SERVER_IP=192.168.206.2
VAIGAI_IP=192.168.206.1
STATE_DIR=/tmp/vaigai-1k
VHOST_SOCK="$STATE_DIR/vhost.sock"
ROOTFS_COW="$STATE_DIR/rootfs.ext4"
SERIAL_LOG="$STATE_DIR/serial.log"
VMLINUX="${VMLINUX:-/work/firecracker/vmlinux}"
ROOTFS="${ROOTFS:-/work/firecracker/alpine.ext4}"

# ── Print traffic & debug commands ───────────────────────────────────────────
print_traffic_commands() {
    local SIP="$1"
    cat <<CMDS

${BOLD}═══ Traffic Commands (at vaigai> prompt) ═══${NC}

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
  stat net
  stat net --rate
  stat cpu
  ping $SIP
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
    info "Cleaning up 1K..."
    stop_pidfile "$STATE_DIR/qemu.pid"
    echo "quit" | "$VAIGAI_BIN" --attach 2>/dev/null || true
    stop_pidfile "$STATE_DIR/vaigai.pid"
    rm -rf "$STATE_DIR"
    ok "1K cleanup done"
}

# ── Server: QEMU microVM (connects to vaigai's vhost socket) ─────────────────
start_server() {
    mkdir -p "$STATE_DIR"

    # Wait for vaigai to create the socket first
    if [[ ! -S "$VHOST_SOCK" ]]; then
        info "Waiting for vaigai to create $VHOST_SOCK (run --vaigai in another terminal)..."
        for i in $(seq 1 60); do
            [[ -S "$VHOST_SOCK" ]] && break
            sleep 0.5
        done
        [[ -S "$VHOST_SOCK" ]] || { err "Socket not found after 30s — run --vaigai first"; exit 1; }
    fi
    ok "Socket found: $VHOST_SOCK"

    # COW copy of rootfs
    cp --reflink=auto "$ROOTFS" "$ROOTFS_COW"

    # QEMU microVM with vhost-user client (connects to vaigai's socket)
    # VM memory backed by memfd shared memory — required by vhost-user protocol
    setsid qemu-system-x86_64 \
        -machine microvm,accel=kvm,pic=off,pit=off \
        -cpu host -m 256M -smp 1 -no-reboot -nographic \
        -object memory-backend-memfd,id=mem,size=256M,share=on \
        -numa node,memdev=mem \
        -kernel "$VMLINUX" \
        -drive "id=rootfs,file=$ROOTFS_COW,format=raw,if=none" \
        -device virtio-blk-device,drive=rootfs \
        -chardev "socket,id=vhost0,path=$VHOST_SOCK" \
        -netdev "vhost-user,id=nd0,chardev=vhost0" \
        -device "virtio-net-device,netdev=nd0,mac=52:54:00:12:34:57" \
        -append "console=ttyS0 reboot=k panic=1 pci=off root=/dev/vda rw quiet vaigai_mode=all ip=$SERVER_IP::$VAIGAI_IP:255.255.255.0::eth0:off" \
        </dev/null >"$SERIAL_LOG" 2>&1 &
    local QEMU_PID=$!
    echo "$QEMU_PID" > "$STATE_DIR/qemu.pid"
    info "QEMU started (PID $QEMU_PID, serial → $SERIAL_LOG)"
    info "VM booting — watch: tail -f $SERIAL_LOG"
}

# ── Tgen: vaigai as net_vhost server (interactive) ───────────────────────────
start_tgen() {
    mkdir -p "$STATE_DIR"
    rm -f "$VHOST_SOCK"

    warn "Start --server in another terminal AFTER this prompt appears."
    info "Starting vaigai with net_vhost (interactive — Ctrl+C or 'quit' to exit)..."
    info "Socket: $VHOST_SOCK"
    echo ""
    print_traffic_commands "$SERVER_IP"
    echo ""

    "$VAIGAI_BIN" -l 0-1 --no-pci \
        --vdev "net_vhost0,iface=$VHOST_SOCK,queues=1,client=0" \
        -- -I "$VAIGAI_IP"
}

# ── All-in-one: vaigai daemon first, then QEMU ───────────────────────────────
start_all() {
    mkdir -p "$STATE_DIR"
    rm -f "$VHOST_SOCK"

    # Start vaigai as daemon (creates socket, waits for QEMU to connect)
    nohup "$VAIGAI_BIN" -l 0-1 --no-pci \
        --vdev "net_vhost0,iface=$VHOST_SOCK,queues=1,client=0" \
        -- -I "$VAIGAI_IP" </dev/null >/tmp/vaigai-1k.log 2>&1 &
    local VAIGAI_PID=$!
    echo "$VAIGAI_PID" > "$STATE_DIR/vaigai.pid"
    info "vaigai started (PID $VAIGAI_PID, log → /tmp/vaigai-1k.log)"

    # Wait for socket
    for i in $(seq 1 20); do [[ -S "$VHOST_SOCK" ]] && break; sleep 0.5; done
    [[ -S "$VHOST_SOCK" ]] || { err "vaigai did not create $VHOST_SOCK"; exit 1; }
    ok "Socket ready: $VHOST_SOCK"

    # Now start QEMU
    start_server

    cat <<EOF

┌──────────────────────────────────────────────────────────────┐
│  vaigai + QEMU VM running — use remote CLI:                  │
│                                                              │
│  $VAIGAI_BIN --attach                                        │
│                                                              │
│  Serial console: tail -f $SERIAL_LOG
│  vaigai log:     tail -f /tmp/vaigai-1k.log                  │
└──────────────────────────────────────────────────────────────┘
EOF
    print_traffic_commands "$SERVER_IP"

    trap 'stop_pidfile "$STATE_DIR/vaigai.pid"; stop_pidfile "$STATE_DIR/qemu.pid"' EXIT
    info "Waiting... Press Ctrl+C to stop."
    wait "$VAIGAI_PID" 2>/dev/null || true
}

# ── Main ─────────────────────────────────────────────────────────────────────
case "$MODE" in
    server)
        setup_hugepages
        trap do_cleanup EXIT
        start_server
        ok "QEMU VM running. Press Ctrl+C to stop and clean up."
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
        start_all
        ;;
esac
