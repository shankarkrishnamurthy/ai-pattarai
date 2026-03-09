#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# ═══════════════════════════════════════════════════════════════════════════════
#  1B — QEMU + Intel i40e NIC (loopback cable between two ports)
# ═══════════════════════════════════════════════════════════════════════════════
#  Topology: vaigai (DPDK i40e, port0) ←loopback→ QEMU VM (vfio passthrough, port1)
#  Network:  10.0.0.1 (vaigai) ↔ 10.0.0.2 (VM)
#  NICs:     83:00.0 → vaigai  (PF passthrough, bind to vfio-pci)
#            83:00.1 → VM      (PF passthrough, bind to vfio-pci)
#  QAT:     PF 0d:00.0 → VM    (PF passthrough, kernel qat_dh895xcc inside VM)
#           PF 0e:00.0 → vaigai (PF passthrough, DPDK crypto_qat PMD)
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

SERVER_IP=10.0.0.2
NIC_VAIGAI=0000:83:00.0
NIC_VM=0000:83:00.1
QAT_PF_VM=0000:0d:00.0
QAT_PF_VAIGAI=0000:0e:00.0
INITRAMFS="${INITRAMFS:-/work/firecracker/initramfs-vm.img}"

# ── Cleanup function ────────────────────────────────────────────────────────
ROOTFS_COW=/tmp/1b-rootfs.ext4
cleanup() {
    info "Cleaning up 1B..."
    [[ -n "${QEMU_PID:-}" ]] && kill "$QEMU_PID" 2>/dev/null
    rm -f "$ROOTFS_COW"
    # Rebind NICs back to i40e
    for DEV in $NIC_VAIGAI $NIC_VM; do
        echo "$DEV" > /sys/bus/pci/devices/$DEV/driver/unbind 2>/dev/null || true
        echo "" > /sys/bus/pci/devices/$DEV/driver_override 2>/dev/null || true
        echo "$DEV" > /sys/bus/pci/drivers/i40e/bind 2>/dev/null || true
    done
    ok "1B cleanup done"
}
trap cleanup EXIT

# ── Setup: bind NICs + QAT PFs to vfio-pci ──────────────────────────────────
modprobe vfio-pci
modprobe vfio_iommu_type1

for DEV in $NIC_VAIGAI $NIC_VM $QAT_PF_VM $QAT_PF_VAIGAI; do
    echo "$DEV" > /sys/bus/pci/devices/$DEV/driver/unbind 2>/dev/null || true
    echo "vfio-pci" > /sys/bus/pci/devices/$DEV/driver_override
    echo "$DEV" > /sys/bus/pci/drivers/vfio-pci/bind
done
info "NICs + QAT bound to vfio-pci"

# ── Start QEMU VM ───────────────────────────────────────────────────────────
# COW copy to avoid corrupting original rootfs
cp --reflink=auto /work/firecracker/rootfs.ext4 "$ROOTFS_COW"

# setsid + </dev/null to avoid SIGHUP/SIGTTIN when backgrounded
setsid qemu-system-x86_64 -machine q35,accel=kvm -cpu host -m 1024M -smp 2 \
    -kernel /boot/vmlinuz-$(uname -r) \
    -initrd "$INITRAMFS" \
    -append "console=ttyS0,115200 root=/dev/vda rw quiet net.ifnames=0 biosdevname=0 vaigai_mode=all modprobe.blacklist=qat_dh895xcc,intel_qat" \
    -drive "file=$ROOTFS_COW,format=raw,if=virtio,cache=unsafe" \
    -nic user,model=virtio,hostfwd=tcp::2222-:22 \
    -device "vfio-pci,host=$NIC_VM" \
    -device "vfio-pci,host=$QAT_PF_VM" \
    -nographic -no-reboot \
    </dev/null >/tmp/1b-vm.log 2>&1 &
sleep 1
QEMU_PID=$(pgrep -f "1b-rootfs" | head -1)
info "QEMU started (PID $QEMU_PID, log → /tmp/1b-vm.log)"

# ── Post-boot setup ─────────────────────────────────────────────────────────
info "Waiting for VM to boot (20s)..."
sleep 20
# The VM init script (vaigai_mode=all) auto-configures eth1 (passthrough NIC)
# with 10.0.0.2/24 and starts services. Verify connectivity:
ssh -o StrictHostKeyChecking=no -p 2222 root@localhost '
    # Ensure passthrough NIC has the test IP
    ip addr show eth1 | grep -q 10.0.0.2 || ip addr add 10.0.0.2/24 dev eth1
    ip link set eth1 up
    ss -tlnp | grep -E "5000|5001|5002|443|4433|80"
'
ok "VM server running at $SERVER_IP"

# ── Start vaigai ─────────────────────────────────────────────────────────────
# i40e NICs (83:xx) are on NUMA node 1 — must use node-1 lcores to avoid
# cross-NUMA mempool/DMA issues where NIC hardware receives but rte_eth_rx_burst
# returns 0.  On this system NUMA node 1 = CPUs 14-27,42-55.
info "Starting vaigai on NUMA node 1 lcores (interactive — Ctrl+C or 'quit' to exit)..."
echo ""
print_traffic_commands "$SERVER_IP"
echo ""

"$VAIGAI_BIN" -l 14-15 -n 4 -a 0000:83:00.0 -a 0000:0e:00.0 -- -I 10.0.0.1
