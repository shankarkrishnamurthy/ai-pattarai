#!/usr/bin/env bash
# Test: HTTP over real Mellanox ConnectX-4 NIC loopback + QEMU microVM.
#
# Topology:
#   vaigAI (DPDK mlx5 on NIC_VAIGAI) ←loopback cable→ QEMU VM (NIC_VM passthrough, nginx)
#
# NIC pair (default: ens30f0np0 ↔ ens30f1np1, PCI 0000:95:00.0 ↔ 0000:95:00.1)
#   - NIC_VAIGAI is used by vaigAI via DPDK mlx5 PMD (bifurcated — no kernel unbind)
#   - NIC_VM is unbound from mlx5_core, bound to vfio-pci, passed to QEMU VM
#
# The QEMU VM boots Alpine Linux with nginx serving static test files.
#
# Tests:
#   T1 — HTTP RPS: TCP SYN flood to nginx:80 (CPS = RPS for connection-per-request)
#         T1a: Unlimited (flood) rate
#         T1b: Rate-limited (TARGET_CPS connections/second)
#   T2 — HTTP Throughput: TCP data throughput to VM discard server (measures NIC capacity)
#
# Prerequisites:
#   - QEMU with KVM + vfio-pci support
#   - Host kernel vmlinuz + initramfs with mlx5 driver
#   - Alpine rootfs ext4 at $ROOTFS (with nginx, socat pre-installed)
#   - Mellanox NIC loopback pair (physical cable connecting two ports)
#   - IOMMU enabled (intel_iommu=on or amd_iommu=on)
#   - vfio-pci kernel module loadable
#   - Root privileges
#
# Parameterised variables (override via environment):
#   NIC_PCI_VAIGAI   PCI BDF for vaigAI side       (default: 0000:95:00.0)
#   NIC_PCI_VM       PCI BDF for VM passthrough     (default: 0000:95:00.1)
#   NIC_IFACE_VM     Kernel interface name for VM NIC (default: ens30f1np1)
#   VAIGAI_IP        vaigAI IP address               (default: 10.0.0.1)
#   VM_IP            VM IP address                    (default: 10.0.0.2)
#   FLOOD_DURATION   T1 flood duration in seconds     (default: 10)
#   TARGET_CPS       T1b rate-limited CPS target      (default: 5000)
#   THROUGHPUT_DUR   T2 throughput test duration       (default: 10)
#   THROUGHPUT_STREAMS T2 number of TCP streams        (default: 4)
#   HTTP_RESP_SIZE   Nginx test file name for throughput (default: 100k)
#   VM_MEM           VM memory in MiB                 (default: 1024)
#   VM_CPUS          VM vCPU count                    (default: 2)
#   DPDK_LCORES      DPDK lcore mask for vaigAI       (default: 0-3)
#   VMLINUX          Path to vmlinuz for QEMU         (default: /boot/vmlinuz-$(uname -r))
#   INITRAMFS        Path to initramfs for QEMU       (default: /boot/initramfs-$(uname -r).img)
#   ROOTFS           Path to Alpine rootfs ext4       (default: /work/firecracker/alpine.ext4)
#
# Usage:
#   bash tests/http_nic.sh [OPTIONS]
#
# Options:
#   --test <1|2|all>   Which test to run (default: all)
#   --keep             Don't tear down on exit (debugging)
#   -h, --help         Show this help message and exit.

set -euo pipefail

# ── parse arguments ───────────────────────────────────────────────────────────
RUN_TESTS="all"
KEEP=0

usage() {
    grep '^#' "$0" | grep -v '^#!/' | sed 's/^# \{0,2\}//'
    exit 0
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --test)
            [[ -n "${2:-}" ]] || { echo "Error: --test requires a value" >&2; exit 1; }
            RUN_TESTS="$2"; shift 2 ;;
        --keep)  KEEP=1; shift ;;
        -h|--help) usage ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

# ── parameterised config ─────────────────────────────────────────────────────
NIC_PCI_VAIGAI="${NIC_PCI_VAIGAI:-0000:95:00.0}"
NIC_PCI_VM="${NIC_PCI_VM:-0000:95:00.1}"
NIC_IFACE_VM="${NIC_IFACE_VM:-ens30f1np1}"
VAIGAI_IP="${VAIGAI_IP:-10.0.0.1}"
VM_IP="${VM_IP:-10.0.0.2}"
FLOOD_DURATION="${FLOOD_DURATION:-10}"
TARGET_CPS="${TARGET_CPS:-5000}"
THROUGHPUT_DUR="${THROUGHPUT_DUR:-10}"
THROUGHPUT_STREAMS="${THROUGHPUT_STREAMS:-4}"
HTTP_RESP_SIZE="${HTTP_RESP_SIZE:-100k}"
VM_MEM="${VM_MEM:-1024}"
VM_CPUS="${VM_CPUS:-2}"
# Use NUMA-aligned cores for the NIC's socket.
# 0000:95:00.0 is on NUMA node 1 → cores 14-27,42-55
# NOTE: Use only 2 lcores (1 mgmt + 1 worker) so all TCP connections
# stay in a single per-worker TCB store. With multiple workers, RSS
# may route SYN-ACKs to a different worker than the one that created
# the TCB, causing connection setup failures.
DPDK_LCORES="${DPDK_LCORES:-14-15}"
VMLINUX="${VMLINUX:-/boot/vmlinuz-$(uname -r)}"
INITRAMFS="${INITRAMFS:-/boot/initramfs-$(uname -r).img}"
ROOTFS="${ROOTFS:-/work/firecracker/alpine.ext4}"
VAIGAI_BIN="$(cd "$(dirname "$0")/.." && pwd)/build/vaigai"

PASS_COUNT=0
FAIL_COUNT=0

# ── colours ───────────────────────────────────────────────────────────────────
GRN='\033[0;32m'; RED='\033[0;31m'; CYN='\033[0;36m'; YLW='\033[0;33m'; NC='\033[0m'
info()  { echo -e "${CYN}[INFO]${NC}  $*"; }
warn()  { echo -e "${YLW}[WARN]${NC}  $*"; }
pass()  { echo -e "${GRN}[PASS]${NC}  http_nic: $*"; ((PASS_COUNT++)) || true; }
fail()  { echo -e "${RED}[FAIL]${NC}  http_nic: $*" >&2; ((FAIL_COUNT++)) || true; }
die()   { echo -e "${RED}[FATAL]${NC} http_nic: $*" >&2; exit 1; }

# ── helper: extract JSON field from vaigai output ─────────────────────────────
json_val() {
    grep -oP "\"$1\": *\K[0-9]+" <<< "$OUTPUT" | tail -1 || echo "0"
}

# ══════════════════════════════════════════════════════════════════════════════
#  vaigai FIFO-based lifecycle (same pattern as tcp_tap.sh / arp_test.sh)
# ══════════════════════════════════════════════════════════════════════════════
VAIGAI_FIFO=""
VAIGAI_PID=""
VAIGAI_LOG=""
VAIGAI_CFG=""
OUTPUT=""

vaigai_start() {
    VAIGAI_CFG=$(mktemp /tmp/vaigai_http_XXXXXX.json)
    cat > "$VAIGAI_CFG" <<EOCFG
{
  "flows": [{
    "src_ip_lo": "$VAIGAI_IP", "src_ip_hi": "$VAIGAI_IP",
    "dst_ip": "$VM_IP", "dst_port": 80,
    "http_url": "/$HTTP_RESP_SIZE",
    "http_host": "$VM_IP",
    "icmp_ping": false
  }],
  "load": { "max_concurrent": 8192, "target_cps": 0, "duration_secs": 0 }
}
EOCFG
    VAIGAI_FIFO=$(mktemp -u /tmp/vaigai_http_fifo_XXXXXX)
    mkfifo "$VAIGAI_FIFO"
    VAIGAI_LOG=$(mktemp /tmp/vaigai_http_out_XXXXXX.log)

    # DPDK mlx5 PMD uses bifurcated driver — no kernel unbind needed.
    # Use -a to allowlist only our NIC PCI address.
    # --socket-mem allocates hugepages on both NUMA nodes (NIC is on socket 1).
    # Auto-detect the NIC's NUMA node for optimal memory placement.
    local nic_numa
    nic_numa=$(cat /sys/bus/pci/devices/$NIC_PCI_VAIGAI/numa_node 2>/dev/null || echo 0)
    local socket_mem
    if [[ "$nic_numa" -eq 0 ]]; then
        socket_mem="256,0"
    else
        socket_mem="0,256"
    fi
    info "NIC $NIC_PCI_VAIGAI on NUMA node $nic_numa, socket-mem=$socket_mem"

    VAIGAI_CONFIG="$VAIGAI_CFG" "$VAIGAI_BIN" \
        -l "$DPDK_LCORES" -n 4 \
        --socket-mem "$socket_mem" \
        -a "$NIC_PCI_VAIGAI" -- \
        < "$VAIGAI_FIFO" > "$VAIGAI_LOG" 2>&1 &
    VAIGAI_PID=$!

    # Keep write end of FIFO open on fd 7 so vaigai doesn't see EOF
    exec 7>"$VAIGAI_FIFO"

    # Wait for DPDK init (mlx5 probe takes a moment)
    info "Waiting for vaigai DPDK mlx5 init..."
    local waited=0
    while [[ $waited -lt 30 ]]; do
        if grep -q "vaigai CLI\|tgen>\|vaigai>" "$VAIGAI_LOG" 2>/dev/null; then
            break
        fi
        if ! kill -0 "$VAIGAI_PID" 2>/dev/null; then
            info "=== vaigai early exit log ==="
            cat "$VAIGAI_LOG" >&2 || true
            info "=== end ==="
            die "vaigai exited prematurely (check log above)"
        fi
        sleep 1
        ((waited++)) || true
    done
    if [[ $waited -ge 30 ]]; then
        info "=== vaigai timeout log ==="
        cat "$VAIGAI_LOG" >&2 || true
        info "=== end ==="
        die "vaigai did not start within 30s"
    fi
    info "vaigai started (PID $VAIGAI_PID) on PCI $NIC_PCI_VAIGAI"
}

vaigai_reset() {
    echo "reset" >&7
    sleep 2
}

vaigai_cmd() {
    local cmd="$1"
    local start_bytes
    start_bytes=$(stat -c%s "$VAIGAI_LOG" 2>/dev/null || echo 0)

    printf '%s\n' "$cmd" >&7

    # Determine wait time from command
    local dur
    local cmd_type
    cmd_type=$(echo "$cmd" | awk '{print $1}')
    if [[ "$cmd_type" == "throughput" ]]; then
        # throughput tx <ip> <port> <duration_s> [streams]
        dur=$(echo "$cmd" | awk '{print $5}')
    elif [[ "$cmd_type" == "ping" ]]; then
        # ping <ip> [count=5] [size=56] [interval_ms=1000]
        local p_count p_interval_ms
        p_count=$(echo "$cmd" | awk '{print $3}')
        p_interval_ms=$(echo "$cmd" | awk '{print $5}')
        [[ "$p_count" =~ ^[0-9]+$ ]] || p_count=5
        [[ "$p_interval_ms" =~ ^[0-9]+$ ]] || p_interval_ms=1000
        dur=$(( (p_count * p_interval_ms + 999) / 1000 ))
    else
        # flood <proto> <ip> <duration_s> [rate] [size] [port]
        dur=$(echo "$cmd" | awk '{print $4}')
    fi
    if [[ "$dur" =~ ^[0-9]+$ ]] && [[ "$dur" -gt 0 ]]; then
        sleep $((dur + 3))
    else
        sleep 5
    fi

    # Request stats
    printf 'stats\n' >&7

    local attempts=0
    local found=0
    while [[ $found -eq 0 ]]; do
        if tail -c +$((start_bytes + 1)) "$VAIGAI_LOG" 2>/dev/null | grep -q '^}'; then
            found=1
        else
            sleep 1
            attempts=$((attempts + 1))
            if [[ $attempts -gt 60 ]]; then
                info "Timed out waiting for stats output"
                info "=== vaigai log tail ==="
                tail -30 "$VAIGAI_LOG" 2>/dev/null || true
                info "=== end log ==="
                break
            fi
        fi
    done
    sleep 0.5

    OUTPUT=$(tail -c +$((start_bytes + 1)) "$VAIGAI_LOG")
}

vaigai_stop() {
    if [[ -n "$VAIGAI_LOG" ]] && [[ -f "$VAIGAI_LOG" ]]; then
        info "=== vaigai process log ==="
        cat "$VAIGAI_LOG" >&2 || true
        info "=== end vaigai log ==="
    fi
    if [[ -n "$VAIGAI_PID" ]] && kill -0 "$VAIGAI_PID" 2>/dev/null; then
        printf 'quit\n' >&7 2>/dev/null || true
        exec 7>&- 2>/dev/null || true
        local waited=0
        while kill -0 "$VAIGAI_PID" 2>/dev/null && [[ $waited -lt 10 ]]; do
            sleep 0.5
            ((waited++)) || true
        done
        if kill -0 "$VAIGAI_PID" 2>/dev/null; then
            kill -9 "$VAIGAI_PID" 2>/dev/null || true
        fi
        wait "$VAIGAI_PID" 2>/dev/null || true
    else
        exec 7>&- 2>/dev/null || true
    fi
    rm -f "$VAIGAI_FIFO" "$VAIGAI_LOG" "$VAIGAI_CFG"
    VAIGAI_PID=""
}

# ══════════════════════════════════════════════════════════════════════════════
#  VFIO-PCI bind/unbind helpers
# ══════════════════════════════════════════════════════════════════════════════
ORIG_DRIVER_VM=""

vfio_bind() {
    local pci_addr="$1"
    local iface="$2"

    # Load vfio-pci if needed
    modprobe vfio-pci 2>/dev/null || true

    # Record original driver
    ORIG_DRIVER_VM=$(basename "$(readlink -f /sys/bus/pci/devices/$pci_addr/driver 2>/dev/null)" 2>/dev/null || echo "")

    if [[ "$ORIG_DRIVER_VM" == "vfio-pci" ]]; then
        info "NIC $pci_addr already bound to vfio-pci"
        return 0
    fi

    # Bring interface down first
    if ip link show "$iface" &>/dev/null; then
        ip link set "$iface" down 2>/dev/null || true
    fi

    # Unbind from current driver
    if [[ -n "$ORIG_DRIVER_VM" ]]; then
        info "Unbinding $pci_addr from $ORIG_DRIVER_VM"
        echo "$pci_addr" > "/sys/bus/pci/drivers/$ORIG_DRIVER_VM/unbind" 2>/dev/null || true
        sleep 0.5
    fi

    # Set driver override to vfio-pci
    echo "vfio-pci" > "/sys/bus/pci/devices/$pci_addr/driver_override"

    # Bind to vfio-pci
    echo "$pci_addr" > /sys/bus/pci/drivers/vfio-pci/bind 2>/dev/null || \
        echo "$pci_addr" > /sys/bus/pci/drivers_probe 2>/dev/null || true
    sleep 0.5

    # Verify
    local cur_driver
    cur_driver=$(basename "$(readlink -f /sys/bus/pci/devices/$pci_addr/driver 2>/dev/null)" 2>/dev/null || echo "")
    if [[ "$cur_driver" == "vfio-pci" ]]; then
        info "Bound $pci_addr to vfio-pci (was: $ORIG_DRIVER_VM)"
    else
        die "Failed to bind $pci_addr to vfio-pci (current: $cur_driver)"
    fi
}

vfio_unbind() {
    local pci_addr="$1"
    local orig_driver="$2"

    if [[ -z "$orig_driver" ]] || [[ "$orig_driver" == "vfio-pci" ]]; then
        return 0
    fi

    # Unbind from vfio-pci
    echo "$pci_addr" > /sys/bus/pci/drivers/vfio-pci/unbind 2>/dev/null || true
    sleep 0.3

    # Clear driver override
    echo "" > "/sys/bus/pci/devices/$pci_addr/driver_override" 2>/dev/null || true

    # Rebind to original driver
    echo "$pci_addr" > "/sys/bus/pci/drivers/$orig_driver/bind" 2>/dev/null || \
        echo "$pci_addr" > /sys/bus/pci/drivers_probe 2>/dev/null || true
    sleep 0.5

    info "Restored $pci_addr to $orig_driver"
}

# ══════════════════════════════════════════════════════════════════════════════
#  QEMU VM lifecycle
# ══════════════════════════════════════════════════════════════════════════════
QEMU_PID=""
ROOTFS_COW=""
QEMU_SERIAL=""

qemu_start() {
    info "Preparing QEMU VM with PCI passthrough of $NIC_PCI_VM"

    # COW copy of rootfs
    ROOTFS_COW=$(mktemp /tmp/vaigai-http-rootfs-XXXXXX.ext4)
    cp --reflink=auto "$ROOTFS" "$ROOTFS_COW"
    chmod 644 "$ROOTFS_COW"

    # Patch /etc/network/interfaces in the COW copy to use the test IP
    # (the original rootfs may have a different static IP configured)
    local mntdir
    mntdir=$(mktemp -d /tmp/vaigai-http-mnt-XXXXXX)
    mount -o loop "$ROOTFS_COW" "$mntdir"
    cat > "$mntdir/etc/network/interfaces" <<IFACES
auto lo
iface lo inet loopback

auto eth0
iface eth0 inet static
    address ${VM_IP}
    netmask 255.255.255.0
IFACES
    umount "$mntdir"
    rmdir "$mntdir"
    info "Patched rootfs: VM eth0 → $VM_IP/24"

    # Serial console log
    QEMU_SERIAL=$(mktemp /tmp/vaigai-http-serial-XXXXXX.log)

    # Find IOMMU group for the NIC
    local iommu_grp
    iommu_grp=$(basename "$(readlink /sys/bus/pci/devices/$NIC_PCI_VM/iommu_group)")
    [[ -c "/dev/vfio/$iommu_grp" ]] || die "VFIO group /dev/vfio/$iommu_grp not found (is $NIC_PCI_VM bound to vfio-pci?)"
    info "Using VFIO IOMMU group $iommu_grp (/dev/vfio/$iommu_grp)"

    # Kernel cmdline: configure networking on the passthrough NIC
    # net.ifnames=0 ensures the Mellanox NIC appears as eth0
    # vaigai_mode=http tells the init script to start nginx
    local kcmd="console=ttyS0 root=/dev/vda rw quiet"
    kcmd+=" net.ifnames=0 biosdevname=0"
    kcmd+=" vaigai_mode=all"
    kcmd+=" ip=${VM_IP}:::255.255.255.0::eth0:off"

    # Kill any leftover QEMU using the same vfio device
    local stale_pid
    stale_pid=$(pgrep -f "vfio-pci,host=$NIC_PCI_VM" 2>/dev/null || true)
    if [[ -n "$stale_pid" ]]; then
        warn "Killing stale QEMU PID $stale_pid using $NIC_PCI_VM"
        kill "$stale_pid" 2>/dev/null; sleep 1
        kill -9 "$stale_pid" 2>/dev/null; wait "$stale_pid" 2>/dev/null || true
    fi

    QEMU_ERR=$(mktemp /tmp/vaigai-http-qemu-err-XXXXXX.log)
    info "Booting QEMU VM ($VM_CPUS vCPUs, ${VM_MEM}M RAM, passthrough $NIC_PCI_VM)"
    qemu-system-x86_64 \
        -machine q35,accel=kvm \
        -cpu host \
        -m "${VM_MEM}M" \
        -smp "$VM_CPUS" \
        -kernel "$VMLINUX" \
        -initrd "$INITRAMFS" \
        -append "$kcmd" \
        -drive "file=$ROOTFS_COW,format=raw,if=virtio,cache=unsafe" \
        -device "vfio-pci,host=$NIC_PCI_VM" \
        -nographic \
        -serial "file:$QEMU_SERIAL" \
        -monitor none \
        -no-reboot \
        </dev/null >/dev/null 2>"$QEMU_ERR" &
    QEMU_PID=$!
    sleep 1

    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        info "=== QEMU early exit ==="
        info "--- stderr ---"
        cat "$QEMU_ERR" 2>/dev/null || true
        info "--- serial ---"
        cat "$QEMU_SERIAL" 2>/dev/null || true
        info "=== end ==="
        die "QEMU exited immediately (check log above)"
    fi

    # Wait for VM to boot and configure networking
    info "Waiting for VM at $VM_IP (up to 90s for kernel + mlx5 init)..."
    local waited=0
    while [[ $waited -lt 90 ]]; do
        # Check serial log for networking progress
        if grep -q "nginx started\|vaigAI test services" "$QEMU_SERIAL" 2>/dev/null; then
            info "VM init scripts completed"
            break
        fi
        # Also try ping (but ARP might not work from host to passthrough NIC)
        sleep 2
        ((waited+=2)) || true

        # Bail if QEMU died
        if ! kill -0 "$QEMU_PID" 2>/dev/null; then
            info "=== QEMU died — stderr ==="
            cat "$QEMU_ERR" 2>/dev/null || true
            info "=== QEMU died — serial log ==="
            cat "$QEMU_SERIAL" 2>/dev/null || true
            info "=== end ==="
            die "QEMU exited during boot"
        fi
    done

    # Show relevant serial output
    info "=== VM serial (last 30 lines) ==="
    tail -30 "$QEMU_SERIAL" 2>/dev/null || true
    info "=== end serial ==="

    # Additional wait for nginx and NIC link to fully come up
    sleep 5

    info "QEMU VM started (PID $QEMU_PID)"
}

qemu_stop() {
    if [[ -n "$QEMU_PID" ]] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill "$QEMU_PID" 2>/dev/null || true
        local waited=0
        while kill -0 "$QEMU_PID" 2>/dev/null && [[ $waited -lt 10 ]]; do
            sleep 0.5
            ((waited++)) || true
        done
        if kill -0 "$QEMU_PID" 2>/dev/null; then
            kill -9 "$QEMU_PID" 2>/dev/null || true
        fi
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    rm -f "$ROOTFS_COW" "$QEMU_SERIAL" "$QEMU_ERR"
    QEMU_PID=""
}

# ── pre-flight checks ────────────────────────────────────────────────────────
[[ $EUID -eq 0 ]]        || die "Must run as root"
[[ -x "$VAIGAI_BIN" ]]   || die "vaigai binary not found: $VAIGAI_BIN"
command -v qemu-system-x86_64 &>/dev/null || die "qemu-system-x86_64 not found"
[[ -f "$VMLINUX" ]]       || die "vmlinuz not found: $VMLINUX"
[[ -f "$INITRAMFS" ]]     || die "initramfs not found: $INITRAMFS"
[[ -f "$ROOTFS" ]]        || die "rootfs not found: $ROOTFS"
[[ -d "/sys/bus/pci/devices/$NIC_PCI_VAIGAI" ]] || die "PCI device $NIC_PCI_VAIGAI not found"
[[ -d "/sys/bus/pci/devices/$NIC_PCI_VM" ]]     || die "PCI device $NIC_PCI_VM not found"
modprobe vfio-pci 2>/dev/null || die "Cannot load vfio-pci module"

# Verify IOMMU groups are isolated (no other devices sharing the group)
vm_iommu_grp=$(basename "$(readlink /sys/bus/pci/devices/$NIC_PCI_VM/iommu_group)")
n_devs=$(ls /sys/bus/pci/devices/$NIC_PCI_VM/iommu_group/devices/ | wc -l)
if [[ "$n_devs" -gt 1 ]]; then
    warn "IOMMU group $vm_iommu_grp has $n_devs devices — passthrough may fail"
    warn "Devices in group:"
    ls /sys/bus/pci/devices/$NIC_PCI_VM/iommu_group/devices/
fi

info "Configuration summary:"
info "  vaigAI NIC:        $NIC_PCI_VAIGAI"
info "  VM NIC:            $NIC_PCI_VM ($NIC_IFACE_VM)"
info "  vaigAI IP:         $VAIGAI_IP"
info "  VM IP:             $VM_IP"
info "  Flood duration:    ${FLOOD_DURATION}s"
info "  Target CPS:        $TARGET_CPS"
info "  Throughput dur:    ${THROUGHPUT_DUR}s"
info "  Throughput streams: $THROUGHPUT_STREAMS"
info "  HTTP response:     $HTTP_RESP_SIZE"
info "  DPDK lcores:       $DPDK_LCORES"
info "  VM:                $VM_CPUS vCPUs, ${VM_MEM}M RAM"

# ── teardown ──────────────────────────────────────────────────────────────────
teardown() {
    [[ $KEEP -eq 1 ]] && { info "Keeping topology (--keep)"; return; }
    info "Tearing down"
    vaigai_stop
    qemu_stop
    # Restore VM NIC to original driver
    if [[ -n "$ORIG_DRIVER_VM" ]] && [[ "$ORIG_DRIVER_VM" != "vfio-pci" ]]; then
        vfio_unbind "$NIC_PCI_VM" "$ORIG_DRIVER_VM"
    fi
}
trap teardown EXIT

# ══════════════════════════════════════════════════════════════════════════════
#  Step 1: Bind VM NIC to vfio-pci
# ══════════════════════════════════════════════════════════════════════════════
info "Step 1: Binding $NIC_PCI_VM to vfio-pci for passthrough"
vfio_bind "$NIC_PCI_VM" "$NIC_IFACE_VM"

# ══════════════════════════════════════════════════════════════════════════════
#  Step 2: Start QEMU VM with PCI passthrough
# ══════════════════════════════════════════════════════════════════════════════
info "Step 2: Starting QEMU VM"
qemu_start

# ══════════════════════════════════════════════════════════════════════════════
#  Step 3: Start vaigai on DPDK mlx5
# ══════════════════════════════════════════════════════════════════════════════
info "Step 3: Starting vaigai (DPDK mlx5 PMD on $NIC_PCI_VAIGAI)"
vaigai_start

# ══════════════════════════════════════════════════════════════════════════════
#  Step 4: Connectivity check — ARP + ICMP ping via vaigai
# ══════════════════════════════════════════════════════════════════════════════
info "Step 4: Connectivity check — ping $VM_IP"

# Retry ping up to 5 times — VM NIC (PCI passthrough) may take time to come up
ping_replies=0
for attempt in 1 2 3 4 5; do
    vaigai_cmd "ping $VM_IP 5 56 500"
    ping_replies=$(echo "$OUTPUT" | grep -c "Reply from" || true)
    local_rx=$(json_val rx_pkts)
    info "  Ping attempt $attempt: $ping_replies replies received, rx_pkts=$local_rx"
    if [[ "$ping_replies" -gt 0 ]]; then
        break
    fi
    info "  Waiting 3s for VM NIC to settle..."
    sleep 3
done

if [[ "$ping_replies" -gt 0 ]]; then
    pass "Connectivity check: ICMP ping successful ($ping_replies replies)"
else
    # Connectivity failed — dump diagnostics and try to root-cause
    warn "Connectivity check failed — dumping diagnostics"
    info "=== vaigai log ==="
    cat "$VAIGAI_LOG" >&2 || true
    info "=== VM serial ==="
    cat "$QEMU_SERIAL" >&2 || true
    info "=== end diagnostics ==="

    # Try packet trace for deeper analysis
    info "Starting packet trace for diagnosis..."
    vaigai_reset
    start_bytes=$(stat -c%s "$VAIGAI_LOG" 2>/dev/null || echo 0)
    printf 'trace start 0 0 50\n' >&7
    sleep 1
    printf 'ping %s 3 56 500\n' "$VM_IP" >&7
    sleep 5
    printf 'trace stop\n' >&7
    sleep 1
    printf 'trace save /tmp/vaigai_http_diag.pcapng\n' >&7
    sleep 1
    info "Diagnostic trace saved to /tmp/vaigai_http_diag.pcapng"
    OUTPUT=$(tail -c +$((start_bytes + 1)) "$VAIGAI_LOG")
    info "=== vaigai trace output ==="
    echo "$OUTPUT" >&2
    info "=== end ==="

    fail "Connectivity check: ICMP ping got no replies ($ping_replies replies, rx_pkts=$local_rx)"
    die "Cannot proceed without connectivity — check NIC loopback cable and VM networking"
fi

# ══════════════════════════════════════════════════════════════════════════════
#  T1: HTTP RPS — SYN flood to nginx:80
#
#  Each SYN completes the 3-way handshake with nginx, establishing a
#  connection. For HTTP "connection-per-request" patterns (HTTP/1.0 or
#  Connection: close), CPS ≈ RPS. This measures the TCP fast path and
#  NIC's connection-establishment performance.
# ══════════════════════════════════════════════════════════════════════════════
run_t1() {
    info "═══════════════════════════════════════════════════════"
    info "T1: HTTP RPS — SYN flood to ${VM_IP}:80"
    info "═══════════════════════════════════════════════════════"

    # ── T1a: Unlimited (flood) CPS ────────────────────────────────────
    info "T1a: Unlimited CPS → ${VM_IP}:80 (${FLOOD_DURATION}s, flood)"
    vaigai_cmd "flood tcp $VM_IP $FLOOD_DURATION 0 56 80"

    local tx_pkts
    tx_pkts=$(json_val tx_pkts)

    info "  tx_pkts=$tx_pkts"
    if [[ "$FLOOD_DURATION" -gt 0 ]] && [[ "$tx_pkts" -gt 0 ]]; then
        local cps=$((tx_pkts / FLOOD_DURATION))
        info "  Flood CPS: $cps SYN packets/second"
    fi

    [[ "$tx_pkts" -gt 0 ]]  && pass "T1a tx_pkts > 0 ($tx_pkts SYN packets sent)" \
                              || fail "T1a tx_pkts = 0 (no SYN packets sent)"

    # ── Reset between sub-tests ───────────────────────────────────────
    vaigai_reset

    # ── T1b: Rate-limited CPS ─────────────────────────────────────────
    info "T1b: Rate-limited CPS → ${VM_IP}:80 (${FLOOD_DURATION}s, target ${TARGET_CPS} cps)"
    vaigai_cmd "flood tcp $VM_IP $FLOOD_DURATION $TARGET_CPS 56 80"

    tx_pkts=$(json_val tx_pkts)

    info "  tx_pkts=$tx_pkts"
    if [[ "$FLOOD_DURATION" -gt 0 ]] && [[ "$tx_pkts" -gt 0 ]]; then
        local actual_cps=$((tx_pkts / FLOOD_DURATION))
        info "  Measured CPS: $actual_cps (target: $TARGET_CPS)"

        # Check rate is within 50% of target (rate limiting is approximate)
        local lower=$((TARGET_CPS / 2))
        local upper=$((TARGET_CPS * 2))
        if [[ "$actual_cps" -ge "$lower" ]] && [[ "$actual_cps" -le "$upper" ]]; then
            pass "T1b rate-limited CPS within range ($actual_cps ≈ $TARGET_CPS)"
        else
            fail "T1b rate-limited CPS out of range ($actual_cps vs target $TARGET_CPS)"
        fi
    fi

    [[ "$tx_pkts" -gt 0 ]]  && pass "T1b tx_pkts > 0 ($tx_pkts SYN packets sent)" \
                              || fail "T1b tx_pkts = 0 (no SYN packets sent)"
}

# ══════════════════════════════════════════════════════════════════════════════
#  T2: HTTP Throughput — TCP data throughput over Mellanox 50G NIC
#
#  Measures raw TCP payload throughput. vaigai opens multiple TCP streams
#  to the VM's discard server (:5001) and pumps data continuously.
#  This validates the NIC's data-plane performance at the TCP layer,
#  which is the transport underpinning HTTP throughput.
# ══════════════════════════════════════════════════════════════════════════════
run_t2() {
    info "═══════════════════════════════════════════════════════"
    info "T2: HTTP Throughput (TCP data) → ${VM_IP}:5001"
    info "═══════════════════════════════════════════════════════"

    info "T2: Throughput TX → ${VM_IP}:5001 (${THROUGHPUT_DUR}s, ${THROUGHPUT_STREAMS} streams)"
    vaigai_cmd "throughput tx $VM_IP 5001 $THROUGHPUT_DUR $THROUGHPUT_STREAMS"

    local payload_tx retransmit conn_open conn_close mbps_line
    payload_tx=$(json_val tcp_payload_tx)
    retransmit=$(json_val tcp_retransmit)
    conn_open=$(json_val tcp_conn_open)
    conn_close=$(json_val tcp_conn_close)
    mbps_line=$(grep -oP '\[SUM\].*' <<< "$OUTPUT" || echo "(no summary)")

    info "  payload_tx=$payload_tx retransmit=$retransmit conn_open=$conn_open conn_close=$conn_close"
    info "  $mbps_line"

    if [[ "$THROUGHPUT_DUR" -gt 0 ]] && [[ "$payload_tx" -gt 0 ]]; then
        local mbps=$(( (payload_tx * 8) / (THROUGHPUT_DUR * 1000000) ))
        info "  Calculated throughput: ${mbps} Mbps"
    fi

    [[ "$payload_tx" -gt 0 ]] && pass "T2 payload_tx > 0 ($payload_tx bytes)" \
                               || fail "T2 payload_tx = 0 (no data transmitted)"
    [[ "$conn_open" -gt 0 ]]  && pass "T2 conn_open > 0 ($conn_open streams connected)" \
                               || fail "T2 conn_open = 0 (no connections established)"
    [[ "$retransmit" -eq 0 ]] && pass "T2 retransmit = 0 (clean transfer)" \
                               || warn "T2 retransmit = $retransmit (some retransmissions)"
}

# ══════════════════════════════════════════════════════════════════════════════
#  Run selected tests
# ══════════════════════════════════════════════════════════════════════════════
should_run() { [[ "$RUN_TESTS" == "all" || "$RUN_TESTS" == "$1" ]]; }

should_run 1 && run_t1
should_run 2 && { vaigai_reset; run_t2; }

# ── summary ───────────────────────────────────────────────────────────────────
echo ""
info "═══════════════════════════════════════════════════════"
info "  HTTP NIC Test Results: $PASS_COUNT passed, $FAIL_COUNT failed"
info "═══════════════════════════════════════════════════════"
[[ $FAIL_COUNT -eq 0 ]] || exit 1
