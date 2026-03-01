#!/usr/bin/env bash
# Test: ARP over dual TAP + Linux bridge + Firecracker microVM.
#
# Validates the ARP subsystem using the same topology as tcp_tap.sh:
#   vaigAI (DPDK net_tap) ↔ tap-vaigai ↔ br-vaigai ↔ tap-fc0 ↔ Firecracker VM
#
# Runs three tests:
#   T1 — ARP Resolution: vaigai sends ARP request for VM, verifies MAC cache
#   T2 — ARP Reply: VM ARPs for vaigai's IP, vaigai responds correctly
#   T3 — ARP + ICMP: End-to-end ICMP ping requiring ARP resolution first
#
# Prerequisites:
#   - firecracker binary in PATH (or $FIRECRACKER)
#   - vmlinux kernel at $VMLINUX (default: /work/firecracker/vmlinux)
#   - Alpine rootfs ext4 at $ROOTFS (default: /work/firecracker/alpine.ext4)
#   - Root privileges
#
# Usage:
#   bash tests/arp_test.sh [OPTIONS]
#
# Options:
#   --test <1|2|3|all>   Which test to run (default: all)
#   --keep               Don't tear down on exit (debugging)
#   -h, --help           Show this help message and exit.

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

# ── config ────────────────────────────────────────────────────────────────────
BRIDGE="br-vaigai"
TAP_VAIGAI="tap-vaigai"
TAP_FC="tap-fc0"
SUBNET="192.168.204"
VAIGAI_IP="${SUBNET}.1"
VM_IP="${SUBNET}.2"
BRIDGE_IP="${SUBNET}.3"   # host kernel uses this for ping — NOT .1
BRIDGE_MAC="02:42:c0:a8:cc:03"  # unique bridge MAC — must differ from TAP
VM_MAC="AA:FC:00:00:00:01"
DPDK_LCORES="0-1"
VAIGAI_BIN="$(cd "$(dirname "$0")/.." && pwd)/build/vaigai"
FIRECRACKER="${FIRECRACKER:-firecracker}"
VMLINUX="${VMLINUX:-/work/firecracker/vmlinux}"
ROOTFS="${ROOTFS:-/work/firecracker/alpine.ext4}"
FC_SOCKET="/tmp/vaigai-arp-fc.sock"
TRACE_DIR=$(mktemp -d /tmp/vaigai-arp-trace-XXXXXX)
PASS_COUNT=0
FAIL_COUNT=0

# ── colours ───────────────────────────────────────────────────────────────────
GRN='\033[0;32m'; RED='\033[0;31m'; CYN='\033[0;36m'; NC='\033[0m'
info()  { echo -e "${CYN}[INFO]${NC}  $*"; }
pass()  { echo -e "${GRN}[PASS]${NC}  arp_test: $*"; ((PASS_COUNT++)) || true; }
fail()  { echo -e "${RED}[FAIL]${NC}  arp_test: $*" >&2; ((FAIL_COUNT++)) || true; }
die()   { echo -e "${RED}[FATAL]${NC} arp_test: $*" >&2; exit 1; }

# ── helper: extract JSON field from vaigai output ─────────────────────────────
json_val() {
    grep -oP "\"$1\": *\K[0-9]+" <<< "$OUTPUT" | tail -1 || echo "0"
}

# ── vaigai FIFO-based lifecycle ───────────────────────────────────────────────
VAIGAI_FIFO=""
VAIGAI_PID=""
VAIGAI_LOG=""
VAIGAI_CFG=""
OUTPUT=""

vaigai_start() {
    VAIGAI_CFG=$(mktemp /tmp/vaigai_arp_XXXXXX.json)
    cat > "$VAIGAI_CFG" <<EOCFG
{
  "flows": [{
    "src_ip_lo": "$VAIGAI_IP", "src_ip_hi": "$VAIGAI_IP",
    "dst_ip": "$VM_IP", "dst_port": 9,
    "icmp_ping": true
  }],
  "load": { "max_concurrent": 1024, "target_cps": 0, "duration_secs": 0 }
}
EOCFG
    VAIGAI_FIFO=$(mktemp -u /tmp/vaigai_arp_fifo_XXXXXX)
    mkfifo "$VAIGAI_FIFO"
    VAIGAI_LOG=$(mktemp /tmp/vaigai_arp_out_XXXXXX.log)

    VAIGAI_CONFIG="$VAIGAI_CFG" "$VAIGAI_BIN" \
        -l "$DPDK_LCORES" -n 1 --no-pci \
        --vdev "net_tap0,iface=$TAP_VAIGAI" -- \
        < "$VAIGAI_FIFO" > "$VAIGAI_LOG" 2>&1 &
    VAIGAI_PID=$!

    exec 7>"$VAIGAI_FIFO"

    sleep 2  # let DPDK init and create the TAP

    if ip link show "$TAP_VAIGAI" &>/dev/null; then
        # CRITICAL: Change the kernel-side TAP MAC before adding to the bridge.
        # When a port is added to a Linux bridge, the bridge registers the port's
        # MAC as a "local" FDB entry.  Frames addressed to a "local" MAC are
        # delivered to the kernel's own IP stack (BR_INPUT) instead of being
        # forwarded through the TAP fd to DPDK.  By setting the kernel MAC to
        # something different from the DPDK MAC, the bridge's "local" entry
        # won't match ARP replies addressed to DPDK's MAC, so the bridge
        # correctly forwards them through the TAP fd.
        local dpdk_mac
        dpdk_mac=$(cat /sys/class/net/$TAP_VAIGAI/address 2>/dev/null || true)
        ip link set "$TAP_VAIGAI" address "02:00:de:ad:00:01"
        info "Changed $TAP_VAIGAI kernel MAC from $dpdk_mac to 02:00:de:ad:00:01 (DPDK keeps $dpdk_mac)"
        ip link set "$TAP_VAIGAI" master "$BRIDGE"
        ip link set "$TAP_VAIGAI" up
        info "Attached $TAP_VAIGAI to $BRIDGE"
    else
        die "DPDK did not create $TAP_VAIGAI — check vaigai log: $VAIGAI_LOG"
    fi
}

vaigai_cmd() {
    local cmd="$1"
    local start_bytes
    start_bytes=$(stat -c%s "$VAIGAI_LOG" 2>/dev/null || echo 0)

    printf '%s\n' "$cmd" >&7

    # Wait based on command duration arg (field 4 for flood)
    local dur
    dur=$(echo "$cmd" | awk '{print $4}')
    if [[ "$dur" =~ ^[0-9]+$ ]] && [[ "$dur" -gt 0 ]]; then
        sleep $((dur + 2))
    else
        sleep 3
    fi

    printf 'stats\n' >&7

    local attempts=0
    local found=0
    while [[ $found -eq 0 ]]; do
        if tail -c +$((start_bytes + 1)) "$VAIGAI_LOG" 2>/dev/null | grep -q '^}'; then
            found=1
        else
            sleep 1
            attempts=$((attempts + 1))
            if [[ $attempts -gt 30 ]]; then
                info "Timed out waiting for stats output"
                tail -30 "$VAIGAI_LOG" 2>/dev/null || true
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

# ── pre-flight ────────────────────────────────────────────────────────────────
[[ $EUID -eq 0 ]]        || die "Must run as root"
[[ -x "$VAIGAI_BIN" ]]   || die "vaigai binary not found: $VAIGAI_BIN"
command -v "$FIRECRACKER" &>/dev/null || die "firecracker not found (set \$FIRECRACKER)"
[[ -f "$VMLINUX" ]]       || die "vmlinux not found: $VMLINUX"
[[ -f "$ROOTFS" ]]        || die "rootfs not found: $ROOTFS"
for cmd_name in ip curl arping tcpdump tshark; do
    command -v "$cmd_name" &>/dev/null || die "Required: $cmd_name"
done
info "Trace output directory: $TRACE_DIR"

# ── tcpdump on bridge (Linux-side ARP capture) ────────────────────────────────
TCPDUMP_PID=""
BRIDGE_PCAP=""

start_bridge_capture() {
    local test_name="$1"
    BRIDGE_PCAP="${TRACE_DIR}/${test_name}_bridge.pcap"
    tcpdump -i "$BRIDGE" -w "$BRIDGE_PCAP" -nn -s0 'arp or icmp' &>/dev/null &
    TCPDUMP_PID=$!
    sleep 0.3  # let tcpdump attach
    info "  tcpdump started on $BRIDGE → $BRIDGE_PCAP (pid $TCPDUMP_PID)"
}

stop_bridge_capture() {
    if [[ -n "$TCPDUMP_PID" ]] && kill -0 "$TCPDUMP_PID" 2>/dev/null; then
        kill "$TCPDUMP_PID" 2>/dev/null
        wait "$TCPDUMP_PID" 2>/dev/null || true
        TCPDUMP_PID=""
    fi
    if [[ -f "$BRIDGE_PCAP" ]] && [[ -s "$BRIDGE_PCAP" ]]; then
        info "  === Bridge capture (tcpdump) ==="
        tcpdump -r "$BRIDGE_PCAP" -nn -e 2>/dev/null | head -50 || true
        info "  === end bridge capture ==="
    else
        info "  Bridge capture is empty (no packets crossed the bridge)"
    fi
}

# ── vaigai pktrace (DPDK-side capture) ────────────────────────────────────────
start_vaigai_trace() {
    local test_name="$1"
    local start_bytes
    start_bytes=$(stat -c%s "$VAIGAI_LOG" 2>/dev/null || echo 0)
    printf 'trace start 0 0 200\n' >&7
    sleep 0.5
    # Verify capture started
    local new_output
    new_output=$(tail -c +$((start_bytes + 1)) "$VAIGAI_LOG" 2>/dev/null || true)
    if echo "$new_output" | grep -q "Capture started"; then
        info "  vaigai pktrace started (port 0, queue 0, max 200 pkts)"
    else
        info "  vaigai pktrace start response: $(echo "$new_output" | tail -2)"
    fi
}

stop_vaigai_trace() {
    local test_name="$1"
    local pcapng="${TRACE_DIR}/${test_name}_vaigai.pcapng"
    local start_bytes
    start_bytes=$(stat -c%s "$VAIGAI_LOG" 2>/dev/null || echo 0)

    printf 'trace stop\n' >&7
    sleep 0.5
    printf 'trace save %s\n' "$pcapng" >&7
    sleep 1

    local new_output
    new_output=$(tail -c +$((start_bytes + 1)) "$VAIGAI_LOG" 2>/dev/null || true)
    info "  vaigai pktrace: $(echo "$new_output" | grep -E 'stopped|Saved|Capture|ring|save' | head -5)"

    if [[ -f "$pcapng" ]] && [[ -s "$pcapng" ]]; then
        info "  === vaigai DPDK capture ($pcapng) ==="
        tshark -r "$pcapng" -V 2>/dev/null | head -80 || \
            tcpdump -r "$pcapng" -nn -e 2>/dev/null | head -50 || \
            info "  (cannot decode pcapng — install tshark or recent tcpdump)"
        info "  === end vaigai capture ==="
    else
        info "  vaigai pcapng is empty or not created: $pcapng"
    fi
}

# ── teardown ──────────────────────────────────────────────────────────────────
FC_PID=""
teardown() {
    # Kill any lingering tcpdump
    [[ -n "$TCPDUMP_PID" ]] && kill "$TCPDUMP_PID" 2>/dev/null && wait "$TCPDUMP_PID" 2>/dev/null || true
    [[ $KEEP -eq 1 ]] && { info "Keeping topology (--keep). Traces in: $TRACE_DIR"; return; }
    info "Tearing down"
    vaigai_stop
    [[ -n "$FC_PID" ]] && kill "$FC_PID" 2>/dev/null && wait "$FC_PID" 2>/dev/null || true
    rm -f "$FC_SOCKET" "$ROOTFS_COW"
    ip link set "$BRIDGE" down 2>/dev/null || true
    ip link del "$BRIDGE"     2>/dev/null || true
    ip link del "$TAP_FC"     2>/dev/null || true
    ip link del "$TAP_VAIGAI" 2>/dev/null || true
    info "Trace files preserved in: $TRACE_DIR"
}
trap teardown EXIT

# ══════════════════════════════════════════════════════════════════════════════
#  Network topology setup (same as tcp_tap.sh)
# ══════════════════════════════════════════════════════════════════════════════
info "Creating bridge $BRIDGE + TAP for Firecracker"

# Clean prior state
ip link del "$BRIDGE"      2>/dev/null || true
ip link del "$TAP_FC"      2>/dev/null || true
ip link del "$TAP_VAIGAI"  2>/dev/null || true

# Bridge — host kernel gets .3, vaigai owns .1 via DPDK TAP.
# Using a different IP avoids the kernel intercepting ARP for .1.
# IMPORTANT: assign a unique MAC to the bridge BEFORE adding ports.
# Without this, the bridge inherits tap-vaigai's MAC, causing the kernel
# to consume unicast ARP replies that should be forwarded to DPDK.
ip link add "$BRIDGE" type bridge
ip link set "$BRIDGE" address "$BRIDGE_MAC"
ip addr add "${BRIDGE_IP}/24" dev "$BRIDGE"
ip link set "$BRIDGE" up

# Disable bridge netfilter — prevents br_netfilter from passing bridged
# ARP/IP frames through iptables/nftables/arptables chains where libvirt
# or podman (netavark) rules may drop them.
if [[ -d /sys/devices/virtual/net/$BRIDGE/bridge ]]; then
    echo 0 > /sys/devices/virtual/net/$BRIDGE/bridge/nf_call_iptables  2>/dev/null || true
    echo 0 > /sys/devices/virtual/net/$BRIDGE/bridge/nf_call_arptables 2>/dev/null || true
    echo 0 > /sys/devices/virtual/net/$BRIDGE/bridge/nf_call_ip6tables 2>/dev/null || true
    info "Disabled bridge netfilter for $BRIDGE"
fi

# TAP for Firecracker VM
ip tuntap add dev "$TAP_FC" mode tap
ip link set "$TAP_FC" master "$BRIDGE"
ip link set "$TAP_FC" up

# NOTE: tap-vaigai is NOT pre-created — DPDK net_tap creates it internally.

# ══════════════════════════════════════════════════════════════════════════════
#  Boot Firecracker VM
# ══════════════════════════════════════════════════════════════════════════════
info "Booting Firecracker VM"
rm -f "$FC_SOCKET"

ROOTFS_COW=$(mktemp /tmp/vaigai-arp-rootfs-XXXXXX.ext4)
cp --reflink=auto "$ROOTFS" "$ROOTFS_COW"

$FIRECRACKER --api-sock "$FC_SOCKET" </dev/null >/dev/null 2>&1 &
FC_PID=$!
sleep 0.3

curl -s --unix-socket "$FC_SOCKET" -X PUT http://localhost/boot-source \
    -H 'Content-Type: application/json' \
    -d "{\"kernel_image_path\": \"$VMLINUX\", \"boot_args\": \"console=ttyS0 reboot=k panic=1 pci=off vaigai_mode=tcp ip=${VM_IP}::${BRIDGE_IP}:255.255.255.0::eth0:off\"}" >/dev/null

curl -s --unix-socket "$FC_SOCKET" -X PUT http://localhost/drives/rootfs \
    -H 'Content-Type: application/json' \
    -d "{\"drive_id\": \"rootfs\", \"path_on_host\": \"$ROOTFS_COW\", \"is_root_device\": true, \"is_read_only\": false}" >/dev/null

curl -s --unix-socket "$FC_SOCKET" -X PUT http://localhost/network-interfaces/eth0 \
    -H 'Content-Type: application/json' \
    -d "{\"iface_id\": \"eth0\", \"host_dev_name\": \"$TAP_FC\", \"guest_mac\": \"$VM_MAC\"}" >/dev/null

curl -s --unix-socket "$FC_SOCKET" -X PUT http://localhost/machine-config \
    -H 'Content-Type: application/json' \
    -d '{"vcpu_count": 1, "mem_size_mib": 128}' >/dev/null

curl -s --unix-socket "$FC_SOCKET" -X PUT http://localhost/actions \
    -H 'Content-Type: application/json' \
    -d '{"action_type": "InstanceStart"}' >/dev/null

info "Waiting for VM at $VM_IP"
for i in $(seq 30); do
    ping -c 1 -W 1 "$VM_IP" &>/dev/null && break
    sleep 0.5
done
ping -c 1 -W 2 "$VM_IP" &>/dev/null || die "VM unreachable at $VM_IP"
info "VM is up"

# ══════════════════════════════════════════════════════════════════════════════
#  Start vaigai (DPDK TAP)
# ══════════════════════════════════════════════════════════════════════════════
info "Starting vaigai (DPDK TAP PMD)"
vaigai_start

# ══════════════════════════════════════════════════════════════════════════════
#  T1: ARP Resolution — vaigai sends ARP request, gets reply from VM
#
#  Uses "flood icmp <vm_ip> 2" which triggers ARP resolution first.
#  If ARP succeeds: arp_request_tx > 0, tx_pkts > 0 (ICMP packets sent)
#  If ARP fails:    tx_pkts = 0 (flood aborts at "ARP resolution failed")
# ══════════════════════════════════════════════════════════════════════════════
run_t1() {
    info "T1: ARP resolution — flood icmp ${VM_IP} (2s, triggers ARP)"

    # ── Diagnostic: show bridge, TAP, and ARP table state before test ──
    info "  --- pre-test diagnostics ---"
    info "  bridge links:"
    bridge link show 2>/dev/null | grep -E "$BRIDGE|$TAP_VAIGAI|$TAP_FC" || true
    info "  vaigai TAP MAC: $(cat /sys/class/net/$TAP_VAIGAI/address 2>/dev/null || echo 'N/A')"
    info "  bridge MAC:     $(cat /sys/class/net/$BRIDGE/address 2>/dev/null || echo 'N/A')"
    info "  tap-fc0 MAC:    $(cat /sys/class/net/$TAP_FC/address 2>/dev/null || echo 'N/A')"
    info "  host ARP table:"
    ip neigh show dev "$BRIDGE" 2>/dev/null || true
    info "  --- end pre-test diagnostics ---"

    # ── Start packet tracing ──
    start_bridge_capture "t1"
    start_vaigai_trace "t1"

    # ── Run the flood command (triggers ARP) ──
    vaigai_cmd "flood icmp $VM_IP 2 100 56"

    # ── Stop tracing and show captures ──
    stop_vaigai_trace "t1"
    stop_bridge_capture

    # ── Evaluate results ──
    local tx_pkts arp_request_tx arp_miss icmp_echo_tx
    tx_pkts=$(json_val tx_pkts)
    arp_request_tx=$(json_val arp_request_tx)
    arp_miss=$(json_val arp_miss)
    icmp_echo_tx=$(json_val icmp_echo_tx)

    info "  tx_pkts=$tx_pkts arp_request_tx=$arp_request_tx arp_miss=$arp_miss icmp_echo_tx=$icmp_echo_tx"

    # If ICMP packets were sent, ARP must have resolved first
    [[ "$tx_pkts" -gt 0 ]]      && pass "T1 tx_pkts > 0 ($tx_pkts) — ARP resolved" \
                                || fail "T1 tx_pkts = 0 — ARP resolution likely failed"
    [[ "$icmp_echo_tx" -gt 0 ]] && pass "T1 icmp_echo_tx > 0 ($icmp_echo_tx)" \
                                || fail "T1 icmp_echo_tx = 0"
}

# ══════════════════════════════════════════════════════════════════════════════
#  T2: ARP Reply — VM sends ARP who-has for vaigai's IP, vaigai responds
#
#  The host kernel sends an arping (ARP request) for VAIGAI_IP (192.168.204.1)
#  from the bridge. Since the bridge has a different IP (.3), the request
#  should reach vaigai via the bridged tap-vaigai.
#
#  Alternatively, we just verify vaigai's arp_reply_tx counter after T1,
#  because the VM itself would have ARPed for .1 to send ICMP echo replies.
#
#  We use the stats from T1's output if available, or re-check after a
#  fresh arping from host through the bridge.
# ══════════════════════════════════════════════════════════════════════════════
run_t2() {
    info "T2: ARP reply — verify vaigai responds to ARP requests for ${VAIGAI_IP}"

    # ── Start packet tracing ──
    start_bridge_capture "t2"
    start_vaigai_trace "t2"

    # Get current counter baseline
    local start_bytes
    start_bytes=$(stat -c%s "$VAIGAI_LOG" 2>/dev/null || echo 0)

    # Send an ARP request for vaigai's IP (.1) from the host bridge interface.
    # arping sends L2 ARP via the bridge; the bridge forwards to tap-vaigai;
    # vaigai receives via DPDK and should reply.
    info "  Sending arping for $VAIGAI_IP from bridge $BRIDGE"
    arping -c 3 -w 5 -I "$BRIDGE" "$VAIGAI_IP" 2>&1 | head -10 || true
    sleep 1

    # ── Stop tracing and show captures ──
    stop_vaigai_trace "t2"
    stop_bridge_capture

    # Request fresh stats
    printf 'stats\n' >&7
    local attempts=0
    local found=0
    while [[ $found -eq 0 ]]; do
        if tail -c +$((start_bytes + 1)) "$VAIGAI_LOG" 2>/dev/null | grep -q '^}'; then
            found=1
        else
            sleep 1
            attempts=$((attempts + 1))
            if [[ $attempts -gt 15 ]]; then
                info "Timed out waiting for stats"
                break
            fi
        fi
    done
    sleep 0.5
    OUTPUT=$(tail -c +$((start_bytes + 1)) "$VAIGAI_LOG")

    local arp_reply_tx rx_pkts
    arp_reply_tx=$(json_val arp_reply_tx)
    rx_pkts=$(json_val rx_pkts)

    info "  arp_reply_tx=$arp_reply_tx rx_pkts=$rx_pkts"

    [[ "$arp_reply_tx" -gt 0 ]] && pass "T2 arp_reply_tx > 0 ($arp_reply_tx) — vaigai replied to ARP" \
                                || fail "T2 arp_reply_tx = 0 — vaigai did not reply to ARP requests"
    [[ "$rx_pkts" -gt 0 ]]     && pass "T2 rx_pkts > 0 ($rx_pkts) — packets received" \
                                || fail "T2 rx_pkts = 0 — no packets received at all"
}

# ══════════════════════════════════════════════════════════════════════════════
#  T3: ARP + ICMP end-to-end — vaigai pings VM, verifies ICMP echo replies
#
#  A full round-trip: vaigai ARP-resolves the VM, sends ICMP echo requests,
#  receives ICMP echo replies.  Validates the entire ARP + L3 datapath.
#  rx_pkts should include ICMP echo replies from the VM.
# ══════════════════════════════════════════════════════════════════════════════
run_t3() {
    info "T3: ARP + ICMP round-trip — flood icmp ${VM_IP} (3s, rate=50 pps)"

    # ── Start packet tracing ──
    start_bridge_capture "t3"
    start_vaigai_trace "t3"

    # ── Run the flood ──
    vaigai_cmd "flood icmp $VM_IP 3 50 56"

    # ── Stop tracing and show captures ──
    stop_vaigai_trace "t3"
    stop_bridge_capture

    # ── Evaluate results ──
    local tx_pkts rx_pkts icmp_echo_tx ip_not_for_us
    tx_pkts=$(json_val tx_pkts)
    rx_pkts=$(json_val rx_pkts)
    icmp_echo_tx=$(json_val icmp_echo_tx)
    ip_not_for_us=$(json_val ip_not_for_us)

    info "  tx_pkts=$tx_pkts rx_pkts=$rx_pkts icmp_echo_tx=$icmp_echo_tx ip_not_for_us=$ip_not_for_us"

    [[ "$tx_pkts" -gt 0 ]]      && pass "T3 tx_pkts > 0 ($tx_pkts)" \
                                || fail "T3 tx_pkts = 0 — no packets sent"
    [[ "$icmp_echo_tx" -gt 0 ]] && pass "T3 icmp_echo_tx > 0 ($icmp_echo_tx)" \
                                || fail "T3 icmp_echo_tx = 0"
    [[ "$rx_pkts" -gt 0 ]]     && pass "T3 rx_pkts > 0 ($rx_pkts) — received replies" \
                                || fail "T3 rx_pkts = 0 — no ICMP echo replies received"
}

# ══════════════════════════════════════════════════════════════════════════════
#  Run selected tests
# ══════════════════════════════════════════════════════════════════════════════
if [[ "$RUN_TESTS" == "all" ]] || [[ "$RUN_TESTS" == "1" ]]; then run_t1; fi
if [[ "$RUN_TESTS" == "all" ]] || [[ "$RUN_TESTS" == "2" ]]; then run_t2; fi
if [[ "$RUN_TESTS" == "all" ]] || [[ "$RUN_TESTS" == "3" ]]; then run_t3; fi

echo ""
info "Results: $PASS_COUNT passed, $FAIL_COUNT failed"

[[ $FAIL_COUNT -eq 0 ]] || exit 1
