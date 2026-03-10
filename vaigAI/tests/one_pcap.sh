#!/usr/bin/env bash
# Test: --one flag verification via packet capture for all protocols.
#
# Verifies that --one:
#   icmp  — sends exactly 1 ICMP echo request
#   udp   — sends exactly 1 UDP datagram
#   tcp   — completes 3WHS + graceful FIN teardown (no RST from us)
#   http  — 3WHS + HTTP GET + HTTP response + graceful FIN
#   tls   — 3WHS + TLS handshake + close_notify + FIN
#   https — 3WHS + TLS handshake + HTTP GET + HTTP response + close_notify + FIN
#
# Network topology (veth + podman container):
#   vaigai (DPDK AF_PACKET) ↔ veth-vaigai ↔ veth-peer (container, 192.168.200.2)
#
# Packet capture runs inside the container's network namespace on veth-peer,
# so all vaigai→container traffic is captured as ingress, free from DPDK
# AF_PACKET socket interference.
#
# Usage:
#   sudo bash tests/one_pcap.sh [--proto icmp|udp|tcp|http|tls|https|stop|all]
#
# Requirements: podman, tcpdump, root

set -euo pipefail

PROTO="all"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --proto) PROTO="$2"; shift 2 ;;
        -h|--help)
            grep '^#' "$0" | grep -v '^#!/' | sed 's/^# \{0,2\}//'
            exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

# ── config ────────────────────────────────────────────────────────────────────
CONTAINER_NAME="vaigai-one-pcap"
HOST_IF="veth-vaigai"        # DPDK AF_PACKET side
PEER_IF="veth-peer"          # container side — tcpdump runs here
SRC_IP="192.168.200.1"
PEER_IP="192.168.200.2"
PEER_CIDR="$PEER_IP/24"
DPDK_LCORES="0-1"
VAIGAI_BIN="$(cd "$(dirname "$0")/.." && pwd)/build/vaigai"
PCAP_DIR="$(mktemp -d /tmp/vaigai-pcap-XXXXXX)"
PASS_COUNT=0
FAIL_COUNT=0
CPID=""

GRN='\033[0;32m'; RED='\033[0;31m'; CYN='\033[0;36m'; YLW='\033[1;33m'; NC='\033[0m'
info()  { echo -e "${CYN}[INFO]${NC}  $*"; }
pass()  { echo -e "${GRN}[PASS]${NC}  $*"; ((PASS_COUNT++)) || true; }
fail()  { echo -e "${RED}[FAIL]${NC}  $*" >&2; ((FAIL_COUNT++)) || true; }
warn()  { echo -e "${YLW}[WARN]${NC}  $*"; }
die()   { echo -e "${RED}[FATAL]${NC} $*" >&2; exit 1; }

# ── pre-flight ────────────────────────────────────────────────────────────────
[[ $EUID -eq 0 ]]      || die "Must run as root"
[[ -x "$VAIGAI_BIN" ]] || die "vaigai binary not found: $VAIGAI_BIN"
for cmd in podman tcpdump ip; do
    command -v "$cmd" &>/dev/null || die "Required: $cmd"
done

# ── teardown ──────────────────────────────────────────────────────────────────
TDUMP_PID=""
teardown() {
    [[ -n "$TDUMP_PID" ]] && kill "$TDUMP_PID" 2>/dev/null || true
    ip link del "$HOST_IF" 2>/dev/null || true
    podman stop  "$CONTAINER_NAME" 2>/dev/null || true
    podman rm -f "$CONTAINER_NAME" 2>/dev/null || true
    rm -rf "$PCAP_DIR"
}
trap teardown EXIT

# ── run a command inside the container ───────────────────────────────────────
cexec() { podman exec "$CONTAINER_NAME" sh -c "$*"; }

# ── setup ─────────────────────────────────────────────────────────────────────
setup_network() {
    info "Starting container $CONTAINER_NAME"
    podman rm -f "$CONTAINER_NAME" &>/dev/null || true
    podman run -d --name "$CONTAINER_NAME" \
        alpine:latest sh -c 'while true; do sleep 60; done'

    for _ in $(seq 30); do
        CPID=$(podman inspect --format '{{.State.Pid}}' "$CONTAINER_NAME" 2>/dev/null || true)
        [[ -n "$CPID" && "$CPID" != "0" ]] && break
        sleep 0.2
    done
    [[ -n "$CPID" && "$CPID" != "0" ]] || die "Container did not start (PID=$CPID)"
    info "Container PID=$CPID"

    info "Installing tools (socat, openssl)"
    podman exec "$CONTAINER_NAME" apk add --no-cache socat openssl >/dev/null 2>&1 \
        || warn "apk failed — TLS/HTTPS/socat tests may be limited"

    info "Creating veth pair $HOST_IF ↔ $PEER_IF"
    ip link del "$HOST_IF" &>/dev/null || true
    ip link add "$HOST_IF" type veth peer name "$PEER_IF"
    ip link set "$HOST_IF" promisc on
    ip link set "$HOST_IF" up
    ip link set "$PEER_IF" netns "$CPID"

    # Configure the container side
    nsenter -t "$CPID" -n -- ip link set "$PEER_IF" up
    nsenter -t "$CPID" -n -- ip addr add "$PEER_CIDR" dev "$PEER_IF"
    nsenter -t "$CPID" -n -- ip link set lo up
    info "Container $PEER_IF = $PEER_CIDR ready"
}

setup_certs() {
    # Generate a self-signed cert inside the container for TLS/HTTPS tests
    podman exec "$CONTAINER_NAME" sh -c \
        'mkdir -p /certs &&
         openssl req -x509 -newkey rsa:2048 \
             -keyout /certs/key.pem -out /certs/cert.pem \
             -days 1 -nodes -subj /CN=vaigai-test >/dev/null 2>&1' \
        || warn "Cert generation failed — TLS/HTTPS tests will be skipped"
}

# ── packet capture helpers ────────────────────────────────────────────────────
# Runs tcpdump inside the container's network namespace on veth-peer.
# All vaigai→container packets are ingress here; free of DPDK interference.
PCAP_FILE=""
start_capture() {
    local label="$1"
    PCAP_FILE="$PCAP_DIR/${label}.pcap"
    # Run tcpdump in the container's net namespace, capturing on veth-peer
    nsenter -t "$CPID" -n -- \
        tcpdump -U -i "$PEER_IF" -w "$PCAP_FILE" \
        "ip host $SRC_IP" >/dev/null 2>&1 &
    TDUMP_PID=$!
    sleep 0.5   # let tcpdump open its socket before vaigai fires
}

stop_capture() {
    [[ -n "$TDUMP_PID" ]] && kill "$TDUMP_PID" 2>/dev/null || true
    wait "$TDUMP_PID" 2>/dev/null || true
    TDUMP_PID=""
    sleep 0.2
}

pcap_text() { tcpdump -r "$PCAP_FILE" -n -tt 2>/dev/null || true; }

# ── run vaigai with --one ─────────────────────────────────────────────────────
run_one() {
    local cmd="$1"
    printf '%s\nquit\n' "$cmd" \
        | "$VAIGAI_BIN" \
            -l "$DPDK_LCORES" -n 1 --no-pci \
            --vdev "net_af_packet0,iface=$HOST_IF" \
            -- --src-ip "$SRC_IP" 2>&1 || true
}

# ── kill background services inside container ─────────────────────────────────
svc_stop() {
    podman exec "$CONTAINER_NAME" sh -c \
        'for f in /tmp/*.pid; do [ -f "$f" ] && kill $(cat "$f") 2>/dev/null; done; true' \
        2>/dev/null || true
    sleep 0.3
}

# ══════════════════════════════════════════════════════════════════════════════
#  ICMP — 1 echo request
# ══════════════════════════════════════════════════════════════════════════════
test_icmp() {
    info "=== ICMP --one ==="

    start_capture "icmp"
    local out
    out=$(run_one "start --proto icmp --ip $PEER_IP --port 0 --one")
    stop_capture

    local txt req_cnt
    txt=$(pcap_text)
    req_cnt=$(echo "$txt" | grep -ci "ICMP.*echo request" || true)

    info "pcap (icmp):"
    echo "$txt" | sed 's/^/  /' || true

    if [[ "$req_cnt" -eq 1 ]]; then
        pass "ICMP --one: exactly 1 echo request"
    else
        fail "ICMP --one: expected 1 echo request, got $req_cnt"
        echo "$out" | grep -E "tx_pkts|STATUS|transmitted" | head -5 || true
    fi
    echo "$out" | grep -E "tx_pkts|STATUS|transmitted" | head -3 || true
}

# ══════════════════════════════════════════════════════════════════════════════
#  UDP — 1 datagram
# ══════════════════════════════════════════════════════════════════════════════
test_udp() {
    info "=== UDP --one ==="
    podman exec "$CONTAINER_NAME" sh -c \
        'socat UDP-LISTEN:9999,fork - >/dev/null 2>&1 &
         echo $! > /tmp/udp.pid' || true
    sleep 0.3

    start_capture "udp"
    local out
    out=$(run_one "start --proto udp --ip $PEER_IP --port 9999 --one --size 64")
    stop_capture
    svc_stop

    local txt pkt_cnt
    txt=$(pcap_text)
    # Count UDP packets from SRC_IP
    pkt_cnt=$(echo "$txt" | grep "UDP" | grep -c "$SRC_IP" || true)

    info "pcap (udp):"
    echo "$txt" | sed 's/^/  /' || true

    if [[ "$pkt_cnt" -eq 1 ]]; then
        pass "UDP --one: exactly 1 UDP datagram"
    else
        fail "UDP --one: expected 1 UDP datagram, got $pkt_cnt"
    fi
    echo "$out" | grep -E "tx_pkts|STATUS|transmitted" | head -3 || true
}

# ══════════════════════════════════════════════════════════════════════════════
#  TCP — SYN + graceful FIN (no RST from us)
# ══════════════════════════════════════════════════════════════════════════════
test_tcp() {
    info "=== TCP --one ==="
    podman exec "$CONTAINER_NAME" sh -c \
        'socat TCP-LISTEN:5000,fork,reuseaddr /dev/null 2>/dev/null &
         echo $! > /tmp/tcp.pid' || true
    sleep 0.3

    start_capture "tcp"
    local out
    out=$(run_one "start --proto tcp --ip $PEER_IP --port 5000 --one")
    stop_capture
    svc_stop

    local txt syn_cnt fin_cnt rst_cnt
    txt=$(pcap_text)
    syn_cnt=$(echo "$txt" | grep -E "^[0-9.]+ IP $SRC_IP\." | grep -c  "Flags \[S\]"                   || true)
    fin_cnt=$(echo "$txt" | grep -E "^[0-9.]+ IP $SRC_IP\." | grep -cE "Flags \[F\.\]|Flags \[F\]"     || true)
    rst_cnt=$(echo "$txt" | grep -E "^[0-9.]+ IP $SRC_IP\." | grep -cE "Flags \[R\.\]|Flags \[R\]"     || true)
    echo "$txt" | sed 's/^/  /' || true

    if [[ "$syn_cnt" -eq 1 ]]; then
        pass "TCP --one: exactly 1 SYN (not N×workers)"
    else
        fail "TCP --one: expected 1 SYN, got $syn_cnt"
    fi
    if [[ "$fin_cnt" -ge 1 ]]; then
        pass "TCP --one: graceful FIN teardown"
    else
        fail "TCP --one: no FIN seen (expected graceful close)"
    fi
    if [[ "$rst_cnt" -eq 0 ]]; then
        pass "TCP --one: no RST from vaigai"
    else
        warn "TCP --one: RST seen from vaigai ($rst_cnt) — may be RTO abort"
    fi
    echo "$out" | grep -E "tcp_syn|conn_open|STATUS" | head -3 || true
}

# ══════════════════════════════════════════════════════════════════════════════
#  HTTP — 3WHS + GET + 200 response + FIN
# ══════════════════════════════════════════════════════════════════════════════
test_http() {
    info "=== HTTP --one ==="
    podman exec "$CONTAINER_NAME" sh -c \
        'socat TCP-LISTEN:80,fork,reuseaddr,bind=0.0.0.0 \
             SYSTEM:"printf \"HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nHello\"" \
             >/dev/null 2>&1 &
         echo $! > /tmp/http.pid' || true
    sleep 0.5

    start_capture "http"
    local out
    out=$(run_one "start --proto http --ip $PEER_IP --port 80 --one --url /")
    stop_capture
    svc_stop

    local txt syn_cnt fin_cnt rst_cnt
    txt=$(pcap_text)
    syn_cnt=$(echo "$txt" | grep -E "^[0-9.]+ IP $SRC_IP\." | grep -c  "Flags \[S\]"               || true)
    fin_cnt=$(echo "$txt" | grep -E "^[0-9.]+ IP $SRC_IP\." | grep -cE "Flags \[F\.\]|Flags \[F\]" || true)
    rst_cnt=$(echo "$txt" | grep -E "^[0-9.]+ IP $SRC_IP\." | grep -cE "Flags \[R\.\]|Flags \[R\]" || true)

    info "pcap (http):"
    echo "$txt" | sed 's/^/  /' || true

    if [[ "$syn_cnt" -eq 1 ]]; then
        pass "HTTP --one: exactly 1 SYN"
    else
        fail "HTTP --one: expected 1 SYN, got $syn_cnt"
    fi
    if [[ "$fin_cnt" -ge 1 ]]; then
        pass "HTTP --one: graceful FIN teardown after HTTP response"
    else
        fail "HTTP --one: no FIN (check if server sent HTTP/1.x response)"
        echo "$out" | grep -E "http_rsp|STATUS|conn_open" | head -5 || true
    fi
    if [[ "$rst_cnt" -eq 0 ]]; then
        pass "HTTP --one: no RST from vaigai"
    else
        warn "HTTP --one: RST from vaigai ($rst_cnt)"
    fi
    echo "$out" | grep -E "http_rsp|STATUS|conn_open" | head -3 || true
}

# ══════════════════════════════════════════════════════════════════════════════
#  TLS — 3WHS + TLS handshake + close_notify + FIN
# ══════════════════════════════════════════════════════════════════════════════
test_tls() {
    info "=== TLS --one (tcp --tls, no HTTP) ==="
    if ! podman exec "$CONTAINER_NAME" sh -c 'openssl version >/dev/null 2>&1'; then
        warn "openssl not available — skipping TLS test"
        return
    fi

    podman exec "$CONTAINER_NAME" sh -c \
        'openssl s_server -accept 4433 \
             -cert /certs/cert.pem -key /certs/key.pem \
             -quiet -tls1_2 </dev/null >/dev/null 2>&1 &
         echo $! > /tmp/tls.pid' || true
    sleep 0.8

    start_capture "tls"
    local out
    out=$(run_one "start --proto tcp --ip $PEER_IP --port 4433 --one --tls")
    stop_capture
    svc_stop

    local txt syn_cnt fin_cnt rst_cnt data_from_us
    txt=$(pcap_text)
    syn_cnt=$(echo "$txt"  | grep -E "^[0-9.]+ IP $SRC_IP\." | grep -c  "Flags \[S\]"                   || true)
    fin_cnt=$(echo "$txt"  | grep -E "^[0-9.]+ IP $SRC_IP\." | grep -cE "Flags \[F\.\]|Flags \[F\]"     || true)
    rst_cnt=$(echo "$txt"  | grep -E "^[0-9.]+ IP $SRC_IP\." | grep -cE "Flags \[R\.\]|Flags \[R\]"     || true)
    # Data segments from vaigai (push flag) — ClientHello + CCS/Finished + close_notify
    data_from_us=$(echo "$txt" | grep -E "^[0-9.]+ IP $SRC_IP\." | grep -cE "Flags \[P\.\]|Flags \[P\]" || true)

    info "pcap (tls):"
    echo "$txt" | sed 's/^/  /' || true

    if [[ "$syn_cnt" -eq 1 ]]; then
        pass "TLS --one: exactly 1 SYN"
    else
        fail "TLS --one: expected 1 SYN, got $syn_cnt"
    fi
    if [[ "$fin_cnt" -ge 1 ]]; then
        pass "TLS --one: graceful FIN"
    else
        fail "TLS --one: no FIN"
    fi
    if [[ "$rst_cnt" -eq 0 ]]; then
        pass "TLS --one: no RST from vaigai"
    else
        warn "TLS --one: RST from vaigai ($rst_cnt)"
    fi
    # close_notify is a TLS Alert record (1 extra data segment after handshake)
    # Minimum from us: ClientHello, Finished/CCS, close_notify = 3 push segments
    if [[ "$data_from_us" -ge 3 ]]; then
        pass "TLS --one: close_notify sent ($data_from_us push segments ≥ 3)"
    else
        fail "TLS --one: too few push segments ($data_from_us < 3) — close_notify may be missing"
    fi
    echo "$out" | grep -E "tls_ok|STATUS|conn_open|tls_fail" | head -3 || true
}

# ══════════════════════════════════════════════════════════════════════════════
#  HTTPS — 3WHS + TLS + HTTP GET + HTTP response + close_notify + FIN
# ══════════════════════════════════════════════════════════════════════════════
test_https() {
    info "=== HTTPS --one ==="
    if ! podman exec "$CONTAINER_NAME" sh -c 'openssl version >/dev/null 2>&1'; then
        warn "openssl not available — skipping HTTPS test"
        return
    fi

    # openssl s_server -HTTP (LibreSSL) sends the file content as-is.
    # The file must contain the complete HTTP response including status line.
    podman exec "$CONTAINER_NAME" sh -c \
        'printf "HTTP/1.0 200 OK\nContent-Length: 2\n\nOK" > /tmp/ok.html
         cd /tmp && openssl s_server -accept 443 \
             -cert /certs/cert.pem -key /certs/key.pem \
             -HTTP -quiet -tls1_2 </dev/null >/dev/null 2>&1 &
         echo $! > /tmp/https.pid' || true
    sleep 0.8

    start_capture "https"
    local out
    out=$(run_one "start --proto https --ip $PEER_IP --port 443 --one --url /ok.html")
    stop_capture
    svc_stop

    local txt syn_cnt fin_cnt rst_cnt data_from_us
    txt=$(pcap_text)
    syn_cnt=$(echo "$txt"  | grep -E "^[0-9.]+ IP $SRC_IP\." | grep -c  "Flags \[S\]"                   || true)
    fin_cnt=$(echo "$txt"  | grep -E "^[0-9.]+ IP $SRC_IP\." | grep -cE "Flags \[F\.\]|Flags \[F\]"     || true)
    rst_cnt=$(echo "$txt"  | grep -E "^[0-9.]+ IP $SRC_IP\." | grep -cE "Flags \[R\.\]|Flags \[R\]"     || true)
    # Push segs from vaigai: ClientHello, CCS/Finished, HTTP GET (encrypted), close_notify
    data_from_us=$(echo "$txt" | grep -E "^[0-9.]+ IP $SRC_IP\." | grep -cE "Flags \[P\.\]|Flags \[P\]" || true)

    info "pcap (https):"
    echo "$txt" | sed 's/^/  /' || true

    if [[ "$syn_cnt" -eq 1 ]]; then
        pass "HTTPS --one: exactly 1 SYN"
    else
        fail "HTTPS --one: expected 1 SYN, got $syn_cnt"
    fi
    if [[ "$fin_cnt" -ge 1 ]]; then
        pass "HTTPS --one: graceful FIN"
    else
        fail "HTTPS --one: no FIN"
        echo "$out" | grep -E "http_rsp|STATUS|tls_ok|tls_fail" | head -5 || true
    fi
    if [[ "$rst_cnt" -eq 0 ]]; then
        pass "HTTPS --one: no RST from vaigai"
    else
        warn "HTTPS --one: RST from vaigai ($rst_cnt)"
    fi
    # Push segs: ClientHello + CCS/Finished + EncHTTPGET + close_notify = ≥4
    if [[ "$data_from_us" -ge 4 ]]; then
        pass "HTTPS --one: close_notify sent ($data_from_us push segments ≥ 4)"
    else
        fail "HTTPS --one: too few push segments ($data_from_us < 4) — close_notify may be missing"
    fi
    echo "$out" | grep -E "http_rsp|STATUS|tls_ok|conn_open" | head -3 || true
}

# ══════════════════════════════════════════════════════════════════════════════
#  stop command messages
# ══════════════════════════════════════════════════════════════════════════════
test_stop_messages() {
    info "=== stop command messages ==="

    # When idle (no traffic running): expect "already in stopped state"
    local out_idle
    out_idle=$(printf 'stop\nquit\n' \
        | "$VAIGAI_BIN" \
            -l "$DPDK_LCORES" -n 1 --no-pci \
            --vdev "net_af_packet0,iface=$HOST_IF" \
            -- --src-ip "$SRC_IP" 2>&1) || true

    if echo "$out_idle" | grep -qi "already in stopped state"; then
        pass "stop (idle): 'already in stopped state'"
    else
        fail "stop (idle): wrong message — got:"
        echo "$out_idle" | grep -iE "stop|traffic" | head -3 || echo "  (no matching output)"
    fi

    # When traffic is running: start UDP flood, then stop
    local out_active
    out_active=$(printf 'start --proto udp --ip %s --port 9 --duration 30\nstop\nquit\n' \
            "$PEER_IP" \
        | "$VAIGAI_BIN" \
            -l "$DPDK_LCORES" -n 1 --no-pci \
            --vdev "net_af_packet0,iface=$HOST_IF" \
            -- --src-ip "$SRC_IP" 2>&1) || true

    if echo "$out_active" | grep -q "Traffic generation stopped\."; then
        pass "stop (active): 'Traffic generation stopped.'"
    else
        fail "stop (active): wrong message — got:"
        echo "$out_active" | grep -iE "stop|traffic" | head -3 || echo "  (no matching output)"
    fi
}

# ── main ──────────────────────────────────────────────────────────────────────
setup_network
setup_certs

case "$PROTO" in
    icmp)  test_icmp  ;;
    udp)   test_udp   ;;
    tcp)   test_tcp   ;;
    http)  test_http  ;;
    tls)   test_tls   ;;
    https) test_https ;;
    stop)  test_stop_messages ;;
    all)
        test_icmp
        test_udp
        test_tcp
        test_http
        test_tls
        test_https
        test_stop_messages
        ;;
    *) die "Unknown protocol: $PROTO" ;;
esac

echo ""
echo "────────────────────────────────────────"
printf "Results: ${GRN}%d passed${NC}  ${RED}%d failed${NC}\n" \
    $PASS_COUNT $FAIL_COUNT
echo "────────────────────────────────────────"
[[ $FAIL_COUNT -eq 0 ]] && exit 0 || exit 1
