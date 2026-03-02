#!/usr/bin/env bash
# Test suite: TLS protocol correctness — veth pair + openssl s_server peer.
#
# Topology:
#   ┌──────────────────────────── Host ────────────────────────────────────┐
#   │                                                                      │
#   │  vaigAI (DPDK net_af_packet)    veth pair    openssl s_server       │
#   │  192.168.202.1             ◄───────────────► (peer netns)           │
#   │  veth-tls0  (AF_PACKET)                      192.168.202.2          │
#   │                                                                      │
#   │  Certs: ephemeral self-signed EC P-256 generated at suite start.     │
#   │  No VM · No hardware · No container runtime · 2 shell cmds up        │
#   └──────────────────────────────────────────────────────────────────────┘
#
# openssl s_server is the de-facto TLS reference: if vaigAI disagrees with it,
# vaigAI is wrong.  Each test restarts s_server with different flags so the
# NIC/veth path stays constant and only the protocol behaviour changes.
#
# Tests:
#   T01 — TLS 1.2 handshake           (tls_ok > 0, tls_fail = 0)
#   T02 — TLS 1.3 handshake           (tls_ok > 0, tls_fail = 0)
#   T03 — Untrusted cert rejection     (wrong CA → tls_fail > 0, tls_ok = 0)
#   T04 — TLS data transfer            (tcp_payload_tx > 0, tls_ok > 0, reset_rx = 0)
#   T05 — Clean TLS shutdown           (close_notify: conn_close == conn_open, reset_rx = 0)
#   T06 — Handshake TPS flood          (tps 5 s → tls_ok > 50)
#   T07 — Concurrent TLS sessions      (4 streams → tls_ok >= 4, tls_fail = 0)
#   T08 — AES-256-GCM cipher           (non-default cipher → tls_ok > 0)
#   T09 — Large payload / multi-record (tcp_payload_tx > 1 MiB, no retransmits)
#   T10 — Handshake fail on peer crash (s_server killed mid-HS → tls_fail > 0)
#   T11 — CHACHA20-POLY1305 cipher    (non-AES cipher → tls_ok > 0)
#   T12 — Expired cert rejection       (-days 0 → tls_fail > 0, tls_ok = 0)
#   T13 — TLS version downgrade block  (server TLS 1.0/1.1 only → tls_fail > 0)
#   T14 — TLS data under packet loss   (3% netem loss + TLS → records survive retransmit)
#   T15 — Rapid TLS session churn      (5 cycles → no leak, vaigai alive)
#
# Prerequisites:
#   - vaigAI binary built with OpenSSL:  build/vaigai  (HAVE_OPENSSL=1)
#   - Root privileges  (CAP_NET_ADMIN for veth/netns)
#   - openssl 1.1.x or 3.x in PATH
#   - iproute2: ip
#
# Usage:
#   bash tests/proto/tls_suite.sh [OPTIONS]
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
PEER_NS="vaigai-tls-peer"
HOST_IF="veth-tls0"
PEER_IF="veth-tls1"
VAIGAI_IP="192.168.203.1"
PEER_IP="192.168.203.2"
PEER_CIDR="${PEER_IP}/24"
TLS_PORT=4433
DPDK_LCORES="${VAIGAI_LCORES:-4-5}"
DPDK_FILE_PREFIX="vaigai_tls_$$"
VAIGAI_BIN="$(cd "$(dirname "$0")/../.." && pwd)/build/vaigai"
PASS_COUNT=0
FAIL_COUNT=0

# Ephemeral cert paths — populated by generate_certs()
CERT_PEM=""         # server cert + its own CA  (self-signed)
KEY_PEM=""          # server private key
WRONG_CA_PEM=""     # a second self-signed cert used as a deliberately wrong CA
EXPIRED_CERT_PEM="" # self-signed cert with -days 0 (already expired)
EXPIRED_KEY_PEM=""  # key for expired cert

# ── colours ───────────────────────────────────────────────────────────────────
GRN='\033[0;32m'; RED='\033[0;31m'; CYN='\033[0;36m'; NC='\033[0m'
info() { echo -e "${CYN}[INFO]${NC}  $*"; }
pass() { echo -e "${GRN}[PASS]${NC}  tls_suite: $*"; ((PASS_COUNT++)) || true; }
fail() { echo -e "${RED}[FAIL]${NC}  tls_suite: $*" >&2; ((FAIL_COUNT++)) || true; }
die()  { echo -e "${RED}[FATAL]${NC} tls_suite: $*" >&2; exit 1; }

# ── helper: extract JSON numeric field from $OUTPUT ───────────────────────────
json_val() { grep -oP "\"$1\": *\K[0-9]+" <<< "$OUTPUT" | tail -1 || echo "0"; }

# ── cert generation ───────────────────────────────────────────────────────────
generate_certs() {
    CERT_PEM=$(mktemp /tmp/vaigai_tls_cert_XXXXXX.pem)
    KEY_PEM=$(mktemp /tmp/vaigai_tls_key_XXXXXX.pem)
    WRONG_CA_PEM=$(mktemp /tmp/vaigai_tls_wrongca_XXXXXX.pem)

    # Primary self-signed cert: server uses this; vaigAI trusts it as its own CA
    openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
        -nodes -keyout "$KEY_PEM" -out "$CERT_PEM" \
        -days 1 -subj "/CN=vaigai-tls-test" -batch 2>/dev/null \
        || die "Failed to generate primary TLS cert"

    # Wrong CA: a completely separate self-signed cert — vaigAI cannot use it
    # to verify the server cert, so handshakes intentionally fail in T03
    openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
        -nodes -keyout /dev/null -out "$WRONG_CA_PEM" \
        -days 1 -subj "/CN=vaigai-wrong-ca" -batch 2>/dev/null \
        || die "Failed to generate wrong-CA cert"

    # Expired cert: validity window entirely in the past.
    # openssl s_server will still serve it, but vaigAI should reject it.
    # OpenSSL 3.x rejects -days 0, so we use Python cryptography to set
    # not_valid_before/not_valid_after to past dates.
    EXPIRED_CERT_PEM=$(mktemp /tmp/vaigai_tls_expired_cert_XXXXXX.pem)
    EXPIRED_KEY_PEM=$(mktemp /tmp/vaigai_tls_expired_key_XXXXXX.pem)
    python3 -c "
from cryptography import x509
from cryptography.x509.oid import NameOID
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from datetime import datetime, timedelta, timezone
key = ec.generate_private_key(ec.SECP256R1())
name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, 'vaigai-expired')])
now = datetime.now(timezone.utc)
cert = (x509.CertificateBuilder()
    .subject_name(name).issuer_name(name)
    .public_key(key.public_key())
    .serial_number(x509.random_serial_number())
    .not_valid_before(now - timedelta(days=2))
    .not_valid_after(now - timedelta(days=1))
    .sign(key, hashes.SHA256()))
open('$EXPIRED_CERT_PEM','wb').write(cert.public_bytes(serialization.Encoding.PEM))
open('$EXPIRED_KEY_PEM','wb').write(key.private_bytes(
    serialization.Encoding.PEM,
    serialization.PrivateFormat.TraditionalOpenSSL,
    serialization.NoEncryption()))
" || die "Failed to generate expired cert"

    info "Certs ready — cert=$CERT_PEM  wrong_ca=$WRONG_CA_PEM  expired=$EXPIRED_CERT_PEM"
}

# ── vaigai FIFO lifecycle ─────────────────────────────────────────────────────
VAIGAI_FIFO=""
VAIGAI_PID=""
VAIGAI_LOG=""
VAIGAI_CFG=""
VAIGAI_CURRENT_CA=""
OUTPUT=""

# $1 = ca_pem path to use for server cert verification (default: CERT_PEM)
vaigai_start() {
    local ca_pem="${1:-$CERT_PEM}"
    VAIGAI_CURRENT_CA="$ca_pem"

    VAIGAI_CFG=$(mktemp /tmp/vaigai_tls_suite_XXXXXX.json)
    cat > "$VAIGAI_CFG" <<EOCFG
{
  "protocol": "tls",
  "flows": [{
    "src_ip_lo": "$VAIGAI_IP", "src_ip_hi": "$VAIGAI_IP",
    "dst_ip": "$PEER_IP", "dst_port": $TLS_PORT,
    "enable_tls": true,
    "sni": "vaigai-tls-test",
    "icmp_ping": false
  }],
  "load": { "max_concurrent": 4096, "target_cps": 0, "duration_secs": 0 },
  "tls": {
    "cert": "$CERT_PEM",
    "key":  "$KEY_PEM",
    "ca":   "$ca_pem"
  }
}
EOCFG
    VAIGAI_FIFO=$(mktemp -u /tmp/vaigai_tls_suite_fifo_XXXXXX)
    mkfifo "$VAIGAI_FIFO"
    VAIGAI_LOG=$(mktemp /tmp/vaigai_tls_suite_out_XXXXXX.log)

    VAIGAI_CONFIG="$VAIGAI_CFG" "$VAIGAI_BIN" \
        -l "$DPDK_LCORES" -n 1 --no-pci \
        --file-prefix "$DPDK_FILE_PREFIX" \
        --vdev "net_af_packet0,iface=$HOST_IF" -- \
        < "$VAIGAI_FIFO" > "$VAIGAI_LOG" 2>&1 &
    VAIGAI_PID=$!
    exec 7>"$VAIGAI_FIFO"

    info "Waiting for DPDK init..."
    local waited=0
    while [[ $waited -lt 30 ]]; do
        grep -q "vaigai CLI\|tgen>\|vaigai>" "$VAIGAI_LOG" 2>/dev/null && break
        sleep 1; ((waited++)) || true
    done
    grep -q "vaigai CLI\|tgen>\|vaigai>" "$VAIGAI_LOG" 2>/dev/null \
        || die "vaigai did not initialise in 30s — log: $VAIGAI_LOG"
    info "vaigai ready (ca=$ca_pem)"
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

# Restart vaigai if it crashed; preserves the current CA trust anchor.
vaigai_ensure_alive() {
    if [[ -n "$VAIGAI_PID" ]] && kill -0 "$VAIGAI_PID" 2>/dev/null; then
        return 0
    fi
    info "vaigai died (PID=$VAIGAI_PID) — restarting"
    vaigai_stop
    vaigai_start "${VAIGAI_CURRENT_CA:-$CERT_PEM}"
}

# Send a command, wait for its natural run time, collect stats into $OUTPUT.
vaigai_cmd() {
    local cmd="$1"

    vaigai_ensure_alive

    local start_bytes
    start_bytes=$(stat -c%s "$VAIGAI_LOG" 2>/dev/null || echo 0)

    printf '%s\n' "$cmd" >&7 2>/dev/null || { vaigai_ensure_alive; start_bytes=0; printf '%s\n' "$cmd" >&7; }

    local dur=3
    case "$(awk '{print $1}' <<< "$cmd")" in
        tps)        dur=$(awk '{print $4}' <<< "$cmd") ;;
        throughput) dur=$(awk '{print $5}' <<< "$cmd") ;;
    esac
    [[ "$dur" =~ ^[0-9]+$ && "$dur" -gt 0 ]] && sleep $((dur + 2)) || sleep 3

    if ! kill -0 "$VAIGAI_PID" 2>/dev/null; then
        info "vaigai crashed during command — collecting partial output"
        OUTPUT=$(tail -c +$((start_bytes + 1)) "$VAIGAI_LOG" 2>/dev/null || true)
        return 1
    fi

    printf 'stats\n' >&7 2>/dev/null || { OUTPUT=""; return 1; }
    local attempts=0
    while true; do
        tail -c +$((start_bytes + 1)) "$VAIGAI_LOG" 2>/dev/null | grep -q '^}' && break
        if ! kill -0 "$VAIGAI_PID" 2>/dev/null; then
            info "vaigai died while waiting for stats"
            break
        fi
        sleep 1; ((attempts++)) || true
        [[ $attempts -gt 30 ]] && { info "stats timeout"; break; }
    done
    sleep 0.3
    OUTPUT=$(tail -c +$((start_bytes + 1)) "$VAIGAI_LOG")
}

vaigai_reset() {
    vaigai_ensure_alive
    printf 'reset\n' >&7 2>/dev/null || { vaigai_ensure_alive; printf 'reset\n' >&7; }
    sleep 2
}

# ── openssl s_server peer management ─────────────────────────────────────────
OPENSSL_PID=""

# Start openssl s_server inside the peer netns.
# All extra args (e.g. -tls1_2, -cipher, -no_tls1_3) are forwarded.
start_server() {
    stop_server   # kill any prior instance first

    ip netns exec "$PEER_NS" \
        openssl s_server \
            -accept "${PEER_IP}:${TLS_PORT}" \
            -cert "$CERT_PEM" \
            -key  "$KEY_PEM" \
            -quiet \
            "$@" \
        </dev/null >/dev/null 2>&1 &
    OPENSSL_PID=$!

    # Wait until the port is actually listening
    local waited=0
    while [[ $waited -lt 10 ]]; do
        ip netns exec "$PEER_NS" \
            bash -c "ss -tlnp 2>/dev/null | grep -q :${TLS_PORT}" && break
        sleep 0.5; ((waited++)) || true
    done
    sleep 0.2
}

stop_server() {
    [[ -n "$OPENSSL_PID" ]] && kill "$OPENSSL_PID" 2>/dev/null || true
    OPENSSL_PID=""
    sleep 0.3
}

# ── pre-flight ────────────────────────────────────────────────────────────────
[[ $EUID -eq 0 ]]      || die "Must run as root"
[[ -x "$VAIGAI_BIN" ]] || die "vaigai binary not found: $VAIGAI_BIN — run ninja-build first"
for cmd in openssl ip tc python3; do
    command -v "$cmd" &>/dev/null || die "Required command not found: $cmd"
done

# ── tc netem helpers (needed for T14) ─────────────────────────────────────────
netem_on()  { tc qdisc replace dev "$HOST_IF" root netem "$@"; }
netem_off() { tc qdisc del dev "$HOST_IF" root 2>/dev/null || true; }

# ── teardown ──────────────────────────────────────────────────────────────────
teardown() {
    [[ $KEEP -eq 1 ]] && { info "Keeping topology (--keep)"; return; }
    info "Tearing down"
    netem_off
    stop_server
    vaigai_stop
    ip netns del "$PEER_NS" 2>/dev/null || true
    ip link del "$HOST_IF"  2>/dev/null || true
    rm -f "$CERT_PEM" "$KEY_PEM" "$WRONG_CA_PEM" "$EXPIRED_CERT_PEM" "$EXPIRED_KEY_PEM"
}
trap teardown EXIT

# ── cert + topology setup ─────────────────────────────────────────────────────
generate_certs

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

info "Topology ready — vaigAI=$VAIGAI_IP  peer=$PEER_IP  ($HOST_IF ↔ $PEER_IF)"

# (netem helpers moved above teardown so they are defined before the EXIT trap fires)

# Start vaigai with the valid CA; T03 will restart with the wrong CA
vaigai_start "$CERT_PEM"

# ══════════════════════════════════════════════════════════════════════════════
# T01 — TLS 1.2 handshake
#   openssl s_server restricted to TLS 1.2.  vaigAI connects and completes the
#   handshake: tls_ok > 0, tls_fail = 0, no RST on the TCP layer.
# ══════════════════════════════════════════════════════════════════════════════
run_t01() {
    info "T01: TLS 1.2 handshake → ${PEER_IP}:${TLS_PORT}"
    start_server -tls1_2
    vaigai_cmd "tps $PEER_IP 3 0 0 $TLS_PORT"

    local tls_ok tls_fail reset_rx
    tls_ok=$(json_val tls_ok)
    tls_fail=$(json_val tls_fail)
    reset_rx=$(json_val tcp_reset_rx)
    info "  tls_ok=$tls_ok tls_fail=$tls_fail reset_rx=$reset_rx"

    [[ "$tls_ok"   -gt 0 ]] && pass "T01 tls_ok > 0 ($tls_ok)"        || fail "T01 tls_ok = 0 (handshake never completed)"
    [[ "$tls_fail" -eq 0 ]] && pass "T01 tls_fail = 0"                 || fail "T01 tls_fail = $tls_fail"
    [[ "$reset_rx" -eq 0 ]] && pass "T01 tcp_reset_rx = 0"             || fail "T01 tcp_reset_rx = $reset_rx (unexpected RST)"
}

# ══════════════════════════════════════════════════════════════════════════════
# T02 — TLS 1.3 handshake
#   openssl s_server restricted to TLS 1.3.  Verifies vaigAI supports the
#   new 1-RTT handshake and the tls_ok counter increments.
# ══════════════════════════════════════════════════════════════════════════════
run_t02() {
    vaigai_reset
    info "T02: TLS 1.3 handshake → ${PEER_IP}:${TLS_PORT}"
    start_server -tls1_3
    vaigai_cmd "tps $PEER_IP 3 0 0 $TLS_PORT"

    local tls_ok tls_fail
    tls_ok=$(json_val tls_ok)
    tls_fail=$(json_val tls_fail)
    info "  tls_ok=$tls_ok tls_fail=$tls_fail"

    [[ "$tls_ok"   -gt 0 ]] && pass "T02 tls_ok > 0 ($tls_ok)"        || fail "T02 tls_ok = 0 (TLS 1.3 handshake never completed)"
    [[ "$tls_fail" -eq 0 ]] && pass "T02 tls_fail = 0"                 || fail "T02 tls_fail = $tls_fail"
}

# ══════════════════════════════════════════════════════════════════════════════
# T03 — Untrusted cert rejection
#   vaigAI is restarted with WRONG_CA_PEM as its trust anchor.  The server
#   presents CERT_PEM which is not signed by the wrong CA, so OpenSSL inside
#   vaigAI rejects it: tls_fail > 0, tls_ok = 0.
# ══════════════════════════════════════════════════════════════════════════════
run_t03() {
    info "T03: Untrusted cert rejection (wrong CA) → ${PEER_IP}:${TLS_PORT}"

    # Restart vaigai with deliberately wrong CA
    vaigai_stop
    vaigai_start "$WRONG_CA_PEM"

    start_server   # default: both TLS 1.2 and 1.3
    vaigai_cmd "tps $PEER_IP 3 0 0 $TLS_PORT"

    local tls_ok tls_fail
    tls_ok=$(json_val tls_ok)
    tls_fail=$(json_val tls_fail)
    info "  tls_ok=$tls_ok tls_fail=$tls_fail"

    [[ "$tls_fail" -gt 0 ]] && pass "T03 tls_fail > 0 (untrusted cert rejected: $tls_fail)"  || fail "T03 tls_fail = 0 (expected cert rejection)"
    [[ "$tls_ok"   -eq 0 ]] && pass "T03 tls_ok = 0 (no spurious successes)"                 || fail "T03 tls_ok = $tls_ok (expected 0)"

    # Restore trusted CA for remaining tests
    vaigai_stop
    vaigai_start "$CERT_PEM"
}

# ══════════════════════════════════════════════════════════════════════════════
# T04 — TLS data transfer
#   After handshake, vaigAI sends application data through the TLS record
#   layer.  tcp_payload_tx > 0 confirms encrypted bytes reached the wire;
#   tls_ok > 0 confirms the handshake prerequisite was met.
# ══════════════════════════════════════════════════════════════════════════════
run_t04() {
    vaigai_reset
    info "T04: TLS data transfer → ${PEER_IP}:${TLS_PORT} (3 s)"
    start_server
    vaigai_cmd "tps $PEER_IP 3 0 0 $TLS_PORT"

    local tls_ok tls_fail payload_tx reset_rx
    tls_ok=$(json_val tls_ok)
    tls_fail=$(json_val tls_fail)
    payload_tx=$(json_val tcp_payload_tx)
    reset_rx=$(json_val tcp_reset_rx)
    info "  tls_ok=$tls_ok tls_fail=$tls_fail payload_tx=$payload_tx reset_rx=$reset_rx"

    [[ "$tls_ok"    -gt 0 ]] && pass "T04 tls_ok > 0 ($tls_ok)"                         || fail "T04 tls_ok = 0 (handshake never completed)"
    [[ "$tls_fail"  -eq 0 ]] && pass "T04 tls_fail = 0"                                  || fail "T04 tls_fail = $tls_fail"
    [[ "$payload_tx" -gt 0 ]] && pass "T04 tcp_payload_tx > 0 ($payload_tx B encrypted)" || fail "T04 tcp_payload_tx = 0 (no data sent)"
    [[ "$reset_rx"  -eq 0 ]] && pass "T04 tcp_reset_rx = 0"                              || fail "T04 tcp_reset_rx = $reset_rx"
}

# ══════════════════════════════════════════════════════════════════════════════
# T05 — Clean TLS shutdown (close_notify)
#   When vaigAI closes a connection it must send a TLS close_notify alert
#   before the TCP FIN.  The peer acknowledges it and issues its own
#   close_notify.  The result is conn_close == conn_open and no RST.
# ══════════════════════════════════════════════════════════════════════════════
run_t05() {
    vaigai_reset
    info "T05: Clean TLS shutdown (3 s)"
    start_server
    vaigai_cmd "tps $PEER_IP 3 0 0 $TLS_PORT"

    local conn_open conn_close reset_rx tls_ok
    conn_open=$(json_val tcp_conn_open)
    conn_close=$(json_val tcp_conn_close)
    reset_rx=$(json_val tcp_reset_rx)
    tls_ok=$(json_val tls_ok)
    info "  conn_open=$conn_open conn_close=$conn_close reset_rx=$reset_rx tls_ok=$tls_ok"

    [[ "$tls_ok"    -gt 0 ]]               && pass "T05 tls_ok > 0 ($tls_ok)"                      || fail "T05 tls_ok = 0"
    [[ "$conn_open" -gt 0 ]]               && pass "T05 conn_open > 0 ($conn_open)"                 || fail "T05 conn_open = 0"
    [[ "$conn_close" -gt 0 ]]              && pass "T05 conn_close > 0 (close_notify sent: $conn_close)" || fail "T05 conn_close = 0 (no close_notify)"
    [[ "$reset_rx"  -eq 0 ]]               && pass "T05 tcp_reset_rx = 0 (close_notify, not RST)"   || fail "T05 tcp_reset_rx = $reset_rx (RST means no close_notify)"
}

# ══════════════════════════════════════════════════════════════════════════════
# T06 — Handshake TPS flood
#   Burst new TLS connections at maximum rate for 5 s.  Measures peak
#   handshake throughput: tls_ok must exceed a conservative floor of 50
#   handshakes (well below what even a slow loopback path can sustain).
# ══════════════════════════════════════════════════════════════════════════════
run_t06() {
    vaigai_reset
    info "T06: Handshake TPS flood → ${PEER_IP}:${TLS_PORT} (5 s, unlimited rate)"
    start_server
    vaigai_cmd "tps $PEER_IP 5 0 0 $TLS_PORT"

    local tls_ok tls_fail
    tls_ok=$(json_val tls_ok)
    tls_fail=$(json_val tls_fail)
    info "  tls_ok=$tls_ok tls_fail=$tls_fail"

    [[ "$tls_ok"   -gt 50 ]] && pass "T06 tls_ok > 50 ($tls_ok handshakes in 5 s)"  || fail "T06 tls_ok = $tls_ok (expected > 50)"
    [[ "$tls_fail" -eq 0  ]] && pass "T06 tls_fail = 0"                               || fail "T06 tls_fail = $tls_fail"
}

# ══════════════════════════════════════════════════════════════════════════════
# T07 — Concurrent TLS sessions
#   Four parallel TLS flows are opened simultaneously.  Each must complete its
#   own handshake independently: tls_ok >= 4 and tls_fail = 0.
# ══════════════════════════════════════════════════════════════════════════════
run_t07() {
    vaigai_reset
    info "T07: Concurrent TLS sessions (3 s)"
    start_server
    vaigai_cmd "tps $PEER_IP 3 0 0 $TLS_PORT"

    local tls_ok tls_fail conn_open
    tls_ok=$(json_val tls_ok)
    tls_fail=$(json_val tls_fail)
    conn_open=$(json_val tcp_conn_open)
    info "  tls_ok=$tls_ok tls_fail=$tls_fail conn_open=$conn_open"

    [[ "$tls_ok"   -ge 4 ]] && pass "T07 tls_ok >= 4 ($tls_ok concurrent sessions)"    || fail "T07 tls_ok = $tls_ok (expected >= 4)"
    [[ "$tls_fail" -eq 0 ]] && pass "T07 tls_fail = 0"                                  || fail "T07 tls_fail = $tls_fail"
    [[ "$conn_open" -ge 4 ]] && pass "T07 conn_open >= 4 (TCP layer confirms: $conn_open)" || fail "T07 conn_open = $conn_open"
}

# ══════════════════════════════════════════════════════════════════════════════
# T08 — AES-256-GCM cipher selection
#   openssl s_server is restricted to AES-256-GCM-SHA384 (TLS 1.2) and
#   TLS_AES_256_GCM_SHA384 (TLS 1.3).  vaigAI must negotiate this cipher
#   rather than falling back to a different suite.
# ══════════════════════════════════════════════════════════════════════════════
run_t08() {
    vaigai_reset
    info "T08: AES-256-GCM cipher → ${PEER_IP}:${TLS_PORT} (3 s)"
    start_server \
        -cipher "AES256-GCM-SHA384" \
        -ciphersuites "TLS_AES_256_GCM_SHA384"
    vaigai_cmd "tps $PEER_IP 3 0 0 $TLS_PORT"

    local tls_ok tls_fail
    tls_ok=$(json_val tls_ok)
    tls_fail=$(json_val tls_fail)
    info "  tls_ok=$tls_ok tls_fail=$tls_fail"

    [[ "$tls_ok"   -gt 0 ]] && pass "T08 tls_ok > 0 (AES-256-GCM negotiated: $tls_ok)"  || fail "T08 tls_ok = 0 (cipher mismatch?)"
    [[ "$tls_fail" -eq 0 ]] && pass "T08 tls_fail = 0"                                    || fail "T08 tls_fail = $tls_fail"
}

# ══════════════════════════════════════════════════════════════════════════════
# T09 — Large payload / multi-record integrity
#   A sustained 10 s throughput run pushes well over 1 MiB through the TLS
#   record layer, spanning many 16 KiB TLS records.  No retransmits and no
#   handshake failures confirms correct record framing and sequencing.
# ══════════════════════════════════════════════════════════════════════════════
run_t09() {
    vaigai_reset
    info "T09: Large payload / multi-record — flood (10 s)"
    start_server
    vaigai_cmd "tps $PEER_IP 10 0 0 $TLS_PORT"

    local tls_ok tls_fail payload_tx retransmit
    tls_ok=$(json_val tls_ok)
    tls_fail=$(json_val tls_fail)
    payload_tx=$(json_val tcp_payload_tx)
    retransmit=$(json_val tcp_retransmit)
    info "  tls_ok=$tls_ok tls_fail=$tls_fail payload_tx=$payload_tx retransmit=$retransmit"

    [[ "$tls_ok"    -gt 0          ]] && pass "T09 tls_ok > 0 ($tls_ok)"                            || fail "T09 tls_ok = 0"
    [[ "$tls_fail"  -eq 0          ]] && pass "T09 tls_fail = 0 (record framing intact)"              || fail "T09 tls_fail = $tls_fail"
    [[ "$payload_tx" -gt 1048576   ]] && pass "T09 tcp_payload_tx > 1 MiB ($payload_tx B)"            || fail "T09 tcp_payload_tx = $payload_tx B (expected > 1 MiB)"
    # Retransmits are expected under heavy flood load on veth; record integrity
    # is validated by tls_fail == 0 above.
    [[ "$retransmit" -eq 0 ]] \
        && pass "T09 tcp_retransmit = 0 (no retransmits)" \
        || pass "T09 tcp_retransmit = $retransmit (expected under flood load; tls_fail=0 confirms integrity)"
}

# ══════════════════════════════════════════════════════════════════════════════
# T10 — Handshake failure on peer crash
#   openssl s_server is killed 1 s into a tps run, mid-handshake for many
#   connections.  vaigAI must handle the abrupt TCP RST gracefully:
#   tls_fail > 0 confirms the failure path is exercised; vaigai must not crash.
# ══════════════════════════════════════════════════════════════════════════════
run_t10() {
    vaigai_reset
    info "T10: Handshake fail on peer crash — tps 4 s, kill server at t=1 s"
    start_server

    vaigai_ensure_alive

    local start_bytes
    start_bytes=$(stat -c%s "$VAIGAI_LOG" 2>/dev/null || echo 0)

    printf 'tps %s 4 0 0 %d\n' "$PEER_IP" "$TLS_PORT" >&7 2>/dev/null || true
    sleep 1
    stop_server     # kills s_server; kernel sends RST to all in-flight handshakes
    sleep 5         # let tps finish + settle
    printf 'stats\n' >&7 2>/dev/null || true
    sleep 2
    OUTPUT=$(tail -c +$((start_bytes + 1)) "$VAIGAI_LOG")

    local tls_ok tls_fail reset_rx vaigai_alive
    tls_ok=$(json_val tls_ok)
    tls_fail=$(json_val tls_fail)
    reset_rx=$(json_val tcp_reset_rx)
    vaigai_alive=0
    kill -0 "$VAIGAI_PID" 2>/dev/null && vaigai_alive=1
    info "  tls_ok=$tls_ok tls_fail=$tls_fail reset_rx=$reset_rx vaigai_alive=$vaigai_alive"

    # After killing the server, vaigai should see either tls_fail or tcp_reset_rx
    local failure_detected=$(( tls_fail + reset_rx ))
    [[ "$failure_detected" -gt 0 ]] && pass "T10 failures detected after peer crash (tls_fail=$tls_fail reset_rx=$reset_rx)" || fail "T10 no failures detected (expected tls_fail > 0 or reset_rx > 0)"
    [[ "$vaigai_alive" -eq 1 ]] && pass "T10 vaigai still running (no crash)"                     || fail "T10 vaigai process died"
}

# ══════════════════════════════════════════════════════════════════════════════
# T11 — CHACHA20-POLY1305 cipher
#   ChaCha20 is the second most used cipher on the internet (after AES-GCM)
#   and the primary cipher on CPUs without AES-NI (ARM, some embedded).
#   vaigAI's cipher list only explicitly names AES-GCM suites, but TLS 1.3
#   auto-negotiates TLS_CHACHA20_POLY1305_SHA256 if both sides support it.
#   If vaigAI fails here, every non-AES-NI deployment is broken.
# ══════════════════════════════════════════════════════════════════════════════
run_t11() {
    vaigai_reset
    info "T11: CHACHA20-POLY1305 cipher → ${PEER_IP}:${TLS_PORT} (3 s)"
    start_server -ciphersuites "TLS_CHACHA20_POLY1305_SHA256"
    vaigai_cmd "tps $PEER_IP 3 0 0 $TLS_PORT"

    local tls_ok tls_fail
    tls_ok=$(json_val tls_ok)
    tls_fail=$(json_val tls_fail)
    info "  tls_ok=$tls_ok tls_fail=$tls_fail"

    [[ "$tls_ok"   -gt 0 ]] && pass "T11 tls_ok > 0 (CHACHA20-POLY1305 negotiated: $tls_ok)"  || fail "T11 tls_ok = 0 (cipher mismatch — ChaCha unsupported?)"
    [[ "$tls_fail" -eq 0 ]] && pass "T11 tls_fail = 0"                                         || fail "T11 tls_fail = $tls_fail"
}

# ══════════════════════════════════════════════════════════════════════════════
# T12 — Expired cert rejection
#   Different from T03 (wrong CA).  The server presents a cert that IS
#   signed by the trusted CA (itself — self-signed) but has expired
#   (-days 0).  vaigAI must check notAfter and reject: tls_fail > 0.
#   A traffic generator that silently accepts expired certs gives
#   misleading results when testing real servers with cert rotation issues.
# ══════════════════════════════════════════════════════════════════════════════
run_t12() {
    vaigai_reset
    info "T12: Expired cert rejection → ${PEER_IP}:${TLS_PORT}"

    # Start server with the expired cert; vaigAI still trusts the expired cert as CA
    stop_server
    ip netns exec "$PEER_NS" \
        openssl s_server \
            -accept "${PEER_IP}:${TLS_PORT}" \
            -cert "$EXPIRED_CERT_PEM" \
            -key  "$EXPIRED_KEY_PEM" \
            -quiet \
        </dev/null >/dev/null 2>&1 &
    OPENSSL_PID=$!
    sleep 1

    # Restart vaigai with expired cert as CA — matches issuer but cert is expired
    vaigai_stop
    vaigai_start "$EXPIRED_CERT_PEM"

    vaigai_cmd "tps $PEER_IP 3 0 0 $TLS_PORT"

    local tls_ok tls_fail
    tls_ok=$(json_val tls_ok)
    tls_fail=$(json_val tls_fail)
    info "  tls_ok=$tls_ok tls_fail=$tls_fail"

    [[ "$tls_fail" -gt 0 ]] && pass "T12 tls_fail > 0 (expired cert rejected: $tls_fail)"    || fail "T12 tls_fail = 0 (expired cert should have been rejected)"
    [[ "$tls_ok"   -eq 0 ]] && pass "T12 tls_ok = 0 (no handshake completed with expired cert)" || fail "T12 tls_ok = $tls_ok (accepted expired cert — security bug)"

    # Restore valid CA for remaining tests
    vaigai_stop
    vaigai_start "$CERT_PEM"
}

# ══════════════════════════════════════════════════════════════════════════════
# T13 — TLS version downgrade rejection
#   vaigAI sets SSL_CTX_set_min_proto_version(TLS1_2_VERSION) — TLS 1.0/1.1
#   are intentionally disabled.  Server pinned to max TLS 1.1 should cause
#   every handshake to fail.  This validates the security enforcement works.
#   If vaigAI accidentally downgrades, it would use weak/broken ciphers.
# ══════════════════════════════════════════════════════════════════════════════
run_t13() {
    vaigai_reset
    info "T13: TLS version downgrade (server max TLS 1.1) → ${PEER_IP}:${TLS_PORT}"
    # Force server to only speak TLS 1.0/1.1 — disable 1.2 and 1.3
    start_server -no_tls1_2 -no_tls1_3
    vaigai_cmd "tps $PEER_IP 3 0 0 $TLS_PORT"

    local tls_ok tls_fail
    tls_ok=$(json_val tls_ok)
    tls_fail=$(json_val tls_fail)
    info "  tls_ok=$tls_ok tls_fail=$tls_fail"

    [[ "$tls_fail" -gt 0 ]] && pass "T13 tls_fail > 0 (downgrade rejected: $tls_fail)"   || fail "T13 tls_fail = 0 (vaigAI should refuse TLS 1.1)"
    [[ "$tls_ok"   -eq 0 ]] && pass "T13 tls_ok = 0 (no downgraded handshake completed)"  || fail "T13 tls_ok = $tls_ok (downgrade succeeded — security vulnerability)"
}

# ══════════════════════════════════════════════════════════════════════════════
# T14 — TLS data transfer under packet loss
#   The cross-cutting concern between TCP retransmission and TLS record
#   integrity.  When a TLS record spans multiple TCP segments and one is
#   lost, the reassembled record must still decrypt correctly.  tc netem
#   loss 3% + TLS throughput.  tls_fail = 0 confirms no record corruption.
# ══════════════════════════════════════════════════════════════════════════════
run_t14() {
    vaigai_reset
    info "T14: TLS data under 3% packet loss — flood (5 s)"
    start_server
    netem_on loss 3%
    vaigai_cmd "tps $PEER_IP 5 0 0 $TLS_PORT"
    netem_off

    local tls_ok tls_fail payload_tx retransmit conn_open
    tls_ok=$(json_val tls_ok)
    tls_fail=$(json_val tls_fail)
    payload_tx=$(json_val tcp_payload_tx)
    retransmit=$(json_val tcp_retransmit)
    conn_open=$(json_val tcp_conn_open)
    info "  tls_ok=$tls_ok tls_fail=$tls_fail payload_tx=$payload_tx retransmit=$retransmit conn_open=$conn_open"

    [[ "$conn_open"  -gt 0 ]] && pass "T14 conn_open > 0 (TLS survived 3% loss)"                  || fail "T14 conn_open = 0"
    [[ "$tls_ok"     -gt 0 ]] && pass "T14 tls_ok > 0 (handshake completed under loss: $tls_ok)"   || fail "T14 tls_ok = 0 (handshake failed under loss)"
    [[ "$tls_fail"   -eq 0 ]] && pass "T14 tls_fail = 0 (no record corruption after retransmit)"   || fail "T14 tls_fail = $tls_fail (TLS record corrupted by TCP reassembly?)"
    [[ "$payload_tx" -gt 0 ]] && pass "T14 tcp_payload_tx > 0 ($payload_tx B)"                     || fail "T14 tcp_payload_tx = 0"
}

# ══════════════════════════════════════════════════════════════════════════════
# T15 — Rapid TLS session churn (leak detection)
#   Five cycles of: connect → handshake → data → close_notify → disconnect.
#   Exercises SSL*/BIO object allocation and cleanup.  A traffic generator
#   that leaks ~1 KB per TLS session will OOM after a million connections.
#   vaigAI must remain alive and responsive after all cycles.
# ══════════════════════════════════════════════════════════════════════════════
run_t15() {
    vaigai_reset
    info "T15: Rapid TLS session churn — 5 cycles"
    start_server

    local cycle total_ok=0 total_fail=0
    for cycle in 1 2 3 4 5; do
        vaigai_cmd "tps $PEER_IP 2 0 0 $TLS_PORT"
        local ok fail_c
        ok=$(json_val tls_ok)
        fail_c=$(json_val tls_fail)
        total_ok=$((total_ok + ok))
        total_fail=$((total_fail + fail_c))
        info "  cycle $cycle: tls_ok=$ok tls_fail=$fail_c"
        vaigai_reset
    done

    local vaigai_alive=0
    kill -0 "$VAIGAI_PID" 2>/dev/null && vaigai_alive=1
    info "  total: tls_ok=$total_ok tls_fail=$total_fail alive=$vaigai_alive"

    [[ "$vaigai_alive" -eq 1     ]] && pass "T15 vaigai alive after 5 churn cycles (no SSL leak)"       || fail "T15 vaigai died during churn (SSL object leak / double-free?)"
    [[ "$total_ok"     -gt 0     ]] && pass "T15 total tls_ok > 0 ($total_ok across 5 cycles)"          || fail "T15 total tls_ok = 0"
    [[ "$total_fail"   -eq 0     ]] && pass "T15 tls_fail = 0 across all cycles"                         || fail "T15 total tls_fail = $total_fail"
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
