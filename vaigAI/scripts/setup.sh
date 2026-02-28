#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# vaigai: hugepage and NIC setup script
set -euo pipefail

HUGEPAGES_1G=${HUGEPAGES_1G:-16}
HUGEPAGES_2M=${HUGEPAGES_2M:-0}
DPDK_DEVBIND=${DPDK_DEVBIND:-$(which dpdk-devbind.py 2>/dev/null || echo "")}

# -------------------------------------------------------------------------
usage() {
    cat <<EOF
Usage: $0 [OPTIONS] <pci-addr> [pci-addr...]

Options:
  --hugepages-1g  N    Number of 1 GB hugepages to allocate (default: $HUGEPAGES_1G)
  --hugepages-2m  N    Number of 2 MB hugepages to allocate (default: $HUGEPAGES_2M)
  --driver        DRV  DPDK PMD driver (default: vfio-pci)
  --unbind             Unbind and reset to kernel driver
  -h, --help           Show this help

Examples:
  $0 0000:01:00.0 0000:01:00.1
  $0 --driver igb_uio 0000:03:00.0
  $0 --unbind 0000:01:00.0
EOF
    exit 1
}

DRIVER="vfio-pci"
UNBIND=0
PCI_ADDRS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --hugepages-1g) HUGEPAGES_1G="$2"; shift 2 ;;
        --hugepages-2m) HUGEPAGES_2M="$2"; shift 2 ;;
        --driver)       DRIVER="$2"; shift 2 ;;
        --unbind)       UNBIND=1; shift ;;
        -h|--help)      usage ;;
        *)              PCI_ADDRS+=("$1"); shift ;;
    esac
done

# -------------------------------------------------------------------------
setup_hugepages_1g() {
    local n="$1"
    if [[ $n -le 0 ]]; then return; fi
    echo "Setting up $n × 1 GB hugepages..."
    for node in /sys/devices/system/node/node*/hugepages/hugepages-1048576kB; do
        [[ -d "$node" ]] || continue
        echo "$n" > "$node/nr_hugepages"
    done
    # Fallback: system-wide
    echo "$n" > /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages 2>/dev/null || true
    grep -i hugepage /proc/meminfo
}

setup_hugepages_2m() {
    local n="$1"
    if [[ $n -le 0 ]]; then return; fi
    echo "Setting up $n × 2 MB hugepages..."
    echo "$n" > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
    grep -i hugepage /proc/meminfo
}

load_vfio() {
    if ! lsmod | grep -q vfio_pci; then
        modprobe vfio-pci || { echo "ERROR: Could not load vfio-pci"; exit 1; }
    fi
    # Enable IOMMU if not already enabled
    if [[ ! -d /sys/kernel/iommu_groups ]]; then
        echo "WARNING: IOMMU not enabled. Add intel_iommu=on or amd_iommu=on to kernel cmdline"
    fi
}

bind_nic() {
    local pci="$1"
    if [[ -z "$DPDK_DEVBIND" ]]; then
        echo "ERROR: dpdk-devbind.py not found in PATH"
        exit 1
    fi
    echo "Binding $pci to $DRIVER..."
    "$DPDK_DEVBIND" --bind="$DRIVER" "$pci"
    echo "  Status:"
    "$DPDK_DEVBIND" --status-dev "$pci"
}

unbind_nic() {
    local pci="$1"
    echo "Unbinding $pci (restoring kernel driver)..."
    "$DPDK_DEVBIND" --unbind "$pci" 2>/dev/null || true
    "$DPDK_DEVBIND" --bind=auto "$pci" 2>/dev/null ||
        echo "WARNING: could not auto-bind $pci to kernel driver"
}

# -------------------------------------------------------------------------
echo "=== vaigai NIC setup ==="
setup_hugepages_1g "$HUGEPAGES_1G"
setup_hugepages_2m "$HUGEPAGES_2M"

if [[ "${#PCI_ADDRS[@]}" -eq 0 ]]; then
    echo "No PCI addresses given — hugepage setup only."
    exit 0
fi

if [[ $UNBIND -eq 1 ]]; then
    for pci in "${PCI_ADDRS[@]}"; do unbind_nic "$pci"; done
else
    [[ "$DRIVER" == "vfio-pci" ]] && load_vfio
    for pci in "${PCI_ADDRS[@]}"; do bind_nic "$pci"; done
fi

echo "=== Setup complete ==="
