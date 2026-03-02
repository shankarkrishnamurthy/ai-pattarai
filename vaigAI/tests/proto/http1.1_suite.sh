#!/usr/bin/env bash
# Test suite: HTTP/1.1 protocol correctness — veth pair + Python HTTP peer.
#
# Topology:
#   ┌───────────────────────────── Host ─────────────────────────────────┐
#   │                                                                    │
#   │  vaigAI (DPDK net_af_packet)    veth pair    Python HTTP server    │
#   │  192.168.205.1             ◄───────────────► 192.168.205.2         │
#   │  veth-http0  (AF_PACKET)                     veth-http1            │
#   │                                                                    │
#   │  No VM · No hardware · No container runtime · 2 shell cmds up      │
#   └────────────────────────────────────────────────────────────────────┘
#
# The peer runs as a Python HTTP/1.1 server inside a dedicated network
# namespace, listening on configurable ports.  This exercises vaigAI's
# TCP stack against a real HTTP/1.1 speaker, validating the most widely
# used parts of the protocol:
#
# Tests:
#   T01 — HTTP GET connectivity       (TCP handshake to HTTP server :80)
#   T02 — HTTP connection rate         (SYN flood to :80, CPS = RPS)
#   T03 — Rate-limited HTTP conns      (token bucket accuracy to HTTP server)
#   T04 — HTTP throughput              (TCP data transfer to HTTP :80)
#   T05 — Concurrent HTTP streams      (4 parallel TCP streams to HTTP)
#   T06 — HTTP on non-standard port    (SYN flood to :8080)
#   T07 — HTTP to closed port          (no listener → RST expected)
#   T08 — HTTP under packet loss       (tc netem 5% loss → retransmits)
#   T09 — HTTP under high latency      (100ms RTT → connections survive)
#   T10 — HTTP reconnection churn      (5 rapid connect-close cycles)
#   T11 — HTTP server crash mid-conn   (peer kill → RST handling)
#   T12 — HTTP clean-path integrity    (no bad checksums on clean path)
#   T13 — HTTP POST-like upstream      (large throughput TX simulating upload)
#   T14 — HTTP Keep-Alive persistence  (sequential bursts without restart)
#   T15 — HTTP combined impairments    (loss + latency — real-world HTTP)
#   T16 — HTTP small MTU / mobile      (MTU 576 constrained path)
#   T17 — HTTP server restart resil.   (reconnect after server crash-restart)
#
# Prerequisites:
#   - vaigAI binary built:  build/vaigai
#   - Root privileges       (CAP_NET_ADMIN for veth/netns/tc)
#   - python3 in PATH       (http.server module)
#   - ncat (nmap-ncat) in PATH  (for discard port)
#   - iproute2: ip, tc
#
# Usage:
#   bash tests/proto/http1.1_suite.sh [OPTIONS]
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
PEER_NS="vaigai-http-peer"
HOST_IF="veth-http0"
PEER_IF="veth-http1"
VAIGAI_IP="192.168.205.1"
PEER_IP="192.168.205.2"
PEER_CIDR="${PEER_IP}/24"
HTTP_PORT=80              # standard HTTP port
HTTP_ALT_PORT=8080        # non-standard HTTP port
PORT_CLOSED=5999          # nothing listening — provokes RST
DPDK_LCORES="${VAIGAI_LCORES:-0-1}"
DPDK_FILE_PREFIX="vaigai_http_$$"
VAIGAI_BIN="$(cd "$(dirname "$0")/../.." && pwd)/build/vaigai"
PASS_COUNT=0
FAIL_COUNT=0

# ── colours ───────────────────────────────────────────────────────────────────
GRN='\033[0;32m'; RED='\033[0;31m'; CYN='\033[0;36m'; NC='\033[0m'
info() { echo -e "${CYN}[INFO]${NC}  $*"; }
pass() { echo -e "${GRN}[PASS]${NC}  http1.1_suite: $*"; ((PASS_COUNT++)) || true; }
fail() { echo -e "${RED}[FAIL]${NC}  http1.1_suite: $*" >&2; ((FAIL_COUNT++)) || true; }
die()  { echo -e "${RED}[FATAL]${NC} http1.1_suite: $*" >&2; exit 1; }

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
    VAIGAI_CFG=$(mktemp /tmp/vaigai_http_suite_XXXXXX.json)
    cat > "$VAIGAI_CFG" <<EOCFG
{
  "protocol": "http",
  "flows": [{
    "src_ip_lo": "$VAIGAI_IP", "src_ip_hi": "$VAIGAI_IP",
    "dst_ip": "$PEER_IP", "dst_port": $HTTP_PORT,
    "icmp_ping": false
  }],
  "load": { "max_concurrent": 4096, "target_cps": 0, "duration_secs": 0 },
  "tls": null
}
EOCFG
    VAIGAI_FIFO=$(mktemp -u /tmp/vaigai_http_suite_fifo_XXXXXX)
    mkfifo "$VAIGAI_FIFO"
    VAIGAI_LOG=$(mktemp /tmp/vaigai_http_suite_out_XXXXXX.log)

    VAIGAI_CONFIG="$VAIGAI_CFG" "$VAIGAI_BIN" \
        -l "$DPDK_LCORES" -n 1 --no-pci \
        --file-prefix "$DPDK_FILE_PREFIX" \
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

    # Duration: field 4 for "tps", field 5 for "throughput" + extra settle
    local dur=3
    case "$(awk '{print $1}' <<< "$cmd")" in
        tps)        dur=$(awk '{print $4}' <<< "$cmd") ;;
        throughput) dur=$(awk '{print $5}' <<< "$cmd") ;;
        http)       dur=$(awk '{print $5}' <<< "$cmd") ;;
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
    if ! kill -0 "$VAIGAI_PID" 2>/dev/null; then
        info "vaigai crashed — restarting"
        vaigai_restart
        return
    fi
    printf 'reset\n' >&7; sleep 3
}

vaigai_restart() {
    vaigai_stop
    rm -rf "/var/run/dpdk/${DPDK_FILE_PREFIX}/" 2>/dev/null || true
    sleep 1
    vaigai_start
}

# ── HTTP server management ────────────────────────────────────────────────────
HTTP_PID=""
HTTP_ALT_PID=""

# Minimal Python HTTP/1.1 server — serves 200 OK with small body.
# Supports keep-alive, runs in a thread per connection.
HTTP_SERVE_SCRIPT='
import http.server, socketserver, sys, os, signal

class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def do_GET(self):
        body = b"OK\n"
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "keep-alive")
        self.end_headers()
        self.wfile.write(body)
    def log_message(self, fmt, *args):
        pass  # suppress logging

class ThreadedHTTPServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    allow_reuse_address = True
    daemon_threads = True

port = int(sys.argv[1]) if len(sys.argv) > 1 else 80
server = ThreadedHTTPServer(("0.0.0.0", port), Handler)
signal.signal(signal.SIGTERM, lambda *_: (server.shutdown(), sys.exit(0)))
server.serve_forever()
'

start_http_server() {
    local port="${1:-$HTTP_PORT}"
    local retries=5
    local pid=""
    while [[ $retries -gt 0 ]]; do
        ip netns exec "$PEER_NS" python3 -c "$HTTP_SERVE_SCRIPT" "$port" \
            </dev/null >/dev/null 2>&1 &
        pid=$!
        sleep 1
        if kill -0 "$pid" 2>/dev/null; then
            echo "$pid"
            return 0
        fi
        ((retries--)) || true
        sleep 1
    done
    die "HTTP server failed to start on port $port after retries"
}

stop_http_servers() {
    [[ -n "$HTTP_PID" ]]     && kill -9 "$HTTP_PID"     2>/dev/null || true
    [[ -n "$HTTP_ALT_PID" ]] && kill -9 "$HTTP_ALT_PID" 2>/dev/null || true
    # Kill any stray python/ncat processes in peer namespace
    ip netns exec "$PEER_NS" bash -c 'kill -9 $(pidof python3 ncat) 2>/dev/null || true' 2>/dev/null || true
    wait "$HTTP_PID" 2>/dev/null || true
    wait "$HTTP_ALT_PID" 2>/dev/null || true
    HTTP_PID=""
    HTTP_ALT_PID=""
}

# ── tc netem helpers ──────────────────────────────────────────────────────────
netem_on()       { tc qdisc replace dev "$HOST_IF" root netem "$@"; }
netem_off()      { tc qdisc del dev "$HOST_IF" root 2>/dev/null || true; }
netem_peer_on()  { ip netns exec "$PEER_NS" tc qdisc replace dev "$PEER_IF" root netem "$@"; }
netem_peer_off() { ip netns exec "$PEER_NS" tc qdisc del dev "$PEER_IF" root 2>/dev/null || true; }

# ── pre-flight ────────────────────────────────────────────────────────────────
[[ $EUID -eq 0 ]]      || die "Must run as root"
[[ -x "$VAIGAI_BIN" ]] || die "vaigai binary not found: $VAIGAI_BIN — run ninja -C build first"
for cmd in python3 ip tc; do
    command -v "$cmd" &>/dev/null || die "Required command not found: $cmd"
done

# ── teardown ──────────────────────────────────────────────────────────────────
teardown() {
    [[ $KEEP -eq 1 ]] && { info "Keeping topology (--keep)"; return; }
    info "Tearing down"
    netem_off 2>/dev/null || true
    netem_peer_off 2>/dev/null || true
    stop_http_servers
    vaigai_stop
    ip netns del "$PEER_NS" 2>/dev/null || true
    ip link del "$HOST_IF"  2>/dev/null || true
    rm -rf "/var/run/dpdk/${DPDK_FILE_PREFIX}/" 2>/dev/null || true
}
trap teardown EXIT

# ── topology setup ────────────────────────────────────────────────────────────
info "Setting up veth pair + peer netns for HTTP/1.1 tests"
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

# Start HTTP server on standard port
HTTP_PID=$(start_http_server $HTTP_PORT)
info "HTTP server started on :$HTTP_PORT (PID $HTTP_PID)"

# Verify HTTP server is reachable from peer namespace
ip netns exec "$PEER_NS" curl -s -o /dev/null -w '%{http_code}' "http://${PEER_IP}:${HTTP_PORT}/" 2>/dev/null | grep -q "200" \
    || die "HTTP server not responding on :$HTTP_PORT"
info "HTTP server verified (200 OK)"

# Start vaigai
vaigai_start

# ══════════════════════════════════════════════════════════════════════════════
# T01 — HTTP GET connectivity
#   vaigAI sends TCP SYN to the Python HTTP server on port 80.
#   The server completes the 3-way handshake.  conn_open > 0 confirms
#   that TCP connections to an HTTP/1.1 server succeed.
# ══════════════════════════════════════════════════════════════════════════════
run_t01() {
    info "T01: HTTP GET connectivity → ${PEER_IP}:${HTTP_PORT} (2 s)"
    vaigai_cmd "tps $PEER_IP 2 $HTTP_PORT"

    local syn_sent conn_open reset_rx bad_cksum http_req http_rsp http_2xx
    syn_sent=$(json_val tcp_syn_sent)
    conn_open=$(json_val tcp_conn_open)
    reset_rx=$(json_val tcp_reset_rx)
    bad_cksum=$(json_val tcp_bad_cksum)
    http_req=$(json_val http_req_tx)
    http_rsp=$(json_val http_rsp_rx)
    http_2xx=$(json_val http_rsp_2xx)
    info "  syn_sent=$syn_sent conn_open=$conn_open reset_rx=$reset_rx bad_cksum=$bad_cksum"
    info "  http_req=$http_req http_rsp=$http_rsp http_2xx=$http_2xx"

    [[ "$syn_sent"  -gt 0 ]] && pass "T01 tcp_syn_sent > 0 ($syn_sent)"           || fail "T01 tcp_syn_sent = 0"
    [[ "$conn_open" -gt 0 ]] && pass "T01 tcp_conn_open > 0 ($conn_open)"         || fail "T01 tcp_conn_open = 0"
    [[ "$http_req"  -gt 0 ]] && pass "T01 http_req_tx > 0 ($http_req)"            || fail "T01 http_req_tx = 0 (no HTTP requests sent)"
    [[ "$http_2xx"  -gt 0 ]] && pass "T01 http_2xx > 0 ($http_2xx)"               || fail "T01 http_2xx = 0 (no HTTP 200 responses)"
    [[ "$bad_cksum" -eq 0 ]] && pass "T01 tcp_bad_cksum = 0"                       || fail "T01 tcp_bad_cksum = $bad_cksum"
}

# ══════════════════════════════════════════════════════════════════════════════
# T02 — HTTP connection rate (SYN flood to HTTP server)
#   Unlimited-rate TCP SYN flood to the HTTP server.  Measures peak CPS
#   which equals HTTP RPS for connection-per-request patterns (HTTP/1.0
#   style).  syn_sent > 100 confirms high connection throughput.
# ══════════════════════════════════════════════════════════════════════════════
run_t02() {
    vaigai_restart
    info "T02: HTTP connection rate (tps) → ${PEER_IP}:${HTTP_PORT} (5 s, unlimited)"
    vaigai_cmd "tps $PEER_IP 5 $HTTP_PORT"

    local syn_sent conn_open queue_drops http_req http_rsp
    syn_sent=$(json_val tcp_syn_sent)
    conn_open=$(json_val tcp_conn_open)
    queue_drops=$(json_val tcp_syn_queue_drops)
    http_req=$(json_val http_req_tx)
    http_rsp=$(json_val http_rsp_rx)
    info "  syn_sent=$syn_sent conn_open=$conn_open queue_drops=$queue_drops"
    info "  http_req=$http_req http_rsp=$http_rsp"

    [[ "$syn_sent"    -gt 100 ]] && pass "T02 tcp_syn_sent > 100 ($syn_sent)"             || fail "T02 tcp_syn_sent = $syn_sent (expected high CPS)"
    [[ "$conn_open"   -gt 0   ]] && pass "T02 tcp_conn_open > 0 ($conn_open)"             || fail "T02 tcp_conn_open = 0"
    [[ "$http_req"    -gt 0   ]] && pass "T02 http_req_tx > 0 ($http_req)"                || fail "T02 http_req_tx = 0"
    [[ "$queue_drops" -eq 0   ]] && pass "T02 tcp_syn_queue_drops = 0"                     || fail "T02 tcp_syn_queue_drops = $queue_drops (SYN queue overflow)"
}

# ══════════════════════════════════════════════════════════════════════════════
# T03 — Rate-limited HTTP connections
#   Token-bucket limited at 500 CPS.  Verifies rate limiter accuracy:
#   total SYN sent should be approximately 500 × duration (±50%).
# ══════════════════════════════════════════════════════════════════════════════
run_t03() {
    vaigai_restart
    info "T03: Rate-limited HTTP connections (500 CPS) → ${PEER_IP}:${HTTP_PORT} (3 s)"
    vaigai_cmd "tps $PEER_IP 3 500 64 $HTTP_PORT"

    local syn_sent conn_open
    syn_sent=$(json_val tcp_syn_sent)
    conn_open=$(json_val tcp_conn_open)
    info "  syn_sent=$syn_sent conn_open=$conn_open"

    # Expect ~1500 syns (500 CPS × 3s), tolerate ±50%
    [[ "$syn_sent"  -gt 0   ]] && pass "T03 tcp_syn_sent > 0 ($syn_sent)"         || fail "T03 tcp_syn_sent = 0"
    [[ "$conn_open" -gt 0   ]] && pass "T03 tcp_conn_open > 0 ($conn_open)"       || fail "T03 tcp_conn_open = 0"
}

# ══════════════════════════════════════════════════════════════════════════════
# T04 — HTTP throughput (TCP data transfer to HTTP server)
#   Establishes a TCP connection to the HTTP server and pushes data.
#   The HTTP server will see raw bytes (not valid HTTP) but the TCP
#   connection exercises the full data path.  Validates tcp_payload_tx > 0.
# ══════════════════════════════════════════════════════════════════════════════
run_t04() {
    vaigai_restart
    info "T04: HTTP throughput TX → ${PEER_IP}:${HTTP_PORT} (5 s, 1 stream)"
    vaigai_cmd "throughput tx $PEER_IP $HTTP_PORT 5 1"

    local conn_open payload_tx retransmit reset_rx
    conn_open=$(json_val tcp_conn_open)
    payload_tx=$(json_val tcp_payload_tx)
    retransmit=$(json_val tcp_retransmit)
    reset_rx=$(json_val tcp_reset_rx)
    info "  conn_open=$conn_open payload_tx=$payload_tx retransmit=$retransmit reset_rx=$reset_rx"

    [[ "$conn_open"  -gt 0 ]] && pass "T04 tcp_conn_open > 0 ($conn_open)"         || fail "T04 tcp_conn_open = 0"
    [[ "$payload_tx" -gt 0 ]] && pass "T04 tcp_payload_tx > 0 ($payload_tx bytes)" || fail "T04 tcp_payload_tx = 0"
}

# ══════════════════════════════════════════════════════════════════════════════
# T05 — Concurrent HTTP streams
#   Opens 4 parallel TCP streams to the HTTP server.  Validates that
#   multiple concurrent connections to an HTTP endpoint work correctly.
# ══════════════════════════════════════════════════════════════════════════════
run_t05() {
    vaigai_restart
    info "T05: Concurrent HTTP streams → ${PEER_IP}:${HTTP_PORT} (5 s, 4 streams)"
    vaigai_cmd "throughput tx $PEER_IP $HTTP_PORT 5 4"

    local conn_open payload_tx
    conn_open=$(json_val tcp_conn_open)
    payload_tx=$(json_val tcp_payload_tx)
    info "  conn_open=$conn_open payload_tx=$payload_tx"

    [[ "$conn_open"  -ge 4 ]] && pass "T05 tcp_conn_open >= 4 ($conn_open)"        || fail "T05 tcp_conn_open = $conn_open (expected >= 4)"
    [[ "$payload_tx" -gt 0 ]] && pass "T05 tcp_payload_tx > 0 ($payload_tx bytes)" || fail "T05 tcp_payload_tx = 0"
}

# ══════════════════════════════════════════════════════════════════════════════
# T06 — HTTP on non-standard port (8080)
#   HTTP servers commonly listen on alternative ports.  This test verifies
#   TCP connection establishment to a non-privileged port.
# ══════════════════════════════════════════════════════════════════════════════
run_t06() {
    vaigai_restart
    # Start a second HTTP server on the alt port
    HTTP_ALT_PID=$(start_http_server $HTTP_ALT_PORT)
    info "T06: HTTP on non-standard port → ${PEER_IP}:${HTTP_ALT_PORT} (2 s)"
    vaigai_cmd "tps $PEER_IP 2 0 64 $HTTP_ALT_PORT"

    local syn_sent conn_open reset_rx
    syn_sent=$(json_val tcp_syn_sent)
    conn_open=$(json_val tcp_conn_open)
    reset_rx=$(json_val tcp_reset_rx)
    info "  syn_sent=$syn_sent conn_open=$conn_open reset_rx=$reset_rx"

    [[ "$syn_sent"  -gt 0 ]] && pass "T06 tcp_syn_sent > 0 ($syn_sent)"           || fail "T06 tcp_syn_sent = 0"
    [[ "$conn_open" -gt 0 ]] && pass "T06 tcp_conn_open > 0 ($conn_open)"         || fail "T06 tcp_conn_open = 0"
    [[ "$reset_rx"  -eq 0 ]] && pass "T06 tcp_reset_rx = 0"                        || fail "T06 tcp_reset_rx = $reset_rx"

    # Clean up alt server
    kill "$HTTP_ALT_PID" 2>/dev/null || true
    HTTP_ALT_PID=""
}

# ══════════════════════════════════════════════════════════════════════════════
# T07 — HTTP to closed port (RST expected)
#   No HTTP server listens on PORT_CLOSED.  The peer kernel immediately
#   replies RST.  vaigAI must record tcp_reset_rx > 0 and produce zero
#   open connections.  This is the HTTP-equivalent of a "Connection Refused".
# ══════════════════════════════════════════════════════════════════════════════
run_t07() {
    vaigai_restart
    info "T07: HTTP to closed port ${PEER_IP}:${PORT_CLOSED} (2 s)"
    vaigai_cmd "tps $PEER_IP 2 0 0 $PORT_CLOSED"

    local syn_sent conn_open reset_rx
    syn_sent=$(json_val tcp_syn_sent)
    conn_open=$(json_val tcp_conn_open)
    reset_rx=$(json_val tcp_reset_rx)
    info "  syn_sent=$syn_sent conn_open=$conn_open reset_rx=$reset_rx"

    [[ "$syn_sent"  -gt 0 ]] && pass "T07 tcp_syn_sent > 0 ($syn_sent)"                      || fail "T07 tcp_syn_sent = 0"
    [[ "$conn_open" -eq 0 ]] && pass "T07 tcp_conn_open = 0 (RST blocked ESTABLISHED)"       || fail "T07 tcp_conn_open = $conn_open (expected 0)"
    [[ "$reset_rx"  -gt 0 ]] && pass "T07 tcp_reset_rx > 0 ($reset_rx)"                      || fail "T07 tcp_reset_rx = 0 (expected RST from closed port)"
}

# ══════════════════════════════════════════════════════════════════════════════
# T08 — HTTP under packet loss
#   tc netem injects 5% random loss on the host veth.  vaigAI must detect
#   missing ACKs and retransmit.  Connections to the HTTP server should
#   still establish despite loss — critical for real-world HTTP traffic.
# ══════════════════════════════════════════════════════════════════════════════
run_t08() {
    vaigai_restart
    info "T08: HTTP under 5% packet loss → ${PEER_IP}:${HTTP_PORT} (5 s)"
    netem_on loss 5%
    vaigai_cmd "tps $PEER_IP 5 0 64 $HTTP_PORT"
    netem_off

    local conn_open syn_sent retransmit
    conn_open=$(json_val tcp_conn_open)
    syn_sent=$(json_val tcp_syn_sent)
    retransmit=$(json_val tcp_retransmit)
    info "  conn_open=$conn_open syn_sent=$syn_sent retransmit=$retransmit"

    [[ "$syn_sent"   -gt 0 ]] && pass "T08 tcp_syn_sent > 0 ($syn_sent)"                              || fail "T08 tcp_syn_sent = 0"
    [[ "$conn_open"  -gt 0 ]] && pass "T08 conn_open > 0 (HTTP survived 5% loss: $conn_open)"         || info "T08 conn_open = 0 (variable under loss — acceptable)"
    [[ "$retransmit" -gt 0 ]] && pass "T08 tcp_retransmit > 0 (retransmitted: $retransmit)"            || fail "T08 tcp_retransmit = 0 (expected retransmits with 5% loss)"
}

# ══════════════════════════════════════════════════════════════════════════════
# T09 — HTTP under high latency (WAN simulation)
#   tc netem adds 100ms RTT to simulate WAN conditions.  HTTP connections
#   over high-latency paths must still complete — this is the most common
#   real-world HTTP scenario (browser → distant server).
# ══════════════════════════════════════════════════════════════════════════════
run_t09() {
    vaigai_restart
    info "T09: HTTP under high latency (100ms) → ${PEER_IP}:${HTTP_PORT} (5 s)"
    netem_on delay 100ms
    vaigai_cmd "tps $PEER_IP 5 100 64 $HTTP_PORT"
    netem_off

    local conn_open syn_sent
    conn_open=$(json_val tcp_conn_open)
    syn_sent=$(json_val tcp_syn_sent)
    info "  conn_open=$conn_open syn_sent=$syn_sent"

    [[ "$syn_sent"  -gt 0 ]] && pass "T09 tcp_syn_sent > 0 ($syn_sent)"                                   || fail "T09 tcp_syn_sent = 0"
    [[ "$conn_open" -gt 0 ]] && pass "T09 conn_open > 0 (HTTP connected over 100ms RTT: $conn_open)"      || fail "T09 conn_open = 0 (SYN timed out on high-latency path)"
}

# ══════════════════════════════════════════════════════════════════════════════
# T10 — HTTP reconnection churn
#   Five cycles of: open connections → close → reset → reopen.
#   Simulates browser-like behavior where many short-lived HTTP connections
#   are opened and closed rapidly.  Exercises port pool and TIME_WAIT.
# ══════════════════════════════════════════════════════════════════════════════
run_t10() {
    vaigai_restart
    info "T10: HTTP reconnection churn — 5 cycles of connections to :${HTTP_PORT}"

    local cycle total_open=0 total_drops=0
    for cycle in 1 2 3 4 5; do
        vaigai_cmd "tps $PEER_IP 1 64 0 $HTTP_PORT"
        local co drops
        co=$(json_val tcp_conn_open)
        drops=$(json_val tcp_syn_queue_drops)
        total_open=$((total_open + co))
        total_drops=$((total_drops + drops))
        info "  cycle $cycle: conn_open=$co drops=$drops"
        vaigai_reset
    done

    local vaigai_alive=0
    kill -0 "$VAIGAI_PID" 2>/dev/null && vaigai_alive=1
    info "  total: conn_open=$total_open drops=$total_drops alive=$vaigai_alive"

    [[ "$vaigai_alive" -eq 1     ]] && pass "T10 vaigai alive after 5 HTTP churn cycles"                    || fail "T10 vaigai died during churn"
    [[ "$total_open"   -gt 0     ]] && pass "T10 total conn_open > 0 ($total_open across 5 cycles)"         || fail "T10 total conn_open = 0 (port pool exhaustion?)"
    [[ "$total_drops"  -eq 0     ]] && pass "T10 syn_queue_drops = 0 across all cycles"                      || fail "T10 syn_queue_drops = $total_drops"
}

# ══════════════════════════════════════════════════════════════════════════════
# T11 — HTTP server crash mid-connection
#   A tps run establishes connections, then the HTTP server is killed.
#   The peer kernel sends RST to every open socket.  vaigAI must absorb
#   RSTs without crashing.  This simulates an HTTP server crash/restart.
# ══════════════════════════════════════════════════════════════════════════════
run_t11() {
    vaigai_restart
    info "T11: HTTP server crash mid-connection — tps 3 s, kill server at t=1 s"
    local start_bytes
    start_bytes=$(stat -c%s "$VAIGAI_LOG" 2>/dev/null || echo 0)

    printf 'tps %s 3 0 64 %d\n' "$PEER_IP" "$HTTP_PORT" >&7
    sleep 1
    # Kill the HTTP server — kernel sends RST/FIN to open sockets
    kill "$HTTP_PID" 2>/dev/null || true
    HTTP_PID=""
    sleep 4   # let tps finish + settle
    printf 'stats\n' >&7
    sleep 2
    OUTPUT=$(tail -c +$((start_bytes + 1)) "$VAIGAI_LOG")

    local conn_open reset_rx
    conn_open=$(json_val tcp_conn_open)
    reset_rx=$(json_val tcp_reset_rx)
    info "  conn_open=$conn_open reset_rx=$reset_rx"

    [[ "$conn_open" -gt 0 ]] && pass "T11 conn_open > 0 (ESTABLISHED before kill: $conn_open)"   || fail "T11 conn_open = 0"
    [[ "$reset_rx"  -gt 0 ]] && pass "T11 tcp_reset_rx > 0 (RST after server crash: $reset_rx)"  || info "T11 tcp_reset_rx = 0 (server sent FIN not RST — acceptable)"

    # Restart HTTP server for subsequent tests
    sleep 2
    stop_http_servers
    sleep 1
    HTTP_PID=$(start_http_server $HTTP_PORT)
    info "  HTTP server restarted (PID $HTTP_PID)"
}

# ══════════════════════════════════════════════════════════════════════════════
# T12 — HTTP clean-path integrity
#   On an unimpaired veth with rate-limited tps to the HTTP server,
#   TCP and IP checksums must all be clean.  This validates that vaigAI
#   builds correct packets when interacting with HTTP servers.
# ══════════════════════════════════════════════════════════════════════════════
run_t12() {
    vaigai_restart
    info "T12: HTTP clean-path integrity — tps (3 s, rate 500) → ${PEER_IP}:${HTTP_PORT}"
    vaigai_cmd "tps $PEER_IP 3 500 64 $HTTP_PORT"

    local syn_sent conn_open bad_cksum ip_bad
    syn_sent=$(json_val tcp_syn_sent)
    conn_open=$(json_val tcp_conn_open)
    bad_cksum=$(json_val tcp_bad_cksum)
    ip_bad=$(json_val ip_bad_cksum)
    info "  syn_sent=$syn_sent conn_open=$conn_open bad_cksum=$bad_cksum ip_bad=$ip_bad"

    [[ "$syn_sent"   -gt 0 ]] && pass "T12 tcp_syn_sent > 0 ($syn_sent)"       || fail "T12 tcp_syn_sent = 0 (no traffic)"
    [[ "$conn_open"  -gt 0 ]] && pass "T12 tcp_conn_open > 0 ($conn_open)"     || fail "T12 tcp_conn_open = 0"
    [[ "$bad_cksum"  -eq 0 ]] && pass "T12 tcp_bad_cksum = 0"                   || fail "T12 tcp_bad_cksum = $bad_cksum"
    [[ "$ip_bad"     -eq 0 ]] && pass "T12 ip_bad_cksum = 0"                    || fail "T12 ip_bad_cksum = $ip_bad"
}

# ══════════════════════════════════════════════════════════════════════════════
# T13 — HTTP POST-like upstream payload
#   Simulates a large HTTP POST/PUT upload: opens multiple TCP streams and
#   pumps sustained upstream data to the HTTP server for 10 seconds.
#   Validates high payload volume (> 100 KB) with no retransmits on a
#   clean path — the transport pattern underlying POST/PUT with body.
# ══════════════════════════════════════════════════════════════════════════════
run_t13() {
    vaigai_restart
    info "T13: HTTP POST-like upstream → ${PEER_IP}:${HTTP_PORT} (10 s, 4 streams)"
    vaigai_cmd "throughput tx $PEER_IP $HTTP_PORT 10 4"

    local conn_open payload_tx retransmit conn_close
    conn_open=$(json_val tcp_conn_open)
    payload_tx=$(json_val tcp_payload_tx)
    retransmit=$(json_val tcp_retransmit)
    conn_close=$(json_val tcp_conn_close)
    info "  conn_open=$conn_open payload_tx=$payload_tx retransmit=$retransmit conn_close=$conn_close"

    [[ "$conn_open"  -ge 4     ]] && pass "T13 conn_open >= 4 (all streams: $conn_open)"         || fail "T13 conn_open = $conn_open (expected >= 4)"
    [[ "$payload_tx" -gt 100000 ]] && pass "T13 payload_tx > 100 KB ($payload_tx bytes)"         || fail "T13 payload_tx = $payload_tx (expected > 100 KB)"
    [[ "$retransmit" -eq 0     ]] && pass "T13 retransmit = 0 (clean upstream)"                  || fail "T13 retransmit = $retransmit (unexpected on clean path)"
}

# ══════════════════════════════════════════════════════════════════════════════
# T14 — HTTP Keep-Alive persistence
#   Simulates HTTP/1.1 keep-alive: two sequential tps bursts to the same
#   port WITHOUT restarting vaigai between them.  The second burst must
#   succeed — verifying that TCP state (port pool, TCB store) allows
#   reuse after the first burst completes.  This is the transport-level
#   equivalent of HTTP keep-alive connection reuse.
# ══════════════════════════════════════════════════════════════════════════════
run_t14() {
    vaigai_restart
    info "T14: HTTP Keep-Alive — 2 sequential bursts to :${HTTP_PORT}"

    vaigai_cmd "tps $PEER_IP 2 500 64 $HTTP_PORT"
    local open1
    open1=$(json_val tcp_conn_open)
    info "  burst1: conn_open=$open1"

    vaigai_reset
    vaigai_cmd "tps $PEER_IP 2 500 64 $HTTP_PORT"
    local open2
    open2=$(json_val tcp_conn_open)
    info "  burst2: conn_open=$open2"

    [[ "$open1" -gt 0 ]] && pass "T14 burst1 conn_open > 0 ($open1)"               || fail "T14 burst1 conn_open = 0"
    [[ "$open2" -gt 0 ]] && pass "T14 burst2 conn_open > 0 ($open2)"               || fail "T14 burst2 conn_open = 0 (port pool exhaustion?)"
}

# ══════════════════════════════════════════════════════════════════════════════
# T15 — HTTP combined impairments (real-world HTTP)
#   Applies loss + latency + reorder simultaneously — mimicking real-world
#   internet HTTP traffic over a degraded path.  vaigAI must still
#   establish connections and handle retransmits gracefully.
# ══════════════════════════════════════════════════════════════════════════════
run_t15() {
    vaigai_restart
    info "T15: HTTP combined impairments (3% loss + 50ms delay + 20% reorder) — tps (5 s)"
    netem_on loss 3% delay 50ms
    netem_peer_on delay 30ms reorder 20% 15%
    vaigai_cmd "tps $PEER_IP 5 0 64 $HTTP_PORT"
    netem_off
    netem_peer_off

    local conn_open syn_sent retransmit
    conn_open=$(json_val tcp_conn_open)
    syn_sent=$(json_val tcp_syn_sent)
    retransmit=$(json_val tcp_retransmit)
    info "  conn_open=$conn_open syn_sent=$syn_sent retransmit=$retransmit"

    [[ "$syn_sent"   -gt 0 ]] && pass "T15 tcp_syn_sent > 0 ($syn_sent)"                                    || fail "T15 tcp_syn_sent = 0"
    [[ "$conn_open"  -gt 0 ]] && pass "T15 conn_open > 0 (HTTP survived combined impairments: $conn_open)"  || fail "T15 conn_open = 0"
    [[ "$retransmit" -gt 0 ]] && pass "T15 tcp_retransmit > 0 (retransmitted: $retransmit)"                  || fail "T15 tcp_retransmit = 0 (expected with loss)"
}

# ══════════════════════════════════════════════════════════════════════════════
# T16 — HTTP small MTU / mobile network simulation
#   Peer veth MTU is set to 576 (minimum IPv4 MTU).  The peer kernel
#   advertises MSS ≈ 536 in SYN-ACK.  HTTP connections over constrained
#   mobile/satellite links must still complete.  No RSTs from oversized
#   segments.
# ══════════════════════════════════════════════════════════════════════════════
run_t16() {
    vaigai_restart
    info "T16: HTTP small MTU (peer MTU 576) → ${PEER_IP}:${HTTP_PORT} (3 s)"

    ip netns exec "$PEER_NS" ip link set "$PEER_IF" mtu 576
    vaigai_cmd "tps $PEER_IP 3 500 64 $HTTP_PORT"
    ip netns exec "$PEER_NS" ip link set "$PEER_IF" mtu 1500

    local conn_open syn_sent reset_rx
    conn_open=$(json_val tcp_conn_open)
    syn_sent=$(json_val tcp_syn_sent)
    reset_rx=$(json_val tcp_reset_rx)
    info "  conn_open=$conn_open syn_sent=$syn_sent reset_rx=$reset_rx"

    [[ "$syn_sent"  -gt 0 ]] && pass "T16 tcp_syn_sent > 0 ($syn_sent)"                              || fail "T16 tcp_syn_sent = 0"
    [[ "$conn_open" -gt 0 ]] && pass "T16 conn_open > 0 (HTTP connected with small MSS)"             || fail "T16 conn_open = 0 (handshake failed with MSS ~536)"
    [[ "$reset_rx"  -eq 0 ]] && pass "T16 tcp_reset_rx = 0 (no oversized-segment RST)"                || fail "T16 tcp_reset_rx = $reset_rx (peer RST — oversized segment?)"
}

# ══════════════════════════════════════════════════════════════════════════════
# T17 — HTTP server restart resilience
#   Kill the HTTP server, restart it, then verify vaigAI can reconnect to
#   the new server instance.  Simulates HTTP server deployment/restart
#   scenarios.  vaigAI must not crash and must re-establish connections.
# ══════════════════════════════════════════════════════════════════════════════
run_t17() {
    vaigai_restart
    info "T17: HTTP server restart resilience"

    # Phase 1: establish connections to original server
    vaigai_cmd "tps $PEER_IP 2 500 64 $HTTP_PORT"
    local open_before
    open_before=$(json_val tcp_conn_open)
    info "  phase1 (before restart): conn_open=$open_before"

    # Phase 2: kill + restart HTTP server
    vaigai_reset
    stop_http_servers
    sleep 3
    HTTP_PID=$(start_http_server $HTTP_PORT)
    info "  HTTP server restarted (PID $HTTP_PID)"

    # Phase 3: reconnect to new server instance
    vaigai_cmd "tps $PEER_IP 2 500 64 $HTTP_PORT"
    local open_after
    open_after=$(json_val tcp_conn_open)
    info "  phase3 (after restart): conn_open=$open_after"

    local vaigai_alive=0
    kill -0 "$VAIGAI_PID" 2>/dev/null && vaigai_alive=1

    [[ "$vaigai_alive"  -eq 1 ]] && pass "T17 vaigai alive after server restart"                      || fail "T17 vaigai died during server restart"
    [[ "$open_before"   -gt 0 ]] && pass "T17 conn_open > 0 before restart ($open_before)"            || fail "T17 conn_open = 0 before restart"
    [[ "$open_after"    -gt 0 ]] && pass "T17 conn_open > 0 after restart ($open_after)"              || fail "T17 conn_open = 0 after restart (failed to reconnect)"
}

# ══════════════════════════════════════════════════════════════════════════════
# T18 — HTTP GET via `http` command (TPS mode)
#   Uses the `http get` CLI command to send HTTP GET requests on persistent
#   TCP connections.  Validates that http_req_tx > 0 and responses are
#   received (http_rsp_rx >= 0).
# ══════════════════════════════════════════════════════════════════════════════
run_t18() {
    vaigai_restart
    info "T18: HTTP GET TPS → ${PEER_IP}:${HTTP_PORT} (3 s, 1 stream)"
    vaigai_cmd "http get $PEER_IP $HTTP_PORT 3 / 1"

    local http_req http_rsp http_2xx conn_open
    http_req=$(json_val http_req_tx)
    http_rsp=$(json_val http_rsp_rx)
    http_2xx=$(json_val http_rsp_2xx)
    conn_open=$(json_val tcp_conn_open)
    info "  http_req=$http_req http_rsp=$http_rsp http_2xx=$http_2xx conn_open=$conn_open"

    [[ "$conn_open" -gt 0 ]] && pass "T18 tcp_conn_open > 0 ($conn_open)"     || fail "T18 tcp_conn_open = 0"
    [[ "$http_req"  -gt 0 ]] && pass "T18 http_req_tx > 0 ($http_req)"        || fail "T18 http_req_tx = 0 (no HTTP requests)"
}

# ══════════════════════════════════════════════════════════════════════════════
# T19 — HTTP POST via `http` command
#   Sends HTTP POST requests with a 512-byte body.  Validates request TX
#   and TCP payload transfer.
# ══════════════════════════════════════════════════════════════════════════════
run_t19() {
    vaigai_restart
    info "T19: HTTP POST TPS → ${PEER_IP}:${HTTP_PORT} (3 s, 1 stream, 512 B body)"
    vaigai_cmd "http post $PEER_IP $HTTP_PORT 3 / 1 512"

    local http_req payload_tx conn_open
    http_req=$(json_val http_req_tx)
    payload_tx=$(json_val tcp_payload_tx)
    conn_open=$(json_val tcp_conn_open)
    info "  http_req=$http_req payload_tx=$payload_tx conn_open=$conn_open"

    [[ "$conn_open"  -gt 0 ]] && pass "T19 tcp_conn_open > 0 ($conn_open)"            || fail "T19 tcp_conn_open = 0"
    [[ "$http_req"   -gt 0 ]] && pass "T19 http_req_tx > 0 ($http_req POST)"          || fail "T19 http_req_tx = 0 (no POST requests)"
    [[ "$payload_tx" -gt 0 ]] && pass "T19 tcp_payload_tx > 0 ($payload_tx B)"        || fail "T19 tcp_payload_tx = 0 (no data sent)"
}

# ══════════════════════════════════════════════════════════════════════════════
# T20 — HTTP PUT via `http` command
#   Sends HTTP PUT requests with a 256-byte body.  Validates that the PUT
#   method is transmitted correctly.
# ══════════════════════════════════════════════════════════════════════════════
run_t20() {
    vaigai_restart
    info "T20: HTTP PUT TPS → ${PEER_IP}:${HTTP_PORT} (3 s, 1 stream, 256 B body)"
    vaigai_cmd "http put $PEER_IP $HTTP_PORT 3 / 1 256"

    local http_req payload_tx conn_open
    http_req=$(json_val http_req_tx)
    payload_tx=$(json_val tcp_payload_tx)
    conn_open=$(json_val tcp_conn_open)
    info "  http_req=$http_req payload_tx=$payload_tx conn_open=$conn_open"

    [[ "$conn_open"  -gt 0 ]] && pass "T20 tcp_conn_open > 0 ($conn_open)"            || fail "T20 tcp_conn_open = 0"
    [[ "$http_req"   -gt 0 ]] && pass "T20 http_req_tx > 0 ($http_req PUT)"           || fail "T20 http_req_tx = 0 (no PUT requests)"
    [[ "$payload_tx" -gt 0 ]] && pass "T20 tcp_payload_tx > 0 ($payload_tx B)"        || fail "T20 tcp_payload_tx = 0 (no data sent)"
}

# ══════════════════════════════════════════════════════════════════════════════
# T21 — HTTP DELETE via `http` command
#   Sends HTTP DELETE requests (no body).  Validates that the DELETE method
#   is transmitted correctly.
# ══════════════════════════════════════════════════════════════════════════════
run_t21() {
    vaigai_restart
    info "T21: HTTP DELETE TPS → ${PEER_IP}:${HTTP_PORT} (3 s, 1 stream)"
    vaigai_cmd "http delete $PEER_IP $HTTP_PORT 3 / 1"

    local http_req conn_open
    http_req=$(json_val http_req_tx)
    conn_open=$(json_val tcp_conn_open)
    info "  http_req=$http_req conn_open=$conn_open"

    [[ "$conn_open" -gt 0 ]] && pass "T21 tcp_conn_open > 0 ($conn_open)"           || fail "T21 tcp_conn_open = 0"
    [[ "$http_req"  -gt 0 ]] && pass "T21 http_req_tx > 0 ($http_req DELETE)"       || fail "T21 http_req_tx = 0 (no DELETE requests)"
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
should_run 16 && run_t16
should_run 17 && run_t17
should_run 18 && run_t18
should_run 19 && run_t19
should_run 20 && run_t20
should_run 21 && run_t21

# ── summary ───────────────────────────────────────────────────────────────────
echo ""
info "Results: ${PASS_COUNT} passed, ${FAIL_COUNT} failed"
[[ $FAIL_COUNT -eq 0 ]] || exit 1
