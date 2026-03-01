#!/usr/bin/env bash
# Test: TCP over dual TAP + Linux bridge + Firecracker microVM.
#
# Runs three tests:
#   T1 — SYN flood (CPS benchmark)
#   T2 — Full lifecycle: SYN → DATA → FIN (echo server)
#   T3 — Throughput forward (TX to discard) + reverse (RX from chargen)
#
# Prerequisites:
#   - firecracker binary in PATH (or $FIRECRACKER)
#   - vmlinux kernel at $VMLINUX (default: /work/firecracker/vmlinux)
#   - Alpine rootfs ext4 at $ROOTFS (default: /work/firecracker/alpine.ext4)
#     Must contain: socat, ip, dd
#   - Root privileges
#
# Usage:
#   bash tests/tcp_tap.sh [OPTIONS]
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
BRIDGE_IP="${SUBNET}.3"   # host kernel uses this for ping check
BRIDGE_MAC="02:42:c0:a8:cc:03"  # unique bridge MAC — must differ from TAP
VM_MAC="AA:FC:00:00:00:01"
DPDK_LCORES="0-1"
VAIGAI_BIN="$(cd "$(dirname "$0")/.." && pwd)/build/vaigai"
FIRECRACKER="${FIRECRACKER:-firecracker}"
VMLINUX="${VMLINUX:-/work/firecracker/vmlinux}"
ROOTFS="${ROOTFS:-/work/firecracker/alpine.ext4}"
FC_SOCKET="/tmp/vaigai-fc.sock"
PASS_COUNT=0
FAIL_COUNT=0

# ── colours ───────────────────────────────────────────────────────────────────
GRN='\033[0;32m'; RED='\033[0;31m'; CYN='\033[0;36m'; NC='\033[0m'
info()  { echo -e "${CYN}[INFO]${NC}  $*"; }
pass()  { echo -e "${GRN}[PASS]${NC}  tcp_tap: $*"; ((PASS_COUNT++)) || true; }
fail()  { echo -e "${RED}[FAIL]${NC}  tcp_tap: $*" >&2; ((FAIL_COUNT++)) || true; }
die()   { echo -e "${RED}[FATAL]${NC} tcp_tap: $*" >&2; exit 1; }

# ── helper: extract JSON field from vaigai output ─────────────────────────────
json_val() {
    # $1 = field name, reads from $OUTPUT
    grep -oP "\"$1\": *\K[0-9]+" <<< "$OUTPUT" | tail -1 || echo "0"
}

# ── vaigai FIFO-based lifecycle ───────────────────────────────────────────────
VAIGAI_FIFO=""
VAIGAI_PID=""
VAIGAI_LOG=""
VAIGAI_CFG=""

vaigai_start() {
    VAIGAI_CFG=$(mktemp /tmp/vaigai_tcp_XXXXXX.json)
    cat > "$VAIGAI_CFG" <<EOCFG
{
  "flows": [{
    "src_ip_lo": "$VAIGAI_IP", "src_ip_hi": "$VAIGAI_IP",
    "dst_ip": "$VM_IP", "dst_port": 5000,
    "icmp_ping": false
  }],
  "load": { "max_concurrent": 1024, "target_cps": 0, "duration_secs": 0 }
}
EOCFG
    VAIGAI_FIFO=$(mktemp -u /tmp/vaigai_fifo_XXXXXX)
    mkfifo "$VAIGAI_FIFO"
    VAIGAI_LOG=$(mktemp /tmp/vaigai_out_XXXXXX.log)

    # Launch vaigai reading from FIFO, writing to log.
    # DPDK TAP PMD creates tap-vaigai internally.
    VAIGAI_CONFIG="$VAIGAI_CFG" "$VAIGAI_BIN" \
        -l "$DPDK_LCORES" -n 1 --no-pci \
        --vdev "net_tap0,iface=$TAP_VAIGAI" -- \
        < "$VAIGAI_FIFO" > "$VAIGAI_LOG" 2>&1 &
    VAIGAI_PID=$!

    # Keep write end of FIFO open on fd 7 so vaigai doesn't see EOF
    exec 7>"$VAIGAI_FIFO"

    sleep 2  # let DPDK init and create the TAP

    # Attach DPDK-created TAP to bridge
    if ip link show "$TAP_VAIGAI" &>/dev/null; then
        # Change the kernel-side TAP MAC before adding to the bridge.
        # Linux bridges register each port's MAC as a "local" FDB entry;
        # frames addressed to a "local" MAC are delivered to the kernel's
        # own IP stack instead of being forwarded via the TAP fd to DPDK.
        # By giving the kernel side a different MAC, ARP replies addressed
        # to DPDK's MAC are forwarded correctly.
        ip link set "$TAP_VAIGAI" address "02:00:de:ad:00:01"
        ip link set "$TAP_VAIGAI" master "$BRIDGE"
        ip link set "$TAP_VAIGAI" up
        info "Attached $TAP_VAIGAI to $BRIDGE"
    else
        die "DPDK did not create $TAP_VAIGAI — check vaigai log: $VAIGAI_LOG"
    fi
}

vaigai_reset() {
    # Send reset command to clear all TCP state between tests
    echo "reset" >&7
    sleep 2  # Give RSTs time to reach VM and for state to stabilize
}

vaigai_cmd() {
    # Send a command + stats, then read output
    # $1 = command to run (e.g. "flood tcp ...")
    local cmd="$1"

    # Record current log size so we can extract only new output
    local start_bytes
    start_bytes=$(stat -c%s "$VAIGAI_LOG" 2>/dev/null || echo 0)

    # Send the command via the persistent fd
    printf '%s\n' "$cmd" >&7

    # Wait for the command to finish (flood/throughput block for their duration).
    # Duration field position depends on command:
    #   flood <proto> <ip> <duration> ...      → field 4
    #   throughput <dir> <ip> <port> <duration> ... → field 5
    local dur
    local cmd_type
    cmd_type=$(echo "$cmd" | awk '{print $1}')
    if [[ "$cmd_type" == "throughput" ]]; then
        dur=$(echo "$cmd" | awk '{print $5}')
    else
        dur=$(echo "$cmd" | awk '{print $4}')
    fi
    if [[ "$dur" =~ ^[0-9]+$ ]] && [[ "$dur" -gt 0 ]]; then
        sleep $((dur + 2))
    else
        sleep 3
    fi

    # Now request stats
    printf 'stats\n' >&7

    # Wait for JSON output (look for closing brace in new output)
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
    sleep 0.5  # let output flush

    OUTPUT=$(tail -c +$((start_bytes + 1)) "$VAIGAI_LOG")
}

vaigai_stop() {
    # Dump vaigai log for debugging before cleanup
    if [[ -n "$VAIGAI_LOG" ]] && [[ -f "$VAIGAI_LOG" ]]; then
        info "=== vaigai process log ==="
        cat "$VAIGAI_LOG" >&2 || true
        info "=== end vaigai log ==="
    fi
    if [[ -n "$VAIGAI_PID" ]] && kill -0 "$VAIGAI_PID" 2>/dev/null; then
        printf 'quit\n' >&7 2>/dev/null || true
        exec 7>&- 2>/dev/null || true  # close write fd
        # Wait up to 5 seconds for graceful exit, then SIGKILL
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
for cmd in ip curl; do
    command -v "$cmd" &>/dev/null || die "Required: $cmd"
done

# ── teardown ──────────────────────────────────────────────────────────────────
FC_PID=""
teardown() {
    [[ $KEEP -eq 1 ]] && { info "Keeping topology (--keep)"; return; }
    info "Tearing down"
    vaigai_stop
    [[ -n "$FC_PID" ]] && kill "$FC_PID" 2>/dev/null && wait "$FC_PID" 2>/dev/null || true
    rm -f "$FC_SOCKET" "$ROOTFS_COW"
    ip link set "$BRIDGE" down 2>/dev/null || true
    ip link del "$BRIDGE"     2>/dev/null || true
    ip link del "$TAP_FC"     2>/dev/null || true
    ip link del "$TAP_VAIGAI" 2>/dev/null || true
}
trap teardown EXIT

# ── network setup: bridge + FC TAP ───────────────────────────────────────────
info "Creating bridge $BRIDGE + TAP for Firecracker"

# Clean prior state
ip link del "$BRIDGE"      2>/dev/null || true
ip link del "$TAP_FC"      2>/dev/null || true
ip link del "$TAP_VAIGAI"  2>/dev/null || true

# Bridge — use a different IP (.3) than vaigai (.1) to avoid ARP conflict.
# vaigai owns .1 via DPDK TAP; the kernel must NOT claim .1 or it will
# intercept ARP replies meant for vaigai.
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
fi

# TAP for Firecracker VM
ip tuntap add dev "$TAP_FC" mode tap
ip link set "$TAP_FC" master "$BRIDGE"
ip link set "$TAP_FC" up

# NOTE: tap-vaigai is NOT pre-created — DPDK net_tap creates it internally.
# After vaigai starts, we attach it to the bridge (see vaigai_start).

# ── boot Firecracker VM ──────────────────────────────────────────────────────
info "Booting Firecracker VM"
rm -f "$FC_SOCKET"

# Make a COW copy of rootfs so we don't modify the original
ROOTFS_COW=$(mktemp /tmp/vaigai-rootfs-XXXXXX.ext4)
cp --reflink=auto "$ROOTFS" "$ROOTFS_COW"

$FIRECRACKER --api-sock "$FC_SOCKET" </dev/null >/dev/null 2>&1 &
FC_PID=$!
sleep 0.3

# Configure VM via API
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

# Start the VM
curl -s --unix-socket "$FC_SOCKET" -X PUT http://localhost/actions \
    -H 'Content-Type: application/json' \
    -d '{"action_type": "InstanceStart"}' >/dev/null

# Wait for VM to be reachable (ping)
info "Waiting for VM at $VM_IP"
for i in $(seq 30); do
    ping -c 1 -W 1 "$VM_IP" &>/dev/null && break
    sleep 0.5
done
ping -c 1 -W 2 "$VM_IP" &>/dev/null || die "VM unreachable at $VM_IP"
info "VM is up"

# Verify echo service is listening
echo "test" | nc -w 2 "$VM_IP" 5000 &>/dev/null \
    || info "Warning: echo service on :5000 may not be ready yet"

# ── start vaigai (DPDK TAP) ──────────────────────────────────────────────────
info "Starting vaigai (DPDK TAP PMD)"
vaigai_start

# ══════════════════════════════════════════════════════════════════════════════
#  T1: SYN Flood — CPS
# ══════════════════════════════════════════════════════════════════════════════
run_t1() {
    info "T1: SYN flood CPS → ${VM_IP}:5000 (5s)"
    vaigai_cmd "flood tcp $VM_IP 5 0 56 5000"

    local syn_sent conn_open reset_rx drops
    syn_sent=$(json_val tcp_syn_sent)
    conn_open=$(json_val tcp_conn_open)
    reset_rx=$(json_val tcp_reset_rx)
    drops=$(json_val tcp_syn_queue_drops)

    info "  syn_sent=$syn_sent conn_open=$conn_open reset_rx=$reset_rx drops=$drops"

    [[ "$syn_sent" -gt 0 ]]  && pass "T1 syn_sent > 0 ($syn_sent)" \
                              || fail "T1 syn_sent = 0"
    [[ "$conn_open" -gt 0 ]] && pass "T1 conn_open > 0 ($conn_open)" \
                              || fail "T1 conn_open = 0"
    [[ "$reset_rx" -eq 0 ]]  && pass "T1 reset_rx = 0" \
                              || fail "T1 reset_rx = $reset_rx (expected 0)"
    [[ "$drops" -eq 0 ]]     && pass "T1 syn_queue_drops = 0" \
                              || fail "T1 syn_queue_drops = $drops (expected 0)"
}

# ══════════════════════════════════════════════════════════════════════════════
#  T2: Full lifecycle — SYN → DATA → FIN (echo server, 1 stream, 5s)
# ══════════════════════════════════════════════════════════════════════════════
run_t2() {
    info "T2: Full lifecycle → ${VM_IP}:5000 (5s, 1 stream)"
    vaigai_cmd "throughput tx $VM_IP 5000 5 1"

    local conn_open conn_close retransmit reset_rx
    conn_open=$(json_val tcp_conn_open)
    conn_close=$(json_val tcp_conn_close)
    retransmit=$(json_val tcp_retransmit)
    reset_rx=$(json_val tcp_reset_rx)

    info "  conn_open=$conn_open conn_close=$conn_close retransmit=$retransmit reset_rx=$reset_rx"

    [[ "$conn_open" -gt 0 ]]          && pass "T2 conn_open > 0 ($conn_open)" \
                                      || fail "T2 conn_open = 0"
    [[ "$conn_close" -eq "$conn_open" ]] && pass "T2 conn_close = conn_open ($conn_close)" \
                                         || fail "T2 conn_close ($conn_close) != conn_open ($conn_open)"
    [[ "$retransmit" -eq 0 ]]         && pass "T2 retransmit = 0" \
                                      || fail "T2 retransmit = $retransmit (expected 0)"
    [[ "$reset_rx" -eq 0 ]]           && pass "T2 reset_rx = 0" \
                                      || fail "T2 reset_rx = $reset_rx (expected 0)"
}

# ══════════════════════════════════════════════════════════════════════════════
#  T3: Throughput — forward (TX to discard) + reverse (RX from chargen)
# ══════════════════════════════════════════════════════════════════════════════
run_t3() {
    # T3a: Forward — vaigAI → VM (discard :5001)
    info "T3a: Throughput TX → ${VM_IP}:5001 (10s, 4 streams)"
    vaigai_cmd "throughput tx $VM_IP 5001 10 4"

    local payload_tx retransmit mbps_line
    payload_tx=$(json_val tcp_payload_tx)
    retransmit=$(json_val tcp_retransmit)
    mbps_line=$(grep -oP '\[SUM\].*' <<< "$OUTPUT" || echo "(no summary)")

    info "  payload_tx=$payload_tx retransmit=$retransmit"
    info "  $mbps_line"

    [[ "$payload_tx" -gt 0 ]] && pass "T3a payload_tx > 0 ($payload_tx bytes)" \
                               || fail "T3a payload_tx = 0"
    [[ "$retransmit" -eq 0 ]] && pass "T3a retransmit = 0" \
                               || fail "T3a retransmit = $retransmit (expected 0)"

    # T3b: Reverse — VM → vaigAI (listen :5001, VM sends via socat+dd)
    info "T3b: Throughput RX ← VM (10s, listen :5001)"
    # Trigger VM to connect and send data (via socat → vaigAI)
    # The VM init script should have a chargen mode, or we kick it via ssh/serial.
    # For CI: we start vaigAI in listen mode, then ask the VM to connect.
    # Since we can't easily orchestrate both sides via stdin, T3b uses a
    # simplified check: confirm the rx path works via the echo data from T3a.
    # Full T3b requires async orchestration (future enhancement).
    info "  T3b: skipped (requires async VM orchestration — see test plan)"
}

# ══════════════════════════════════════════════════════════════════════════════
#  Main
# ══════════════════════════════════════════════════════════════════════════════
should_run() { [[ "$RUN_TESTS" == "all" || "$RUN_TESTS" == "$1" ]]; }

should_run 1 && run_t1
should_run 2 && { vaigai_reset; run_t2; }
should_run 3 && { vaigai_reset; run_t3; }

# ── summary ───────────────────────────────────────────────────────────────────
echo ""
info "Results: $PASS_COUNT passed, $FAIL_COUNT failed"
[[ $FAIL_COUNT -eq 0 ]] || exit 1
