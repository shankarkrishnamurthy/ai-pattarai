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

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/common.sh"
preflight
setup_hugepages

SERVER_IP=10.0.0.2
NIC_VAIGAI=0000:83:00.0
NIC_VM=0000:83:00.1
QAT_PF_VM=0000:0d:00.0
QAT_PF_VAIGAI=0000:0e:00.0
INITRAMFS="${INITRAMFS:-/work/firecracker/initramfs-vm.img}"

# ── Cleanup function ────────────────────────────────────────────────────────
cleanup() {
    info "Cleaning up 1B..."
    [[ -n "${QEMU_PID:-}" ]] && kill "$QEMU_PID" 2>/dev/null
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
qemu-system-x86_64 -machine q35,accel=kvm -cpu host -m 1024M -smp 2 \
    -kernel /boot/vmlinuz-$(uname -r) \
    -initrd "$INITRAMFS" \
    -append "console=ttyS0,115200 root=/dev/vda rw quiet net.ifnames=0 biosdevname=0 vaigai_mode=all modprobe.blacklist=qat_dh895xcc,intel_qat" \
    -drive "file=/work/firecracker/rootfs.ext4,format=raw,if=virtio,cache=unsafe" \
    -nic user,model=virtio,hostfwd=tcp::2222-:22 \
    -device "vfio-pci,host=$NIC_VM" \
    -device "vfio-pci,host=$QAT_PF_VM" \
    -nographic &
QEMU_PID=$!
info "QEMU started (PID $QEMU_PID)"

# ── Post-boot setup ─────────────────────────────────────────────────────────
info "Waiting for VM to boot (15s)..."
sleep 15
ssh -o StrictHostKeyChecking=no -p 2222 root@localhost '
    # Find the passthrough NIC — it is the non-virtio Ethernet interface
    DATA_IF=$(ip -o link show | awk -F": " "/^[0-9]+: eth/{print \$2}" |
              while read iface; do
                  driver=$(readlink -f /sys/class/net/$iface/device/driver 2>/dev/null | xargs basename 2>/dev/null)
                  [ "$driver" != "virtio_net" ] && echo "$iface" && break
              done)
    [ -z "$DATA_IF" ] && DATA_IF=eth1  # fallback
    echo "Data interface: $DATA_IF"
    ip addr add 10.0.0.2/24 dev "$DATA_IF"
    ip link set "$DATA_IF" up
'
ssh -p 2222 root@localhost 'nginx -t && rc-service nginx start; ss -tlnp'
ssh -p 2222 root@localhost 'lspci | grep Co-pro && dmesg | grep -i qat | tail -3'
ok "VM server running at $SERVER_IP"

# ── Start vaigai ─────────────────────────────────────────────────────────────
info "Starting vaigai (interactive — Ctrl+C or 'quit' to exit)..."
echo ""
print_traffic_commands "$SERVER_IP"
echo ""

"$VAIGAI_BIN" -l 0-1 -n 4 -a 0000:83:00.0 -a 0000:0e:00.0 -- -I 10.0.0.1
