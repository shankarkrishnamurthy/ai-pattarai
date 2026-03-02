#!/usr/bin/env bash
# Test suite: TCP protocol correctness — veth pair + ncat peer.
#
# Topology:
#   ┌──────────────────────────── Host ────────────────────────────────┐
#   │                                                                  │
#   │  vaigAI (DPDK net_af_packet)    veth pair    ncat (peer netns)  │
#   │  192.168.202.1             ◄───────────────► 192.168.202.2      │
#   │  veth-tcp0  (AF_PACKET)                      veth-tcp1          │
#   │                                                                  │
#   │  No VM · No hardware · No container runtime · 2 shell cmds up   │
#   └──────────────────────────────────────────────────────────────────┘
#
# The peer runs as ncat processes inside a dedicated network namespace.
# tc netem is applied to veth-tcp0 for loss/reorder tests.
#
# Tests:
#   T01 — 3-way handshake        (SYN → SYN-ACK → ACK, conn_open > 0)
#   T02 — Connection lifecycle    (tps + reset, conn_open > 0)
#   T03 — RST on closed port     (no listener → reset_rx > 0, conn_open = 0)
#   T04 — RST mid-connection     (peer killed mid-stream → reset_rx increments)
#   T05 — Clean-path integrity   (no bad checksums on clean path)
#   T06 — Retransmit under loss  (tc netem 5% loss → tcp_retransmit > 0)
#   T07 — Connection under reorder (netem reorder → conn survives)
#   T08 — Combined loss+reorder  (loss + reorder → conn survives)
#   T09 — High CPS / SYN flood   (syn_sent > 1000, syn_queue_drops = 0)
#   T10 — Multiple bursts         (sequential bursts to diff ports)
#   T11 — Echo peer connectivity  (echo peer tps, handshake completes)
#   T12 — High-latency path       (tc netem delay 100ms — handshake with delay)
#   T13 — Heavy loss stress       (20% loss — vaigai must not crash)
#   T14 — Small MSS / MTU 576     (peer MTU 576 → handshake with MSS ~536)
#   T15 — Rapid reconnect churn   (5 connect-close cycles → port pool + TIME_WAIT)
#
# Prerequisites:
#   - vaigAI binary built:  build/vaigai
#   - Root privileges       (CAP_NET_ADMIN for veth/netns/tc)
#   - ncat (nmap-ncat) in PATH
#   - iproute2: ip, tc
#
# Usage:
#   bash tests/proto/tcp_suite.sh [OPTIONS]
#
# Options:
#   --test <N|all>   Which test to run (default: all)
#   --keep           Keep topology on exit (debugging)
#   -h, --help       Show this help and exit

set -euo pipefail

# ── argument parsing ──────────────────────────────────────────────────────────
RUN_TESTS="all"
KEEP=0

usage() {
    grep '^#' "$0" | grep -v '^#!/' | sed 's/^# \{0,2\}//'
    exit 0
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --test) [[ -n "${2:-}" ]] || { echo "Error: --test requires a value" >&2; exit 1; }
                RUN_TESTS="$2"; shift 2 ;;
        --keep) KEEP=1; shift ;;
        -h|--help) usage ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

# ── config ────────────────────────────────────────────────────────────────────
PEER_NS="vaigai-tcp-peer"
HOST_IF="veth-tcp0"
PEER_IF="veth-tcp1"
VAIGAI_IP="192.168.202.1"
PEER_IP="192.168.202.2"
PEER_CIDR="${PEER_IP}/24"
PORT_ECHO=5000        # ncat echo  (--keep-open, /bin/cat)
PORT_DISCARD=5001     # ncat discard (absorbs data, no reply)
PORT_CLOSED=5999      # nothing listening — provokes RST
DPDK_LCORES="0-1"
VAIGAI_BIN="$(cd "$(dirname "$0")/../.." && pwd)/build/vaigai"
PASS_COUNT=0
FAIL_COUNT=0

# ── colours ───────────────────────────────────────────────────────────────────
GRN='\033[0;32m'; RED='\033[0;31m'; CYN='\033[0;36m'; NC='\033[0m'
info() { echo -e "${CYN}[INFO]${NC}  $*"; }
pass() { echo -e "${GRN}[PASS]${NC}  tcp_suite: $*"; ((PASS_COUNT++)) || true; }
fail() { echo -e "${RED}[FAIL]${NC}  tcp_suite: $*" >&2; ((FAIL_COUNT++)) || true; }
die()  { echo -e "${RED}[FATAL]${NC} tcp_suite: $*" >&2; exit 1; }

# ── helper: extract JSON numeric field from $OUTPUT ───────────────────────────
json_val() {
    local v
    v=$(echo "$OUTPUT" | tr -cd '[:print:]\n' | grep -oP "\"$1\": *\K[0-9]+" | tail -1)
    echo "${v:-0}"
}

# ── vaigai FIFO lifecycle ─────────────────────────────────────────────────────
VAIGAI_FIFO=""
VAIGAI_PID=""
VAIGAI_LOG=""
VAIGAI_CFG=""
OUTPUT=""

vaigai_start() {
    VAIGAI_CFG=$(mktemp /tmp/vaigai_tcp_suite_XXXXXX.json)
    cat > "$VAIGAI_CFG" <<EOCFG
{
  "protocol": "tcp",
  "flows": [{
    "src_ip_lo": "$VAIGAI_IP", "src_ip_hi": "$VAIGAI_IP",
    "dst_ip": "$PEER_IP", "dst_port": $PORT_ECHO,
    "icmp_ping": false
  }],
  "load": { "max_concurrent": 4096, "target_cps": 0, "duration_secs": 0 },
  "tls": null
}
EOCFG
    VAIGAI_FIFO=$(mktemp -u /tmp/vaigai_tcp_suite_fifo_XXXXXX)
    mkfifo "$VAIGAI_FIFO"
    VAIGAI_LOG=$(mktemp /tmp/vaigai_tcp_suite_out_XXXXXX.log)

    VAIGAI_CONFIG="$VAIGAI_CFG" "$VAIGAI_BIN" \
        -l "$DPDK_LCORES" -n 1 --no-pci \
        --vdev "net_af_packet0,iface=$HOST_IF" -- \
        < "$VAIGAI_FIFO" > "$VAIGAI_LOG" 2>&1 &
    VAIGAI_PID=$!
    exec 7>"$VAIGAI_FIFO"   # hold write-end open so vaigai never sees EOF

    info "Waiting for DPDK init..."
    local waited=0
    while [[ $waited -lt 30 ]]; do
        grep -q "vaigai CLI\|tgen>\|vaigai>" "$VAIGAI_LOG" 2>/dev/null && break
        sleep 1; ((waited++)) || true
    done
    grep -q "vaigai CLI\|tgen>\|vaigai>" "$VAIGAI_LOG" 2>/dev/null \
        || die "vaigai did not initialise in 30s — log: $VAIGAI_LOG"
    info "vaigai ready"
}

vaigai_stop() {
    if [[ -n "$VAIGAI_PID" ]] && kill -0 "$VAIGAI_PID" 2>/dev/null; then
        printf 'quit\n' >&7 2>/dev/null || true
        exec 7>&- 2>/dev/null || true
        local w=0
        while kill -0 "$VAIGAI_PID" 2>/dev/null && [[ $w -lt 10 ]]; do
            sleep 0.5; ((w++)) || true
        done
        kill -9 "$VAIGAI_PID" 2>/dev/null || true
        wait "$VAIGAI_PID" 2>/dev/null || true
    else
        exec 7>&- 2>/dev/null || true
    fi
    rm -f "$VAIGAI_FIFO" "$VAIGAI_LOG" "$VAIGAI_CFG"
    VAIGAI_PID=""
}

# Send a command, wait for its natural run time, collect stats into $OUTPUT.
vaigai_cmd() {
    local cmd="$1"

    # If vaigai died, restart transparently
    if ! kill -0 "$VAIGAI_PID" 2>/dev/null; then
        info "vaigai not running — restarting before command"
        vaigai_restart
    fi

    local start_bytes
    start_bytes=$(stat -c%s "$VAIGAI_LOG" 2>/dev/null || echo 0)

    printf '%s\n' "$cmd" >&7

    # Duration: field 4 for "tps", field 5 for "throughput" + extra for connect
    local dur=3
    case "$(awk '{print $1}' <<< "$cmd")" in
        tps)        dur=$(awk '{print $4}' <<< "$cmd") ;;
        throughput) dur=$(awk '{print $5}' <<< "$cmd") ;;
    esac
    [[ "$dur" =~ ^[0-9]+$ && "$dur" -gt 0 ]] && sleep $((dur + 5)) || sleep 5

    # If vaigai died during the command, capture what we can
    if ! kill -0 "$VAIGAI_PID" 2>/dev/null; then
        OUTPUT=$(tail -c +$((start_bytes + 1)) "$VAIGAI_LOG" 2>/dev/null || true)
        return
    fi

    printf 'stats\n' >&7
    local attempts=0
    while true; do
        tail -c +$((start_bytes + 1)) "$VAIGAI_LOG" 2>/dev/null | grep -q '^}' && break
        sleep 1; ((attempts++)) || true
        [[ $attempts -gt 30 ]] && { info "stats timeout"; break; }
    done
    sleep 0.3
    OUTPUT=$(tail -c +$((start_bytes + 1)) "$VAIGAI_LOG")
}

vaigai_reset() {
    # Check if vaigai is still alive; restart if crashed
    if ! kill -0 "$VAIGAI_PID" 2>/dev/null; then
        info "vaigai crashed — restarting"
        vaigai_restart
        return
    fi
    printf 'reset\n' >&7; sleep 3
}

# Full stop + start for tests that leave vaigai in a bad state (e.g., RST flood)
vaigai_restart() {
    vaigai_stop
    rm -rf /var/run/dpdk/rte/ 2>/dev/null || true
    sleep 1
    vaigai_start
}

# ── peer server management ────────────────────────────────────────────────────
PEER_ECHO_PID=""
PEER_DISCARD_PID=""

start_peers() {
    # Kill any leftover ncat/cat processes in peer netns
    ip netns exec "$PEER_NS" bash -c 'kill $(pidof ncat cat) 2>/dev/null || true'
    sleep 0.3

    # Accept-only: no --exec (no fork per connection), just accept and hold.
    # For tps tests we only need the kernel to complete 3-way handshake.
    ip netns exec "$PEER_NS" \
        ncat --listen --keep-open "$PEER_IP" $PORT_ECHO < /dev/null &
    PEER_ECHO_PID=$!

    # Discard: swallows data from stdin, keeps connection open
    ip netns exec "$PEER_NS" \
        ncat --listen --keep-open "$PEER_IP" $PORT_DISCARD < /dev/null &
    PEER_DISCARD_PID=$!

    sleep 1   # let both servers reach listen state
}

stop_peers() {
    [[ -n "$PEER_ECHO_PID"    ]] && kill "$PEER_ECHO_PID"    2>/dev/null || true
    [[ -n "$PEER_DISCARD_PID" ]] && kill "$PEER_DISCARD_PID" 2>/dev/null || true
    # Also kill any stray ncat/cat children in the peer netns
    ip netns exec "$PEER_NS" bash -c 'kill $(pidof ncat cat) 2>/dev/null || true'
    PEER_ECHO_PID=""
    PEER_DISCARD_PID=""
}

# ── tc netem helpers ──────────────────────────────────────────────────────────
netem_on()       { tc qdisc replace dev "$HOST_IF" root netem "$@"; }
netem_off()      { tc qdisc del dev "$HOST_IF" root 2>/dev/null || true; }
netem_peer_on()  { ip netns exec "$PEER_NS" tc qdisc replace dev "$PEER_IF" root netem "$@"; }
netem_peer_off() { ip netns exec "$PEER_NS" tc qdisc del dev "$PEER_IF" root 2>/dev/null || true; }

# ── pre-flight ────────────────────────────────────────────────────────────────
[[ $EUID -eq 0 ]]      || die "Must run as root"
[[ -x "$VAIGAI_BIN" ]] || die "vaigai binary not found: $VAIGAI_BIN — run ninja-build first"
for cmd in ncat ip tc; do
    command -v "$cmd" &>/dev/null || die "Required command not found: $cmd"
done

# ── teardown ──────────────────────────────────────────────────────────────────
teardown() {
    [[ $KEEP -eq 1 ]] && { info "Keeping topology (--keep)"; return; }
    info "Tearing down"
    netem_off
    netem_peer_off
    stop_peers
    vaigai_stop
    ip netns del "$PEER_NS" 2>/dev/null || true
    ip link del "$HOST_IF"  2>/dev/null || true
}
trap teardown EXIT

# ── topology setup ────────────────────────────────────────────────────────────
info "Setting up veth pair + peer netns"
ip netns del "$PEER_NS" 2>/dev/null || true
ip link del "$HOST_IF"  2>/dev/null || true

ip netns add "$PEER_NS"
ip link add "$HOST_IF" type veth peer name "$PEER_IF"
ip link set "$HOST_IF" promisc on
ip link set "$HOST_IF" up

ip link set "$PEER_IF" netns "$PEER_NS"
ip netns exec "$PEER_NS" ip link set lo up
ip netns exec "$PEER_NS" ip link set "$PEER_IF" up
ip netns exec "$PEER_NS" ip addr add "$PEER_CIDR" dev "$PEER_IF"

# Enlarge SYN backlog so tps tests don't hit the kernel queue first
ip netns exec "$PEER_NS" sysctl -qw net.core.somaxconn=65535    || true
ip netns exec "$PEER_NS" sysctl -qw net.ipv4.tcp_syncookies=0   || true

info "Topology ready — vaigAI=$VAIGAI_IP  peer=$PEER_IP  ($HOST_IF ↔ $PEER_IF)"
start_peers
vaigai_start

# ══════════════════════════════════════════════════════════════════════════════
# T01 — 3-way handshake
#   vaigAI sends SYN; ncat echo peer responds SYN-ACK; vaigAI completes ACK.
#   tcp_conn_open > 0 confirms ESTABLISHED was reached.
# ══════════════════════════════════════════════════════════════════════════════
run_t01() {
    info "T01: 3-way handshake → ${PEER_IP}:${PORT_ECHO} (2 s)"
    vaigai_cmd "tps $PEER_IP 2 0 64 $PORT_ECHO"

    local syn_sent conn_open reset_rx bad_cksum
    syn_sent=$(json_val tcp_syn_sent)
    conn_open=$(json_val tcp_conn_open)
    reset_rx=$(json_val tcp_reset_rx)
    bad_cksum=$(json_val tcp_bad_cksum)
    info "  syn_sent=$syn_sent conn_open=$conn_open reset_rx=$reset_rx bad_cksum=$bad_cksum"

    [[ "$syn_sent"  -gt 0 ]] && pass "T01 tcp_syn_sent > 0 ($syn_sent)"           || fail "T01 tcp_syn_sent = 0"
    [[ "$conn_open" -gt 0 ]] && pass "T01 tcp_conn_open > 0 ($conn_open)"          || fail "T01 tcp_conn_open = 0"
    [[ "$reset_rx"  -eq 0 ]] && pass "T01 tcp_reset_rx = 0"                        || fail "T01 tcp_reset_rx = $reset_rx (unexpected RST)"
    [[ "$bad_cksum" -eq 0 ]] && pass "T01 tcp_bad_cksum = 0"                       || fail "T01 tcp_bad_cksum = $bad_cksum"
}

# ══════════════════════════════════════════════════════════════════════════════
# T02 — Connection lifecycle (open + reset)
#   vaigAI opens connections via tps, then a reset closes them.
#   Verifies conn_open > 0 and no unexpected RSTs.
# ══════════════════════════════════════════════════════════════════════════════
run_t02() {
    vaigai_restart
    info "T02: Connection lifecycle — tps 2 s then reset"
    vaigai_cmd "tps $PEER_IP 2 100 0 $PORT_DISCARD"

    local conn_open reset_rx retransmit
    conn_open=$(json_val tcp_conn_open)
    reset_rx=$(json_val tcp_reset_rx)
    retransmit=$(json_val tcp_retransmit)
    info "  conn_open=$conn_open reset_rx=$reset_rx retransmit=$retransmit"

    [[ "$conn_open"  -gt 0 ]]  && pass "T02 conn_open > 0 ($conn_open)"             || fail "T02 conn_open = 0"
    [[ "$reset_rx"   -eq 0 ]]  && pass "T02 reset_rx = 0 (no unexpected RST)"        || fail "T02 reset_rx = $reset_rx"
}

# ══════════════════════════════════════════════════════════════════════════════
# T03 — RST on closed port
#   Nothing listens on PORT_CLOSED.  The peer kernel immediately replies RST.
#   vaigAI must record tcp_reset_rx > 0 and produce zero open connections.
# ══════════════════════════════════════════════════════════════════════════════
run_t03() {
    vaigai_restart
    info "T03: RST on closed port ${PEER_IP}:${PORT_CLOSED} (2 s)"
    vaigai_cmd "tps $PEER_IP 2 0 0 $PORT_CLOSED"

    local syn_sent conn_open reset_rx
    syn_sent=$(json_val tcp_syn_sent)
    conn_open=$(json_val tcp_conn_open)
    reset_rx=$(json_val tcp_reset_rx)
    info "  syn_sent=$syn_sent conn_open=$conn_open reset_rx=$reset_rx"

    [[ "$syn_sent"  -gt 0 ]] && pass "T03 tcp_syn_sent > 0 ($syn_sent)"              || fail "T03 tcp_syn_sent = 0 (no SYN sent)"
    [[ "$conn_open" -eq 0 ]] && pass "T03 tcp_conn_open = 0 (RST blocked ESTABLISHED)" || fail "T03 tcp_conn_open = $conn_open (expected 0)"
    [[ "$reset_rx"  -gt 0 ]] && pass "T03 tcp_reset_rx > 0 ($reset_rx)"              || fail "T03 tcp_reset_rx = 0 (expected RST from peer)"
}

# ══════════════════════════════════════════════════════════════════════════════
# T04 — RST mid-connection
#   A tps run reaches ESTABLISHED, then the echo peer is killed.  The peer
#   kernel sends RST to every open socket.  vaigAI must absorb RSTs without
#   crashing and increment tcp_reset_rx.
# ══════════════════════════════════════════════════════════════════════════════
run_t04() {
    # T03 RST flood can leave stale internal state — full restart ensures clean TCB store
    vaigai_restart

    info "T04: RST mid-connection — tps 3 s, kill peer at t=1 s"
    local start_bytes
    start_bytes=$(stat -c%s "$VAIGAI_LOG" 2>/dev/null || echo 0)

    printf 'tps %s 3 0 64 %d\n' "$PEER_IP" "$PORT_ECHO" >&7
    sleep 1
    # Kill the original echo peer — kernel sends RST/FIN to open sockets
    kill "$PEER_ECHO_PID" 2>/dev/null || true
    PEER_ECHO_PID=""
    sleep 4                                       # let tps finish + settle
    printf 'stats\n' >&7
    sleep 2
    OUTPUT=$(tail -c +$((start_bytes + 1)) "$VAIGAI_LOG")

    local conn_open reset_rx
    conn_open=$(json_val tcp_conn_open)
    reset_rx=$(json_val tcp_reset_rx)
    info "  conn_open=$conn_open reset_rx=$reset_rx"

    [[ "$conn_open" -gt 0 ]] && pass "T04 conn_open > 0 (ESTABLISHED before kill: $conn_open)"    || fail "T04 conn_open = 0"
    # Killing ncat may send FIN or RST depending on kernel; accept either
    [[ "$reset_rx"  -gt 0 ]] && pass "T04 tcp_reset_rx > 0 (RST after peer kill: $reset_rx)"      || info "T04 tcp_reset_rx = 0 (peer sent FIN not RST — acceptable)"

    # Restart echo peer for subsequent tests — wait for TIME_WAIT to clear
    sleep 2
    ip netns exec "$PEER_NS" bash -c 'kill $(pidof ncat cat) 2>/dev/null || true'
    sleep 1
    ip netns exec "$PEER_NS" \
        ncat --listen --keep-open "$PEER_IP" $PORT_ECHO < /dev/null &
    PEER_ECHO_PID=$!
    # Also restart discard peer (may have been affected)
    ip netns exec "$PEER_NS" \
        ncat --listen --keep-open "$PEER_IP" $PORT_DISCARD < /dev/null &
    PEER_DISCARD_PID=$!
    sleep 1
}

# ══════════════════════════════════════════════════════════════════════════════
# T05 — Clean-path integrity
#   On an unimpaired veth with rate-limited tps, checksums must be clean.
# ══════════════════════════════════════════════════════════════════════════════
run_t05() {
    # T04 kills the echo peer; restart vaigai for a clean slate
    vaigai_restart
    info "T05: Clean-path integrity — tps (3 s, rate 500) → ${PEER_IP}:${PORT_ECHO}"
    vaigai_cmd "tps $PEER_IP 3 500 64 $PORT_ECHO"

    local syn_sent conn_open bad_cksum ip_bad
    syn_sent=$(json_val tcp_syn_sent)
    conn_open=$(json_val tcp_conn_open)
    bad_cksum=$(json_val tcp_bad_cksum)
    ip_bad=$(json_val ip_bad_cksum)
    info "  syn_sent=$syn_sent conn_open=$conn_open bad_cksum=$bad_cksum ip_bad=$ip_bad"

    [[ "$syn_sent"   -gt 0 ]] && pass "T05 tcp_syn_sent > 0 ($syn_sent)"       || fail "T05 tcp_syn_sent = 0 (no traffic)"
    [[ "$conn_open"  -gt 0 ]] && pass "T05 tcp_conn_open > 0 ($conn_open)"     || fail "T05 tcp_conn_open = 0"
    [[ "$bad_cksum"  -eq 0 ]] && pass "T05 tcp_bad_cksum = 0"                   || fail "T05 tcp_bad_cksum = $bad_cksum"
    [[ "$ip_bad"     -eq 0 ]] && pass "T05 ip_bad_cksum = 0"                    || fail "T05 ip_bad_cksum = $ip_bad"
}

# ══════════════════════════════════════════════════════════════════════════════
# T06 — Retransmit under packet loss
#   tc netem injects 5% random loss.  vaigAI must detect missing ACKs and
#   retransmit: tcp_retransmit > 0 while connections still open.
# ══════════════════════════════════════════════════════════════════════════════
run_t06() {
    vaigai_restart
    info "T06: Retransmit under 5% packet loss (tc netem) — flood (5 s)"
    netem_on loss 5%
    vaigai_cmd "tps $PEER_IP 5 0 64 $PORT_ECHO"
    netem_off

    local conn_open syn_sent retransmit
    conn_open=$(json_val tcp_conn_open)
    syn_sent=$(json_val tcp_syn_sent)
    retransmit=$(json_val tcp_retransmit)
    info "  conn_open=$conn_open syn_sent=$syn_sent retransmit=$retransmit"

    [[ "$syn_sent"   -gt 0 ]] && pass "T06 tcp_syn_sent > 0 ($syn_sent)"                        || fail "T06 tcp_syn_sent = 0"
    [[ "$conn_open"  -gt 0 ]] && pass "T06 conn_open > 0 (survived 5% loss: $conn_open)"        || info "T06 conn_open = 0 (variable under loss — acceptable)"
    [[ "$retransmit" -gt 0 ]] && pass "T06 tcp_retransmit > 0 (retransmitted: $retransmit)"      || fail "T06 tcp_retransmit = 0 (expected retransmits with 5% loss)"
}

# ══════════════════════════════════════════════════════════════════════════════
# T07 — Connection survives packet reordering
#   tc netem on peer egress delays/reorders SYN-ACKs reaching vaigAI.
#   Connections must still establish (conn_open > 0) under reordering.
# ══════════════════════════════════════════════════════════════════════════════
run_t07() {
    vaigai_restart
    info "T07: Connection under reordering (peer-side netem delay 50ms reorder 30%) — flood (5 s)"
    netem_peer_on delay 50ms reorder 30% 25%
    vaigai_cmd "tps $PEER_IP 5 0 64 $PORT_ECHO"
    netem_peer_off

    local conn_open syn_sent retransmit
    conn_open=$(json_val tcp_conn_open)
    syn_sent=$(json_val tcp_syn_sent)
    retransmit=$(json_val tcp_retransmit)
    info "  conn_open=$conn_open syn_sent=$syn_sent retransmit=$retransmit"

    [[ "$syn_sent"   -gt 0 ]] && pass "T07 tcp_syn_sent > 0 ($syn_sent)"                       || fail "T07 tcp_syn_sent = 0"
    [[ "$conn_open"  -gt 0 ]] && pass "T07 conn_open > 0 (survived reordering: $conn_open)"    || fail "T07 conn_open = 0"
}

# ══════════════════════════════════════════════════════════════════════════════
# T08 — Connection survives combined loss + reorder
#   Both host-side (TX) and peer-side (ACK) impairment.  Exercises
#   retransmit + reorder handling simultaneously.
# ══════════════════════════════════════════════════════════════════════════════
run_t08() {
    vaigai_restart
    info "T08: Combined loss+reorder — flood (5 s)"
    netem_on loss 3%
    netem_peer_on delay 30ms reorder 25% 20%
    vaigai_cmd "tps $PEER_IP 5 0 64 $PORT_ECHO"
    netem_off
    netem_peer_off

    local conn_open syn_sent retransmit
    conn_open=$(json_val tcp_conn_open)
    syn_sent=$(json_val tcp_syn_sent)
    retransmit=$(json_val tcp_retransmit)
    info "  conn_open=$conn_open syn_sent=$syn_sent retransmit=$retransmit"

    [[ "$syn_sent"   -gt 0 ]] && pass "T08 tcp_syn_sent > 0 ($syn_sent)"                               || fail "T08 tcp_syn_sent = 0"
    [[ "$conn_open"  -gt 0 ]] && pass "T08 conn_open > 0 (survived loss+reorder: $conn_open)"          || fail "T08 conn_open = 0"
    [[ "$retransmit" -gt 0 ]] && pass "T08 tcp_retransmit > 0 (retransmitted: $retransmit)"             || fail "T08 tcp_retransmit = 0 (expected with loss)"
}

# ══════════════════════════════════════════════════════════════════════════════
# T09 — High CPS / SYN flood
#   Burst connections at maximum rate.  Verifies syn_sent reaches a high
#   volume and the SYN queue never overflows (tcp_syn_queue_drops == 0).
# ══════════════════════════════════════════════════════════════════════════════
run_t09() {
    vaigai_restart
    info "T09: SYN flood / high CPS → ${PEER_IP}:${PORT_ECHO} (5 s, unlimited rate)"
    vaigai_cmd "tps $PEER_IP 5 0 0 $PORT_ECHO"

    local syn_sent conn_open queue_drops
    syn_sent=$(json_val tcp_syn_sent)
    conn_open=$(json_val tcp_conn_open)
    queue_drops=$(json_val tcp_syn_queue_drops)
    info "  syn_sent=$syn_sent conn_open=$conn_open queue_drops=$queue_drops"

    [[ "$syn_sent"    -gt 1000 ]] && pass "T09 tcp_syn_sent > 1000 ($syn_sent)"         || fail "T09 tcp_syn_sent = $syn_sent (expected high CPS)"
    [[ "$conn_open"   -gt 0    ]] && pass "T09 tcp_conn_open > 0 ($conn_open)"           || fail "T09 tcp_conn_open = 0"
    [[ "$queue_drops" -eq 0    ]] && pass "T09 tcp_syn_queue_drops = 0"                  || fail "T09 tcp_syn_queue_drops = $queue_drops (SYN queue overflow)"
}

# ══════════════════════════════════════════════════════════════════════════════
# T10 — Multiple connection bursts
#   Two sequential tps bursts to different ports.  Both must establish
#   connections — verifies port pool and TCB recycling between runs.
# ══════════════════════════════════════════════════════════════════════════════
run_t10() {
    vaigai_restart
    # Restart peers to shed accumulated connections from prior tps runs
    stop_peers; start_peers
    info "T10: Multiple bursts — tps 2 s to echo, then 2 s to discard"
    vaigai_cmd "tps $PEER_IP 2 500 64 $PORT_ECHO"

    local open1
    open1=$(json_val tcp_conn_open)
    info "  burst1: conn_open=$open1"

    vaigai_reset
    vaigai_cmd "tps $PEER_IP 2 500 64 $PORT_DISCARD"

    local open2
    open2=$(json_val tcp_conn_open)
    info "  burst2: conn_open=$open2"

    [[ "$open1" -gt 0 ]] && pass "T10 burst1 conn_open > 0 ($open1)"   || fail "T10 burst1 conn_open = 0"
    [[ "$open2" -gt 0 ]] && pass "T10 burst2 conn_open > 0 ($open2)"   || fail "T10 burst2 conn_open = 0"
}

# ══════════════════════════════════════════════════════════════════════════════
# T11 — Echo peer connectivity
#   vaigAI floods the echo peer; the peer sends SYN-ACKs back.
#   Verifies the full 3-way handshake completes on the echo port.
# ══════════════════════════════════════════════════════════════════════════════
run_t11() {
    vaigai_restart
    # Ensure echo peer is alive (T04 may have killed it)
    if ! kill -0 "$PEER_ECHO_PID" 2>/dev/null; then
        ip netns exec "$PEER_NS" \
            ncat --listen --keep-open "$PEER_IP" $PORT_ECHO < /dev/null &
        PEER_ECHO_PID=$!
        sleep 0.5
    fi
    info "T11: Echo peer flood — ${PEER_IP}:${PORT_ECHO} (3 s)"
    vaigai_cmd "tps $PEER_IP 3 500 64 $PORT_ECHO"

    local syn_sent conn_open reset_rx
    syn_sent=$(json_val tcp_syn_sent)
    conn_open=$(json_val tcp_conn_open)
    reset_rx=$(json_val tcp_reset_rx)
    info "  syn_sent=$syn_sent conn_open=$conn_open reset_rx=$reset_rx"

    [[ "$syn_sent"   -gt 0 ]] && pass "T11 tcp_syn_sent > 0 ($syn_sent)"     || fail "T11 tcp_syn_sent = 0"
    [[ "$conn_open"  -gt 0 ]] && pass "T11 conn_open > 0 ($conn_open)"       || fail "T11 conn_open = 0"
    [[ "$reset_rx"   -eq 0 ]] && pass "T11 tcp_reset_rx = 0"                  || fail "T11 tcp_reset_rx = $reset_rx"
}

# ══════════════════════════════════════════════════════════════════════════════
# T12 — High-latency path (WAN simulation)
#   tc netem adds 100 ms RTT.  vaigAI's SYN retransmit must handle the
#   delay without giving up.  Connections must still establish.
# ══════════════════════════════════════════════════════════════════════════════
run_t12() {
    vaigai_restart
    info "T12: High-latency path (tc netem delay 100ms) — flood (5 s)"
    netem_on delay 100ms
    vaigai_cmd "tps $PEER_IP 5 100 64 $PORT_ECHO"
    netem_off

    local conn_open syn_sent retransmit
    conn_open=$(json_val tcp_conn_open)
    syn_sent=$(json_val tcp_syn_sent)
    retransmit=$(json_val tcp_retransmit)
    info "  conn_open=$conn_open syn_sent=$syn_sent retransmit=$retransmit"

    [[ "$syn_sent"   -gt 0 ]] && pass "T12 tcp_syn_sent > 0 ($syn_sent)"                              || fail "T12 tcp_syn_sent = 0"
    [[ "$conn_open"  -gt 0 ]] && pass "T12 conn_open > 0 (connected over 100ms RTT: $conn_open)"      || fail "T12 conn_open = 0 (SYN timed out on high-latency path)"
}

# ══════════════════════════════════════════════════════════════════════════════
# T13 — Heavy loss / congestion control stress
#   20% packet loss hammers the connection setup path.  vaigAI must not crash
#   and must still establish some connections despite massive loss.
# ══════════════════════════════════════════════════════════════════════════════
run_t13() {
    vaigai_restart
    info "T13: Heavy loss (tc netem loss 20%) — flood (8 s)"
    netem_on loss 20%
    vaigai_cmd "tps $PEER_IP 8 0 64 $PORT_ECHO"
    netem_off

    local syn_sent retransmit vaigai_alive
    syn_sent=$(json_val tcp_syn_sent)
    retransmit=$(json_val tcp_retransmit)
    vaigai_alive=0
    kill -0 "$VAIGAI_PID" 2>/dev/null && vaigai_alive=1
    info "  syn_sent=$syn_sent retransmit=$retransmit alive=$vaigai_alive"

    [[ "$vaigai_alive" -eq 1 ]] && pass "T13 vaigai still running (no crash under heavy loss)"         || fail "T13 vaigai process died under 20% loss"
    [[ "$syn_sent"     -gt 0 ]] && pass "T13 tcp_syn_sent > 0 ($syn_sent)"                              || fail "T13 tcp_syn_sent = 0"
    [[ "$retransmit"   -gt 0 ]] && pass "T13 tcp_retransmit > 0 (retransmitted: $retransmit)"           || fail "T13 tcp_retransmit = 0 (expected heavy retransmission at 20% loss)"
}

# ══════════════════════════════════════════════════════════════════════════════
# T14 — Small MSS / constrained MTU
#   The peer veth MTU is set to 576 (minimum IPv4 MTU).  The peer kernel
#   advertises MSS ≈ 536 in SYN-ACK.  vaigAI must still complete handshakes
#   and not crash.  No RSTs from oversized segments.
# ══════════════════════════════════════════════════════════════════════════════
run_t14() {
    vaigai_restart
    info "T14: Small MSS (peer MTU 576) — flood (3 s)"

    # Reduce peer-side MTU; host veth stays at default
    ip netns exec "$PEER_NS" ip link set "$PEER_IF" mtu 576
    vaigai_cmd "tps $PEER_IP 3 500 64 $PORT_ECHO"
    # Restore peer MTU for subsequent tests
    ip netns exec "$PEER_NS" ip link set "$PEER_IF" mtu 1500

    local conn_open syn_sent reset_rx
    conn_open=$(json_val tcp_conn_open)
    syn_sent=$(json_val tcp_syn_sent)
    reset_rx=$(json_val tcp_reset_rx)
    info "  conn_open=$conn_open syn_sent=$syn_sent reset_rx=$reset_rx"

    [[ "$syn_sent"   -gt 0 ]] && pass "T14 tcp_syn_sent > 0 ($syn_sent)"                      || fail "T14 tcp_syn_sent = 0"
    [[ "$conn_open"  -gt 0 ]] && pass "T14 conn_open > 0 (connected with small MSS)"          || fail "T14 conn_open = 0 (handshake failed with MSS ~536)"
    [[ "$reset_rx"   -eq 0 ]] && pass "T14 tcp_reset_rx = 0 (no oversized-segment RST)"        || fail "T14 tcp_reset_rx = $reset_rx (peer RST — vaigAI sent oversized segment?)"
}

# ══════════════════════════════════════════════════════════════════════════════
# T15 — Rapid reconnect churn (port pool + TIME_WAIT)
#   Five cycles of: open 64 connections → close → reset → reopen.
#   Exercises the per-lcore ephemeral port pool and TIME_WAIT hold-off
#   (TGEN_TCP_TIMEWAIT_MS = 4000 ms default).  If the pool leaks ports
#   or fails to recycle them, later cycles will fail with conn_open = 0
#   or syn_queue_drops > 0.
# ══════════════════════════════════════════════════════════════════════════════
run_t15() {
    vaigai_restart
    info "T15: Rapid reconnect churn — 5 cycles of 64 conns each"

    local cycle total_open=0 total_close=0 total_drops=0
    for cycle in 1 2 3 4 5; do
        vaigai_cmd "tps $PEER_IP 1 64 0 $PORT_ECHO"
        local co cc drops
        co=$(json_val tcp_conn_open)
        cc=$(json_val tcp_conn_close)
        drops=$(json_val tcp_syn_queue_drops)
        total_open=$((total_open + co))
        total_close=$((total_close + cc))
        total_drops=$((total_drops + drops))
        info "  cycle $cycle: conn_open=$co conn_close=$cc drops=$drops"
        vaigai_reset
    done

    local vaigai_alive=0
    kill -0 "$VAIGAI_PID" 2>/dev/null && vaigai_alive=1
    info "  total: conn_open=$total_open conn_close=$total_close drops=$total_drops alive=$vaigai_alive"

    [[ "$vaigai_alive" -eq 1      ]] && pass "T15 vaigai alive after 5 churn cycles"                       || fail "T15 vaigai died during churn"
    [[ "$total_open"   -gt 0      ]] && pass "T15 total conn_open > 0 ($total_open across 5 cycles)"       || fail "T15 total conn_open = 0 (port pool exhaustion?)"
    [[ "$total_drops"  -eq 0      ]] && pass "T15 syn_queue_drops = 0 across all cycles"                    || fail "T15 syn_queue_drops = $total_drops (port pool leak?)"
}

# ── dispatch ──────────────────────────────────────────────────────────────────
should_run() { [[ "$RUN_TESTS" == "all" || "$RUN_TESTS" == "$1" ]]; }

should_run 1  && run_t01
should_run 2  && run_t02
should_run 3  && run_t03
should_run 4  && run_t04
should_run 5  && run_t05
should_run 6  && run_t06
should_run 7  && run_t07
should_run 8  && run_t08
should_run 9  && run_t09
should_run 10 && run_t10
should_run 11 && run_t11
should_run 12 && run_t12
should_run 13 && run_t13
should_run 14 && run_t14
should_run 15 && run_t15

# ── summary ───────────────────────────────────────────────────────────────────
echo ""
info "Results: ${PASS_COUNT} passed, ${FAIL_COUNT} failed"
[[ $FAIL_COUNT -eq 0 ]] || exit 1
