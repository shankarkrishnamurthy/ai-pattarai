#!/usr/bin/env bash
# Test: UDP from vaigAI (net_af_packet or net_af_xdp / veth) to an
#       Alpine container.
#
# Modes:
#   Rate-limited (default):  1000 pps for 1 second.
#   Flood (--flood-seconds): line-rate for N seconds.
#
# Cross-validates vaigai telemetry (udp_tx) against the container's
# kernel counter (/proc/net/snmp NoPorts).
#
# Self-contained — no external library needed.
# Must run as root.
#
# Usage:
#   bash tests/udp_veth.sh [OPTIONS]
#
# Options:
#   -f, --flood-seconds <N>  Run unlimited flood for N seconds instead of
#                            the default rate-limited test.
#   -x, --xdp                Use AF_XDP (net_af_xdp) instead of the default
#                            net_af_packet.  Requires kernel >= 5.4, libbpf,
#                            and CAP_NET_ADMIN + CAP_BPF (or root).
#   -h, --help               Show this help message and exit.

set -euo pipefail

# ── parse arguments ───────────────────────────────────────────────────────────
FLOOD_MODE=0
FLOOD_SECONDS=0
XDP_MODE=0

usage() {
    grep '^#' "$0" | grep -v '^#!/' | sed 's/^# \{0,2\}//'
    exit 0
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -f|--flood-seconds)
            [[ -n "${2:-}" && "$2" =~ ^[0-9]+$ ]] \
                || { echo "Error: --flood-seconds requires a positive integer" >&2; exit 1; }
            FLOOD_MODE=1
            FLOOD_SECONDS="$2"
            shift 2
            ;;
        -x|--xdp)
            XDP_MODE=1
            shift
            ;;
        -h|--help) usage ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

# ── config ────────────────────────────────────────────────────────────────────
CONTAINER_NAME="vaigai-test-udp"
HOST_IF="veth-udptest"
PEER_IF="veth-udppeer"
PEER_IP="192.168.201.2"
PEER_CIDR="$PEER_IP/24"
SRC_IP="192.168.201.1"
DST_PORT=9
PKT_SIZE=64
DPDK_LCORES="0-1"
VAIGAI_BIN="$(cd "$(dirname "$0")/.." && pwd)/build/vaigai"
PASS_COUNT=0
FAIL_COUNT=0

# Select DPDK vdev based on mode
if [[ $XDP_MODE -eq 1 ]]; then
    HOST_IF="veth-udpxdp"
    PEER_IF="veth-udpxdppeer"
    VDEV_ARG="net_af_xdp0,iface=$HOST_IF"
    MODE_LABEL="AF_XDP"
else
    VDEV_ARG="net_af_packet0,iface=$HOST_IF"
    MODE_LABEL="AF_PACKET"
fi

# ── colours ───────────────────────────────────────────────────────────────────
GRN='\033[0;32m'; RED='\033[0;31m'; CYN='\033[0;36m'; YLW='\033[0;33m'; NC='\033[0m'
info()  { echo -e "${CYN}[INFO]${NC}  $*"; }
pass()  { echo -e "${GRN}[PASS]${NC}  udp_veth: $*"; ((PASS_COUNT++)) || true; }
fail()  { echo -e "${RED}[FAIL]${NC}  udp_veth: $*" >&2; ((FAIL_COUNT++)) || true; }
die()   { echo -e "${RED}[FATAL]${NC} udp_veth: $*" >&2; exit 1; }

# ── pre-flight ────────────────────────────────────────────────────────────────
[[ $EUID -eq 0 ]]      || die "Must run as root"
[[ -x "$VAIGAI_BIN" ]] || die "vaigai binary not found at $VAIGAI_BIN — run ninja-build first"
for cmd in podman ip nsenter; do
    command -v "$cmd" &>/dev/null || die "Required command not found: $cmd"
done

if [[ $XDP_MODE -eq 1 ]]; then
    info "AF_XDP mode selected — running extra pre-flight checks"

    KVER=$(uname -r)
    KMAJOR=${KVER%%.*}
    KREST=${KVER#*.}
    KMINOR=${KREST%%.*}
    if [[ $KMAJOR -lt 5 || ( $KMAJOR -eq 5 && $KMINOR -lt 4 ) ]]; then
        die "AF_XDP requires kernel >= 5.4 (running $KVER)"
    fi

    if ! pkg-config --exists libbpf 2>/dev/null \
       && ! compgen -G '/usr/lib*/libbpf.so*' >/dev/null 2>&1; then
        die "libbpf not found — install libbpf-dev / libbpf-devel"
    fi

    info "Kernel $KVER OK, libbpf found"
fi

# ── teardown (always runs on exit) ────────────────────────────────────────────
CFG=$(mktemp /tmp/vaigai_udp_XXXXXX.json)
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
[[ -n "$CPID" && "$CPID" != "0" ]] || die "Container did not start"
info "Container PID=$CPID"

info "Setting up veth pair $HOST_IF <-> $PEER_IF ($MODE_LABEL)"
ip link del "$HOST_IF" &>/dev/null || true
ip link add "$HOST_IF" type veth peer name "$PEER_IF"
ip link set "$HOST_IF" promisc on
ip link set "$HOST_IF" up

if [[ $XDP_MODE -eq 1 ]]; then
    ethtool -L "$HOST_IF" combined 1 2>/dev/null || true
    ip link set "$HOST_IF" mtu 3498 2>/dev/null || true
    info "AF_XDP: $HOST_IF queue/mtu configured"
fi

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
    "dst_port":  $DST_PORT,
    "protocol":  "udp",
    "icmp_ping": false
  }],
  "load": { "max_concurrent": 1, "target_cps": 0, "duration_s": 0 },
  "tls": null
}
EOF

# ── helper: read container UDP NoPorts counter ────────────────────────────────
container_noports() {
    nsenter -t "$CPID" -n cat /proc/net/snmp \
        | awk '/^Udp:/ && !/InDatagrams/ {print $3}'
}

# ── helper: read container veth RX packets ────────────────────────────────────
container_rx_pkts() {
    nsenter -t "$CPID" -n cat /proc/net/dev \
        | awk -v iface="$PEER_IF:" '$1 == iface {print $2}'
}

# ══════════════════════════════════════════════════════════════════════════════
#  Run test
# ══════════════════════════════════════════════════════════════════════════════
BEFORE_NP=$(container_noports)
info "Container NoPorts before: $BEFORE_NP"
info "Using DPDK vdev: $VDEV_ARG"

if [[ $FLOOD_MODE -eq 1 ]]; then
    info "Flood UDP -> $PEER_IP:$DST_PORT for ${FLOOD_SECONDS}s (line rate)"
    OUTPUT=$(printf 'flood udp %s %d 0 %d %d\nquit\n' \
                 "$PEER_IP" "$FLOOD_SECONDS" "$PKT_SIZE" "$DST_PORT" \
             | VAIGAI_CONFIG="$CFG" "$VAIGAI_BIN" \
                   -l "$DPDK_LCORES" -n 1 --no-pci \
                   --vdev "$VDEV_ARG" -- 2>&1) || true
else
    info "Rate-limited UDP -> $PEER_IP:$DST_PORT (1000 pps × 1s)"
    OUTPUT=$(printf 'flood udp %s 1 1000 %d %d\nquit\n' \
                 "$PEER_IP" "$PKT_SIZE" "$DST_PORT" \
             | VAIGAI_CONFIG="$CFG" "$VAIGAI_BIN" \
                   -l "$DPDK_LCORES" -n 1 --no-pci \
                   --vdev "$VDEV_ARG" -- 2>&1) || true
fi

# Extract vaigai counters
VAIGAI_TX=$(echo "$OUTPUT" | grep -oP '"udp_tx": \K[0-9]+' || echo "0")
VAIGAI_PKTS=$(echo "$OUTPUT" | grep -oP '^\d+(?= packets transmitted)' || echo "0")

sleep 0.2  # kernel may lag slightly
AFTER_NP=$(container_noports)
DELTA_NP=$((AFTER_NP - BEFORE_NP))

info "vaigai: udp_tx=$VAIGAI_TX, packets_transmitted=$VAIGAI_PKTS"
info "container: NoPorts delta=$DELTA_NP ($BEFORE_NP -> $AFTER_NP)"

# ── assert ───────────────────────────────────────────────────────────────────
if [[ $FLOOD_MODE -eq 1 ]]; then
    if [[ "$VAIGAI_TX" -gt 0 ]]; then
        pass "flood: vaigai sent $VAIGAI_TX packets in ${FLOOD_SECONDS}s"
    else
        fail "flood: vaigai sent 0 packets"
    fi
else
    if [[ "$VAIGAI_TX" -ge 900 && "$VAIGAI_TX" -le 1100 ]]; then
        pass "rate-limited: vaigai sent $VAIGAI_TX packets (~1000 expected)"
    else
        fail "rate-limited: expected ~1000, got $VAIGAI_TX"
    fi
fi

# Cross-validate against container kernel counter
if [[ "$DELTA_NP" -eq "$VAIGAI_TX" ]]; then
    pass "container NoPorts delta ($DELTA_NP) == vaigai udp_tx ($VAIGAI_TX)"
else
    fail "container NoPorts delta ($DELTA_NP) != vaigai udp_tx ($VAIGAI_TX)"
fi

echo "$OUTPUT" | sed -n '/--- .* statistics ---/,/^}$/p'

# ══════════════════════════════════════════════════════════════════════════════
#  Summary
# ══════════════════════════════════════════════════════════════════════════════
echo ""
info "Results: $PASS_COUNT passed, $FAIL_COUNT failed"
[[ $FAIL_COUNT -eq 0 ]] || exit 1
