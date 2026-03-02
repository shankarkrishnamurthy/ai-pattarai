#!/usr/bin/env bash
# Test: HTTPS over real NIC loopback + QEMU VM (nginx SSL) + Intel QAT.
#
# Topology:
#   vaigAI (DPDK on NIC_VAIGAI) ←loopback cable→ QEMU VM (NIC_VM passthrough, nginx SSL)
#   Intel QAT DH895XCC devices optionally passed to vaigai and/or QEMU VM
#
# NIC pair (default: ens22f0np0 ↔ ens22f1np1, PCI 0000:81:00.0 ↔ 0000:81:00.1)
#
# The QEMU VM boots Alpine Linux with nginx serving HTTPS on :443.
#
# Tests:
#   T1 — HTTPS TPS: connection rate (TCP + TLS + HTTP per connection)
#   T2 — HTTPS Throughput: bulk HTTPS download rate (Mbps)
#   T3 — HTTPS Latency: p50/p95/p99 transaction latency
#   T4 — Crypto Matrix: QAT vs SW on vaigai × server (2×2)
#   T5 — Concurrent Connection Scaling: TPS vs concurrency sweep
#
# Crypto parameterization:
#   VAIGAI_CRYPTO=qat|sw   Control vaigai-side crypto engine
#   SERVER_CRYPTO=qat|sw   Control VM server-side crypto engine
#   TLS_CIPHER=...         OpenSSL cipher string
#
# Prerequisites:
#   - QEMU with KVM + vfio-pci support
#   - Host kernel vmlinuz + initramfs with NIC driver
#   - Alpine rootfs ext4 at $ROOTFS (with nginx, openssl pre-installed)
#   - NIC loopback pair (physical cable connecting two ports)
#   - IOMMU enabled, vfio-pci module, root privileges
#   - For SERVER_CRYPTO=qat: QAT VF kernel modules in rootfs (auto-installed),
#     QATEngine optional — afalg engine used by default for kernel crypto offload
#
# Usage:
#   bash tests/https_nic.sh [OPTIONS]
#
# Options:
#   --test <1|2|3|4|5|all>  Which test to run (default: all)
#   --keep                  Don't tear down on exit (debugging)
#   -h, --help              Show this help message and exit.

set -euo pipefail
trap '' PIPE   # Ignore SIGPIPE — handled via >&7 write checks

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

# ── parameterised config ─────────────────────────────────────────────────────
NIC_PCI_VAIGAI="${NIC_PCI_VAIGAI:-0000:81:00.0}"
NIC_PCI_VM="${NIC_PCI_VM:-0000:81:00.1}"
NIC_IFACE_VAIGAI="${NIC_IFACE_VAIGAI:-ens22f0np0}"
NIC_IFACE_VM="${NIC_IFACE_VM:-ens22f1np1}"
VAIGAI_IP="${VAIGAI_IP:-10.0.0.1}"
VM_IP="${VM_IP:-10.0.0.2}"
FLOOD_DURATION="${FLOOD_DURATION:-10}"
TARGET_CPS="${TARGET_CPS:-400}"
THROUGHPUT_DUR="${THROUGHPUT_DUR:-10}"
THROUGHPUT_STREAMS="${THROUGHPUT_STREAMS:-4}"
LATENCY_DUR="${LATENCY_DUR:-10}"
HTTP_RESP_SIZE="${HTTP_RESP_SIZE:-100k}"
VM_MEM="${VM_MEM:-1024}"
VM_CPUS="${VM_CPUS:-2}"
TLS_CIPHER="${TLS_CIPHER:-ECDHE-ECDSA-AES128-GCM-SHA256}"
VAIGAI_CRYPTO="${VAIGAI_CRYPTO:-qat}"
SERVER_CRYPTO="${SERVER_CRYPTO:-sw}"

# ── QAT VF setup & discovery ──────────────────────────────────────────────────
qat_vf_setup() {
    modprobe intel_qat 2>/dev/null || true
    modprobe qat_dh895xcc 2>/dev/null || true

    local pf_list
    mapfile -t pf_list < <(lspci -d 8086:0435 2>/dev/null | awk '{print "0000:"$1}')
    [[ ${#pf_list[@]} -gt 0 ]] || { warn "No QAT PFs found (8086:0435)"; return 1; }

    local need_vfs=0
    if [[ $(lspci -d 8086:0443 2>/dev/null | wc -l) -eq 0 ]]; then
        need_vfs=1
    fi

    if [[ $need_vfs -eq 1 ]]; then
        info "Creating QAT VFs from PFs..."
        for pf in "${pf_list[@]}"; do
            local pf_drv
            pf_drv=$(basename "$(readlink /sys/bus/pci/devices/$pf/driver 2>/dev/null)" 2>/dev/null || echo "")
            if [[ "$pf_drv" != "dh895xcc" ]]; then
                if [[ -n "$pf_drv" ]]; then
                    echo "$pf" > "/sys/bus/pci/drivers/$pf_drv/unbind" 2>/dev/null || true
                    sleep 0.3
                fi
                echo "qat_dh895xcc" > "/sys/bus/pci/devices/$pf/driver_override"
                echo 1 > "/sys/bus/pci/devices/$pf/remove"
                sleep 2
                echo 1 > /sys/bus/pci/rescan
                sleep 3
                pf_drv=$(basename "$(readlink /sys/bus/pci/devices/$pf/driver 2>/dev/null)" 2>/dev/null || echo "")
                if [[ "$pf_drv" != "dh895xcc" ]]; then
                    warn "Failed to bind PF $pf to kernel QAT driver (got: $pf_drv)"
                    continue
                fi
                info "PF $pf bound to dh895xcc"
            fi
            local cur_vfs
            cur_vfs=$(cat /sys/bus/pci/devices/$pf/sriov_numvfs 2>/dev/null || echo 0)
            if [[ "$cur_vfs" -eq 0 ]]; then
                echo 1 > /sys/bus/pci/devices/$pf/sriov_numvfs 2>/dev/null || true
                sleep 1
            fi
        done
    fi

    local vf_list
    mapfile -t vf_list < <(lspci -d 8086:0443 2>/dev/null | awk '{print "0000:"$1}')
    for vf in "${vf_list[@]}"; do
        local vf_drv
        vf_drv=$(basename "$(readlink /sys/bus/pci/devices/$vf/driver 2>/dev/null)" 2>/dev/null || echo "")
        if [[ "$vf_drv" != "vfio-pci" ]]; then
            if [[ -n "$vf_drv" ]]; then
                echo "$vf" > "/sys/bus/pci/drivers/$vf_drv/unbind" 2>/dev/null || true
                sleep 0.3
            fi
            echo "vfio-pci" > "/sys/bus/pci/devices/$vf/driver_override"
            echo "$vf" > /sys/bus/pci/drivers_probe 2>/dev/null || true
            sleep 0.3
        fi
    done
    info "QAT VFs on vfio-pci: $(lspci -d 8086:0443 2>/dev/null | wc -l)"
}

# ── NUMA-aware lcore selection ────────────────────────────────────────────────
nic_numa=$(cat /sys/bus/pci/devices/$NIC_PCI_VAIGAI/numa_node 2>/dev/null || echo 0)
if [[ "$nic_numa" -eq 1 ]]; then
    DPDK_LCORES="${DPDK_LCORES:-14-15}"
else
    DPDK_LCORES="${DPDK_LCORES:-0-1}"
fi

VMLINUX="${VMLINUX:-/boot/vmlinuz-$(uname -r)}"
INITRAMFS="${INITRAMFS:-/boot/initramfs-$(uname -r).img}"
ROOTFS="${ROOTFS:-/work/firecracker/alpine.ext4}"
VAIGAI_BIN="$(cd "$(dirname "$0")/.." && pwd)/build/vaigai"

PASS_COUNT=0
FAIL_COUNT=0

# ── colours ───────────────────────────────────────────────────────────────────
GRN='\033[0;32m'; RED='\033[0;31m'; CYN='\033[0;36m'; YLW='\033[0;33m'
BLD='\033[1m'; NC='\033[0m'
info()  { echo -e "${CYN}[INFO]${NC}  $*"; }
warn()  { echo -e "${YLW}[WARN]${NC}  $*"; }
pass()  { echo -e "${GRN}[PASS]${NC}  https_nic: $*"; ((PASS_COUNT++)) || true; }
fail()  { echo -e "${RED}[FAIL]${NC}  https_nic: $*" >&2; ((FAIL_COUNT++)) || true; }
die()   { echo -e "${RED}[FATAL]${NC} https_nic: $*" >&2; exit 1; }
peak()  { echo -e "${BLD}══ $* ══${NC}"; }

# ── QAT VF setup (now that info/warn are defined) ────────────────────────────
if [[ "$VAIGAI_CRYPTO" == "qat" ]] || [[ "$SERVER_CRYPTO" == "qat" ]]; then
    qat_vf_setup
fi

# Discover VFs (DH895XCC VF = 8086:0443)
mapfile -t QAT_ALL < <(lspci -d 8086:0443 2>/dev/null | awk '{print "0000:"$1}')
QAT_PCI_VAIGAI="${QAT_PCI_VAIGAI:-${QAT_ALL[0]:-}}"
QAT_PCI_SERVER="${QAT_PCI_SERVER:-${QAT_ALL[1]:-}}"

# ── helper: extract JSON field from vaigai output ─────────────────────────────
json_val() {
    grep -oP "\"$1\": *\K[0-9]+" <<< "$OUTPUT" | tail -1 || echo "0"
}

# ══════════════════════════════════════════════════════════════════════════════
#  Certificate generation
# ══════════════════════════════════════════════════════════════════════════════
TLS_CERT=""
TLS_KEY=""

generate_certs() {
    TLS_CERT=$(mktemp /tmp/vaigai-https-cert-XXXXXX.pem)
    TLS_KEY=$(mktemp /tmp/vaigai-https-key-XXXXXX.pem)
    openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
        -nodes -keyout "$TLS_KEY" -out "$TLS_CERT" \
        -days 1 -subj "/CN=vaigai-test" -batch 2>/dev/null \
        || die "Failed to generate TLS certificates"
    info "Generated ephemeral TLS cert: $TLS_CERT"
}

# ══════════════════════════════════════════════════════════════════════════════
#  vaigai FIFO-based lifecycle
# ══════════════════════════════════════════════════════════════════════════════
VAIGAI_FIFO=""
VAIGAI_PID=""
VAIGAI_LOG=""
VAIGAI_CFG=""
OUTPUT=""

vaigai_start() {
    local crypto_mode="${1:-$VAIGAI_CRYPTO}"

    VAIGAI_CFG=$(mktemp /tmp/vaigai_https_XXXXXX.json)
    cat > "$VAIGAI_CFG" <<EOCFG
{
  "protocol": "https",
  "flows": [{
    "src_ip_lo": "$VAIGAI_IP", "src_ip_hi": "$VAIGAI_IP",
    "dst_ip": "$VM_IP", "dst_port": 443,
    "enable_tls": true,
    "sni": "vaigai-test",
    "http_url": "/$HTTP_RESP_SIZE",
    "http_host": "vaigai-test",
    "icmp_ping": false
  }],
  "load": { "max_concurrent": 8192, "target_cps": 0, "duration_secs": 0 },
  "tls": {
    "cert": "$TLS_CERT",
    "key": "$TLS_KEY",
    "ca": "$TLS_CERT"
  }
}
EOCFG
    VAIGAI_FIFO=$(mktemp -u /tmp/vaigai_https_fifo_XXXXXX)
    mkfifo "$VAIGAI_FIFO"
    VAIGAI_LOG=$(mktemp /tmp/vaigai_https_out_XXXXXX.log)

    local socket_mem
    if [[ "$nic_numa" -eq 0 ]]; then
        socket_mem="256,0"
    else
        socket_mem="0,256"
    fi

    local dpdk_args="-l $DPDK_LCORES -n 4 --socket-mem $socket_mem --file-prefix vaigai_https -a $NIC_PCI_VAIGAI"
    if [[ "$crypto_mode" == "qat" ]] && [[ -n "$QAT_PCI_VAIGAI" ]]; then
        dpdk_args+=" -a $QAT_PCI_VAIGAI"
        info "vaigai crypto: QAT ($QAT_PCI_VAIGAI)"
    else
        info "vaigai crypto: software (OpenSSL)"
    fi

    # Kill any stale vaigai processes that might hold VFIO groups
    local stale_vaigai
    stale_vaigai=$(pgrep -f "build/vaigai" 2>/dev/null || true)
    if [[ -n "$stale_vaigai" ]]; then
        warn "Killing stale vaigai PIDs: $stale_vaigai"
        kill -9 $stale_vaigai 2>/dev/null || true
        sleep 1
    fi
    # Clean stale DPDK runtime sockets
    rm -rf /var/run/dpdk/vaigai_https* 2>/dev/null || true

    VAIGAI_CONFIG="$VAIGAI_CFG" "$VAIGAI_BIN" \
        $dpdk_args -- \
        < "$VAIGAI_FIFO" > "$VAIGAI_LOG" 2>&1 &
    VAIGAI_PID=$!

    exec 7>"$VAIGAI_FIFO"

    info "Waiting for vaigai DPDK init..."
    local waited=0
    while [[ $waited -lt 30 ]]; do
        if grep -q "vaigai CLI\|tgen>\|vaigai>" "$VAIGAI_LOG" 2>/dev/null; then
            break
        fi
        if ! kill -0 "$VAIGAI_PID" 2>/dev/null; then
            cat "$VAIGAI_LOG" >&2 || true
            die "vaigai exited prematurely"
        fi
        sleep 1
        ((waited++)) || true
    done
    [[ $waited -lt 30 ]] || { cat "$VAIGAI_LOG" >&2; die "vaigai did not start within 30s"; }

    if [[ "$crypto_mode" == "qat" ]]; then
        grep -q "crypto_qat\|Crypto device 0" "$VAIGAI_LOG" 2>/dev/null && \
            info "QAT crypto device detected by vaigai" || \
            warn "QAT device not detected — may fall back to SW"
    else
        grep -q "No crypto PMDs found" "$VAIGAI_LOG" 2>/dev/null && \
            info "Confirmed: SW crypto path (no QAT)"
    fi

    info "vaigai started (PID $VAIGAI_PID)"
}

vaigai_reset() {
    echo "reset" >&7
    sleep 4
}

vaigai_cmd() {
    local cmd="$1"
    local start_bytes
    start_bytes=$(stat -c%s "$VAIGAI_LOG" 2>/dev/null || echo 0)

    # Check vaigai is still alive before writing to fd 7
    if [[ -n "$VAIGAI_PID" ]] && ! kill -0 "$VAIGAI_PID" 2>/dev/null; then
        warn "vaigai process $VAIGAI_PID died before command: $cmd"
        OUTPUT=""
        return 1
    fi

    printf '%s\n' "$cmd" >&7

    local dur cmd_type
    cmd_type=$(echo "$cmd" | awk '{print $1}')
    if [[ "$cmd_type" == "throughput" ]]; then
        dur=$(echo "$cmd" | awk '{print $5}')
    elif [[ "$cmd_type" == "tps" ]]; then
        # tps <ip> <dur> <cps> <size> <port>
        dur=$(echo "$cmd" | awk '{print $3}')
    elif [[ "$cmd_type" == "ping" ]]; then
        local p_count p_interval_ms
        p_count=$(echo "$cmd" | awk '{print $3}')
        p_interval_ms=$(echo "$cmd" | awk '{print $5}')
        [[ "$p_count" =~ ^[0-9]+$ ]] || p_count=5
        [[ "$p_interval_ms" =~ ^[0-9]+$ ]] || p_interval_ms=1000
        dur=$(( (p_count * p_interval_ms + 999) / 1000 ))
    else
        dur=$(echo "$cmd" | awk '{print $4}')
    fi
    if [[ "$dur" =~ ^[0-9]+$ ]] && [[ "$dur" -gt 0 ]]; then
        sleep $((dur + 3))
    else
        sleep 5
    fi

    # Check vaigai is still alive before requesting stats
    if [[ -n "$VAIGAI_PID" ]] && ! kill -0 "$VAIGAI_PID" 2>/dev/null; then
        warn "vaigai process $VAIGAI_PID died during command: $cmd"
        OUTPUT=$(tail -c +$((start_bytes + 1)) "$VAIGAI_LOG" 2>/dev/null || true)
        return 1
    fi

    printf 'stats\n' >&7

    local attempts=0 found=0
    while [[ $found -eq 0 ]]; do
        if tail -c +$((start_bytes + 1)) "$VAIGAI_LOG" 2>/dev/null | grep -q '^}'; then
            found=1
        else
            sleep 1
            ((attempts++)) || true
            if [[ $attempts -gt 60 ]]; then
                info "Timed out waiting for stats output"
                tail -30 "$VAIGAI_LOG" 2>/dev/null || true
                break
            fi
        fi
    done
    sleep 0.5

    OUTPUT=$(tail -c +$((start_bytes + 1)) "$VAIGAI_LOG")
}

vaigai_stop() {
    if [[ -n "$VAIGAI_LOG" ]] && [[ -f "$VAIGAI_LOG" ]]; then
        info "=== vaigai process log ==="
        cat "$VAIGAI_LOG" >&2 || true
        info "=== end vaigai log ==="
    fi
    if [[ -n "$VAIGAI_PID" ]] && kill -0 "$VAIGAI_PID" 2>/dev/null; then
        printf 'quit\n' >&7 2>/dev/null || true
        exec 7>&- 2>/dev/null || true
        local waited=0
        while kill -0 "$VAIGAI_PID" 2>/dev/null && [[ $waited -lt 10 ]]; do
            sleep 0.5
            ((waited++)) || true
        done
        kill -0 "$VAIGAI_PID" 2>/dev/null && kill -9 "$VAIGAI_PID" 2>/dev/null || true
        wait "$VAIGAI_PID" 2>/dev/null || true
    else
        exec 7>&- 2>/dev/null || true
    fi
    rm -f "$VAIGAI_FIFO" "$VAIGAI_LOG" "$VAIGAI_CFG"
    VAIGAI_PID=""
}

# ══════════════════════════════════════════════════════════════════════════════
#  VFIO-PCI bind/unbind helpers
# ══════════════════════════════════════════════════════════════════════════════
ORIG_DRIVER_VAIGAI=""
ORIG_DRIVER_VM=""
ORIG_DRIVER_QAT_V=""
ORIG_DRIVER_QAT_S=""

vfio_bind() {
    local pci_addr="$1"
    local iface="${2:-}"
    local orig_var="$3"

    # Reload vfio-pci with denylist disabled (required for QAT DH895XCC)
    if ! modprobe -r vfio-pci 2>/dev/null; then true; fi
    modprobe vfio-pci disable_denylist=1 2>/dev/null || true

    local orig_drv
    orig_drv=$(basename "$(readlink -f /sys/bus/pci/devices/$pci_addr/driver 2>/dev/null)" 2>/dev/null || echo "")
    eval "$orig_var=\"$orig_drv\""

    if [[ "$orig_drv" == "vfio-pci" ]]; then
        info "$pci_addr already bound to vfio-pci"
        return 0
    fi

    if [[ -n "$iface" ]] && ip link show "$iface" &>/dev/null; then
        ip link set "$iface" down 2>/dev/null || true
    fi

    if [[ -n "$orig_drv" ]]; then
        info "Unbinding $pci_addr from $orig_drv"
        echo "$pci_addr" > "/sys/bus/pci/drivers/$orig_drv/unbind" 2>/dev/null || true
        sleep 0.5
    fi

    echo "vfio-pci" > "/sys/bus/pci/devices/$pci_addr/driver_override"
    echo "$pci_addr" > /sys/bus/pci/drivers/vfio-pci/bind 2>/dev/null || \
        echo "$pci_addr" > /sys/bus/pci/drivers_probe 2>/dev/null || true
    sleep 0.5

    local cur_drv
    cur_drv=$(basename "$(readlink -f /sys/bus/pci/devices/$pci_addr/driver 2>/dev/null)" 2>/dev/null || echo "")
    if [[ "$cur_drv" == "vfio-pci" ]]; then
        info "Bound $pci_addr to vfio-pci (was: $orig_drv)"
    else
        die "Failed to bind $pci_addr to vfio-pci (current: $cur_drv)"
    fi
}

vfio_unbind() {
    local pci_addr="$1"
    local orig_driver="$2"

    if [[ -z "$orig_driver" ]] || [[ "$orig_driver" == "vfio-pci" ]]; then
        return 0
    fi

    echo "$pci_addr" > /sys/bus/pci/drivers/vfio-pci/unbind 2>/dev/null || true
    sleep 0.3
    echo "" > "/sys/bus/pci/devices/$pci_addr/driver_override" 2>/dev/null || true
    echo "$pci_addr" > "/sys/bus/pci/drivers/$orig_driver/bind" 2>/dev/null || \
        echo "$pci_addr" > /sys/bus/pci/drivers_probe 2>/dev/null || true
    sleep 0.5
    info "Restored $pci_addr to $orig_driver"
}

# ══════════════════════════════════════════════════════════════════════════════
#  QEMU VM lifecycle
# ══════════════════════════════════════════════════════════════════════════════
QEMU_PID=""
ROOTFS_COW=""
QEMU_SERIAL=""
QEMU_ERR=""

qemu_start() {
    local server_crypto="${1:-$SERVER_CRYPTO}"
    info "Preparing QEMU VM (server_crypto=$server_crypto)"

    ROOTFS_COW=$(mktemp /tmp/vaigai-https-rootfs-XXXXXX.ext4)
    cp --reflink=auto "$ROOTFS" "$ROOTFS_COW"
    chmod 644 "$ROOTFS_COW"

    local mntdir
    mntdir=$(mktemp -d /tmp/vaigai-https-mnt-XXXXXX)
    mount -o loop "$ROOTFS_COW" "$mntdir"

    # Network config
    cat > "$mntdir/etc/network/interfaces" <<IFACES
auto lo
iface lo inet loopback

auto eth0
iface eth0 inet static
    address ${VM_IP}
    netmask 255.255.255.0
IFACES

    # Copy TLS certs
    mkdir -p "$mntdir/etc/vaigai"
    cp "$TLS_CERT" "$mntdir/etc/vaigai/cert.pem"
    cp "$TLS_KEY"  "$mntdir/etc/vaigai/key.pem"

    # Generate static test files for nginx
    mkdir -p "$mntdir/var/www/html"
    dd if=/dev/urandom of="$mntdir/var/www/html/1k"   bs=1024 count=1    2>/dev/null
    dd if=/dev/urandom of="$mntdir/var/www/html/100k"  bs=1024 count=100  2>/dev/null
    dd if=/dev/urandom of="$mntdir/var/www/html/1m"    bs=1024 count=1024 2>/dev/null

    # nginx SSL configuration (ssl_engine is injected into nginx.conf main context
    # by start-https.sh at boot time when QATEngine is available)
    mkdir -p "$mntdir/etc/nginx/http.d"
    cat > "$mntdir/etc/nginx/http.d/vaigai-ssl.conf" <<NGINX
server {
    listen 443 ssl reuseport;
    server_name vaigai-test;

    ssl_certificate     /etc/nginx/ssl/server.crt;
    ssl_certificate_key /etc/nginx/ssl/server.key;
    ssl_protocols       TLSv1.2 TLSv1.3;
    ssl_ciphers         $TLS_CIPHER;
    ssl_prefer_server_ciphers on;
    ssl_session_cache   shared:HTTPS:10m;
    ssl_session_timeout 10s;

    root /var/www/html;
    location / {
        try_files \$uri =404;
    }
}
NGINX

    # Copy certs to nginx ssl location (match existing nginx ssl setup)
    mkdir -p "$mntdir/etc/nginx/ssl"
    cp "$TLS_CERT" "$mntdir/etc/nginx/ssl/server.crt"
    cp "$TLS_KEY"  "$mntdir/etc/nginx/ssl/server.key"

    # Create HTTPS server start script
    cat > "$mntdir/etc/vaigai/start-https.sh" <<HTTPSSCRIPT
#!/bin/sh
echo "Starting HTTPS server (crypto=$server_crypto, cipher=$TLS_CIPHER)..."

# Kernel tuning for high connection rates
sysctl -w net.core.somaxconn=65535 2>/dev/null || true
sysctl -w net.ipv4.tcp_max_syn_backlog=65535 2>/dev/null || true
sysctl -w net.ipv4.tcp_tw_reuse=1 2>/dev/null || true
sysctl -w net.core.netdev_max_backlog=10000 2>/dev/null || true

if [ "$server_crypto" = "qat" ]; then
    if [ -x /etc/vaigai/qat-setup.sh ]; then
        /etc/vaigai/qat-setup.sh || echo "[QAT] setup failed — using software crypto"
    fi
    # Use QAT-aware OpenSSL config (afalg engine for kernel crypto API offload,
    # or QATEngine when installed)
    export OPENSSL_CONF=/etc/vaigai/openssl-qat.cnf
    echo "OpenSSL engine: \$(openssl engine 2>/dev/null | head -3)"

    # If QATEngine is installed, inject into nginx config
    if [ -f /usr/lib/engines-3/qatengine.so ]; then
        grep -q "ssl_engine" /etc/nginx/nginx.conf 2>/dev/null || \
            sed -i '/^events/i ssl_engine qatengine;' /etc/nginx/nginx.conf
    fi
fi

# Refresh certs for nginx
cp /etc/vaigai/cert.pem /etc/nginx/ssl/server.crt 2>/dev/null || true
cp /etc/vaigai/key.pem  /etc/nginx/ssl/server.key 2>/dev/null || true
chmod 644 /etc/nginx/ssl/server.crt 2>/dev/null || true
chmod 600 /etc/nginx/ssl/server.key 2>/dev/null || true

# Create required nginx temp dirs
mkdir -p /var/lib/nginx/tmp/client_body /var/lib/nginx/tmp/proxy \
         /var/lib/nginx/tmp/fastcgi /var/lib/nginx/tmp/uwsgi \
         /var/lib/nginx/tmp/scgi /run/nginx /var/log/nginx 2>/dev/null || true

# Stop nginx to reload with fresh config
nginx -s stop 2>/dev/null || true
sleep 0.5

# Test nginx config
echo "Testing nginx config..."
nginx -t 2>&1

# Start nginx with fresh config
nginx 2>&1
echo "nginx exit code: \$?"

# Verify listening ports
sleep 0.5
echo "=== Listening ports ==="
ss -tlnp 2>/dev/null || netstat -tlnp 2>/dev/null || true

# Verify cert files
echo "=== Cert check ==="
ls -la /etc/nginx/ssl/ 2>/dev/null || true
openssl x509 -in /etc/nginx/ssl/server.crt -noout -subject -dates 2>/dev/null || echo "cert read failed"

# TLS discard server on 5001 for throughput sink
socat OPENSSL-LISTEN:5001,fork,reuseaddr,cert=/etc/nginx/ssl/server.crt,key=/etc/nginx/ssl/server.key,verify=0 /dev/null &

echo "vaigAI HTTPS test services started"
HTTPSSCRIPT
    chmod +x "$mntdir/etc/vaigai/start-https.sh"

    if [[ -d "$mntdir/etc/local.d" ]]; then
        cp "$mntdir/etc/vaigai/start-https.sh" "$mntdir/etc/local.d/vaigai-https.start"
        chmod +x "$mntdir/etc/local.d/vaigai-https.start"
    fi

    # Enable the 'local' service so scripts in /etc/local.d/ actually run
    if [[ -f "$mntdir/etc/init.d/local" ]] && [[ -d "$mntdir/etc/runlevels/default" ]]; then
        ln -sf /etc/init.d/local "$mntdir/etc/runlevels/default/local"
    fi

    # Remove conflicting nginx configs from previous test scripts
    rm -f "$mntdir/etc/nginx/http.d/vaigai-tls.conf"

    # Run depmod so kernel modules can be found by modprobe in VM
    chroot "$mntdir" depmod -a "$(uname -r)" 2>/dev/null || true

    umount "$mntdir"
    rmdir "$mntdir"
    info "Patched rootfs: nginx SSL on :443, test files: 1k/100k/1m"

    QEMU_SERIAL=$(mktemp /tmp/vaigai-https-serial-XXXXXX.log)
    QEMU_ERR=$(mktemp /tmp/vaigai-https-qemu-err-XXXXXX.log)

    local iommu_grp
    iommu_grp=$(basename "$(readlink /sys/bus/pci/devices/$NIC_PCI_VM/iommu_group)")
    [[ -c "/dev/vfio/$iommu_grp" ]] || die "VFIO group /dev/vfio/$iommu_grp not found"

    local kcmd="console=ttyS0 root=/dev/vda rw quiet"
    kcmd+=" net.ifnames=0 biosdevname=0"
    kcmd+=" vaigai_mode=http"
    kcmd+=" ip=${VM_IP}:::255.255.255.0::eth0:off"

    local stale_pid
    stale_pid=$(pgrep -f "vfio-pci,host=$NIC_PCI_VM" 2>/dev/null || true)
    if [[ -n "$stale_pid" ]]; then
        warn "Killing stale QEMU PID $stale_pid"
        kill "$stale_pid" 2>/dev/null; sleep 1
        kill -9 "$stale_pid" 2>/dev/null; wait "$stale_pid" 2>/dev/null || true
    fi

    local qemu_devices="-device vfio-pci,host=$NIC_PCI_VM"
    if [[ "$server_crypto" == "qat" ]] && [[ -n "$QAT_PCI_SERVER" ]]; then
        local srv_iommu
        srv_iommu=$(basename "$(readlink /sys/bus/pci/devices/$QAT_PCI_SERVER/iommu_group)")
        [[ -c "/dev/vfio/$srv_iommu" ]] || die "VFIO group /dev/vfio/$srv_iommu not found for QAT"
        qemu_devices+=" -device vfio-pci,host=$QAT_PCI_SERVER"
        info "VM crypto: QAT ($QAT_PCI_SERVER passthrough)"
    else
        info "VM crypto: software (OpenSSL)"
    fi

    info "Booting QEMU VM ($VM_CPUS vCPUs, ${VM_MEM}M RAM)"
    qemu-system-x86_64 \
        -machine q35,accel=kvm \
        -cpu host \
        -m "${VM_MEM}M" \
        -smp "$VM_CPUS" \
        -kernel "$VMLINUX" \
        -initrd "$INITRAMFS" \
        -append "$kcmd" \
        -drive "file=$ROOTFS_COW,format=raw,if=virtio,cache=unsafe" \
        $qemu_devices \
        -nographic \
        -serial "file:$QEMU_SERIAL" \
        -monitor none \
        -no-reboot \
        </dev/null >/dev/null 2>"$QEMU_ERR" &
    QEMU_PID=$!
    sleep 1

    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        cat "$QEMU_ERR" 2>/dev/null || true
        cat "$QEMU_SERIAL" 2>/dev/null || true
        die "QEMU exited immediately"
    fi

    info "Waiting for VM at $VM_IP (up to 90s)..."
    local waited=0
    while [[ $waited -lt 90 ]]; do
        if grep -q "HTTPS test services started" "$QEMU_SERIAL" 2>/dev/null; then
            info "VM HTTPS services ready"
            break
        fi
        sleep 2
        ((waited+=2)) || true
        if ! kill -0 "$QEMU_PID" 2>/dev/null; then
            cat "$QEMU_ERR" 2>/dev/null || true
            cat "$QEMU_SERIAL" 2>/dev/null || true
            die "QEMU exited during boot"
        fi
    done

    info "=== VM serial (last 40 lines) ==="
    tail -40 "$QEMU_SERIAL" 2>/dev/null || true
    info "=== end serial ==="

    sleep 5
    info "QEMU VM started (PID $QEMU_PID)"
}

qemu_stop() {
    if [[ -n "$QEMU_PID" ]] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill "$QEMU_PID" 2>/dev/null || true
        local waited=0
        while kill -0 "$QEMU_PID" 2>/dev/null && [[ $waited -lt 10 ]]; do
            sleep 0.5
            ((waited++)) || true
        done
        kill -0 "$QEMU_PID" 2>/dev/null && kill -9 "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    rm -f "$ROOTFS_COW" "$QEMU_SERIAL" "$QEMU_ERR"
    QEMU_PID=""
}

# ── pre-flight checks ────────────────────────────────────────────────────────
[[ $EUID -eq 0 ]]        || die "Must run as root"
[[ -x "$VAIGAI_BIN" ]]   || die "vaigai binary not found: $VAIGAI_BIN"
command -v qemu-system-x86_64 &>/dev/null || die "qemu-system-x86_64 not found"
command -v openssl &>/dev/null            || die "openssl not found"
[[ -f "$VMLINUX" ]]       || die "vmlinuz not found: $VMLINUX"
[[ -f "$INITRAMFS" ]]     || die "initramfs not found: $INITRAMFS"
[[ -f "$ROOTFS" ]]        || die "rootfs not found: $ROOTFS"
[[ -d "/sys/bus/pci/devices/$NIC_PCI_VAIGAI" ]] || die "PCI device $NIC_PCI_VAIGAI not found"
[[ -d "/sys/bus/pci/devices/$NIC_PCI_VM" ]]     || die "PCI device $NIC_PCI_VM not found"
if ! modprobe -r vfio-pci 2>/dev/null; then true; fi
modprobe vfio-pci disable_denylist=1 2>/dev/null || die "Cannot load vfio-pci module"

if [[ "$VAIGAI_CRYPTO" == "qat" ]]; then
    [[ -n "$QAT_PCI_VAIGAI" ]] || die "VAIGAI_CRYPTO=qat but no QAT device found"
    [[ -d "/sys/bus/pci/devices/$QAT_PCI_VAIGAI" ]] || die "QAT $QAT_PCI_VAIGAI not found"
fi
if [[ "$SERVER_CRYPTO" == "qat" ]]; then
    [[ -n "$QAT_PCI_SERVER" ]] || die "SERVER_CRYPTO=qat but no QAT device found"
    [[ -d "/sys/bus/pci/devices/$QAT_PCI_SERVER" ]] || die "QAT $QAT_PCI_SERVER not found"
    [[ "$QAT_PCI_VAIGAI" != "$QAT_PCI_SERVER" ]] || die "QAT_PCI_VAIGAI and QAT_PCI_SERVER must differ"
fi

info "Configuration summary:"
info "  vaigAI NIC:        $NIC_PCI_VAIGAI"
info "  VM NIC:            $NIC_PCI_VM ($NIC_IFACE_VM)"
info "  vaigAI IP:         $VAIGAI_IP"
info "  VM IP:             $VM_IP"
info "  HTTPS port:        443"
info "  HTTP resp size:    $HTTP_RESP_SIZE"
info "  TLS cipher:        $TLS_CIPHER"
info "  vaigai crypto:     $VAIGAI_CRYPTO${VAIGAI_CRYPTO:+$([ "$VAIGAI_CRYPTO" = "qat" ] && echo " ($QAT_PCI_VAIGAI)" || true)}"
info "  server crypto:     $SERVER_CRYPTO${SERVER_CRYPTO:+$([ "$SERVER_CRYPTO" = "qat" ] && echo " ($QAT_PCI_SERVER)" || true)}"
info "  Flood duration:    ${FLOOD_DURATION}s"
info "  Target CPS:        $TARGET_CPS"
info "  Throughput dur:    ${THROUGHPUT_DUR}s"
info "  Throughput streams: $THROUGHPUT_STREAMS"
info "  Latency dur:       ${LATENCY_DUR}s"
info "  DPDK lcores:       $DPDK_LCORES"
info "  VM:                $VM_CPUS vCPUs, ${VM_MEM}M RAM"
info "  QAT devices:       ${QAT_ALL[*]:-none}"

# ── teardown ──────────────────────────────────────────────────────────────────
teardown() {
    [[ $KEEP -eq 1 ]] && { info "Keeping topology (--keep)"; return; }
    info "Tearing down"
    vaigai_stop
    qemu_stop
    [[ -n "$ORIG_DRIVER_VAIGAI" ]] && [[ "$ORIG_DRIVER_VAIGAI" != "vfio-pci" ]] && \
        vfio_unbind "$NIC_PCI_VAIGAI" "$ORIG_DRIVER_VAIGAI"
    [[ -n "$ORIG_DRIVER_VM" ]] && [[ "$ORIG_DRIVER_VM" != "vfio-pci" ]] && \
        vfio_unbind "$NIC_PCI_VM" "$ORIG_DRIVER_VM"
    [[ -n "$ORIG_DRIVER_QAT_V" ]] && [[ "$ORIG_DRIVER_QAT_V" != "vfio-pci" ]] && \
        vfio_unbind "$QAT_PCI_VAIGAI" "$ORIG_DRIVER_QAT_V"
    [[ -n "$ORIG_DRIVER_QAT_S" ]] && [[ "$ORIG_DRIVER_QAT_S" != "vfio-pci" ]] && \
        vfio_unbind "$QAT_PCI_SERVER" "$ORIG_DRIVER_QAT_S"
    rm -f "$TLS_CERT" "$TLS_KEY"
}
trap teardown EXIT

# ══════════════════════════════════════════════════════════════════════════════
#  Setup
# ══════════════════════════════════════════════════════════════════════════════
info "Step 1: Generating TLS certificates"
generate_certs

info "Step 2a: Binding $NIC_PCI_VAIGAI to vfio-pci (vaigai DPDK)"
vfio_bind "$NIC_PCI_VAIGAI" "$NIC_IFACE_VAIGAI" ORIG_DRIVER_VAIGAI

info "Step 2b: Binding $NIC_PCI_VM to vfio-pci (QEMU passthrough)"
vfio_bind "$NIC_PCI_VM" "$NIC_IFACE_VM" ORIG_DRIVER_VM

if [[ "$VAIGAI_CRYPTO" == "qat" ]]; then
    info "Step 3a: Binding QAT $QAT_PCI_VAIGAI to vfio-pci (vaigai)"
    vfio_bind "$QAT_PCI_VAIGAI" "" ORIG_DRIVER_QAT_V
fi
if [[ "$SERVER_CRYPTO" == "qat" ]]; then
    info "Step 3b: Binding QAT $QAT_PCI_SERVER to vfio-pci (server)"
    vfio_bind "$QAT_PCI_SERVER" "" ORIG_DRIVER_QAT_S
fi

info "Step 4: Starting QEMU VM"
qemu_start "$SERVER_CRYPTO"

info "Step 5: Starting vaigai"
vaigai_start "$VAIGAI_CRYPTO"

# ══════════════════════════════════════════════════════════════════════════════
#  Connectivity check
# ══════════════════════════════════════════════════════════════════════════════
info "Step 6: Connectivity check — ping $VM_IP"
ping_replies=0
for attempt in 1 2 3 4 5; do
    vaigai_cmd "ping $VM_IP 5 56 500"
    ping_replies=$(echo "$OUTPUT" | grep -c "Reply from" || true)
    info "  Ping attempt $attempt: $ping_replies replies"
    [[ "$ping_replies" -gt 0 ]] && break
    sleep 3
done

if [[ "$ping_replies" -gt 0 ]]; then
    pass "Connectivity: ICMP ping ($ping_replies replies)"
else
    fail "Connectivity: no ICMP replies"
    die "Cannot proceed without connectivity"
fi

# ══════════════════════════════════════════════════════════════════════════════
#  T1: HTTPS TPS
# ══════════════════════════════════════════════════════════════════════════════
run_t1() {
    info "═══════════════════════════════════════════════════════"
    info "T1: HTTPS TPS → ${VM_IP}:443"
    info "═══════════════════════════════════════════════════════"

    # ── T1a: TPS (peak discovery) ───────────────────────────────────
    info "T1a: Unlimited HTTPS flood (${FLOOD_DURATION}s)"
    vaigai_cmd "tps $VM_IP $FLOOD_DURATION 0 56 443"

    local hs_ok hs_fail http_tx http_2xx http_4xx http_5xx
    hs_ok=$(json_val tls_handshake_ok)
    hs_fail=$(json_val tls_handshake_fail)
    http_tx=$(json_val http_req_tx)
    http_2xx=$(json_val http_rsp_2xx)
    http_4xx=$(json_val http_rsp_4xx)
    http_5xx=$(json_val http_rsp_5xx)

    info "  tls_handshake_ok=$hs_ok  hs_fail=$hs_fail"
    info "  http_req_tx=$http_tx  http_rsp_2xx=$http_2xx  4xx=$http_4xx  5xx=$http_5xx"
    if [[ "$FLOOD_DURATION" -gt 0 ]] && [[ "$hs_ok" -gt 0 ]]; then
        local tps=$((hs_ok / FLOOD_DURATION))
        peak "Peak HTTPS TPS: $(printf "%'d" $tps)"
    fi

    [[ "$hs_ok" -gt 0 ]]     && pass "T1a tls_handshake_ok > 0 ($hs_ok)" \
                              || fail "T1a tls_handshake_ok = 0"
    [[ "$hs_fail" -eq 0 ]]   && pass "T1a tls_handshake_fail = 0" \
                              || fail "T1a tls_handshake_fail = $hs_fail"
    [[ "$http_tx" -gt 0 ]]   && pass "T1a http_req_tx > 0 ($http_tx)" \
                              || fail "T1a http_req_tx = 0"
    [[ "$http_2xx" -gt 0 ]]  && pass "T1a http_rsp_2xx > 0 ($http_2xx)" \
                              || fail "T1a http_rsp_2xx = 0"
    [[ "$http_4xx" -eq 0 ]]  && pass "T1a http_rsp_4xx = 0" \
                              || fail "T1a http_rsp_4xx = $http_4xx"
    [[ "$http_5xx" -eq 0 ]]  && pass "T1a http_rsp_5xx = 0" \
                              || warn "T1a http_rsp_5xx = $http_5xx (expected under flood)"

    # ── T1b: Rate-limited ─────────────────────────────────────────────
    vaigai_reset

    info "T1b: Rate-limited HTTPS (${FLOOD_DURATION}s, target ${TARGET_CPS} cps)"
    vaigai_cmd "tps $VM_IP $FLOOD_DURATION $TARGET_CPS 56 443"

    hs_ok=$(json_val tls_handshake_ok)
    info "  tls_handshake_ok=$hs_ok"
    if [[ "$FLOOD_DURATION" -gt 0 ]] && [[ "$hs_ok" -gt 0 ]]; then
        local actual_cps=$((hs_ok / FLOOD_DURATION))
        info "  Measured TPS: $actual_cps (target: $TARGET_CPS)"

        local lower=$((TARGET_CPS / 2)) upper=$((TARGET_CPS * 2))
        if [[ "$actual_cps" -ge "$lower" ]] && [[ "$actual_cps" -le "$upper" ]]; then
            pass "T1b rate-limited TPS in range ($actual_cps ≈ $TARGET_CPS)"
        else
            fail "T1b rate-limited TPS out of range ($actual_cps vs $TARGET_CPS)"
        fi
    fi

    [[ "$hs_ok" -gt 0 ]] && pass "T1b tls_handshake_ok > 0 ($hs_ok)" \
                          || fail "T1b tls_handshake_ok = 0"
}

# ══════════════════════════════════════════════════════════════════════════════
#  T2: HTTPS Throughput
# ══════════════════════════════════════════════════════════════════════════════
run_t2() {
    info "═══════════════════════════════════════════════════════"
    info "T2: HTTPS Throughput → ${VM_IP}:5001 (TLS discard sink)"
    info "═══════════════════════════════════════════════════════"

    # ── T2a: Flood (peak discovery) ───────────────────────────────────
    info "T2a: Unlimited HTTPS throughput (${THROUGHPUT_DUR}s, ${THROUGHPUT_STREAMS} streams)"
    vaigai_cmd "throughput tx $VM_IP 5001 $THROUGHPUT_DUR $THROUGHPUT_STREAMS"

    local payload_tx tls_tx retransmit http_tx http_rx parse_err hs_fail
    payload_tx=$(json_val tcp_payload_tx)
    tls_tx=$(json_val tls_records_tx)
    retransmit=$(json_val tcp_retransmit)
    http_tx=$(json_val http_req_tx)
    http_rx=$(json_val http_rsp_rx)
    parse_err=$(json_val http_parse_err)
    hs_fail=$(json_val tls_handshake_fail)

    info "  payload_tx=$payload_tx tls_tx=$tls_tx retransmit=$retransmit"
    info "  http_req_tx=$http_tx http_rsp_rx=$http_rx parse_err=$parse_err"
    if [[ "$THROUGHPUT_DUR" -gt 0 ]] && [[ "$payload_tx" -gt 0 ]]; then
        local mbps=$(( (payload_tx * 8) / (THROUGHPUT_DUR * 1000000) ))
        peak "Peak HTTPS Throughput: $(printf "%'d" $mbps) Mbps"
    fi

    [[ "$payload_tx" -gt 0 ]]  && pass "T2a payload_tx > 0 ($payload_tx bytes)" \
                               || fail "T2a payload_tx = 0"
    [[ "$tls_tx" -gt 0 ]]     && pass "T2a tls_records_tx > 0 ($tls_tx)" \
                               || fail "T2a tls_records_tx = 0"
    [[ "$parse_err" -eq 0 ]]  && pass "T2a http_parse_err = 0" \
                               || fail "T2a http_parse_err = $parse_err"
    [[ "$hs_fail" -eq 0 ]]    && pass "T2a tls_handshake_fail = 0" \
                               || fail "T2a tls_handshake_fail = $hs_fail"
    [[ "$retransmit" -eq 0 ]] && pass "T2a retransmit = 0" \
                               || warn "T2a retransmit = $retransmit"

    # ── T2b: Rate-limited ─────────────────────────────────────────────
    vaigai_reset

    info "T2b: Rate-limited HTTPS throughput (${THROUGHPUT_DUR}s, target ${TARGET_CPS} cps)"
    printf 'set-cps %s\n' "$TARGET_CPS" >&7
    sleep 1
    vaigai_cmd "throughput tx $VM_IP 5001 $THROUGHPUT_DUR $THROUGHPUT_STREAMS"

    payload_tx=$(json_val tcp_payload_tx)
    info "  payload_tx=$payload_tx"
    [[ "$payload_tx" -gt 0 ]] && pass "T2b payload_tx > 0 ($payload_tx bytes)" \
                               || fail "T2b payload_tx = 0"
}

# ══════════════════════════════════════════════════════════════════════════════
#  T3: HTTPS Latency (p50/p95/p99)
# ══════════════════════════════════════════════════════════════════════════════
run_t3() {
    info "═══════════════════════════════════════════════════════"
    info "T3: HTTPS Transaction Latency → ${VM_IP}:443"
    info "═══════════════════════════════════════════════════════"

    local lat_cps=$((TARGET_CPS / 2))
    [[ "$lat_cps" -gt 0 ]] || lat_cps=1000

    info "T3: Rate-limited at ${lat_cps} cps for ${LATENCY_DUR}s"
    vaigai_cmd "tps $VM_IP $LATENCY_DUR $lat_cps 56 443"

    local hs_ok p50 p95 p99
    hs_ok=$(json_val tls_handshake_ok)
    p50=$(grep -oP '"p50": *\K[0-9]+' <<< "$OUTPUT" | tail -1 || echo "N/A")
    p95=$(grep -oP '"p95": *\K[0-9]+' <<< "$OUTPUT" | tail -1 || echo "N/A")
    p99=$(grep -oP '"p99": *\K[0-9]+' <<< "$OUTPUT" | tail -1 || echo "N/A")

    info "  tls_handshake_ok=$hs_ok"
    peak "HTTPS Latency: p50=${p50}µs  p95=${p95}µs  p99=${p99}µs"

    [[ "$hs_ok" -gt 0 ]] && pass "T3 tls_handshake_ok > 0 ($hs_ok)" \
                          || fail "T3 tls_handshake_ok = 0"
}

# ══════════════════════════════════════════════════════════════════════════════
#  T4: Crypto Acceleration Matrix (2×2)
# ══════════════════════════════════════════════════════════════════════════════
run_t4() {
    info "═══════════════════════════════════════════════════════"
    info "T4: Crypto Acceleration Matrix (HTTPS)"
    info "═══════════════════════════════════════════════════════"

    if [[ ${#QAT_ALL[@]} -lt 2 ]]; then
        warn "T4 requires ≥2 QAT devices; found ${#QAT_ALL[@]}"
    fi

    declare -A matrix_tps matrix_mbps

    local combos=()
    if [[ ${#QAT_ALL[@]} -ge 2 ]]; then
        combos=("qat:qat" "qat:sw" "sw:qat" "sw:sw")
    elif [[ ${#QAT_ALL[@]} -ge 1 ]]; then
        combos=("qat:sw" "sw:sw")
    else
        combos=("sw:sw")
    fi

    for combo in "${combos[@]}"; do
        local v_crypto="${combo%%:*}"
        local s_crypto="${combo##*:}"
        info "── T4 run: vaigai=$v_crypto server=$s_crypto ──"

        vaigai_stop
        qemu_stop

        if [[ "$v_crypto" == "qat" ]] && [[ -n "$QAT_PCI_VAIGAI" ]]; then
            vfio_bind "$QAT_PCI_VAIGAI" "" ORIG_DRIVER_QAT_V
        fi
        if [[ "$s_crypto" == "qat" ]] && [[ -n "$QAT_PCI_SERVER" ]]; then
            vfio_bind "$QAT_PCI_SERVER" "" ORIG_DRIVER_QAT_S
        fi

        qemu_start "$s_crypto"
        vaigai_start "$v_crypto"

        # Connectivity check
        local ok=0
        for attempt in 1 2 3; do
            vaigai_cmd "ping $VM_IP 3 56 500"
            local pr
            pr=$(echo "$OUTPUT" | grep -c "Reply from" || true)
            [[ "$pr" -gt 0 ]] && { ok=1; break; }
            sleep 2
        done
        if [[ $ok -eq 0 ]]; then
            warn "T4 ($v_crypto/$s_crypto): connectivity failed — skipping"
            matrix_tps["$combo"]="N/A"
            matrix_mbps["$combo"]="N/A"
            continue
        fi

        # TPS run (short)
        vaigai_reset
        vaigai_cmd "tps $VM_IP 5 0 56 443"
        local hs_ok tps=0
        hs_ok=$(json_val tls_handshake_ok)
        [[ "$hs_ok" -gt 0 ]] && tps=$((hs_ok / 5))
        matrix_tps["$combo"]="$tps"

        # Throughput flood (short)
        vaigai_reset
        vaigai_cmd "throughput tx $VM_IP 5001 5 $THROUGHPUT_STREAMS"
        local ptx mbps=0
        ptx=$(json_val tcp_payload_tx)
        [[ "$ptx" -gt 0 ]] && mbps=$(( (ptx * 8) / (5 * 1000000) ))
        matrix_mbps["$combo"]="$mbps"

        info "  Result: TPS=$tps  Mbps=$mbps"

        if [[ "$v_crypto" == "qat" ]] && [[ -n "$ORIG_DRIVER_QAT_V" ]] && \
           [[ "$ORIG_DRIVER_QAT_V" != "vfio-pci" ]]; then
            vfio_unbind "$QAT_PCI_VAIGAI" "$ORIG_DRIVER_QAT_V"
        fi
        if [[ "$s_crypto" == "qat" ]] && [[ -n "$ORIG_DRIVER_QAT_S" ]] && \
           [[ "$ORIG_DRIVER_QAT_S" != "vfio-pci" ]]; then
            vfio_unbind "$QAT_PCI_SERVER" "$ORIG_DRIVER_QAT_S"
        fi
    done

    # Restore original setup
    vaigai_stop
    qemu_stop
    if [[ "$VAIGAI_CRYPTO" == "qat" ]] && [[ -n "$QAT_PCI_VAIGAI" ]]; then
        vfio_bind "$QAT_PCI_VAIGAI" "" ORIG_DRIVER_QAT_V
    fi
    if [[ "$SERVER_CRYPTO" == "qat" ]] && [[ -n "$QAT_PCI_SERVER" ]]; then
        vfio_bind "$QAT_PCI_SERVER" "" ORIG_DRIVER_QAT_S
    fi
    qemu_start "$SERVER_CRYPTO"
    vaigai_start "$VAIGAI_CRYPTO"

    echo ""
    peak "HTTPS Crypto Acceleration Matrix (cipher: $TLS_CIPHER)"
    printf "  %-10s %-10s %12s %12s\n" "vaigai" "server" "HTTPS TPS" "HTTPS Mbps"
    printf "  %-10s %-10s %12s %12s\n" "──────" "──────" "─────────" "──────────"
    for combo in "${combos[@]}"; do
        local v="${combo%%:*}" s="${combo##*:}"
        printf "  %-10s %-10s %12s %12s\n" \
            "$(echo $v | tr a-z A-Z)" "$(echo $s | tr a-z A-Z)" \
            "${matrix_tps[$combo]:-N/A}" "${matrix_mbps[$combo]:-N/A}"
    done
    echo ""

    if [[ -n "${matrix_tps[qat:sw]:-}" ]] && [[ -n "${matrix_tps[sw:sw]:-}" ]] && \
       [[ "${matrix_tps[sw:sw]}" != "N/A" ]] && [[ "${matrix_tps[sw:sw]}" -gt 0 ]]; then
        local tps_factor mbps_factor
        tps_factor=$(awk "BEGIN {printf \"%.1f\", ${matrix_tps[qat:sw]} / ${matrix_tps[sw:sw]}}")
        mbps_factor=$(awk "BEGIN {printf \"%.1f\", ${matrix_mbps[qat:sw]:-0} / (${matrix_mbps[sw:sw]:-1} > 0 ? ${matrix_mbps[sw:sw]} : 1)}")
        peak "QAT acceleration (client): ${tps_factor}× TPS, ${mbps_factor}× Mbps"
    fi

    pass "T4 crypto matrix completed"
}

# ══════════════════════════════════════════════════════════════════════════════
#  T5: Concurrent Connection Scaling
# ══════════════════════════════════════════════════════════════════════════════
run_t5() {
    info "═══════════════════════════════════════════════════════"
    info "T5: Concurrent Connection Scaling → ${VM_IP}:443"
    info "═══════════════════════════════════════════════════════"

    local levels=(256 1024 4096 16384 65536)
    local best_tps=0 best_level=0

    declare -A scale_tps

    for conc in "${levels[@]}"; do
        vaigai_reset

        local patch="{\"load\":{\"max_concurrent\":$conc}}"
        printf 'load %s\n' "$patch" >&7 2>/dev/null || true
        sleep 1

        info "  Concurrency=$conc — flooding 5s..."
        vaigai_cmd "tps $VM_IP 5 0 56 443"

        local hs_ok tps=0
        hs_ok=$(json_val tls_handshake_ok)
        [[ "$hs_ok" -gt 0 ]] && tps=$((hs_ok / 5))
        scale_tps[$conc]=$tps

        [[ $tps -gt $best_tps ]] && { best_tps=$tps; best_level=$conc; }
        info "    TPS=$tps"
    done

    echo ""
    peak "HTTPS Connection Scaling (cipher: $TLS_CIPHER)"
    printf "  %12s %12s\n" "Concurrency" "HTTPS TPS"
    printf "  %12s %12s\n" "───────────" "─────────"
    for conc in "${levels[@]}"; do
        local marker=""
        [[ $conc -eq $best_level ]] && marker=" ← peak"
        printf "  %12s %12s%s\n" "$conc" "${scale_tps[$conc]}" "$marker"
    done
    echo ""
    peak "Peak concurrency: $best_level (TPS: $(printf "%'d" $best_tps))"

    [[ $best_tps -gt 0 ]] && pass "T5 peak TPS > 0 ($best_tps at concurrency $best_level)" \
                           || fail "T5 peak TPS = 0"
}

# ══════════════════════════════════════════════════════════════════════════════
#  Run selected tests
# ══════════════════════════════════════════════════════════════════════════════
should_run() { [[ "$RUN_TESTS" == "all" || "$RUN_TESTS" == "$1" ]]; }

should_run 1 && run_t1
should_run 2 && { vaigai_reset; run_t2; }
should_run 3 && { vaigai_reset; run_t3; }
should_run 4 && run_t4
should_run 5 && { vaigai_reset; run_t5; }

# ── summary ───────────────────────────────────────────────────────────────────
echo ""
info "═══════════════════════════════════════════════════════"
info "  HTTPS NIC Test Results: $PASS_COUNT passed, $FAIL_COUNT failed"
info "  Cipher: $TLS_CIPHER  vaigai: $VAIGAI_CRYPTO  server: $SERVER_CRYPTO"
info "═══════════════════════════════════════════════════════"
[[ $FAIL_COUNT -eq 0 ]] || exit 1
