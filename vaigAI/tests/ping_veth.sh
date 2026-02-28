#!/usr/bin/env bash
# Test: ICMP ping from vaigAI (net_af_packet / veth) to an Alpine container.
#
# Self-contained — no external library needed.
# Must run as root.
#
# Usage:
#   bash tests/ping_veth.sh [OPTIONS]
#
# Options:
#   -f, --flood <seconds>   Flood ping mode: send packets as fast as possible
#                           for the given number of seconds (overrides default
#                           count/interval behaviour).
#   -h, --help              Show this help message and exit.

set -euo pipefail

# ── parse arguments ───────────────────────────────────────────────────────────
FLOOD_MODE=0
FLOOD_SECONDS=0

usage() {
    grep '^#' "$0" | grep -v '^#!/' | sed 's/^# \{0,2\}//'
    exit 0
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -f|--flood)
            [[ -n "${2:-}" && "$2" =~ ^[0-9]+$ ]] \
                || { echo "Error: --flood requires a positive integer (seconds)" >&2; exit 1; }
            FLOOD_MODE=1
            FLOOD_SECONDS="$2"
            shift 2
            ;;
        -h|--help) usage ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

# ── config ────────────────────────────────────────────────────────────────────
CONTAINER_NAME="vaigai-test-ping"
HOST_IF="veth-vaigai"
PEER_IF="veth-tpeer"
PEER_IP="192.168.200.2"
PEER_CIDR="$PEER_IP/24"
SRC_IP="192.168.200.1"
PING_COUNT=5
PING_SIZE=56
PING_INTERVAL_MS=1000
DPDK_LCORES="0-1"
VAIGAI_BIN="$(cd "$(dirname "$0")/.." && pwd)/build/vaigai"

# In flood mode the CLI 'flood' command handles timing internally;
# no external 'timeout' wrapper needed.
if [[ $FLOOD_MODE -eq 1 ]]; then
    PING_INTERVAL_MS=0
    PING_COUNT=0  # unused in flood mode
fi

# ── colours ───────────────────────────────────────────────────────────────────
GRN='\033[0;32m'; RED='\033[0;31m'; CYN='\033[0;36m'; NC='\033[0m'
info()  { echo -e "${CYN}[INFO]${NC}  $*"; }
pass()  { echo -e "${GRN}[PASS]${NC}  ping_veth: $*"; }
fail()  { echo -e "${RED}[FAIL]${NC}  ping_veth: $*" >&2; exit 1; }

# ── pre-flight ────────────────────────────────────────────────────────────────
[[ $EUID -eq 0 ]]      || fail "Must run as root"
[[ -x "$VAIGAI_BIN" ]] || fail "vaigai binary not found at $VAIGAI_BIN — run ninja-build first"
for cmd in podman ip nsenter; do
    command -v "$cmd" &>/dev/null || fail "Required command not found: $cmd"
done

# ── teardown (always runs on exit) ────────────────────────────────────────────
CFG=$(mktemp /tmp/vaigai_ping_XXXXXX.json)
teardown() {
    info "Tearing down"
    rm -f "$CFG"
    ip link del "$HOST_IF" &>/dev/null || true
    podman stop "$CONTAINER_NAME" &>/dev/null || true
    podman rm   "$CONTAINER_NAME" &>/dev/null || true
}
trap teardown EXIT

# ── setup ─────────────────────────────────────────────────────────────────────
info "Starting container $CONTAINER_NAME"
podman rm -f "$CONTAINER_NAME" &>/dev/null || true
podman run -d --name "$CONTAINER_NAME" --network none \
    alpine:latest sh -c 'while true; do sleep 60; done'

# Wait for container PID
CPID=""
for _ in $(seq 20); do
    CPID=$(podman inspect --format '{{.State.Pid}}' "$CONTAINER_NAME" 2>/dev/null || true)
    [[ -n "$CPID" && "$CPID" != "0" ]] && break
    sleep 0.2
done
[[ -n "$CPID" && "$CPID" != "0" ]] || fail "Container did not start"
info "Container PID=$CPID"

info "Setting up veth pair $HOST_IF <-> $PEER_IF"
ip link del "$HOST_IF" &>/dev/null || true
ip link add "$HOST_IF" type veth peer name "$PEER_IF"
ip link set "$HOST_IF" promisc on
ip link set "$HOST_IF" up
ip link set "$PEER_IF" netns "$CPID"
nsenter -t "$CPID" -n ip link set "$PEER_IF" up
nsenter -t "$CPID" -n ip addr add "$PEER_CIDR" dev "$PEER_IF"
info "Container $PEER_IF configured: $PEER_CIDR"

# ── write ephemeral config ────────────────────────────────────────────────────
cat > "$CFG" <<EOF
{
  "flows": [{
    "src_ip_lo": "$SRC_IP",
    "src_ip_hi": "$SRC_IP",
    "dst_ip":    "$PEER_IP",
    "dst_port":  0,
    "protocol":  "icmp",
    "icmp_ping": true
  }],
  "load": { "max_concurrent": 1, "target_cps": 0, "duration_s": 0 },
  "tls": null
}
EOF

# ── run ───────────────────────────────────────────────────────────────────────
if [[ $FLOOD_MODE -eq 1 ]]; then
    info "Flood ICMP -> $PEER_IP for ${FLOOD_SECONDS}s (workers generate at line rate)"
    OUTPUT=$(printf 'flood icmp %s %d 0 %d\nquit\n' \
                 "$PEER_IP" "$FLOOD_SECONDS" "$PING_SIZE" \
             | VAIGAI_CONFIG="$CFG" "$VAIGAI_BIN" \
                   -l "$DPDK_LCORES" -n 1 --no-pci \
                   --vdev "net_af_packet0,iface=$HOST_IF" -- 2>&1) || true
else
    info "Pinging $PEER_IP ($PING_COUNT packets, interval=${PING_INTERVAL_MS}ms)"
    OUTPUT=$(printf 'ping %s %d %d %d\nquit\n' \
                 "$PEER_IP" "$PING_COUNT" "$PING_SIZE" "$PING_INTERVAL_MS" \
             | VAIGAI_CONFIG="$CFG" "$VAIGAI_BIN" \
                   -l "$DPDK_LCORES" -n 1 --no-pci \
                   --vdev "net_af_packet0,iface=$HOST_IF" -- 2>&1) || true
fi

# ── assert ────────────────────────────────────────────────────────────────────
if [[ $FLOOD_MODE -eq 1 ]]; then
    # The CLI prints "<N> packets transmitted".  Verify N > 0.
    TX_COUNT=$(echo "$OUTPUT" | grep -oP '^\d+(?= packets transmitted)' || echo "0")
    if [[ "$TX_COUNT" -gt 0 ]]; then
        pass "Flood ICMP to $PEER_IP for ${FLOOD_SECONDS}s: ${TX_COUNT} packets transmitted"
    else
        echo "$OUTPUT" >&2
        fail "Expected '> 0 packets transmitted' in flood output"
    fi
    # Show telemetry section
    echo "$OUTPUT" | sed -n '/--- flood statistics ---/,$ p'
else
    EXPECTED="${PING_COUNT} packets transmitted, ${PING_COUNT} received, 0% packet loss"
    if echo "$OUTPUT" | grep -qF "$EXPECTED"; then
        pass "5/5 ICMP echo replies received from $PEER_IP (0% loss)"
    else
        echo "$OUTPUT" >&2
        fail "Expected \"$EXPECTED\" not found in output"
    fi
fi
