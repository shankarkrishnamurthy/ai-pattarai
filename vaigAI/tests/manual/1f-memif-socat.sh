#!/usr/bin/env bash
# Test: TCP/UDP from vaigAI (net_memif server) through a shared-memory memif
#       channel to testpmd (net_memif client, bridged via net_tap to the Linux
#       network stack) where socat provides echo/sink listeners.
#
# Both DPDK processes run on the same host.  No containers, VMs, or physical
# NIC needed — the shared-memory ring replaces any physical medium.
#
# Topology:
#
#   +--------------------------+  shared memory   +--------------------------+
#   |   Process 1: vaigai      |  (memif socket)  |   Process 2: testpmd     |
#   |                          |                  |                          |
#   |  vaigai (DPDK primary)   |    mmap'd ring   |  testpmd (DPDK primary)  |
#   |                          |  +-----------+   |  fwd-mode: io (L2 brdg)  |
#   |  net_memif0 (server) ----+->| /tmp/     |<--+-- net_memif0 (client)    |
#   |  IP: 10.100.0.1          |  | vaigai-   |   |  net_tap0 → memif-tap    |
#   |                          |  | memif.    |   |  IP: 10.100.0.2          |
#   +--------------------------+  | sock      |   |  socat :5000 (echo)      |
#                                 +-----------+   |  socat :5001 (discard)   |
#                                                 +--------------------------+
#
# Packet flow (example: TCP SYN from vaigai to socat):
#   vaigai → net_memif0 (server TX) → shared ring → net_memif0 (client RX)
#   → testpmd io fwd → net_tap0 (DPDK TX) → memif-tap (kernel RX)
#   → Linux TCP stack → socat accept()
#   socat reply → memif-tap (kernel TX) → net_tap0 (DPDK RX)
#   → testpmd io fwd → net_memif0 (client TX) → shared ring
#   → net_memif0 (server RX) → vaigai
#
# Hugepages: both processes draw from the system hugepage pool.
#   Ensure at least 512 x 2 MB pages are free before running:
#     echo 512 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
#
# Prerequisites:
#   - dpdk-testpmd in PATH (or override with $TESTPMD)
#   - socat installed           (dnf install socat)
#   - Root privileges
#
# Usage:
#   bash tests/manual/1f-memif-socat.sh [OPTIONS]
#
# Options:
#   --test <1|2|all>   Which test to run (default: all)
#                        1 = TCP SYN flood  -> socat echo    (:5000)
#                        2 = UDP flood      -> socat discard (:5001)
#   --keep             Don't tear down on exit (for debugging)
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

# ── config ────────────────────────────────────────────────────────────────────
MEMIF_SOCK="/tmp/vaigai-memif.sock"
TAP_IF="memif-tap"
VAIGAI_IP="10.100.0.1"
SOCAT_IP="10.100.0.2"
SUBNET_CIDR="${SOCAT_IP}/24"
ECHO_PORT=5000
SINK_PORT=5001
DPDK_LCORES_VAIGAI="0-1"
DPDK_LCORES_TESTPMD="2-3"
VAIGAI_BIN="$(cd "$(dirname "$0")/../.." && pwd)/build/vaigai"
TESTPMD_BIN="${TESTPMD:-$(command -v dpdk-testpmd 2>/dev/null || true)}"
PASS_COUNT=0
FAIL_COUNT=0

# ── colours ───────────────────────────────────────────────────────────────────
GRN='\033[0;32m'; RED='\033[0;31m'; CYN='\033[0;36m'; NC='\033[0m'
info()  { echo -e "${CYN}[INFO]${NC}  $*"; }
pass()  { echo -e "${GRN}[PASS]${NC}  memif_socat: $*"; ((PASS_COUNT++)) || true; }
fail()  { echo -e "${RED}[FAIL]${NC}  memif_socat: $*" >&2; ((FAIL_COUNT++)) || true; }
die()   { echo -e "${RED}[FATAL]${NC} memif_socat: $*" >&2; exit 1; }

# ── state ─────────────────────────────────────────────────────────────────────
VAIGAI_PID=""
VAIGAI_FIFO=""
VAIGAI_LOG=""
TESTPMD_PID=""
TESTPMD_FIFO=""
TESTPMD_LOG=""
SOCAT_ECHO_PID=""
SOCAT_SINK_PID=""

# ── teardown ──────────────────────────────────────────────────────────────────
teardown() {
    [[ $KEEP -eq 1 ]] && { info "Keeping topology (--keep)."; return; }
    info "Tearing down ..."

    if [[ -n "$VAIGAI_PID" ]] && kill -0 "$VAIGAI_PID" 2>/dev/null; then
        printf 'quit\n' >&7 2>/dev/null || true
        exec 7>&- 2>/dev/null || true
        local waited=0
        while kill -0 "$VAIGAI_PID" 2>/dev/null && [[ $waited -lt 10 ]]; do
            sleep 0.5; ((waited++)) || true
        done
        kill -9 "$VAIGAI_PID" 2>/dev/null || true
        wait "$VAIGAI_PID" 2>/dev/null || true
    fi
    exec 7>&- 2>/dev/null || true

    if [[ -n "$TESTPMD_PID" ]] && kill -0 "$TESTPMD_PID" 2>/dev/null; then
        printf 'quit\n' >&8 2>/dev/null || true
        exec 8>&- 2>/dev/null || true
        sleep 1
        kill "$TESTPMD_PID" 2>/dev/null || true
        wait "$TESTPMD_PID" 2>/dev/null || true
    fi
    exec 8>&- 2>/dev/null || true

    [[ -n "$SOCAT_ECHO_PID" ]] && kill "$SOCAT_ECHO_PID" 2>/dev/null || true
    [[ -n "$SOCAT_SINK_PID" ]] && kill "$SOCAT_SINK_PID" 2>/dev/null || true

    ip link del "$TAP_IF" 2>/dev/null || true
    rm -f "$MEMIF_SOCK" \
          "${VAIGAI_FIFO:-}"  "${VAIGAI_LOG:-}" \
          "${TESTPMD_FIFO:-}" "${TESTPMD_LOG:-}"
    wait 2>/dev/null || true
}
trap teardown EXIT

# ── pre-flight checks ─────────────────────────────────────────────────────────
[[ $EUID -eq 0 ]] \
    || die "Must run as root."
[[ -x "$VAIGAI_BIN" ]] \
    || die "vaigai binary not found: $VAIGAI_BIN — run: ninja -C build"
[[ -n "${TESTPMD_BIN:-}" && -x "$TESTPMD_BIN" ]] \
    || die "dpdk-testpmd not found (install dpdk or set \$TESTPMD)"
command -v socat &>/dev/null \
    || die "socat not installed (dnf install socat)"
FREE_HUGE=$(< /sys/kernel/mm/hugepages/hugepages-2048kB/free_hugepages)
[[ "$FREE_HUGE" -ge 256 ]] \
    || die "Need >= 256 free 2 MB hugepages (have $FREE_HUGE). Run: echo 512 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages"

# Clean up any leftovers from a prior run.
rm -f "$MEMIF_SOCK"
ip link del "$TAP_IF" 2>/dev/null || true

# ── step 1: start vaigai (memif server — creates socket and listens) ──────────
info "Starting vaigai (net_memif server, $VAIGAI_IP) ..."
VAIGAI_FIFO=$(mktemp -u /tmp/vaigai_memif_fifo_XXXXXX)
VAIGAI_LOG=$(mktemp /tmp/vaigai_memif_XXXXXX.log)
mkfifo "$VAIGAI_FIFO"

"$VAIGAI_BIN" \
    -l "$DPDK_LCORES_VAIGAI" -n 1 --no-pci \
    --vdev "net_memif0,role=server,socket=$MEMIF_SOCK" \
    -- --src-ip "$VAIGAI_IP" \
    < "$VAIGAI_FIFO" > "$VAIGAI_LOG" 2>&1 &
VAIGAI_PID=$!
exec 7>"$VAIGAI_FIFO"   # hold write-end open so vaigai never sees EOF

sleep 3
kill -0 "$VAIGAI_PID" 2>/dev/null \
    || { info "=== vaigai log ==="; cat "$VAIGAI_LOG" >&2; die "vaigai failed to start."; }
info "vaigai PID $VAIGAI_PID  socket: $MEMIF_SOCK"

# ── step 2: start testpmd (memif client + net_tap L2 bridge) ─────────────────
info "Starting testpmd (net_memif client + net_tap L2 bridge) ..."
TESTPMD_FIFO=$(mktemp -u /tmp/testpmd_memif_fifo_XXXXXX)
TESTPMD_LOG=$(mktemp /tmp/testpmd_memif_XXXXXX.log)
mkfifo "$TESTPMD_FIFO"

# --interactive    : run testpmd in interactive mode (reads commands from FIFO)
# --forward-mode=io: forward all frames between paired ports (L2 bridge)
# --port-topology=paired: port 0 (memif) <-> port 1 (tap)
"$TESTPMD_BIN" \
    -l "$DPDK_LCORES_TESTPMD" -n 1 \
    --no-pci \
    -m 256 \
    --vdev "net_memif0,role=client,socket=$MEMIF_SOCK" \
    --vdev "net_tap0,iface=$TAP_IF" \
    -- \
    --total-num-mbufs 8192 \
    --interactive \
    --forward-mode=io \
    --port-topology=paired \
    < "$TESTPMD_FIFO" > "$TESTPMD_LOG" 2>&1 &
TESTPMD_PID=$!
exec 8>"$TESTPMD_FIFO"  # hold write-end open

sleep 4
kill -0 "$TESTPMD_PID" 2>/dev/null \
    || { info "=== testpmd log ==="; cat "$TESTPMD_LOG" >&2; die "testpmd failed to start."; }

# Send 'start' to begin io forwarding
echo "start" >&8
sleep 1
info "testpmd PID $TESTPMD_PID"

# ── step 3: configure the tap interface ───────────────────────────────────────
info "Waiting for $TAP_IF to appear ..."
for i in $(seq 1 15); do
    ip link show "$TAP_IF" &>/dev/null && break
    sleep 1
done
ip link show "$TAP_IF" &>/dev/null \
    || { cat "$TESTPMD_LOG" >&2; die "$TAP_IF did not appear — see testpmd log above."; }

ip addr add "$SUBNET_CIDR" dev "$TAP_IF"
ip link set "$TAP_IF" up
info "$TAP_IF up with IP $SOCAT_IP"

# ── step 4: start socat listeners ─────────────────────────────────────────────
info "Starting socat listeners on $SOCAT_IP ..."
socat TCP-LISTEN:$ECHO_PORT,bind=$SOCAT_IP,fork,reuseaddr PIPE &
SOCAT_ECHO_PID=$!
socat -u UDP4-RECVFROM:$SINK_PORT,bind=$SOCAT_IP,fork /dev/null &
SOCAT_SINK_PID=$!
sleep 1
kill -0 "$SOCAT_ECHO_PID" 2>/dev/null || die "socat echo listener failed to start"
kill -0 "$SOCAT_SINK_PID" 2>/dev/null || die "socat sink listener failed to start"
info "socat echo PID=$SOCAT_ECHO_PID  sink PID=$SOCAT_SINK_PID"

# ── helpers ───────────────────────────────────────────────────────────────────
vaigai_cmd() {
    local cmd="$1"
    local start_bytes
    start_bytes=$(stat -c%s "$VAIGAI_LOG" 2>/dev/null || echo 0)

    printf '%s\n' "$cmd" >&7

    local dur
    dur=$(echo "$cmd" | grep -oP '(?<=--duration )\d+' || echo 0)
    [[ "$dur" -gt 0 ]] && sleep $((dur + 2)) || sleep 3

    printf 'stats\n' >&7

    local attempts=0
    while true; do
        tail -c +$((start_bytes + 1)) "$VAIGAI_LOG" 2>/dev/null | grep -q 'Workers:' && break
        sleep 1; ((attempts++)) || true
        if [[ $attempts -gt 30 ]]; then
            info "Timed out waiting for stats output"
            tail -30 "$VAIGAI_LOG" 2>/dev/null || true
            break
        fi
    done
    sleep 0.5

    OUTPUT=$(tail -c +$((start_bytes + 1)) "$VAIGAI_LOG")
}

json_val() {
    grep -oP "\b$1:\s*\K[0-9]+" <<< "$OUTPUT" | tail -1 || echo 0
}

# ── T1: TCP SYN flood ─────────────────────────────────────────────────────────
if [[ "$RUN_TESTS" == "1" || "$RUN_TESTS" == "all" ]]; then
    info "T1: TCP SYN flood -> socat echo on $SOCAT_IP:$ECHO_PORT (5 s)"
    vaigai_cmd "start --proto tcp --ip $SOCAT_IP --port $ECHO_PORT --duration 5"
    TX=$(json_val "tx_pkts")
    RX=$(json_val "rx_pkts")
    if [[ "$TX" -gt 0 ]]; then
        pass "T1 TCP SYN flood: TX=$TX RX=$RX"
    else
        fail "T1 TCP SYN flood: no packets sent (TX=0) — check $VAIGAI_LOG and $TESTPMD_LOG"
    fi
    printf 'reset\n' >&7; sleep 2
fi

# ── T2: UDP flood ─────────────────────────────────────────────────────────────
if [[ "$RUN_TESTS" == "2" || "$RUN_TESTS" == "all" ]]; then
    info "T2: UDP flood -> socat discard on $SOCAT_IP:$SINK_PORT (5 s)"
    vaigai_cmd "start --proto udp --ip $SOCAT_IP --port $SINK_PORT --size 512 --duration 5"
    TX=$(json_val "tx_pkts")
    if [[ "$TX" -gt 0 ]]; then
        pass "T2 UDP flood: TX=$TX"
    else
        fail "T2 UDP flood: no packets sent (TX=0) — check $VAIGAI_LOG and $TESTPMD_LOG"
    fi
    printf 'reset\n' >&7; sleep 1
fi

# ── results ───────────────────────────────────────────────────────────────────
echo ""
echo -e "Results: ${GRN}${PASS_COUNT} passed${NC}, ${RED}${FAIL_COUNT} failed${NC}"
[[ $FAIL_COUNT -eq 0 ]] || exit 1
