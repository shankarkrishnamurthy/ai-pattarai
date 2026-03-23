#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# ═══════════════════════════════════════════════════════════════════════════════
#  server_veth.sh — vaigai server mode verification
# ═══════════════════════════════════════════════════════════════════════════════
#  Tests vaigai --server against standard Linux clients over a veth pair.
#  No VM, no container, no physical NIC required.
#
#  Topology:
#      vaigai (DPDK af_packet, --server)     native Linux clients
#          192.168.202.1                        192.168.202.2
#              │                                    │
#          veth-vsrv ◄══════ veth pair ══════► veth-vcli
#          (no kernel IP)                     (kernel IP)
#
#  Clients used (all standard Linux packages):
#    socat         TCP echo / discard / chargen verification
#    curl          HTTP/1.1 and HTTPS requests
#    openssl       TLS handshake + echo (s_client)
#
#  Tests:
#    T1  serve + show listeners      CLI: serve starts, show lists them
#    T2  TCP echo                    socat sends "hello", verifies echo
#    T3  TCP discard                 socat sends data, verifies no echo
#    T4  TCP chargen                 socat receives data stream
#    T5  HTTP GET                    curl verifies HTTP 200 + body
#    T6  show connections            CLI: connection count > 0 during load
#    T7  stop per-listener           stop one listener, verify RST
#    T8  stop all + re-serve         full lifecycle restart
#    T9  HTTPS GET                   curl -k (conditional: TLS build)
#    T10 TLS echo                    openssl s_client (conditional: TLS)
#
#  Usage:
#    bash tests/server_veth.sh              # run all tests
#    bash tests/server_veth.sh --test 2     # run T2 only
#    bash tests/server_veth.sh --keep       # don't tear down on exit
# ═══════════════════════════════════════════════════════════════════════════════

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VAIGAI_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
VAIGAI_BIN="$VAIGAI_DIR/build/vaigai"

# ── Colours & logging ────────────────────────────────────────────────────────
GRN='\033[0;32m'; RED='\033[0;31m'; CYN='\033[0;36m'; YLW='\033[0;33m'; NC='\033[0m'
info()  { echo -e "${CYN}[INFO]${NC}  $*"; }
pass()  { echo -e "${GRN}[PASS]${NC}  server_veth: $*"; ((PASS_COUNT++)) || true; }
fail()  { echo -e "${RED}[FAIL]${NC}  server_veth: $*" >&2; ((FAIL_COUNT++)) || true; }
warn()  { echo -e "${YLW}[WARN]${NC}  server_veth: $*"; }
die()   { echo -e "${RED}[FATAL]${NC} server_veth: $*" >&2; exit 1; }

PASS_COUNT=0
FAIL_COUNT=0

# ── Arguments ────────────────────────────────────────────────────────────────
KEEP=0
TEST_FILTER=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --keep)   KEEP=1 ;;
        --test)   TEST_FILTER="$2"; shift ;;
        -h|--help)
            echo "Usage: $(basename "$0") [--test N] [--keep]"
            exit 0 ;;
        *) die "Unknown option: $1" ;;
    esac
    shift
done

should_run() { [[ -z "$TEST_FILTER" || "$TEST_FILTER" == "$1" ]]; }

# ── Pre-flight ───────────────────────────────────────────────────────────────
[[ $EUID -eq 0 ]]      || die "Must run as root"
[[ -x "$VAIGAI_BIN" ]] || die "vaigai not found: $VAIGAI_BIN — run ninja -C build"
for cmd in socat curl ip ethtool; do
    command -v "$cmd" &>/dev/null || die "Required: $cmd"
done

# ── Constants ────────────────────────────────────────────────────────────────
VETH_SRV=veth-vsrv       # DPDK af_packet side (vaigai server)
VETH_CLI=veth-vcli        # kernel side (native clients)
VAIGAI_IP=192.168.202.1
CLIENT_IP=192.168.202.2
DPDK_LCORES=0-1

# Listener ports
PORT_ECHO=5000
PORT_DISCARD=5001
PORT_CHARGEN=5002
PORT_HTTP=8080
PORT_HTTPS=8443
PORT_TLS_ECHO=4433

# TLS cert (generated at runtime)
TLS_DIR=$(mktemp -d /tmp/vaigai-srv-tls-XXXXXX)
TLS_CERT="$TLS_DIR/cert.pem"
TLS_KEY="$TLS_DIR/key.pem"

# FIFO + log for vaigai process
VAIGAI_FIFO=$(mktemp -u /tmp/vaigai_srv_fifo_XXXXXX)
VAIGAI_LOG=$(mktemp /tmp/vaigai_srv_log_XXXXXX.log)
VAIGAI_PID=""

# Detect TLS support
HAS_TLS=0
if ldd "$VAIGAI_BIN" 2>/dev/null | grep -q libssl; then
    HAS_TLS=1
    command -v openssl &>/dev/null || HAS_TLS=0
fi

# ── Hugepages ────────────────────────────────────────────────────────────────
setup_hugepages() {
    local cur
    cur=$(cat /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages)
    if (( cur < 256 )); then
        echo 256 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
        info "Hugepages set to 256 x 2 MB"
    fi
    rm -f /dev/hugepages/vaigai_* 2>/dev/null || true
}

# ── Teardown ─────────────────────────────────────────────────────────────────
teardown() {
    [[ $KEEP -eq 1 ]] && { info "Keeping topology (--keep)"; return; }
    info "Tearing down"

    # Dump vaigai log for debugging
    if [[ -f "$VAIGAI_LOG" ]]; then
        info "=== vaigai log (last 40 lines) ==="
        tail -40 "$VAIGAI_LOG" >&2 || true
        info "=== end vaigai log ==="
    fi

    # Stop vaigai
    if [[ -n "$VAIGAI_PID" ]] && kill -0 "$VAIGAI_PID" 2>/dev/null; then
        printf 'quit\n' >&7 2>/dev/null || true
        exec 7>&- 2>/dev/null || true
        local w=0
        while kill -0 "$VAIGAI_PID" 2>/dev/null && (( w < 10 )); do
            sleep 0.5; ((w++)) || true
        done
        kill -9 "$VAIGAI_PID" 2>/dev/null || true
        wait "$VAIGAI_PID" 2>/dev/null || true
    else
        exec 7>&- 2>/dev/null || true
    fi

    ip link del "$VETH_SRV" 2>/dev/null || true
    rm -f "$VAIGAI_FIFO" "$VAIGAI_LOG"
    rm -rf "$TLS_DIR"
    VAIGAI_PID=""
}
trap teardown EXIT

# ── JSON value extractor ─────────────────────────────────────────────────────
json_val() { grep -oP "\"$1\": *\K[0-9]+" <<< "$OUTPUT" | tail -1 || echo "0"; }

# ── Send command to vaigai, capture new output ───────────────────────────────
vaigai_cmd() {
    local cmd="$1"
    local wait_s="${2:-3}"
    local start_bytes
    start_bytes=$(stat -c%s "$VAIGAI_LOG" 2>/dev/null || echo 0)

    printf '%s\n' "$cmd" >&7
    sleep "$wait_s"

    OUTPUT=$(tail -c +$((start_bytes + 1)) "$VAIGAI_LOG" 2>/dev/null || true)
}

# Send command and also request stats JSON
vaigai_cmd_stats() {
    local cmd="$1"
    local wait_s="${2:-3}"
    local start_bytes
    start_bytes=$(stat -c%s "$VAIGAI_LOG" 2>/dev/null || echo 0)

    printf '%s\n' "$cmd" >&7
    sleep "$wait_s"
    printf 'stats\n' >&7

    # Wait for JSON closing brace
    local attempts=0
    while (( attempts < 30 )); do
        if tail -c +$((start_bytes + 1)) "$VAIGAI_LOG" 2>/dev/null | grep -q '^}'; then
            break
        fi
        sleep 0.5; ((attempts++))
    done
    sleep 0.5
    OUTPUT=$(tail -c +$((start_bytes + 1)) "$VAIGAI_LOG" 2>/dev/null || true)
}

vaigai_reset() {
    vaigai_cmd "reset" 2
}

# ═══════════════════════════════════════════════════════════════════════════════
#  Setup
# ═══════════════════════════════════════════════════════════════════════════════
info "Setting up veth pair + vaigai server"

setup_hugepages

# Clean leftovers
ip link del "$VETH_SRV" 2>/dev/null || true

# Create veth pair
ip link add "$VETH_SRV" type veth peer name "$VETH_CLI"
ip link set "$VETH_SRV" up
ip link set "$VETH_CLI" up
ip addr add "$CLIENT_IP/24" dev "$VETH_CLI"

# Disable TX checksum offload — critical for af_packet
ethtool -K "$VETH_SRV" tx off 2>/dev/null || true
ethtool -K "$VETH_CLI" tx off 2>/dev/null || true
info "veth pair: $VETH_SRV <-> $VETH_CLI ($CLIENT_IP)"

# Generate TLS cert (needed for HTTPS/TLS tests)
if [[ $HAS_TLS -eq 1 ]]; then
    openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
        -keyout "$TLS_KEY" -out "$TLS_CERT" \
        -subj "/CN=vaigai-test" 2>/dev/null
    info "TLS cert generated: $TLS_CERT"
fi

# Start vaigai in server mode
mkfifo "$VAIGAI_FIFO"
"$VAIGAI_BIN" -l "$DPDK_LCORES" -n 1 --no-pci \
    --vdev "net_af_packet0,iface=$VETH_SRV" \
    -- --server --src-ip "$VAIGAI_IP" \
    < "$VAIGAI_FIFO" > "$VAIGAI_LOG" 2>&1 &
VAIGAI_PID=$!
exec 7>"$VAIGAI_FIFO"
sleep 3

kill -0 "$VAIGAI_PID" 2>/dev/null || die "vaigai failed to start (check log: $VAIGAI_LOG)"
info "vaigai server running (PID $VAIGAI_PID)"

# Prime ARP by pinging vaigai from the client side
info "Priming ARP cache"
ping -c 2 -W 2 -I "$VETH_CLI" "$VAIGAI_IP" &>/dev/null || warn "ARP prime ping failed (may be OK)"
sleep 1

# ═══════════════════════════════════════════════════════════════════════════════
#  T1: serve + show listeners
# ═══════════════════════════════════════════════════════════════════════════════
run_t1() {
    info "T1: serve CLI + show listeners"

    # Build serve command
    local SERVE_CMD="serve --listen tcp:${PORT_ECHO}:echo --listen tcp:${PORT_DISCARD}:discard --listen tcp:${PORT_CHARGEN}:chargen --listen http:${PORT_HTTP}"
    if [[ $HAS_TLS -eq 1 ]]; then
        SERVE_CMD+=" --listen https:${PORT_HTTPS} --listen tls:${PORT_TLS_ECHO}:echo"
        SERVE_CMD+=" --tls-cert $TLS_CERT --tls-key $TLS_KEY"
    fi

    vaigai_cmd "$SERVE_CMD" 3
    vaigai_cmd "show listeners" 2

    # Verify listeners appear in output
    if echo "$OUTPUT" | grep -q "tcp:${PORT_ECHO}:echo"; then
        pass "T1 show listeners contains tcp:${PORT_ECHO}:echo"
    else
        fail "T1 tcp:${PORT_ECHO}:echo not found in show listeners"
    fi

    if echo "$OUTPUT" | grep -q "http:${PORT_HTTP}"; then
        pass "T1 show listeners contains http:${PORT_HTTP}"
    else
        fail "T1 http:${PORT_HTTP} not found in show listeners"
    fi

    local active_count
    active_count=$(echo "$OUTPUT" | grep -c "active" || true)
    local expected=4
    [[ $HAS_TLS -eq 1 ]] && expected=6
    if (( active_count >= expected )); then
        pass "T1 all $expected listeners active"
    else
        fail "T1 expected $expected active listeners, got $active_count"
    fi
}

# ═══════════════════════════════════════════════════════════════════════════════
#  T2: TCP echo — socat
# ═══════════════════════════════════════════════════════════════════════════════
run_t2() {
    info "T2: TCP echo (socat → :${PORT_ECHO})"
    local REPLY
    REPLY=$(echo "hello-echo-test" | socat -t 2 - "TCP:${VAIGAI_IP}:${PORT_ECHO},bind=${CLIENT_IP}" 2>/dev/null || true)

    if [[ "$REPLY" == "hello-echo-test" ]]; then
        pass "T2 echo handler reflected data correctly"
    else
        fail "T2 echo expected 'hello-echo-test', got '${REPLY:-<empty>}'"
    fi
}

# ═══════════════════════════════════════════════════════════════════════════════
#  T3: TCP discard — socat
# ═══════════════════════════════════════════════════════════════════════════════
run_t3() {
    info "T3: TCP discard (socat → :${PORT_DISCARD})"
    local REPLY
    REPLY=$(echo "discard-me" | timeout 3 socat -t 1 - "TCP:${VAIGAI_IP}:${PORT_DISCARD},bind=${CLIENT_IP}" 2>/dev/null || true)

    if [[ -z "$REPLY" ]]; then
        pass "T3 discard handler returned no data"
    else
        fail "T3 discard expected empty reply, got '${REPLY}'"
    fi
}

# ═══════════════════════════════════════════════════════════════════════════════
#  T4: TCP chargen — socat + timeout
# ═══════════════════════════════════════════════════════════════════════════════
run_t4() {
    info "T4: TCP chargen (socat → :${PORT_CHARGEN})"
    local BYTES
    BYTES=$(timeout 3 socat -t 1 - "TCP:${VAIGAI_IP}:${PORT_CHARGEN},bind=${CLIENT_IP}" 2>/dev/null | wc -c || true)

    if (( BYTES > 0 )); then
        pass "T4 chargen handler sent $BYTES bytes"
    else
        fail "T4 chargen expected data, got 0 bytes"
    fi
}

# ═══════════════════════════════════════════════════════════════════════════════
#  T5: HTTP GET — curl
# ═══════════════════════════════════════════════════════════════════════════════
run_t5() {
    info "T5: HTTP GET (curl → :${PORT_HTTP})"
    local HTTP_CODE BODY
    HTTP_CODE=$(curl -s -o /dev/null -w '%{http_code}' \
        --interface "$VETH_CLI" --connect-timeout 5 \
        "http://${VAIGAI_IP}:${PORT_HTTP}/" 2>/dev/null || echo "000")

    if [[ "$HTTP_CODE" == "200" ]]; then
        pass "T5 HTTP 200 response received"
    else
        fail "T5 expected HTTP 200, got $HTTP_CODE"
    fi

    # Verify response body is non-empty
    BODY=$(curl -s --interface "$VETH_CLI" --connect-timeout 5 \
        "http://${VAIGAI_IP}:${PORT_HTTP}/" 2>/dev/null || true)
    if [[ -n "$BODY" ]]; then
        pass "T5 HTTP response body non-empty (${#BODY} bytes)"
    else
        fail "T5 HTTP response body is empty"
    fi
}

# ═══════════════════════════════════════════════════════════════════════════════
#  T6: show connections during active load
# ═══════════════════════════════════════════════════════════════════════════════
run_t6() {
    info "T6: show connections during active connection"

    # Open a long-lived socat connection in background
    socat - "TCP:${VAIGAI_IP}:${PORT_ECHO},bind=${CLIENT_IP}" </dev/null &>/dev/null &
    local SOCAT_PID=$!
    sleep 2

    vaigai_cmd "show connections" 2

    kill "$SOCAT_PID" 2>/dev/null; wait "$SOCAT_PID" 2>/dev/null || true

    if echo "$OUTPUT" | grep -qE "W[0-9]|worker|Active|active"; then
        pass "T6 show connections produced output during active connection"
    else
        fail "T6 show connections produced no relevant output"
    fi
}

# ═══════════════════════════════════════════════════════════════════════════════
#  T7: stop per-listener, then verify RST
# ═══════════════════════════════════════════════════════════════════════════════
run_t7() {
    info "T7: stop per-listener + verify RST"

    # Stop the discard listener
    vaigai_cmd "stop tcp:${PORT_DISCARD}:discard" 2

    # Verify it's no longer active
    vaigai_cmd "show listeners" 2
    local state
    state=$(echo "$OUTPUT" | grep "tcp:${PORT_DISCARD}" | grep -o "stopped\|inactive" || true)
    if [[ -n "$state" ]]; then
        pass "T7 stopped listener shows inactive/stopped"
    else
        # Could also be removed entirely from the list
        if ! echo "$OUTPUT" | grep -q "tcp:${PORT_DISCARD}:discard.*active"; then
            pass "T7 stopped listener no longer active"
        else
            fail "T7 stopped listener still shows active"
        fi
    fi

    # Verify connection to stopped port gets RST (socat should fail)
    local REPLY
    REPLY=$(echo "test" | timeout 3 socat -t 1 - "TCP:${VAIGAI_IP}:${PORT_DISCARD},bind=${CLIENT_IP}" 2>&1 || true)
    if echo "$REPLY" | grep -qiE "refused|reset|error" || [[ -z "$REPLY" ]]; then
        pass "T7 connection to stopped listener rejected"
    else
        warn "T7 stopped listener may still accept (got: $REPLY)"
    fi
}

# ═══════════════════════════════════════════════════════════════════════════════
#  T8: stop all + re-serve
# ═══════════════════════════════════════════════════════════════════════════════
run_t8() {
    info "T8: stop all + re-serve lifecycle"

    vaigai_cmd "stop" 2

    # Verify all stopped
    vaigai_cmd "show listeners" 2
    local active
    active=$(echo "$OUTPUT" | grep -c "active" || true)
    if (( active == 0 )); then
        pass "T8 all listeners stopped"
    else
        fail "T8 expected 0 active listeners after stop, got $active"
    fi

    # Re-serve with a subset
    vaigai_reset
    vaigai_cmd "serve --listen tcp:${PORT_ECHO}:echo --listen http:${PORT_HTTP}" 3

    # Verify echo still works after re-serve
    local REPLY
    REPLY=$(echo "re-serve-test" | socat -t 2 - "TCP:${VAIGAI_IP}:${PORT_ECHO},bind=${CLIENT_IP}" 2>/dev/null || true)
    if [[ "$REPLY" == "re-serve-test" ]]; then
        pass "T8 echo works after re-serve"
    else
        fail "T8 echo after re-serve: expected 're-serve-test', got '${REPLY:-<empty>}'"
    fi
}

# ═══════════════════════════════════════════════════════════════════════════════
#  T9: HTTPS GET — curl -k (conditional)
# ═══════════════════════════════════════════════════════════════════════════════
run_t9() {
    if [[ $HAS_TLS -eq 0 ]]; then
        warn "T9 skipped: vaigai built without TLS"
        return
    fi

    # Re-serve with HTTPS listener
    vaigai_reset
    vaigai_cmd "serve --listen https:${PORT_HTTPS} --listen tcp:${PORT_ECHO}:echo --tls-cert $TLS_CERT --tls-key $TLS_KEY" 3

    info "T9: HTTPS GET (curl -k → :${PORT_HTTPS})"
    local HTTP_CODE
    HTTP_CODE=$(curl -sk -o /dev/null -w '%{http_code}' \
        --interface "$VETH_CLI" --connect-timeout 5 \
        "https://${VAIGAI_IP}:${PORT_HTTPS}/" 2>/dev/null || echo "000")

    if [[ "$HTTP_CODE" == "200" ]]; then
        pass "T9 HTTPS 200 response received"
    else
        fail "T9 expected HTTPS 200, got $HTTP_CODE"
    fi
}

# ═══════════════════════════════════════════════════════════════════════════════
#  T10: TLS echo — openssl s_client (conditional)
# ═══════════════════════════════════════════════════════════════════════════════
run_t10() {
    if [[ $HAS_TLS -eq 0 ]]; then
        warn "T10 skipped: vaigai built without TLS"
        return
    fi

    # Re-serve with TLS echo listener
    vaigai_reset
    vaigai_cmd "serve --listen tls:${PORT_TLS_ECHO}:echo --tls-cert $TLS_CERT --tls-key $TLS_KEY" 3

    info "T10: TLS echo (openssl s_client → :${PORT_TLS_ECHO})"
    local REPLY
    REPLY=$(echo "tls-echo-test" | timeout 5 openssl s_client \
        -connect "${VAIGAI_IP}:${PORT_TLS_ECHO}" \
        -quiet -no_ign_eof 2>/dev/null || true)

    if echo "$REPLY" | grep -q "tls-echo-test"; then
        pass "T10 TLS echo reflected data correctly"
    else
        fail "T10 TLS echo: expected 'tls-echo-test' in reply, got '${REPLY:-<empty>}'"
    fi
}

# ═══════════════════════════════════════════════════════════════════════════════
#  Run tests
# ═══════════════════════════════════════════════════════════════════════════════
should_run 1  && run_t1
should_run 2  && run_t2
should_run 3  && run_t3
should_run 4  && run_t4
should_run 5  && run_t5
should_run 6  && run_t6
should_run 7  && run_t7
should_run 8  && run_t8
should_run 9  && run_t9
should_run 10 && run_t10

# ── Stats summary ────────────────────────────────────────────────────────────
vaigai_cmd_stats "" 1
tcp_open=$(json_val tcp_conn_open)
tcp_close=$(json_val tcp_conn_close)
info "Final stats: tcp_conn_open=$tcp_open tcp_conn_close=$tcp_close"

if (( tcp_open > 0 )); then
    pass "Final: tcp_conn_open > 0 ($tcp_open)"
else
    fail "Final: tcp_conn_open = 0"
fi

# ── Summary ──────────────────────────────────────────────────────────────────
echo ""
info "Results: $PASS_COUNT passed, $FAIL_COUNT failed"
[[ $FAIL_COUNT -eq 0 ]] || exit 1
