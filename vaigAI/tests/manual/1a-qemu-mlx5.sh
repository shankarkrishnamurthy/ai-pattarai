#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# ═══════════════════════════════════════════════════════════════════════════════
#  1A — QEMU + Mellanox ConnectX NIC (loopback cable between two ports)
# ═══════════════════════════════════════════════════════════════════════════════
#  Topology: vaigai (DPDK mlx5, port0) ←loopback→ QEMU VM (vfio passthrough, port1)
#  Network:  10.0.0.1 (vaigai) ↔ 10.0.0.2 (VM)
#  NICs:     95:00.0 → vaigai  (bifurcated mlx5, stays on mlx5_core)
#            95:00.1 → VM      (PF passthrough, bind to vfio-pci)
#  QAT:     PF 0d:00.0 → VM    (PF passthrough, kernel qat_dh895xcc inside VM)
#           PF 0e:00.0 → vaigai (PF passthrough, DPDK crypto_qat PMD)
# ═══════════════════════════════════════════════════════════════════════════════

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/common.sh"
preflight
setup_hugepages

SERVER_IP=10.0.0.2
NIC_VM=0000:95:00.1
NIC_IFACE=ens30f1np1
QAT_PF_VM=0000:0d:00.0
QAT_PF_VAIGAI=0000:0e:00.0
INITRAMFS="${INITRAMFS:-/work/firecracker/initramfs-vm.img}"

# ── Cleanup function ────────────────────────────────────────────────────────
ROOTFS_COW=/tmp/1a-rootfs.ext4
cleanup() {
    info "Cleaning up 1A..."
    [[ -n "${QEMU_PID:-}" ]] && kill "$QEMU_PID" 2>/dev/null
    rm -f "$ROOTFS_COW"
    # Rebind VM NIC back to mlx5_core
    echo "$NIC_VM" > /sys/bus/pci/devices/$NIC_VM/driver/unbind 2>/dev/null || true
    echo "" > /sys/bus/pci/devices/$NIC_VM/driver_override 2>/dev/null || true
    echo "$NIC_VM" > /sys/bus/pci/drivers/mlx5_core/bind 2>/dev/null || true
    ok "1A cleanup done"
}
trap cleanup EXIT

# ── Setup: bind VM NIC + QAT PFs to vfio-pci ───────────────────────────────
modprobe vfio-pci
modprobe vfio_iommu_type1

ip link set "$NIC_IFACE" down 2>/dev/null || true
for DEV in $NIC_VM $QAT_PF_VM $QAT_PF_VAIGAI; do
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
    </dev/null >/tmp/1a-vm.log 2>&1 &
sleep 1
QEMU_PID=$(pgrep -f "1a-rootfs" | head -1)
info "QEMU started (PID $QEMU_PID, log → /tmp/1a-vm.log)"

# ── Post-boot setup ─────────────────────────────────────────────────────────
info "Waiting for VM to boot (15s)..."
sleep 15
# The VM init script (vaigai_mode=all) auto-configures eth1 (passthrough NIC)
# with 10.0.0.2/24 and starts services. Just verify:
ssh -o StrictHostKeyChecking=no -p 2222 root@localhost '
    # Ensure passthrough NIC has the test IP
    ip addr show eth1 | grep -q 10.0.0.2 || ip addr add 10.0.0.2/24 dev eth1
    ip link set eth1 up
    ss -tlnp | grep -E "5000|5001|5002|443|4433|80"
'
ok "VM server running at $SERVER_IP"

# ── Start vaigai ─────────────────────────────────────────────────────────────
info "Starting vaigai (interactive — Ctrl+C or 'quit' to exit)..."
echo ""
print_traffic_commands "$SERVER_IP"
echo ""

"$VAIGAI_BIN" -l 14-15 -n 4 -a 0000:95:00.0 -a 0000:0e:00.0 -- -I 10.0.0.1
