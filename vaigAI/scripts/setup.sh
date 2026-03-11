#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# vaigai: hugepage and NIC setup script
set -euo pipefail

HUGEPAGES_1G=${HUGEPAGES_1G:-16}
HUGEPAGES_2M=${HUGEPAGES_2M:-0}
DPDK_DEVBIND=${DPDK_DEVBIND:-$(which dpdk-devbind.py 2>/dev/null || echo "")}
# vaigai ephemeral port range — must match TGEN_EPHEMERAL_LO/HI in types.h
VAIGAI_PORT_LO=10000
VAIGAI_PORT_HI=59999
NFT_TABLE=vaigai

# -------------------------------------------------------------------------
usage() {
    cat <<EOF
Usage: $0 [OPTIONS] <pci-addr> [pci-addr...]

Options:
  --hugepages-1g  N    Number of 1 GB hugepages to allocate (default: $HUGEPAGES_1G)
  --hugepages-2m  N    Number of 2 MB hugepages to allocate (default: $HUGEPAGES_2M)
  --driver        DRV  DPDK PMD driver (default: vfio-pci)
  --unbind             Unbind and reset to kernel driver
  --suppress-rst       Add nftables rule to prevent kernel TCP stack from
                       sending RST on vaigai's ephemeral port range.
                       Required when using AF_PACKET (--no-pci) on NICs that
                       have kernel IP addresses assigned (e.g. eno1, ens20f0np0).
                       Without this, the kernel sends RST to remote hosts for
                       every TCP connection vaigai opens, killing the sessions.
  --clear-rst          Remove the RST suppression nftables rule.
  -h, --help           Show this help

Examples:
  $0 0000:01:00.0 0000:01:00.1
  $0 --driver igb_uio 0000:03:00.0
  $0 --unbind 0000:01:00.0
  $0 --suppress-rst            # required before using AF_PACKET on live NICs
  $0 --clear-rst               # remove when done
EOF
    exit 1
}

DRIVER="vfio-pci"
UNBIND=0
SUPPRESS_RST=0
CLEAR_RST=0
PCI_ADDRS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --hugepages-1g) HUGEPAGES_1G="$2"; shift 2 ;;
        --hugepages-2m) HUGEPAGES_2M="$2"; shift 2 ;;
        --driver)       DRIVER="$2"; shift 2 ;;
        --unbind)       UNBIND=1; shift ;;
        --suppress-rst) SUPPRESS_RST=1; shift ;;
        --clear-rst)    CLEAR_RST=1; shift ;;
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

# ── AF_PACKET: kernel RST suppression ─────────────────────────────────────
# When vaigai uses AF_PACKET (--no-pci) on a NIC that has a kernel IP address,
# the kernel TCP/IP stack sees every incoming TCP packet and sends RST for
# connections it doesn't own (vaigai's ephemeral ports).  Those RSTs reach the
# remote server and abort the connection before vaigai can complete the exchange.
# The nftables rule below silently drops outbound RSTs on vaigai's port range,
# preventing the kernel from interfering.  It has no effect on normal traffic.
suppress_kernel_rst() {
    echo "Adding nftables RST suppression for vaigai ports $VAIGAI_PORT_LO-$VAIGAI_PORT_HI..."
    if ! nft list table ip "$NFT_TABLE" &>/dev/null; then
        nft add table ip "$NFT_TABLE"
    fi
    if ! nft list chain ip "$NFT_TABLE" output &>/dev/null; then
        nft add chain ip "$NFT_TABLE" output \
            "{ type filter hook output priority -10; policy accept; }"
    fi
    # Flush and re-add to ensure idempotency
    nft flush chain ip "$NFT_TABLE" output
    nft add rule ip "$NFT_TABLE" output \
        tcp flags rst tcp sport "$VAIGAI_PORT_LO"-"$VAIGAI_PORT_HI" drop
    echo "  RST suppression active."
    echo "  To view:   nft list table ip $NFT_TABLE"
    echo "  To remove: $0 --clear-rst   (or: nft delete table ip $NFT_TABLE)"
}

clear_kernel_rst() {
    if nft list table ip "$NFT_TABLE" &>/dev/null; then
        nft delete table ip "$NFT_TABLE"
        echo "Removed nftables RST suppression (table ip $NFT_TABLE deleted)."
    else
        echo "RST suppression table ip $NFT_TABLE not found — nothing to remove."
    fi
}

# -------------------------------------------------------------------------
echo "=== vaigai NIC setup ==="

if [[ $SUPPRESS_RST -eq 1 ]]; then
    suppress_kernel_rst
fi

if [[ $CLEAR_RST -eq 1 ]]; then
    clear_kernel_rst
fi

setup_hugepages_1g "$HUGEPAGES_1G"
setup_hugepages_2m "$HUGEPAGES_2M"

if [[ "${#PCI_ADDRS[@]}" -eq 0 ]]; then
    if [[ $SUPPRESS_RST -eq 0 && $CLEAR_RST -eq 0 ]]; then
        echo "No PCI addresses given — hugepage setup only."
    fi
    exit 0
fi

if [[ $UNBIND -eq 1 ]]; then
    for pci in "${PCI_ADDRS[@]}"; do unbind_nic "$pci"; done
else
    [[ "$DRIVER" == "vfio-pci" ]] && load_vfio
    for pci in "${PCI_ADDRS[@]}"; do bind_nic "$pci"; done
fi

echo "=== Setup complete ==="
