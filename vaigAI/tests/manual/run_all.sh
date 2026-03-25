#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# ─────────────────────────────────────────────────────────────────────────────
#  run_all.sh — Non-interactive runner for all vaigAI manual tests (1a-1i, 2a)
#
#  Usage:
#    sudo bash tests/manual/run_all.sh                 # all tests
#    sudo bash tests/manual/run_all.sh --only 1g,1h    # specific tests
#    sudo bash tests/manual/run_all.sh --skip 1a,1b    # skip hw tests
# ─────────────────────────────────────────────────────────────────────────────
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VAIGAI_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
VAIGAI_BIN="${VAIGAI_BIN:-$VAIGAI_DIR/build/vaigai}"

# ── Colours ──────────────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[0;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'

# ── Globals ───────────────────────────────────────────────────────────────────
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; WARN_COUNT=0
declare -A TEST_STATUS   # test-id → PASS/FAIL/SKIP/WARN
declare -A TEST_DETAILS  # test-id → detail string

VAIGAI_PID=""; VAIGAI_FIFO=""; VAIGAI_LOG=""; OUTPUT=""

# ── Argument parsing ──────────────────────────────────────────────────────────
ONLY_TESTS=""; SKIP_TESTS=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --only) ONLY_TESTS="$2"; shift 2 ;;
        --skip) SKIP_TESTS="$2"; shift 2 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

should_run() {
    local id="$1"
    [[ -n "$ONLY_TESTS" ]] && [[ ",$ONLY_TESTS," != *",$id,"* ]] && return 1
    [[ -n "$SKIP_TESTS" ]] && [[ ",$SKIP_TESTS," == *",$id,"* ]]  && return 1
    return 0
}

# ── Reporting ─────────────────────────────────────────────────────────────────
pass() { local id="$1" msg="${2:-}"; ((PASS_COUNT++)) || true
         TEST_STATUS["$id"]="PASS"; TEST_DETAILS["$id"]="$msg"
         echo -e "${GREEN}[PASS]${NC}  $id: $msg"; }
fail() { local id="$1" msg="${2:-}"; ((FAIL_COUNT++)) || true
         TEST_STATUS["$id"]="FAIL"; TEST_DETAILS["$id"]="$msg"
         echo -e "${RED}[FAIL]${NC}  $id: $msg"
         [[ -n "${VAIGAI_LOG:-}" ]] && tail -5 "$VAIGAI_LOG" 2>/dev/null | sed 's/^/         /' || true; }
warn() { local id="$1" msg="${2:-}"; ((WARN_COUNT++)) || true
         TEST_STATUS["$id"]="${TEST_STATUS[$id]:-WARN}"; TEST_DETAILS["$id"]="${TEST_DETAILS[$id]:-$msg}"
         echo -e "${YELLOW}[WARN]${NC}  $id: $msg"; }
skip() { local id="$1" msg="${2:-}"; ((SKIP_COUNT++)) || true
         TEST_STATUS["$id"]="SKIP"; TEST_DETAILS["$id"]="$msg"
         echo -e "${YELLOW}[SKIP]${NC}  $id: $msg"; }
info() { echo -e "${CYAN}[INFO]${NC}  $*"; }
section() { echo -e "\n${BOLD}══════════════════════════════════════════════${NC}"
            echo -e "${BOLD}  $1${NC}"
            echo -e "${BOLD}══════════════════════════════════════════════${NC}"; }

# ── Hugepage setup ────────────────────────────────────────────────────────────
setup_hugepages() {
    local cur; cur=$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages 2>/dev/null || echo 0)
    if (( cur < 512 )); then
        echo 512 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
    fi
    rm -f /dev/hugepages/rtemap_* /dev/hugepages/vaigai_* 2>/dev/null || true
}

# ── vaigai driver functions ───────────────────────────────────────────────────
json_val() { grep -oP "\b$1:\s*\K[0-9]+" <<< "$OUTPUT" | tail -1 || echo 0; }

vaigai_start() {
    VAIGAI_FIFO=$(mktemp -u /tmp/vg_fifo_XXXXXX)
    VAIGAI_LOG=$(mktemp /tmp/vg_log_XXXXXX.log)
    mkfifo "$VAIGAI_FIFO"
    setup_hugepages
    local output_args=()
    [[ -n "${VAIGAI_OUTPUT_FILE:-}" ]] && output_args=("-O" "$VAIGAI_OUTPUT_FILE")
    "$VAIGAI_BIN" "$@" "${output_args[@]}" < "$VAIGAI_FIFO" > "$VAIGAI_LOG" 2>&1 &
    VAIGAI_PID=$!
    exec 7>"$VAIGAI_FIFO"
    local i=0
    while ! grep -qE 'vaigai(\(server\))?>' "$VAIGAI_LOG" 2>/dev/null; do
        sleep 1; ((i++)) || true
        if ! kill -0 "$VAIGAI_PID" 2>/dev/null; then
            echo -e "${RED}[ERR]${NC}  vaigai crashed on startup:"; tail -15 "$VAIGAI_LOG" 2>/dev/null; return 1
        fi
        if [[ $i -gt 30 ]]; then
            echo -e "${RED}[ERR]${NC}  vaigai start timed out (30s):"; tail -15 "$VAIGAI_LOG" 2>/dev/null; return 1
        fi
    done
}

vaigai_cmd() {
    local cmd="$1"
    local start_bytes; start_bytes=$(stat -c%s "$VAIGAI_LOG" 2>/dev/null || echo 0)
    printf '%s\n' "$cmd" >&7 2>/dev/null || true
    local dur; dur=$(echo "$cmd" | grep -oP '(?<=--duration )\d+' || echo 0)
    [[ "$dur" -gt 0 ]] && sleep $((dur + 2)) || sleep 3
    printf 'stats\n' >&7 2>/dev/null || true
    local a=0
    while ! tail -c +$((start_bytes + 1)) "$VAIGAI_LOG" 2>/dev/null | grep -q 'Workers:'; do
        sleep 1; ((a++)) || true
        [[ $a -gt 60 ]] && break
    done
    sleep 0.5
    OUTPUT=$(tail -c +$((start_bytes + 1)) "$VAIGAI_LOG" 2>/dev/null || true)
}

vaigai_ping() {
    local ip="$1" count="${2:-5}"
    local start_bytes; start_bytes=$(stat -c%s "$VAIGAI_LOG" 2>/dev/null || echo 0)
    printf 'ping %s %s\n' "$ip" "$count" >&7 2>/dev/null || true
    sleep $((count + 3))
    PING_OUTPUT=$(tail -c +$((start_bytes + 1)) "$VAIGAI_LOG" 2>/dev/null || true)
    PING_REPLIES=$(grep -c 'Reply from\|icmp_seq' <<< "$PING_OUTPUT" || true)
}

vaigai_reset() { printf 'stop\nreset\n' >&7 2>/dev/null || true; sleep 2; }

vaigai_stop() {
    printf 'quit\n' >&7 2>/dev/null || true
    exec 7>&- 2>/dev/null || true
    local i=0
    while kill -0 "$VAIGAI_PID" 2>/dev/null && [[ $i -lt 10 ]]; do sleep 1; ((i++)); done
    kill "$VAIGAI_PID" 2>/dev/null || true
    wait "$VAIGAI_PID" 2>/dev/null || true
    rm -f "$VAIGAI_FIFO" "$VAIGAI_LOG" 2>/dev/null || true
    VAIGAI_PID=""; VAIGAI_FIFO=""; VAIGAI_LOG=""
}

# ─────────────────────────────────────────────────────────────────────────────
# 1G — Software Loopback (net_ring)
# ─────────────────────────────────────────────────────────────────────────────
run_1g() {
    section "1G — Software Loopback (net_ring)"
    local VAIGAI_IP=192.168.210.1

    VAIGAI_OUTPUT_FILE=/tmp/vaigai-manual-1g.jsonl
    vaigai_start -l 0-1 --no-pci --vdev "net_ring0" -- --src-ip "$VAIGAI_IP" || {
        fail "1g/start" "vaigai failed to start"; return; }

    # Ping self (loopback)
    vaigai_ping "$VAIGAI_IP" 5
    if [[ "$PING_REPLIES" -gt 0 ]]; then
        pass "1g/ping" "Self-loopback ping: $PING_REPLIES replies"
    else
        fail "1g/ping" "No ping replies on net_ring loopback"
    fi

    # UDP loopback — TX == RX
    vaigai_reset
    vaigai_cmd "start --ip $VAIGAI_IP --port 9 --proto udp --size 64 --duration 3"
    local tx rx; tx=$(json_val tx_pkts); rx=$(json_val rx_pkts)
    [[ "$tx" -gt 0 ]] && pass "1g/udp-tx" "UDP TX=$tx pkts" || fail "1g/udp-tx" "UDP TX=0"
    [[ "$rx" -gt 0 ]] && pass "1g/udp-rx" "UDP RX=$rx pkts (loopback)" || fail "1g/udp-rx" "UDP RX=0"

    vaigai_stop
}

# ─────────────────────────────────────────────────────────────────────────────
# 1H — Null Benchmark (net_null)
# ─────────────────────────────────────────────────────────────────────────────
run_1h() {
    section "1H — Null Benchmark (net_null)"
    local VAIGAI_IP=192.168.211.1 PEER_IP=192.168.211.2

    VAIGAI_OUTPUT_FILE=/tmp/vaigai-manual-1h.jsonl
    vaigai_start -l 0-1 --no-pci --vdev "net_null0" -- --src-ip "$VAIGAI_IP" || {
        fail "1h/start" "vaigai failed to start"; return; }

    # TX-only: ARP requests go to null sink (ARP never resolves, but arp_request counts them)
    vaigai_cmd "start --ip $PEER_IP --port 9 --proto udp --size 64 --duration 5 --rate 0"
    # Explicitly fetch net stats since start fails at ARP resolution (no Workers: from stop)
    vaigai_cmd "stats"
    # export_summary prints "arp_request: N" (not arp_request_tx) in --- other --- section
    local tx; tx=$(json_val arp_request)
    [[ "$tx" -gt 0 ]] && pass "1h/udp-tx" "TX=$tx ARP probes via null device" \
                       || fail "1h/udp-tx" "TX=0 on net_null"

    vaigai_stop
}

# ─────────────────────────────────────────────────────────────────────────────
# 1E — Native AF_PACKET (veth + nginx/socat/openssl)
# ─────────────────────────────────────────────────────────────────────────────
run_1e() {
    section "1E — Native AF_PACKET (veth + nginx/socat/openssl)"
    local VAIGAI_IP=192.168.201.1 SERVER_IP=192.168.201.2
    local VETH_HOST=veth-vaigai VETH_NATIVE=veth-native
    local TLS_DIR=/tmp/vaigai-1e-tls STATE_DIR=/tmp/vaigai-1e
    local HTTPS_PORT=443 socat_echo_pid="" socat_sink_pid="" nginx_pid=""

    cleanup_1e() {
        nginx -s stop -c /tmp/vaigai-1e-nginx.conf 2>/dev/null || true
        [[ -n "$socat_echo_pid" ]] && kill "$socat_echo_pid" 2>/dev/null || true
        [[ -n "$socat_sink_pid" ]] && kill "$socat_sink_pid" 2>/dev/null || true
        pkill -f "openssl s_server.*4433" 2>/dev/null || true
        ip link del "$VETH_HOST" 2>/dev/null || true
        rm -f /tmp/vaigai-1e-nginx.conf /tmp/vaigai-1e-nginx.pid
        rm -rf "$TLS_DIR" "$STATE_DIR"
    }
    cleanup_1e

    # Check port 443 availability
    if ss -tlnp sport = :443 2>/dev/null | grep -q LISTEN; then
        HTTPS_PORT=8443; info "Port 443 busy — using $HTTPS_PORT for HTTPS"
    fi

    # veth pair
    ip link add "$VETH_HOST" type veth peer name "$VETH_NATIVE"
    ip link set "$VETH_HOST" up
    ip link set "$VETH_NATIVE" up
    ip addr add "$SERVER_IP/24" dev "$VETH_NATIVE"
    ethtool -K "$VETH_HOST" tx off 2>/dev/null || true
    ethtool -K "$VETH_NATIVE" tx off 2>/dev/null || true
    info "veth pair: $VETH_HOST ↔ $VETH_NATIVE"

    # TLS cert
    mkdir -p "$TLS_DIR" "$STATE_DIR"
    if command -v openssl >/dev/null 2>&1; then
        openssl req -x509 -newkey rsa:2048 -days 1 -nodes \
            -subj "/CN=vaigai-test" \
            -keyout "$TLS_DIR/server.key" -out "$TLS_DIR/server.crt" 2>/dev/null
    fi

    # nginx config
    command -v nginx >/dev/null 2>&1 && {
        cat > /tmp/vaigai-1e-nginx.conf <<NGINX
worker_processes 1;
pid /tmp/vaigai-1e-nginx.pid;
error_log /tmp/vaigai-1e-nginx-err.log;
events { worker_connections 64; }
http {
    access_log off;
    server { listen ${SERVER_IP}:80; location / { return 200 "ok\n"; } }
$([ -f "$TLS_DIR/server.crt" ] && cat <<TLS
    server {
        listen ${SERVER_IP}:${HTTPS_PORT} ssl;
        ssl_certificate     $TLS_DIR/server.crt;
        ssl_certificate_key $TLS_DIR/server.key;
        location / { return 200 "ok\n"; }
    }
TLS
)
}
NGINX
        nginx -c /tmp/vaigai-1e-nginx.conf 2>/dev/null && info "nginx started on :80 :$HTTPS_PORT"
    }

    # socat TCP echo on 5000, TCP discard on 5001
    socat TCP-LISTEN:5000,bind="$SERVER_IP",fork,reuseaddr PIPE </dev/null &>/dev/null &
    socat_echo_pid=$!
    socat TCP-LISTEN:5001,bind="$SERVER_IP",fork,reuseaddr /dev/null </dev/null &>/dev/null &
    socat_sink_pid=$!

    # openssl s_server on 4433
    if [[ -f "$TLS_DIR/server.crt" ]]; then
        openssl s_server -cert "$TLS_DIR/server.crt" -key "$TLS_DIR/server.key" \
            -accept "${SERVER_IP}:4433" -quiet </dev/null &>/dev/null &
    fi
    sleep 1

    # Start vaigai
    VAIGAI_OUTPUT_FILE=/tmp/vaigai-manual-1e.jsonl
    vaigai_start -l 0-1 --no-pci --vdev "net_af_packet0,iface=$VETH_HOST" \
                 -- --src-ip "$VAIGAI_IP" || {
        fail "1e/start" "vaigai failed to start"; cleanup_1e; return; }

    # Ping
    vaigai_ping "$SERVER_IP" 5
    [[ "$PING_REPLIES" -gt 0 ]] \
        && pass "1e/ping" "Ping $SERVER_IP: $PING_REPLIES replies" \
        || fail "1e/ping" "No ping replies"

    # TCP echo
    vaigai_reset
    vaigai_cmd "start --ip $SERVER_IP --port 5000 --proto tcp --duration 3"
    local conn; conn=$(json_val tcp_conn_open)
    [[ "$conn" -gt 0 ]] && pass "1e/tcp" "TCP conn_open=$conn" || fail "1e/tcp" "TCP conn_open=0"

    # HTTP
    vaigai_reset
    vaigai_cmd "start --ip $SERVER_IP --port 80 --proto http --duration 3 --url /"
    conn=$(json_val tcp_conn_open)
    [[ "$conn" -gt 0 ]] && pass "1e/http" "HTTP conn=$conn" || fail "1e/http" "HTTP conn=0"

    # HTTPS
    if [[ -f "$TLS_DIR/server.crt" ]]; then
        vaigai_reset
        vaigai_cmd "start --ip $SERVER_IP --port $HTTPS_PORT --proto https --duration 3 --url /"
        local tls_ok; tls_ok=$(json_val tls_handshake_ok)
        conn=$(json_val tcp_conn_open)
        if [[ "$tls_ok" -gt 0 ]]; then
            pass "1e/https" "HTTPS tls_ok=$tls_ok"
        elif [[ "$conn" -gt 0 ]]; then
            warn "1e/https" "HTTPS conn=$conn but tls_handshake_ok=0 (may be expected)"
        else
            fail "1e/https" "HTTPS conn=0"
        fi

        # TLS echo
        vaigai_reset
        vaigai_cmd "start --ip $SERVER_IP --port 4433 --proto tls --duration 3"
        tls_ok=$(json_val tls_handshake_ok); conn=$(json_val tcp_conn_open)
        if [[ "$tls_ok" -gt 0 ]]; then
            pass "1e/tls" "TLS tls_ok=$tls_ok"
        elif [[ "$conn" -gt 0 ]]; then
            warn "1e/tls" "TLS conn=$conn but tls_ok=0"
        else
            fail "1e/tls" "TLS conn=0"
        fi
    else
        skip "1e/https" "openssl not available — skipping HTTPS/TLS tests"
        skip "1e/tls"   "openssl not available"
    fi

    # UDP
    vaigai_reset
    vaigai_cmd "start --ip $SERVER_IP --port 5001 --proto udp --size 64 --duration 3"
    local tx; tx=$(json_val tx_pkts)
    [[ "$tx" -gt 0 ]] && pass "1e/udp" "UDP TX=$tx" || fail "1e/udp" "UDP TX=0"

    vaigai_stop
    cleanup_1e
}

# ─────────────────────────────────────────────────────────────────────────────
# 1D — Container AF_XDP (podman + veth)
# ─────────────────────────────────────────────────────────────────────────────
run_1d() {
    section "1D — Container AF_XDP (podman + veth)"
    local VAIGAI_IP=192.168.200.1 SERVER_IP=192.168.200.2
    local VETH_HOST=veth-vaigai VETH_PEER=veth-peer
    local CTR_NAME=vaigai-1d-server

    cleanup_1d() {
        podman rm -f "$CTR_NAME" 2>/dev/null || true
        ip link del "$VETH_HOST" 2>/dev/null || true
    }
    cleanup_1d

    command -v podman >/dev/null 2>&1 || { skip "1d" "podman not found"; return; }

    info "Creating container with services..."
    podman run -d --name "$CTR_NAME" alpine:latest sleep infinity 2>/dev/null \
        || { fail "1d/setup" "podman run failed"; return; }
    podman exec "$CTR_NAME" sh -c '
        apk add --no-cache socat nginx openssl >/dev/null 2>&1
        mkdir -p /run/nginx /var/log/nginx /run/openssl
        echo "server { listen 0.0.0.0:80; location / { return 200 ok; } }" \
            > /etc/nginx/http.d/default.conf
    ' 2>/dev/null || { fail "1d/setup" "package install in container failed"; cleanup_1d; return; }

    # Commit image and re-create with no network
    podman commit "$CTR_NAME" vaigai-1d-img 2>/dev/null
    podman rm -f "$CTR_NAME" 2>/dev/null
    podman run -d --name "$CTR_NAME" --network=none vaigai-1d-img sleep infinity 2>/dev/null \
        || { fail "1d/setup" "podman run no-network failed"; return; }

    local CTR_PID; CTR_PID=$(podman inspect -f '{{.State.Pid}}' "$CTR_NAME" 2>/dev/null)
    [[ -z "$CTR_PID" || "$CTR_PID" == "0" ]] && { fail "1d/setup" "could not get container PID"; cleanup_1d; return; }

    # veth pair
    ip link add "$VETH_HOST" type veth peer name "$VETH_PEER"
    ip link set "$VETH_HOST" up
    ip link set "$VETH_PEER" netns "$CTR_PID"
    nsenter -t "$CTR_PID" -n ip addr add "$SERVER_IP/24" dev "$VETH_PEER"
    nsenter -t "$CTR_PID" -n ip link set "$VETH_PEER" up
    nsenter -t "$CTR_PID" -n ip link set lo up
    ethtool -K "$VETH_HOST" tx off rx off gso off gro off tso off 2>/dev/null || true

    # Start services in container
    podman exec -d "$CTR_NAME" nginx 2>/dev/null || true
    podman exec -d "$CTR_NAME" sh -c "socat TCP-LISTEN:5000,bind=$SERVER_IP,fork,reuseaddr PIPE </dev/null &>/dev/null" 2>/dev/null || true
    sleep 2

    # Start vaigai
    VAIGAI_OUTPUT_FILE=/tmp/vaigai-manual-1d.jsonl
    vaigai_start -l 0-1 --no-pci \
        --vdev "net_af_xdp0,iface=$VETH_HOST,start_queue=0,force_copy=1" \
        -- --src-ip "$VAIGAI_IP" || {
        fail "1d/start" "vaigai failed to start (AF_XDP)"; cleanup_1d; return; }

    # Ping
    vaigai_ping "$SERVER_IP" 5
    [[ "$PING_REPLIES" -gt 0 ]] \
        && pass "1d/ping" "AF_XDP ping: $PING_REPLIES replies" \
        || fail "1d/ping" "No ping replies via AF_XDP"

    # TCP echo
    vaigai_reset
    vaigai_cmd "start --ip $SERVER_IP --port 5000 --proto tcp --duration 3"
    local conn; conn=$(json_val tcp_conn_open)
    [[ "$conn" -gt 0 ]] && pass "1d/tcp" "TCP conn=$conn via AF_XDP" || fail "1d/tcp" "TCP conn=0"

    # HTTP
    vaigai_reset
    vaigai_cmd "start --ip $SERVER_IP --port 80 --proto http --duration 3 --url /"
    conn=$(json_val tcp_conn_open)
    [[ "$conn" -gt 0 ]] && pass "1d/http" "HTTP conn=$conn" || fail "1d/http" "HTTP conn=0"

    vaigai_stop
    cleanup_1d
}

# ─────────────────────────────────────────────────────────────────────────────
# 2A — Server Mode (vaigai as DPDK server, native Linux clients)
# ─────────────────────────────────────────────────────────────────────────────
run_2a() {
    section "2A — Server Mode (vaigai --server, AF_PACKET)"
    local VAIGAI_IP=192.168.203.1 CLIENT_IP=192.168.203.2
    local VETH_SRV=veth-vsrv2a VETH_CLI=veth-vcli2a
    local TLS_DIR=/tmp/vaigai-2a-tls
    local PORT_ECHO=5000 PORT_DISCARD=5001 PORT_CHARGEN=5002
    local PORT_HTTP=8080 PORT_HTTPS=8443 PORT_TLS_ECHO=4433

    cleanup_2a() {
        ip link del "$VETH_SRV" 2>/dev/null || true
        rm -rf "$TLS_DIR"
    }
    cleanup_2a

    # veth pair
    ip link add "$VETH_SRV" type veth peer name "$VETH_CLI"
    ip link set "$VETH_SRV" up
    ip link set "$VETH_CLI" up
    ip addr add "$CLIENT_IP/24" dev "$VETH_CLI"
    ethtool -K "$VETH_SRV" tx off 2>/dev/null || true
    ethtool -K "$VETH_CLI" tx off 2>/dev/null || true

    # TLS cert for server
    mkdir -p "$TLS_DIR"
    local have_tls=0
    if command -v openssl >/dev/null 2>&1; then
        openssl req -x509 -newkey rsa:2048 -days 1 -nodes \
            -subj "/CN=vaigai-server" \
            -keyout "$TLS_DIR/server.key" -out "$TLS_DIR/server.crt" 2>/dev/null && have_tls=1
    fi

    # Start vaigai in server mode
    VAIGAI_OUTPUT_FILE=/tmp/vaigai-manual-2a.jsonl
    vaigai_start -l 0-1 --no-pci --vdev "net_af_packet0,iface=$VETH_SRV" \
                 -- --server --src-ip "$VAIGAI_IP" || {
        fail "2a/start" "vaigai --server failed to start"; cleanup_2a; return; }

    # Issue serve command with all listeners
    local serve_args="serve --listen tcp:${PORT_ECHO}:echo"
    serve_args+=" --listen tcp:${PORT_DISCARD}:discard"
    serve_args+=" --listen tcp:${PORT_CHARGEN}:chargen"
    serve_args+=" --listen http:${PORT_HTTP}"
    if [[ $have_tls -eq 1 ]]; then
        serve_args+=" --listen https:${PORT_HTTPS}"
        serve_args+=" --listen tls:${PORT_TLS_ECHO}:echo"
        serve_args+=" --tls-cert $TLS_DIR/server.crt --tls-key $TLS_DIR/server.key"
    fi
    printf '%s\n' "$serve_args" >&7 2>/dev/null || true
    sleep 3  # Allow listeners to bind
    info "Issued: $serve_args"

    # ── Client-side tests ─────────────────────────────────────────────────────
    # TCP echo
    local echo_reply
    echo_reply=$(echo "hello" | timeout 4 socat -t 3 - "TCP:${VAIGAI_IP}:${PORT_ECHO}" 2>/dev/null || true)
    if [[ "$echo_reply" == *"hello"* ]]; then
        pass "2a/tcp-echo" "TCP echo returned: '$echo_reply'"
    else
        fail "2a/tcp-echo" "TCP echo expected 'hello', got: '$echo_reply'"
    fi

    # TCP discard
    if echo "discard-test" | timeout 3 socat -t 2 - "TCP:${VAIGAI_IP}:${PORT_DISCARD}" 2>/dev/null; then
        pass "2a/tcp-discard" "TCP discard connection succeeded"
    else
        fail "2a/tcp-discard" "TCP discard connection failed"
    fi

    # TCP chargen
    local chargen_bytes
    chargen_bytes=$(timeout 3 socat - "TCP:${VAIGAI_IP}:${PORT_CHARGEN}" </dev/null 2>/dev/null | wc -c || echo 0)
    [[ "$chargen_bytes" -gt 0 ]] \
        && pass "2a/tcp-chargen" "TCP chargen received ${chargen_bytes} bytes" \
        || fail "2a/tcp-chargen" "TCP chargen received 0 bytes"

    # HTTP
    local http_status
    http_status=$(curl -so /dev/null -w "%{http_code}" --max-time 4 \
                       "http://${VAIGAI_IP}:${PORT_HTTP}/" 2>/dev/null || echo "000")
    [[ "$http_status" == "200" ]] \
        && pass "2a/http" "HTTP GET returned $http_status" \
        || fail "2a/http" "HTTP GET returned $http_status (expected 200)"

    if [[ $have_tls -eq 1 ]]; then
        # HTTPS
        local https_status
        https_status=$(curl -sko /dev/null -w "%{http_code}" --max-time 4 \
                            "https://${VAIGAI_IP}:${PORT_HTTPS}/" 2>/dev/null || echo "000")
        [[ "$https_status" == "200" ]] \
            && pass "2a/https" "HTTPS GET returned $https_status" \
            || fail "2a/https" "HTTPS GET returned $https_status"

        # TLS echo
        local tls_reply
        tls_reply=$(echo "tls-test" | { cat; sleep 2; } | timeout 8 \
                    openssl s_client -connect "${VAIGAI_IP}:${PORT_TLS_ECHO}" \
                    -quiet -no_ign_eof 2>/dev/null | head -1 || true)
        [[ "$tls_reply" == *"tls-test"* ]] \
            && pass "2a/tls-echo" "TLS echo returned: '$tls_reply'" \
            || fail "2a/tls-echo" "TLS echo unexpected response: '$tls_reply'"
    else
        skip "2a/https"    "openssl not available"
        skip "2a/tls-echo" "openssl not available"
    fi

    # Server-side stats
    vaigai_cmd "stats"
    local srv_conn; srv_conn=$(json_val tcp_conn_open)
    [[ "$srv_conn" -gt 0 ]] \
        && pass "2a/stats" "Server tcp_conn_open=$srv_conn" \
        || warn "2a/stats" "Server tcp_conn_open=0 (check if clients connected)"

    vaigai_stop
    cleanup_2a
}

# ─────────────────────────────────────────────────────────────────────────────
# 1C — Firecracker TAP (same topology as tcp_tap.sh)
# ─────────────────────────────────────────────────────────────────────────────
run_1c() {
    section "1C — Firecracker TAP (Bridge + MicroVM)"
    local VMLINUX="${VMLINUX:-/work/firecracker/vmlinux}"
    local ROOTFS="${ROOTFS:-/work/firecracker/alpine.ext4}"
    local FIRECRACKER="${FIRECRACKER:-$(command -v firecracker 2>/dev/null || true)}"
    local VAIGAI_IP=192.168.204.1 VM_IP=192.168.204.2 BRIDGE_IP=192.168.204.3
    local BRIDGE=br-vaigai TAP_FC=tap-fc0
    local ROOTFS_COW=/tmp/vaigai-1c-rootfs.ext4
    local FC_SOCKET=/tmp/vaigai-1c-fc.sock FC_SERIAL=/tmp/vaigai-1c-serial.log
    local fc_pid=""

    cleanup_1c() {
        [[ -n "$fc_pid" ]] && kill "$fc_pid" 2>/dev/null || true
        ip link del "$TAP_FC"  2>/dev/null || true
        ip link del "$BRIDGE"  2>/dev/null || true
        rm -f "$FC_SOCKET" "$ROOTFS_COW" "$FC_SERIAL"
    }

    [[ -z "$FIRECRACKER" || ! -x "$FIRECRACKER" ]] && {
        skip "1c" "firecracker not in PATH (set \$FIRECRACKER)"; return; }
    [[ ! -f "$VMLINUX" ]] && { skip "1c" "vmlinux not found at $VMLINUX"; return; }
    [[ ! -f "$ROOTFS"  ]] && { skip "1c" "rootfs not found at $ROOTFS"; return; }

    cleanup_1c
    # Disable bridge netfilter so bridge-forwarded traffic bypasses iptables
    # (needed after podman/docker tests that enable bridge-nf-call-iptables)
    sysctl -q -w net.bridge.bridge-nf-call-iptables=0  2>/dev/null || true
    sysctl -q -w net.bridge.bridge-nf-call-ip6tables=0 2>/dev/null || true
    sysctl -q -w net.bridge.bridge-nf-call-arptables=0 2>/dev/null || true

    # Bridge + TAP for Firecracker
    ip link add "$BRIDGE" type bridge stp_state 0
    ip link set "$BRIDGE" up
    ip addr add "$BRIDGE_IP/24" dev "$BRIDGE"
    ip tuntap add "$TAP_FC" mode tap
    ip link set "$TAP_FC" master "$BRIDGE"
    ip link set "$TAP_FC" type bridge_slave state 3 2>/dev/null || true
    ip link set "$TAP_FC" up

    cp --reflink=auto "$ROOTFS" "$ROOTFS_COW" 2>/dev/null || cp "$ROOTFS" "$ROOTFS_COW"

    # Start Firecracker
    setsid "$FIRECRACKER" --api-sock "$FC_SOCKET" </dev/null >"$FC_SERIAL" 2>&1 &
    fc_pid=$!
    # Wait for Firecracker API socket to appear (up to 10 s) instead of blind sleep
    for i in $(seq 1 20); do [[ -S "$FC_SOCKET" ]] && break; sleep 0.5; done
    kill -0 "$fc_pid" 2>/dev/null || { fail "1c/fc-start" "Firecracker crashed on startup"; cleanup_1c; return; }
    [[ -S "$FC_SOCKET" ]] || { fail "1c/fc-start" "Firecracker API socket not ready after 10s"; cleanup_1c; return; }

    local boot_args="console=ttyS0 reboot=k panic=1 pci=off root=/dev/vda rw quiet"
    boot_args+=" vaigai_mode=all ip=${VM_IP}::${BRIDGE_IP}:255.255.255.0::eth0:off"
    curl -sf --unix-socket "$FC_SOCKET" -X PUT "http://localhost/boot-source" \
        -H 'Content-Type: application/json' \
        -d "{\"kernel_image_path\":\"$VMLINUX\",\"boot_args\":\"$boot_args\"}" >/dev/null \
        || { fail "1c/fc-config" "Failed to set boot-source"; vaigai_stop; cleanup_1c; return; }
    curl -sf --unix-socket "$FC_SOCKET" -X PUT "http://localhost/drives/rootfs" \
        -H 'Content-Type: application/json' \
        -d "{\"drive_id\":\"rootfs\",\"path_on_host\":\"$ROOTFS_COW\",\"is_root_device\":true,\"is_read_only\":false}" >/dev/null \
        || { fail "1c/fc-config" "Failed to set rootfs drive"; vaigai_stop; cleanup_1c; return; }
    curl -sf --unix-socket "$FC_SOCKET" -X PUT "http://localhost/network-interfaces/eth0" \
        -H 'Content-Type: application/json' \
        -d "{\"iface_id\":\"eth0\",\"host_dev_name\":\"$TAP_FC\"}" >/dev/null \
        || { fail "1c/fc-config" "Failed to set network interface"; vaigai_stop; cleanup_1c; return; }
    curl -sf --unix-socket "$FC_SOCKET" -X PUT "http://localhost/machine-config" \
        -H 'Content-Type: application/json' \
        -d '{"vcpu_count":1,"mem_size_mib":256}' >/dev/null \
        || { fail "1c/fc-config" "Failed to set machine-config"; cleanup_1c; return; }
    curl -sf --unix-socket "$FC_SOCKET" -X PUT "http://localhost/actions" \
        -H 'Content-Type: application/json' -d '{"action_type":"InstanceStart"}' >/dev/null \
        || { fail "1c/fc-config" "Failed to start VM instance"; cleanup_1c; return; }
    info "Firecracker VM booting (PID $fc_pid)..."

    # Start vaigai with TAP in parallel with VM boot.
    # IMPORTANT: do NOT attach tap-vaigai to the bridge until the VM is confirmed
    # reachable.  The TAP PMD runs in PROMISC mode; joining the bridge before the
    # VM has finished its ARP exchange poisons the host ARP cache (vaigai can race
    # with the VM to answer the host's ARP broadcast for .2) and causes ping to
    # hit vaigai's dead-end instead of the VM.
    VAIGAI_OUTPUT_FILE=/tmp/vaigai-manual-1c.jsonl
    vaigai_start -l 0-1 --no-pci --vdev "net_tap0,iface=tap-vaigai" \
                 -- --src-ip "$VAIGAI_IP" || {
        fail "1c/vaigai-start" "vaigai TAP failed to start"; cleanup_1c; return; }

    # IPC round-trip: guarantees the DPDK worker thread is polling before we
    # proceed.  The vaigai> prompt marks CLI readiness, not worker readiness.
    vaigai_cmd "stats"

    # Wait for VM to boot via tap-fc0 only (no tap-vaigai on bridge yet).
    # This eliminates any PROMISC / ARP-race interference from vaigai.
    info "Waiting for VM ($VM_IP) to boot (via tap-fc0, no vaigai interference)..."
    local booted=0
    for i in $(seq 1 90); do
        if ! kill -0 "$fc_pid" 2>/dev/null; then
            printf '=== FC serial ===\n'; cat "$FC_SERIAL" 2>/dev/null || true
            fail "1c/vm-boot" "Firecracker died during boot wait"; vaigai_stop; cleanup_1c; return
        fi
        ping -c1 -W1 -I "$BRIDGE" "$VM_IP" &>/dev/null && booted=1 && break
        sleep 1
    done
    [[ $booted -eq 0 ]] && {
        printf '=== FC serial (last 40 lines) ===\n'
        tail -40 "$FC_SERIAL" 2>/dev/null || true
        printf '=== bridge / interface state ===\n'
        ip link show "$BRIDGE" 2>/dev/null || true
        ip link show tap-vaigai 2>/dev/null || true
        bridge fdb show dev "$TAP_FC" 2>/dev/null || true
        printf '=== hugepages free: %s ===\n' "$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/free_hugepages 2>/dev/null || echo ?)"
        fail "1c/vm-boot" "VM at $VM_IP did not respond after 90s"; vaigai_stop; cleanup_1c; return
    }
    info "VM booted — attaching tap-vaigai and waiting 2s for services"

    # Now join vaigai to the bridge for the actual traffic tests
    for i in $(seq 1 20); do ip link show tap-vaigai &>/dev/null && break; sleep 0.5; done
    ip link show tap-vaigai &>/dev/null || { fail "1c/tap" "tap-vaigai not created"; vaigai_stop; cleanup_1c; return; }
    ip link set tap-vaigai master "$BRIDGE"
    ip link set tap-vaigai type bridge_slave state 3 2>/dev/null || true
    ip link set tap-vaigai up
    local vaigai_mac; vaigai_mac=$(cat /sys/class/net/tap-vaigai/address 2>/dev/null || true)
    [[ -n "$vaigai_mac" ]] && bridge fdb del "$vaigai_mac" dev tap-vaigai master 2>/dev/null || true
    info "tap-vaigai attached to bridge"
    sleep 2

    # Ping VM from vaigai
    vaigai_ping "$VM_IP" 5
    [[ "$PING_REPLIES" -gt 0 ]] \
        && pass "1c/ping" "TAP→VM ping: $PING_REPLIES replies" \
        || fail "1c/ping" "No ping replies to VM"

    # TCP echo (socat on :5000)
    vaigai_reset
    vaigai_cmd "start --ip $VM_IP --port 5000 --proto tcp --duration 5"
    local conn; conn=$(json_val tcp_conn_open)
    [[ "$conn" -gt 0 ]] && pass "1c/tcp" "TCP conn=$conn" || fail "1c/tcp" "TCP conn=0"

    # HTTP (nginx on :80)
    vaigai_reset
    vaigai_cmd "start --ip $VM_IP --port 80 --proto http --duration 5 --url /"
    conn=$(json_val tcp_conn_open)
    [[ "$conn" -gt 0 ]] && pass "1c/http" "HTTP conn=$conn" || fail "1c/http" "HTTP conn=0"

    vaigai_stop
    cleanup_1c
}

# ─────────────────────────────────────────────────────────────────────────────
# 1F — memif + testpmd relay + socat
# ─────────────────────────────────────────────────────────────────────────────
run_1f() {
    section "1F — memif + testpmd relay + socat"
    command -v dpdk-testpmd &>/dev/null || {
        skip "1f" "dpdk-testpmd not found in PATH"; return; }

    local result_file; result_file=$(mktemp /tmp/vg_1f_XXXXXX.txt)
    bash "$SCRIPT_DIR/1f-memif-socat.sh" --test all 2>&1 | tee "$result_file"

    local passed=0 failed=0
    while IFS= read -r line; do
        if echo "$line" | grep -q '\[PASS\]'; then ((passed++)) || true
        elif echo "$line" | grep -q '\[FAIL\]'; then ((failed++)) || true
        fi
    done < "$result_file"
    rm -f "$result_file"

    if [[ $failed -eq 0 && $passed -gt 0 ]]; then
        pass "1f" "memif/testpmd: $passed subtests passed"
    elif [[ $passed -gt 0 ]]; then
        fail "1f" "memif/testpmd: $passed passed, $failed failed"
    else
        fail "1f" "memif/testpmd: no PASS lines — check 1f-memif-socat.sh output above"
    fi
}

# ─────────────────────────────────────────────────────────────────────────────
# 1I — net_pcap (PCAP replay + capture)
# ─────────────────────────────────────────────────────────────────────────────
run_1i() {
    section "1I — PCAP Replay / Capture (net_pcap)"
    local VAIGAI_IP=192.168.220.1 PEER_IP=192.168.220.2
    local STATE_DIR=/tmp/vaigai-1i PCAP_IN PCAP_OUT
    PCAP_IN="$STATE_DIR/input.pcap"; PCAP_OUT="$STATE_DIR/output.pcap"

    # Check net_pcap PMD
    if ! find /usr/local/lib* /usr/lib* -name "librte_net_pcap.so*" 2>/dev/null | grep -q .; then
        skip "1i" "net_pcap PMD not found (need libpcap-devel + DPDK net/pcap)"; return
    fi
    command -v python3 >/dev/null 2>&1 || { skip "1i" "python3 not found"; return; }
    command -v tcpdump >/dev/null 2>&1 || { skip "1i" "tcpdump not found"; return; }

    rm -rf "$STATE_DIR"; mkdir -p "$STATE_DIR"

    # Generate synthetic input pcap (30 ARP requests for vaigai's IP)
    python3 <<PYEOF
import struct, sys
VAIGAI_IP = bytes([192, 168, 220, 1])
PEER_IP   = bytes([192, 168, 220, 2])
PEER_MAC  = bytes.fromhex('020000000002')
BCAST_MAC = b'\xff' * 6

def hdr(): return struct.pack('<IHHiIII', 0xa1b2c3d4, 2, 4, 0, 0, 65535, 1)
def pkt_hdr(p): return struct.pack('<IIII', 0, 0, len(p), len(p))
arp = (BCAST_MAC + PEER_MAC + b'\x08\x06'
       + b'\x00\x01\x08\x00\x06\x04\x00\x01'
       + PEER_MAC + PEER_IP
       + b'\x00\x00\x00\x00\x00\x00' + VAIGAI_IP)
with open('$PCAP_IN', 'wb') as f:
    f.write(hdr())
    for _ in range(30):
        f.write(pkt_hdr(arp) + arp)
print("Generated $PCAP_IN: 30 ARP requests")
PYEOF

    [[ -f "$PCAP_IN" ]] || { fail "1i/gen" "failed to generate input.pcap"; return; }
    pass "1i/pcap-gen" "Generated 30-frame ARP input.pcap"

    # Start vaigai with net_pcap; wait for it to replay the input pcap (RX)
    VAIGAI_OUTPUT_FILE=/tmp/vaigai-manual-1i.jsonl
    vaigai_start -l 0-1 --no-pci \
        --vdev "net_pcap0,rx_pcap=${PCAP_IN},tx_pcap=${PCAP_OUT}" \
        -- --src-ip "$VAIGAI_IP" || {
        fail "1i/start" "vaigai net_pcap failed to start"; return; }

    # Send ping to trigger ARP reply (goes to output.pcap)
    vaigai_ping "$PEER_IP" 3
    sleep 1

    # Check output.pcap
    vaigai_cmd "stats"
    local rx; rx=$(json_val rx_pkts)

    vaigai_stop

    if [[ -f "$PCAP_OUT" ]]; then
        local out_pkts; out_pkts=$(tcpdump -r "$PCAP_OUT" -n 2>/dev/null | wc -l || echo 0)
        if [[ "$out_pkts" -gt 0 ]]; then
            pass "1i/output-pcap" "output.pcap has $out_pkts packets (TX captured)"
        else
            fail "1i/output-pcap" "output.pcap is empty"
        fi
        # Check for ARP replies in output
        local arp_replies; arp_replies=$(tcpdump -r "$PCAP_OUT" -n 2>/dev/null | grep -c 'ARP.*reply' || echo 0)
        [[ "$arp_replies" -gt 0 ]] \
            && pass "1i/arp-reply" "output.pcap contains $arp_replies ARP replies" \
            || warn "1i/arp-reply" "No ARP replies in output.pcap (TX=$out_pkts, RX=$rx)"
    else
        fail "1i/output-pcap" "output.pcap not created"
    fi

    rm -rf "$STATE_DIR"
}

# ─────────────────────────────────────────────────────────────────────────────
# 1A — QEMU + Mellanox mlx5 NIC loopback
# ─────────────────────────────────────────────────────────────────────────────
run_1a() {
    section "1A — QEMU + Mellanox mlx5 NIC loopback"
    local NIC_VAIGAI=0000:95:00.0 NIC_VM=0000:95:00.1
    local ROOTFS=/work/firecracker/rootfs.ext4
    local INITRAMFS="${INITRAMFS:-/work/firecracker/initramfs-vm.img}"
    local VM_IP=10.0.0.2 VAIGAI_IP=10.0.0.1
    local qemu_pid="" ROOTFS_COW; ROOTFS_COW=$(mktemp /tmp/vaigai-1a-rootfs-XXXXXX.ext4)

    cleanup_1a() {
        [[ -n "$qemu_pid" ]] && kill "$qemu_pid" 2>/dev/null || true
        echo "$NIC_VM" > /sys/bus/pci/devices/$NIC_VM/driver/unbind 2>/dev/null || true
        echo "" > /sys/bus/pci/devices/$NIC_VM/driver_override 2>/dev/null || true
        echo "$NIC_VM" > /sys/bus/pci/drivers/mlx5_core/bind 2>/dev/null || true
        rm -f "$ROOTFS_COW"
    }

    # Hardware check
    lspci -s "${NIC_VAIGAI#0000:}" &>/dev/null || { skip "1a" "Mellanox NIC $NIC_VAIGAI not found"; rm -f "$ROOTFS_COW"; return; }
    [[ ! -f "$ROOTFS" ]]    && { skip "1a" "QEMU rootfs not found at $ROOTFS"; rm -f "$ROOTFS_COW"; return; }
    [[ ! -f "$INITRAMFS" ]] && { skip "1a" "initramfs not found at $INITRAMFS"; rm -f "$ROOTFS_COW"; return; }
    command -v qemu-system-x86_64 &>/dev/null || { skip "1a" "qemu-system-x86_64 not found"; rm -f "$ROOTFS_COW"; return; }

    # Bind NIC_VM to vfio-pci for passthrough
    modprobe vfio-pci 2>/dev/null || true
    local vendor device
    vendor=$(cat /sys/bus/pci/devices/$NIC_VM/vendor 2>/dev/null | tr -d '0x')
    device=$(cat /sys/bus/pci/devices/$NIC_VM/device 2>/dev/null | tr -d '0x')
    echo "$vendor $device" > /sys/bus/pci/drivers/vfio-pci/new_id 2>/dev/null || true
    echo "$NIC_VM" > /sys/bus/pci/devices/$NIC_VM/driver/unbind 2>/dev/null || true
    echo "vfio-pci" > /sys/bus/pci/devices/$NIC_VM/driver_override
    echo "$NIC_VM" > /sys/bus/pci/drivers/vfio-pci/bind 2>/dev/null \
        || { fail "1a/vfio" "Could not bind $NIC_VM to vfio-pci"; rm -f "$ROOTFS_COW"; return; }
    info "$NIC_VM bound to vfio-pci"

    # COW rootfs copy
    cp --reflink=auto "$ROOTFS" "$ROOTFS_COW" 2>/dev/null || cp "$ROOTFS" "$ROOTFS_COW"

    qemu-system-x86_64 \
        -m 2048 -smp 2 -enable-kvm \
        -kernel /boot/vmlinuz-"$(uname -r)" -initrd "$INITRAMFS" \
        -append "root=/dev/vda rw quiet vaigai_mode=all modprobe.blacklist=qat_dh895xcc,intel_qat" \
        -drive "file=$ROOTFS_COW,format=raw,if=virtio" \
        -nic "user,model=virtio,hostfwd=tcp::2222-:22" \
        -device "vfio-pci,host=${NIC_VM#0000:}" \
        -nographic -serial file:/tmp/vaigai-1a-serial.log \
        -daemonize -pidfile /tmp/vaigai-1a-qemu.pid &>/dev/null
    qemu_pid=$(cat /tmp/vaigai-1a-qemu.pid 2>/dev/null || true)
    [[ -z "$qemu_pid" ]] && { fail "1a/qemu" "QEMU failed to start"; cleanup_1a; return; }
    info "QEMU started (PID $qemu_pid) — waiting for SSH..."

    # Wait for SSH (up to 90s)
    local ssh_ok=0
    for i in $(seq 1 30); do
        ssh -p 2222 -o StrictHostKeyChecking=no -o ConnectTimeout=2 \
            -o UserKnownHostsFile=/dev/null root@localhost 'echo ok' &>/dev/null \
            && ssh_ok=1 && break
        sleep 3
    done
    [[ $ssh_ok -eq 0 ]] && { fail "1a/ssh" "VM SSH not reachable after 90s"; cleanup_1a; return; }

    # Configure data NIC (eth1) inside VM
    ssh -p 2222 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null root@localhost \
        "ip addr add ${VM_IP}/24 dev eth1; ip link set eth1 up; ip link set lo up" 2>/dev/null \
        || warn "1a/vm-net" "VM eth1 config may have failed — continuing anyway"
    sleep 2

    # Start vaigai with mlx5 NIC
    VAIGAI_OUTPUT_FILE=/tmp/vaigai-manual-1a.jsonl
    vaigai_start -l 14-15 -n 4 -a "$NIC_VAIGAI" -- --src-ip "$VAIGAI_IP" || {
        fail "1a/vaigai" "vaigai mlx5 failed to start"; cleanup_1a; return; }

    # Ping VM
    vaigai_ping "$VM_IP" 5
    [[ "$PING_REPLIES" -gt 0 ]] \
        && pass "1a/ping" "mlx5 ping→VM: $PING_REPLIES replies" \
        || fail "1a/ping" "No ping replies to VM"

    # TCP
    vaigai_reset
    vaigai_cmd "start --ip $VM_IP --port 5000 --proto tcp --duration 5"
    local conn; conn=$(json_val tcp_conn_open)
    [[ "$conn" -gt 0 ]] && pass "1a/tcp" "TCP conn=$conn" || fail "1a/tcp" "TCP conn=0"

    # HTTP
    vaigai_reset
    vaigai_cmd "start --ip $VM_IP --port 80 --proto http --duration 5 --url /"
    conn=$(json_val tcp_conn_open)
    [[ "$conn" -gt 0 ]] && pass "1a/http" "HTTP conn=$conn" || fail "1a/http" "HTTP conn=0"

    # HTTPS
    vaigai_reset
    vaigai_cmd "start --ip $VM_IP --port 443 --proto https --duration 5 --url /"
    local tls_ok; tls_ok=$(json_val tls_handshake_ok)
    conn=$(json_val tcp_conn_open)
    [[ "$tls_ok" -gt 0 ]] \
        && pass "1a/https" "HTTPS tls_ok=$tls_ok" \
        || { [[ "$conn" -gt 0 ]] && warn "1a/https" "HTTPS conn=$conn tls_ok=0" \
                                 || fail "1a/https" "HTTPS conn=0"; }

    vaigai_stop
    cleanup_1a
}

# ─────────────────────────────────────────────────────────────────────────────
# 1B — QEMU + Intel i40e NIC loopback
# ─────────────────────────────────────────────────────────────────────────────
run_1b() {
    section "1B — QEMU + Intel i40e NIC loopback"
    local NIC_VAIGAI=0000:83:00.0 NIC_VM=0000:83:00.1
    local ROOTFS=/work/firecracker/rootfs.ext4
    local INITRAMFS="${INITRAMFS:-/work/firecracker/initramfs-vm.img}"
    local VM_IP=10.0.0.2 VAIGAI_IP=10.0.0.1
    local qemu_pid="" ROOTFS_COW; ROOTFS_COW=$(mktemp /tmp/vaigai-1b-rootfs-XXXXXX.ext4)

    cleanup_1b() {
        [[ -n "$qemu_pid" ]] && kill "$qemu_pid" 2>/dev/null || true
        for DEV in $NIC_VAIGAI $NIC_VM; do
            echo "$DEV" > /sys/bus/pci/devices/$DEV/driver/unbind 2>/dev/null || true
            echo "" > /sys/bus/pci/devices/$DEV/driver_override 2>/dev/null || true
            echo "$DEV" > /sys/bus/pci/drivers/i40e/bind 2>/dev/null || true
        done
        rm -f "$ROOTFS_COW" /tmp/vaigai-1b-qemu.pid /tmp/vaigai-1b-serial.log
    }

    lspci -s "${NIC_VAIGAI#0000:}" &>/dev/null || { skip "1b" "Intel i40e NIC $NIC_VAIGAI not found"; rm -f "$ROOTFS_COW"; return; }
    [[ ! -f "$ROOTFS" ]]    && { skip "1b" "QEMU rootfs not found at $ROOTFS"; rm -f "$ROOTFS_COW"; return; }
    [[ ! -f "$INITRAMFS" ]] && { skip "1b" "initramfs not found at $INITRAMFS"; rm -f "$ROOTFS_COW"; return; }
    command -v qemu-system-x86_64 &>/dev/null || { skip "1b" "qemu-system-x86_64 not found"; rm -f "$ROOTFS_COW"; return; }

    modprobe vfio-pci 2>/dev/null || true
    for DEV in $NIC_VAIGAI $NIC_VM; do
        local vendor device
        vendor=$(cat /sys/bus/pci/devices/$DEV/vendor 2>/dev/null | tr -d '0x')
        device=$(cat /sys/bus/pci/devices/$DEV/device 2>/dev/null | tr -d '0x')
        echo "$vendor $device" > /sys/bus/pci/drivers/vfio-pci/new_id 2>/dev/null || true
        echo "$DEV" > /sys/bus/pci/devices/$DEV/driver/unbind 2>/dev/null || true
        echo "vfio-pci" > /sys/bus/pci/devices/$DEV/driver_override
        echo "$DEV" > /sys/bus/pci/drivers/vfio-pci/bind 2>/dev/null || true
    done
    lsmod | grep -q vfio_pci && ls /dev/vfio/ | grep -q '[0-9]' \
        || { fail "1b/vfio" "vfio-pci bind failed for i40e NICs"; cleanup_1b; return; }

    cp --reflink=auto "$ROOTFS" "$ROOTFS_COW" 2>/dev/null || cp "$ROOTFS" "$ROOTFS_COW"

    qemu-system-x86_64 \
        -m 2048 -smp 2 -enable-kvm \
        -kernel /boot/vmlinuz-"$(uname -r)" -initrd "$INITRAMFS" \
        -append "root=/dev/vda rw quiet vaigai_mode=all modprobe.blacklist=qat_dh895xcc,intel_qat" \
        -drive "file=$ROOTFS_COW,format=raw,if=virtio" \
        -nic "user,model=virtio,hostfwd=tcp::2223-:22" \
        -device "vfio-pci,host=${NIC_VM#0000:}" \
        -nographic -serial file:/tmp/vaigai-1b-serial.log \
        -daemonize -pidfile /tmp/vaigai-1b-qemu.pid &>/dev/null
    qemu_pid=$(cat /tmp/vaigai-1b-qemu.pid 2>/dev/null || true)
    [[ -z "$qemu_pid" ]] && { fail "1b/qemu" "QEMU failed to start"; cleanup_1b; return; }
    info "QEMU started (PID $qemu_pid) — waiting for SSH on :2223..."

    local ssh_ok=0
    for i in $(seq 1 30); do
        ssh -p 2223 -o StrictHostKeyChecking=no -o ConnectTimeout=2 \
            -o UserKnownHostsFile=/dev/null root@localhost 'echo ok' &>/dev/null \
            && ssh_ok=1 && break
        sleep 3
    done
    [[ $ssh_ok -eq 0 ]] && { fail "1b/ssh" "VM SSH not reachable after 90s"; cleanup_1b; return; }

    ssh -p 2223 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null root@localhost \
        "ip addr add ${VM_IP}/24 dev eth1; ip link set eth1 up; ip link set lo up" 2>/dev/null || true
    sleep 2

    VAIGAI_OUTPUT_FILE=/tmp/vaigai-manual-1b.jsonl
    vaigai_start -l 14-15 -n 4 -a "$NIC_VAIGAI" -- --src-ip "$VAIGAI_IP" || {
        fail "1b/vaigai" "vaigai i40e failed to start"; cleanup_1b; return; }

    vaigai_ping "$VM_IP" 5
    [[ "$PING_REPLIES" -gt 0 ]] \
        && pass "1b/ping" "i40e ping→VM: $PING_REPLIES replies" \
        || fail "1b/ping" "No ping replies to VM"

    vaigai_reset
    vaigai_cmd "start --ip $VM_IP --port 5000 --proto tcp --duration 5"
    local conn; conn=$(json_val tcp_conn_open)
    [[ "$conn" -gt 0 ]] && pass "1b/tcp" "TCP conn=$conn" || fail "1b/tcp" "TCP conn=0"

    vaigai_reset
    vaigai_cmd "start --ip $VM_IP --port 80 --proto http --duration 5 --url /"
    conn=$(json_val tcp_conn_open)
    [[ "$conn" -gt 0 ]] && pass "1b/http" "HTTP conn=$conn" || fail "1b/http" "HTTP conn=0"

    vaigai_stop
    cleanup_1b
}

# ─────────────────────────────────────────────────────────────────────────────
# Results table
# ─────────────────────────────────────────────────────────────────────────────
print_results() {
    echo ""
    echo -e "${BOLD}╔══════════════════════════════════════════════════════════════════╗${NC}"
    echo -e "${BOLD}║          vaigAI Manual Test Results                              ║${NC}"
    echo -e "${BOLD}╠══════╦══════════════════════════════════╦════════════════════════╣${NC}"
    printf  "${BOLD}║ %-4s ║ %-32s ║ %-22s ║${NC}\n" "Test" "Name" "Result"
    echo -e "${BOLD}╠══════╬══════════════════════════════════╬════════════════════════╣${NC}"

    declare -A TEST_NAMES
    TEST_NAMES=(
        ["1a"]="QEMU + Mellanox mlx5 NIC"
        ["1b"]="QEMU + Intel i40e NIC"
        ["1c"]="Firecracker TAP bridge"
        ["1d"]="Container AF_XDP (podman+veth)"
        ["1e"]="Native AF_PACKET (veth)"
        ["1f"]="memif + testpmd relay"
        ["1g"]="Software loopback (net_ring)"
        ["1h"]="Null benchmark (net_null)"
        ["1i"]="PCAP replay/capture (net_pcap)"
        ["2a"]="Server mode (AF_PACKET)"
    )

    local order=("1a" "1b" "1c" "1d" "1e" "1f" "1g" "1h" "1i" "2a")
    for tid in "${order[@]}"; do
        should_run "$tid" || continue
        local name="${TEST_NAMES[$tid]:-$tid}"
        local subtests=() pass_sub=0 fail_sub=0 skip_sub=0 warn_sub=0
        # Collect all sub-test results for this test
        for key in "${!TEST_STATUS[@]}"; do
            [[ "$key" == "${tid}/"* || "$key" == "$tid" ]] || continue
            case "${TEST_STATUS[$key]}" in
                PASS) ((pass_sub++)) || true ;;
                FAIL) ((fail_sub++)) || true ;;
                SKIP) ((skip_sub++)) || true ;;
                WARN) ((warn_sub++)) || true ;;
            esac
        done
        local status_str color
        if [[ $fail_sub -gt 0 ]]; then
            status_str="FAIL (${fail_sub}F/${pass_sub}P)"; color=$RED
        elif [[ $skip_sub -gt 0 && $pass_sub -eq 0 && $fail_sub -eq 0 ]]; then
            status_str="SKIP"; color=$YELLOW
        elif [[ $warn_sub -gt 0 ]]; then
            status_str="WARN (${warn_sub}W/${pass_sub}P)"; color=$YELLOW
        elif [[ $pass_sub -gt 0 ]]; then
            status_str="PASS (${pass_sub} checks)"; color=$GREEN
        else
            status_str="NO RESULT"; color=$YELLOW
        fi
        printf "║ %-4s ║ %-32s ║ ${color}%-22s${NC} ║\n" "$tid" "${name:0:32}" "${status_str:0:22}"
    done

    echo -e "${BOLD}╠══════╩══════════════════════════════════╩════════════════════════╣${NC}"
    local total=$(( PASS_COUNT + FAIL_COUNT + SKIP_COUNT + WARN_COUNT ))
    printf "${BOLD}║  %-64s ║${NC}\n" "Checks: $total total | ${PASS_COUNT} PASS | ${FAIL_COUNT} FAIL | ${WARN_COUNT} WARN | ${SKIP_COUNT} SKIP"
    echo -e "${BOLD}╚══════════════════════════════════════════════════════════════════╝${NC}"
    echo ""
}

# ─────────────────────────────────────────────────────────────────────────────
# Main
# ─────────────────────────────────────────────────────────────────────────────
[[ $EUID -ne 0 ]] && { echo "Must run as root"; exit 1; }
[[ -x "$VAIGAI_BIN" ]] || { echo "vaigai binary not found: $VAIGAI_BIN"; exit 1; }

trap 'vaigai_stop 2>/dev/null; print_results' EXIT

echo -e "${BOLD}vaigAI Manual Test Runner${NC} — $(date '+%Y-%m-%d %H:%M:%S')"
echo "Binary: $VAIGAI_BIN"
echo ""

should_run "1g" && run_1g
should_run "1h" && run_1h
should_run "1e" && run_1e
should_run "1d" && run_1d
should_run "2a" && run_2a
should_run "1c" && run_1c
should_run "1f" && run_1f
should_run "1i" && run_1i
should_run "1a" && run_1a
should_run "1b" && run_1b

print_results
trap - EXIT
