#!/usr/bin/env bash
# ╔══════════════════════════════════════════════════════════════════════════════╗
# ║  vaigAI — Complete Test Environment Setup                                   ║
# ║                                                                              ║
# ║  Builds every dependency from source on a fresh Fedora 42+ x86_64 host:     ║
# ║    1. DPDK 24.11.1  (i40e + crypto_qat + tap/af_packet/mlx5)                ║
# ║    2. Firecracker microVM  (binary + guest kernel + rootfs)                  ║
# ║    3. QEMU VM rootfs  (Alpine 3.23 + nginx + openssl + QAT offload)         ║
# ║    4. QAT offload on both sides  (vaigai: DPDK crypto_qat, server: afalg)   ║
# ║    5. vaigAI itself                                                          ║
# ║                                                                              ║
# ║  Usage:                                                                      ║
# ║    bash scripts/test-env.sh              # build everything                  ║
# ║    bash scripts/test-env.sh --step dpdk  # build only DPDK                   ║
# ║    bash scripts/test-env.sh --step rootfs                                    ║
# ║    bash scripts/test-env.sh --step kernel                                    ║
# ║    bash scripts/test-env.sh --step vaigai                                    ║
# ║    bash scripts/test-env.sh --step verify                                    ║
# ║    bash scripts/test-env.sh --help                                           ║
# ║                                                                              ║
# ║  Result:                                                                     ║
# ║    /work/dpdk-stable-24.11.1/          DPDK source + build                   ║
# ║    /work/firecracker/vmlinux           Guest kernel (Firecracker)            ║
# ║    /work/firecracker/alpine.ext4       Rootfs (Firecracker TCP/ARP tests)    ║
# ║    /work/firecracker/rootfs.ext4      Rootfs (QEMU HTTP/HTTPS/TLS tests)    ║
# ║    vaigAI/build/vaigai                 Traffic generator binary              ║
# ║                                                                              ║
# ║  Hardware Requirements:                                                      ║
# ║    - Intel XXV710 (i40e) NIC pair with loopback cable                        ║
# ║    - Intel QAT DH895XCC  (optional, for crypto offload tests)                ║
# ║    - KVM support (/dev/kvm)                                                  ║
# ║    - IOMMU enabled  (intel_iommu=on in kernel cmdline)                       ║
# ╚══════════════════════════════════════════════════════════════════════════════╝
set -euo pipefail

# ── Layout ────────────────────────────────────────────────────────────────────
WORK="${WORK:-/work}"
DPDK_VER="24.11.1"
DPDK_DIR="$WORK/dpdk-stable-$DPDK_VER"
FC_DIR="$WORK/firecracker"
FC_VER="1.14.2"
KERNEL_VER="6.1.163"
ALPINE_VER="3.23"
ROOTFS="$FC_DIR/alpine.ext4"
ROOTFS_SIZE_MB=128
QEMU_ROOTFS="$FC_DIR/rootfs.ext4"
QEMU_ROOTFS_SIZE_MB=256
VAIGAI_DIR="$(cd "$(dirname "$0")/.." && pwd)"

# ── Colours ───────────────────────────────────────────────────────────────────
GRN='\033[0;32m'; CYN='\033[0;36m'; YLW='\033[0;33m'; RED='\033[0;31m'; NC='\033[0m'
info()  { echo -e "${CYN}[test-env]${NC} $*"; }
ok()    { echo -e "${GRN}[  OK   ]${NC} $*"; }
warn()  { echo -e "${YLW}[ WARN  ]${NC} $*"; }
die()   { echo -e "${RED}[ FATAL ]${NC} $*" >&2; exit 1; }
banner(){ echo -e "\n${GRN}════════════════════════════════════════════════════════${NC}"; echo -e "${GRN}  $*${NC}"; echo -e "${GRN}════════════════════════════════════════════════════════${NC}\n"; }

# ── Argument parsing ──────────────────────────────────────────────────────────
STEP="all"
usage() {
    sed -n '/^# ║/s/^# ║ *//p' "$0" | sed 's/ *║$//'
    echo ""
    echo "Steps: all | host-packages | dpdk | firecracker | kernel | rootfs | qemu-rootfs | vm-initramfs | qat-host | vaigai | verify"
    exit 0
}
while [[ $# -gt 0 ]]; do
    case "$1" in
        --step) STEP="$2"; shift 2 ;;
        -h|--help) usage ;;
        *) die "Unknown option: $1" ;;
    esac
done
should_run() { [[ "$STEP" == "all" || "$STEP" == "$1" ]]; }

[[ $EUID -eq 0 ]] || die "Run as root"
mkdir -p "$WORK"

# ══════════════════════════════════════════════════════════════════════════════
#  STEP 1: Host Packages
# ══════════════════════════════════════════════════════════════════════════════
#
#  Installs every library and tool needed on the host.
#  Idempotent — safe to re-run.
#
install_host_packages() {
    banner "Step 1: Host Packages (Fedora)"

    dnf install -y \
        gcc gcc-c++ meson ninja-build pkgconfig make cmake \
        numactl-devel rdma-core-devel libibverbs-devel libmlx5 \
        openssl-devel readline-devel libbpf-devel jansson-devel libmicrohttpd-devel \
        python3-pyelftools python3-pip nasm \
        socat iproute bridge-utils podman \
        qemu-system-x86-core \
        elfutils-libelf-devel flex bison bc perl \
        curl wget tar xz gzip \
        2>&1 | tail -5

    ok "Host packages installed"
}

# ══════════════════════════════════════════════════════════════════════════════
#  STEP 2: DPDK 24.11.1
# ══════════════════════════════════════════════════════════════════════════════
#
#  Builds DPDK with these driver groups:
#
#    net/       i40e, tap, af_packet, af_xdp, mlx5   — NIC PMDs
#    common/    mlx5, qat                             — shared drivers
#    bus/       pci, vdev, auxiliary                   — bus drivers
#
#  The `common/qat` driver provides BOTH crypto_qat AND compress_qat.
#  It links against OpenSSL for fallback and against libqat/libusdm if present.
#
#  Installed to /usr/local so pkg-config finds `libdpdk` automatically.
#
build_dpdk() {
    banner "Step 2: DPDK $DPDK_VER"

    if [[ ! -d "$DPDK_DIR" ]]; then
        info "Downloading DPDK $DPDK_VER"
        cd "$WORK"
        curl -LO "https://fast.dpdk.org/rel/dpdk-${DPDK_VER}.tar.xz"
        tar xf "dpdk-${DPDK_VER}.tar.xz"
        rm -f "dpdk-${DPDK_VER}.tar.xz"
    fi

    cd "$DPDK_DIR"

    # ── Driver list ──────────────────────────────────────────────────
    #   common/qat   →  crypto_qat PMD (QAT DH895XCC / C62x / 4xxx)
    #   net/i40e     →  XXV710 / XL710 25/40 GbE NIC
    #   net/tap      →  TAP PMD for veth/bridge tests
    #   net/af_packet → AF_PACKET PMD for veth tests
    #   net/af_xdp   →  AF_XDP PMD (optional, needs libbpf)
    #   net/mlx5     →  Mellanox ConnectX NIC
    local drivers="net/af_packet,net/af_xdp,net/tap,net/mlx5,net/i40e"
    drivers+=",common/mlx5,common/qat"
    drivers+=",bus/auxiliary,bus/pci,bus/vdev"

    info "Configuring: enable_drivers=$drivers"
    meson setup builddir \
        -Ddisable_apps='*' \
        -Denable_drivers="$drivers" \
        -Dtests=false \
        -Dexamples='' \
        -Dprefix=/usr/local \
        --wipe 2>&1 | tail -20

    info "Building ($(nproc) jobs)"
    ninja -C builddir -j"$(nproc)" 2>&1 | tail -5

    info "Installing to /usr/local"
    ninja -C builddir install
    ldconfig

    # Verify
    pkg-config --modversion libdpdk || die "libdpdk not found after install"
    ok "DPDK $DPDK_VER installed ($(pkg-config --modversion libdpdk))"
    ok "  crypto_qat: $(find builddir/drivers -name 'librte_common_qat*' | head -1 || echo 'NOT FOUND')"
    ok "  net_i40e:   $(find builddir/drivers -name 'librte_net_i40e*' | head -1 || echo 'NOT FOUND')"
}

# ══════════════════════════════════════════════════════════════════════════════
#  STEP 3: Firecracker Binary
# ══════════════════════════════════════════════════════════════════════════════
#
#  Firecracker is used by: tcp_tap.sh, udp_veth.sh
#  QEMU tests (tls_nic.sh, https_nic.sh) use qemu-system-x86_64 instead.
#
install_firecracker() {
    banner "Step 3: Firecracker v$FC_VER"

    mkdir -p "$FC_DIR"

    if command -v firecracker &>/dev/null; then
        ok "Firecracker already installed: $(firecracker --version 2>&1 | head -1)"
        return
    fi

    cd /tmp
    curl -Lo "fc.tgz" \
        "https://github.com/firecracker-microvm/firecracker/releases/download/v${FC_VER}/firecracker-v${FC_VER}-x86_64.tgz"
    tar xf fc.tgz
    cp "release-v${FC_VER}-x86_64/firecracker-v${FC_VER}-x86_64" /usr/local/bin/firecracker
    chmod +x /usr/local/bin/firecracker
    rm -rf "release-v${FC_VER}-x86_64" fc.tgz

    ok "Firecracker $(firecracker --version 2>&1 | head -1)"
}

# ══════════════════════════════════════════════════════════════════════════════
#  STEP 4: Guest Kernel (Linux 6.1.x)
# ══════════════════════════════════════════════════════════════════════════════
#
#  Two consumers, different requirements:
#
#  ┌──────────────┬───────────────────────────────────────────────────┐
#  │ Consumer     │ Kernel requirements                               │
#  ├──────────────┼───────────────────────────────────────────────────┤
#  │ Firecracker  │ Minimal: virtio-net, virtio-blk, ext4, serial,   │
#  │ (tcp/udp)    │ no modules, statically linked ELF vmlinux         │
#  ├──────────────┼───────────────────────────────────────────────────┤
#  │ QEMU         │ Uses HOST kernel (/boot/vmlinuz-$(uname -r))     │
#  │ (tls/https)  │ + host initramfs for i40e + vfio-pci + QAT       │
#  └──────────────┴───────────────────────────────────────────────────┘
#
#  This step builds the Firecracker kernel only.
#  QEMU tests boot the host kernel directly — no guest kernel build needed.
#
build_guest_kernel() {
    banner "Step 4: Guest Kernel (Linux $KERNEL_VER for Firecracker)"

    if [[ -f "$FC_DIR/vmlinux" ]]; then
        ok "Guest kernel exists: $FC_DIR/vmlinux"
        return
    fi

    local ksrc="$FC_DIR/linux-src"
    if [[ ! -d "$ksrc" ]]; then
        info "Downloading Linux $KERNEL_VER"
        mkdir -p "$ksrc"
        local major="${KERNEL_VER%%.*}"
        curl -L "https://cdn.kernel.org/pub/linux/kernel/v${major}.x/linux-${KERNEL_VER}.tar.xz" \
            | tar xJ -C "$ksrc" --strip-components=1
    fi

    cd "$ksrc"

    # ── Minimal config for Firecracker ───────────────────────────────
    #  Goal: smallest kernel that boots virtio-net + ext4 + serial.
    #  No modules, no PCI passthrough, no IOMMU — Firecracker is a microVM.
    info "Generating minimal .config"
    make defconfig
    cat >> .config <<'KCONF'
# ── Firecracker mandatory ──
CONFIG_VIRTIO_NET=y
CONFIG_VIRTIO_BLK=y
CONFIG_VIRTIO_MMIO=y
CONFIG_VIRTIO_CONSOLE=y
CONFIG_VIRTIO_PCI=y
CONFIG_VIRTIO_RING=y
CONFIG_EXT4_FS=y
CONFIG_SERIAL_8250=y
CONFIG_SERIAL_8250_CONSOLE=y
CONFIG_DEVTMPFS=y
CONFIG_DEVTMPFS_MOUNT=y
# ── Networking ──
CONFIG_NET=y
CONFIG_INET=y
CONFIG_IPV6=n
CONFIG_NETFILTER=y
# ── Keep small ──
CONFIG_MODULES=n
CONFIG_DEBUG_INFO=n
CONFIG_SOUND=n
CONFIG_DRM=n
CONFIG_USB=n
CONFIG_WIRELESS=n
CONFIG_WLAN=n
CONFIG_BT=n
KCONF
    make olddefconfig

    info "Building vmlinux ($(nproc) jobs)"
    make vmlinux -j"$(nproc)" 2>&1 | tail -5

    cp vmlinux "$FC_DIR/vmlinux"
    ok "Guest kernel: $FC_DIR/vmlinux ($(du -h "$FC_DIR/vmlinux" | cut -f1))"
}

# ══════════════════════════════════════════════════════════════════════════════
#  STEP 5: Alpine Rootfs (128 MB ext4)
# ══════════════════════════════════════════════════════════════════════════════
#
#  A single rootfs image serves ALL test types.  The vaigai OpenRC service
#  reads `vaigai_mode=` from the kernel cmdline and starts the appropriate
#  listeners.  Test scripts inject additional config (certs, nginx vhosts)
#  at runtime via COW-copy + mount + write + unmount.
#
#  ┌──────────────────────────────────────────────────────────────────────────┐
#  │                       Rootfs Contents                                    │
#  ├──────────────┬───────────────────────────────────────────────────────────┤
#  │ Base OS      │ Alpine Linux 3.23 (musl, busybox, OpenRC)                │
#  │ Networking   │ iproute2, iptables, bridge, ifupdown-ng                  │
#  │ TCP servers  │ socat (echo:5000, discard:5001, chargen:5002)            │
#  │ HTTP server  │ nginx (HTTP :80, TLS :4433/:443)                         │
#  │ TLS tools    │ openssl (s_server, cert gen)                             │
#  │ Debug        │ curl, tcpdump, bash, coreutils, strace                   │
#  │ QAT support  │ kernel afalg engine (enabled via openssl.cnf)            │
#  │ Init service │ /etc/init.d/vaigai (starts services per vaigai_mode)     │
#  └──────────────┴───────────────────────────────────────────────────────────┘
#
build_rootfs() {
    banner "Step 5: Alpine $ALPINE_VER Rootfs ($ROOTFS_SIZE_MB MB)"

    local mnt
    mnt=$(mktemp -d /tmp/vaigai-rootfs-XXXXXX)

    info "Creating ${ROOTFS_SIZE_MB}MB ext4 image"
    dd if=/dev/zero of="$ROOTFS" bs=1M count=$ROOTFS_SIZE_MB status=none
    mkfs.ext4 -q -F -L vaigai-rootfs "$ROOTFS"
    mount -o loop "$ROOTFS" "$mnt"

    # ── Bootstrap Alpine ─────────────────────────────────────────────
    info "Bootstrapping Alpine $ALPINE_VER via apk"
    local mirror="https://dl-cdn.alpinelinux.org/alpine/v${ALPINE_VER}"

    # Install apk-tools statically if not available
    if ! command -v apk &>/dev/null; then
        info "Installing apk-tools-static"
        curl -fLo /tmp/apk-tools-static.apk \
            "${mirror}/main/x86_64/apk-tools-static-2.14.6-r3.apk" 2>/dev/null \
            || curl -fLo /tmp/apk-tools-static.apk \
            "${mirror}/main/x86_64/$(curl -sL "${mirror}/main/x86_64/" | grep -o 'apk-tools-static-[^"]*\.apk' | tail -1)" 2>/dev/null
        tar xf /tmp/apk-tools-static.apk -C /tmp sbin/apk.static 2>/dev/null || true
        APK=/tmp/sbin/apk.static
    else
        APK=apk
    fi

    mkdir -p "$mnt/etc/apk"
    echo "${mirror}/main"      >  "$mnt/etc/apk/repositories"
    echo "${mirror}/community" >> "$mnt/etc/apk/repositories"

    $APK add --root "$mnt" --initdb --no-cache --arch x86_64 --allow-untrusted \
        alpine-base openrc busybox busybox-openrc busybox-suid \
        bash coreutils \
        socat curl tcpdump \
        iproute2 iptables bridge ifupdown-ng \
        nginx nginx-openrc openssl \
        readline ca-certificates \
        2>&1 | tail -10

    # ── Network config ───────────────────────────────────────────────
    cat > "$mnt/etc/network/interfaces" <<'NET'
auto lo
iface lo inet loopback

auto eth0
iface eth0 inet static
    address 192.168.204.2
    netmask 255.255.255.0
    gateway 192.168.204.1
NET

    # ── fstab ────────────────────────────────────────────────────────
    cat > "$mnt/etc/fstab" <<'FSTAB'
/dev/vda    /        ext4   rw,noatime   0 1
proc        /proc    proc   defaults     0 0
sysfs       /sys     sysfs  defaults     0 0
devtmpfs    /dev     devtmpfs defaults   0 0
FSTAB

    # ── hostname / resolv ────────────────────────────────────────────
    echo "vaigai-vm" > "$mnt/etc/hostname"
    echo "nameserver 8.8.8.8" > "$mnt/etc/resolv.conf"

    # ── Enable OpenRC services ───────────────────────────────────────
    for svc in devfs dmesg mdev networking; do
        ln -sf "/etc/init.d/$svc" "$mnt/etc/runlevels/boot/$svc" 2>/dev/null || true
    done
    for svc in local; do
        ln -sf "/etc/init.d/$svc" "$mnt/etc/runlevels/default/$svc" 2>/dev/null || true
    done

    # Root login (no password — serial console)
    sed -i 's|root:.*|root::0:0:root:/root:/bin/bash|' "$mnt/etc/passwd" 2>/dev/null || true
    echo "ttyS0::respawn:/sbin/getty -L ttyS0 115200 vt100" >> "$mnt/etc/inittab" 2>/dev/null || true

    # ── nginx configs ────────────────────────────────────────────────
    #  Default HTTP server on :80 (for http_nic.sh)
    mkdir -p "$mnt/etc/nginx/http.d" "$mnt/etc/nginx/ssl" "$mnt/var/www/html"
    cat > "$mnt/etc/nginx/http.d/default.conf" <<'NGINX_HTTP'
server {
    listen       80 default_server;
    listen       [::]:80 default_server;
    server_name  _;
    root   /var/www/html;
    index  index.html;
    location / {
        try_files $uri $uri/ =404;
    }
}
NGINX_HTTP

    #  TLS server on :4433 and :5001 (for tls_nic.sh)
    cat > "$mnt/etc/nginx/http.d/vaigai-tls.conf" <<'NGINX_TLS'
# vaigAI TLS test server — ciphers match vaigai's ECDHE config
server {
    listen       4433 ssl reuseport;
    server_name  _;
    ssl_certificate     /etc/nginx/ssl/server.crt;
    ssl_certificate_key /etc/nginx/ssl/server.key;
    ssl_protocols       TLSv1.2 TLSv1.3;
    ssl_ciphers         ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384;
    ssl_prefer_server_ciphers on;
    ssl_session_cache   shared:TLS:10m;
    ssl_session_timeout 10s;
    location / { return 200 "OK\n"; }
}

server {
    listen       5001 ssl reuseport;
    server_name  _;
    ssl_certificate     /etc/nginx/ssl/server.crt;
    ssl_certificate_key /etc/nginx/ssl/server.key;
    ssl_protocols       TLSv1.2 TLSv1.3;
    ssl_ciphers         ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384;
    ssl_prefer_server_ciphers on;
    ssl_session_cache   shared:BULK:10m;
    ssl_session_timeout 10s;
    location / { return 200 "OK\n"; }
}
NGINX_TLS

    # Generate a default self-signed cert so nginx always starts clean.
    # Test scripts replace this at runtime with ephemeral certs.
    openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
        -sha256 -days 3650 -nodes -subj '/CN=localhost' \
        -keyout "$mnt/etc/nginx/ssl/server.key" \
        -out "$mnt/etc/nginx/ssl/server.crt" 2>/dev/null

    # ── QAT support files ────────────────────────────────────────────
    #
    #  Server-side QAT crypto offload uses the kernel's AF_ALG interface.
    #  When a QAT VF is passed through to the VM via vfio-pci and the
    #  qat_dh895xccvf kernel module binds to it, the kernel registers
    #  QAT-accelerated crypto algorithms.  OpenSSL's built-in afalg engine
    #  automatically offloads to hardware via AF_ALG sockets.
    #
    #  Architecture:
    #
    #    nginx → OpenSSL → afalg engine → AF_ALG socket
    #                                         ↓
    #                              kernel crypto subsystem
    #                                         ↓
    #                              qat_dh895xccvf driver → QAT VF hardware
    #
    #  No QATEngine build required.  The afalg path offloads AES-CBC, AES-GCM,
    #  SHA-256, and SHA-512.  RSA/ECDSA remain in software (acceptable for
    #  server-side since the server does one RSA sign per handshake).
    #
    mkdir -p "$mnt/etc/vaigai"

    # OpenSSL config that activates the afalg engine
    cat > "$mnt/etc/vaigai/openssl-qat.cnf" <<'SSLCNF'
# OpenSSL configuration with hardware crypto offload for vaigAI
# Usage: OPENSSL_CONF=/etc/vaigai/openssl-qat.cnf openssl s_server ...

HOME = .
openssl_conf = openssl_init

[openssl_init]
providers = provider_sect
engines   = engine_sect

[provider_sect]
default = default_sect

[default_sect]
activate = 1

[engine_sect]
# afalg engine: offloads symmetric crypto (AES, SHA) to hardware
# via the kernel crypto API (AF_ALG).  When QAT VF is bound to
# qat_dh895xccvf, the kernel routes crypto ops through QAT hardware.
afalg = afalg_sect

[afalg_sect]
init = 1
default_algorithms = ALL
SSLCNF

    # QAT VF initialization script (called by test scripts when SERVER_CRYPTO=qat)
    cat > "$mnt/etc/vaigai/qat-setup.sh" <<'QATSETUP'
#!/bin/sh
# QAT VF initialization for vaigAI VM.
# Loads kernel modules and verifies QAT VF binding.
set -e

echo "[QAT] Loading kernel modules..."
modprobe crc8            2>/dev/null || true
modprobe intel_qat       2>/dev/null || true
modprobe qat_dh895xccvf  2>/dev/null || true
sleep 2

QAT_DEV=$(lspci -d 8086:0443 2>/dev/null | head -1)
if [ -z "$QAT_DEV" ]; then
    echo "[QAT] No QAT VF (8086:0443) detected — software crypto"
    exit 1
fi

QAT_BDF=$(echo "$QAT_DEV" | awk '{print $1}')
QAT_DRV=$(basename "$(readlink "/sys/bus/pci/devices/0000:$QAT_BDF/driver" 2>/dev/null)" 2>/dev/null || echo "none")
echo "[QAT] VF: $QAT_DEV  driver: $QAT_DRV"

[ "$QAT_DRV" = "qat_dh895xccvf" ] || { echo "[QAT] WARN: wrong driver"; exit 1; }

if [ -f /proc/crypto ]; then
    echo "[QAT] Algorithms: $(grep -c 'driver.*qat' /proc/crypto 2>/dev/null || echo 0) QAT"
fi
echo "[QAT] VF ready"
QATSETUP
    chmod +x "$mnt/etc/vaigai/qat-setup.sh"

    # ── vaigai OpenRC service ────────────────────────────────────────
    #  Reads vaigai_mode= from kernel cmdline:
    #    tcp  → socat echo/discard/chargen on 5000-5002
    #    tls  → openssl s_server on 4433 + nginx TLS
    #    http → nginx HTTP on 80
    #    all  → all of the above (default)
    cat > "$mnt/etc/init.d/vaigai" <<'INITSCRIPT'
#!/sbin/openrc-run

description="vaigAI test services"

depend() {
    need net
    after networking
}

start() {
    ebegin "Starting vaigAI test services"

    MODE=$(sed -n 's/.*vaigai_mode=\([a-z]*\).*/\1/p' /proc/cmdline)
    [ -z "$MODE" ] && MODE="all"

    case "$MODE" in
        tcp|all)
            start-stop-daemon --start --background --make-pidfile \
                --pidfile /run/socat-echo.pid \
                --exec /usr/bin/socat -- TCP-L:5000,fork,reuseaddr PIPE
            start-stop-daemon --start --background --make-pidfile \
                --pidfile /run/socat-discard.pid \
                --exec /usr/bin/socat -- TCP-L:5001,fork,reuseaddr /dev/null
            start-stop-daemon --start --background --make-pidfile \
                --pidfile /run/socat-chargen.pid \
                --exec /usr/bin/socat -- "TCP-L:5002,fork,reuseaddr" "SYSTEM:dd if=/dev/zero bs=64k count=16384 2>/dev/null"
            einfo "TCP: echo(:5000) discard(:5001) chargen(:5002)"
            ;;
    esac

    case "$MODE" in
        http|tls|https|all)
            mkdir -p /var/www/html
            dd if=/dev/urandom of=/var/www/html/1k   bs=1024 count=1    2>/dev/null
            dd if=/dev/urandom of=/var/www/html/10k  bs=1024 count=10   2>/dev/null
            dd if=/dev/urandom of=/var/www/html/100k bs=1024 count=100  2>/dev/null
            dd if=/dev/urandom of=/var/www/html/1m   bs=1024 count=1024 2>/dev/null
            echo "OK" > /var/www/html/index.html
            ;;
    esac

    case "$MODE" in
        tls|all)
            if [ ! -f /etc/ssl/vm.key ]; then
                openssl req -x509 -newkey rsa:2048 \
                    -keyout /etc/ssl/vm.key -out /etc/ssl/vm.crt \
                    -days 3650 -nodes -subj "/CN=vaigai-vm" 2>/dev/null
            fi
            ;;
    esac

    case "$MODE" in
        http|https|all)
            nginx 2>/dev/null || true
            einfo "nginx started"
            ;;
        tls)
            nginx 2>/dev/null || true
            einfo "nginx started (TLS)"
            ;;
    esac

    eend 0
}

stop() {
    ebegin "Stopping vaigAI test services"
    for pid in /run/socat-*.pid; do
        [ -f "$pid" ] && start-stop-daemon --stop --pidfile "$pid" 2>/dev/null || true
    done
    nginx -s stop 2>/dev/null || true
    eend 0
}
INITSCRIPT
    chmod +x "$mnt/etc/init.d/vaigai"
    ln -sf /etc/init.d/vaigai "$mnt/etc/runlevels/default/vaigai" 2>/dev/null || true

    # ── Finalize ─────────────────────────────────────────────────────
    sync
    umount "$mnt"
    rmdir "$mnt"

    ok "Rootfs: $ROOTFS ($(du -h "$ROOTFS" | cut -f1))"
    ok "  Packages: $(mount -o loop,ro "$ROOTFS" "$mnt" 2>/dev/null; mkdir -p "$mnt" 2>/dev/null; cat "$mnt/lib/apk/db/installed" 2>/dev/null | grep -c '^P:' || echo '?'; umount "$mnt" 2>/dev/null; rmdir "$mnt" 2>/dev/null; true)"
}

# ══════════════════════════════════════════════════════════════════════════════
#  STEP 5b: QEMU Rootfs (256 MB ext4 — extends alpine.ext4 with QAT support)
# ══════════════════════════════════════════════════════════════════════════════
#
#  The QEMU rootfs is used by http_nic.sh, https_nic.sh, and tls_nic.sh.
#  It extends alpine.ext4 with QAT firmware, kernel modules, nginx TLS config,
#  SSL certificates, and OpenSSL afalg engine configuration for QAT offload.
#
build_qemu_rootfs() {
    banner "Step 5b: QEMU Rootfs ($QEMU_ROOTFS_SIZE_MB MB)"

    [[ -f "$ROOTFS" ]] || die "alpine.ext4 must be built first (run --step rootfs)"

    local mnt
    mnt=$(mktemp -d /tmp/vaigai-qemu-rootfs-XXXXXX)

    info "Creating ${QEMU_ROOTFS_SIZE_MB}MB ext4 image from alpine.ext4"
    dd if=/dev/zero of="$QEMU_ROOTFS" bs=1M count=$QEMU_ROOTFS_SIZE_MB status=none
    mkfs.ext4 -q -F -L vaigai-qemu-rootfs "$QEMU_ROOTFS"
    mount -o loop "$QEMU_ROOTFS" "$mnt"

    # Copy contents from alpine.ext4
    local alpine_mnt
    alpine_mnt=$(mktemp -d /tmp/vaigai-alpine-mnt-XXXXXX)
    mount -o loop,ro "$ROOTFS" "$alpine_mnt"
    cp -a "$alpine_mnt"/. "$mnt"/
    umount "$alpine_mnt"
    rmdir "$alpine_mnt"

    # ── QAT firmware ──
    info "Installing QAT DH895XCC firmware"
    mkdir -p "$mnt/lib/firmware" "$mnt/usr/lib/firmware"
    for fw in qat_895xcc.bin qat_895xcc_mmp.bin; do
        if [[ -f "/usr/lib/firmware/$fw" ]]; then
            cp "/usr/lib/firmware/$fw" "$mnt/lib/firmware/"
            cp "/usr/lib/firmware/$fw" "$mnt/usr/lib/firmware/"
        elif [[ -f "/lib/firmware/$fw" ]]; then
            cp "/lib/firmware/$fw" "$mnt/lib/firmware/"
            cp "/lib/firmware/$fw" "$mnt/usr/lib/firmware/"
        else
            warn "QAT firmware $fw not found on host — skipping"
        fi
    done

    # ── QAT kernel modules ──
    local kver
    kver=$(uname -r)
    info "Installing QAT kernel modules ($kver)"
    local moddir="/lib/modules/$kver"
    local dst="$mnt/lib/modules/$kver"
    mkdir -p "$dst/kernel/drivers/crypto/intel/qat"
    for mod in intel_qat.ko qat_dh895xcc.ko qat_dh895xccvf.ko; do
        local src
        src=$(find "$moddir" -name "$mod" -o -name "${mod}.xz" -o -name "${mod}.zst" 2>/dev/null | head -1)
        if [[ -n "$src" ]]; then
            cp "$src" "$dst/kernel/drivers/crypto/intel/qat/"
        fi
    done
    # Copy modules.dep so modprobe works
    for f in modules.dep modules.dep.bin modules.alias modules.alias.bin modules.symbols modules.symbols.bin modules.builtin modules.builtin.bin modules.order; do
        [[ -f "$moddir/$f" ]] && cp "$moddir/$f" "$dst/" 2>/dev/null || true
    done

    # ── SSL certificates ──
    info "Generating SSL certificates"
    mkdir -p "$mnt/etc/nginx/ssl"
    openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
        -keyout "$mnt/etc/nginx/ssl/server.key" \
        -out "$mnt/etc/nginx/ssl/server.crt" \
        -subj "/CN=vaigai-test" 2>/dev/null

    # ── nginx TLS vhost ──
    info "Configuring nginx TLS vhost"
    mkdir -p "$mnt/etc/nginx/http.d"
    cat > "$mnt/etc/nginx/http.d/vaigai-tls.conf" << 'NGINX_EOF'
server {
    listen       443 ssl reuseport;
    server_name  _;
    ssl_certificate     /etc/nginx/ssl/server.crt;
    ssl_certificate_key /etc/nginx/ssl/server.key;
    ssl_protocols       TLSv1.2 TLSv1.3;
    ssl_ciphers         HIGH:!aNULL:!MD5;
    ssl_session_cache   shared:SSL_QEMU:10m;
    ssl_session_timeout 5m;
    location / {
        root /var/www/html;
        try_files $uri $uri/ =404;
    }
}
NGINX_EOF

    # ── Static test files ──
    mkdir -p "$mnt/var/www/html"
    dd if=/dev/urandom of="$mnt/var/www/html/100k.bin" bs=1024 count=100 status=none 2>/dev/null || true

    # ── OpenSSL afalg engine config for QAT ──
    info "Installing OpenSSL afalg engine config"
    mkdir -p "$mnt/etc/vaigai"
    cat > "$mnt/etc/vaigai/openssl-qat.cnf" << 'OSSL_EOF'
openssl_conf = openssl_init
[openssl_init]
engines = engine_section
[engine_section]
afalg = afalg_section
[afalg_section]
default_algorithms = ALL
init = 1
OSSL_EOF

    # ── QAT setup script ──
    cat > "$mnt/etc/vaigai/qat-setup.sh" << 'QAT_EOF'
#!/bin/sh
# QAT PF initialization for vaigAI VM (full PF passthrough).
set -e
echo "[QAT] Loading kernel modules..."
modprobe crc8            2>/dev/null || true
modprobe intel_qat       2>/dev/null || true
modprobe qat_dh895xcc    2>/dev/null || true
sleep 2

QAT_DEV=$(lspci -d 8086:0435 2>/dev/null | head -1)
if [ -z "$QAT_DEV" ]; then
    echo "[QAT] No QAT PF (8086:0435) detected — software crypto"
    exit 1
fi

QAT_COUNT=$(lspci -d 8086:0435 2>/dev/null | wc -l)
echo "[QAT] Found $QAT_COUNT QAT PF device(s):"
lspci -d 8086:0435 2>/dev/null

for dev in $(lspci -d 8086:0435 2>/dev/null | awk '{print $1}'); do
    QAT_DRV=$(basename "$(readlink "/sys/bus/pci/devices/0000:$dev/driver" 2>/dev/null)" 2>/dev/null || echo "none")
    echo "[QAT] PF 0000:$dev  driver: $QAT_DRV"
done

if [ -f /proc/crypto ]; then
    echo "[QAT] Algorithms: $(grep -c 'driver.*qat' /proc/crypto 2>/dev/null || echo 0) QAT-accelerated"
fi
echo "[QAT] PFs ready"
QAT_EOF
    chmod +x "$mnt/etc/vaigai/qat-setup.sh"

    # ── QAT modprobe blacklist ──
    # Prevent automatic QAT module loading via PCI alias during initramfs
    # phase which carries stale firmware. Modules are loaded explicitly by
    # the qat init service after the rootfs (with correct firmware) is mounted.
    info "Adding QAT modprobe blacklist"
    cat > "$mnt/etc/modprobe.d/qat-blacklist.conf" << 'BLEOF'
# Prevent automatic QAT module loading via PCI alias.
# The initramfs may carry stale firmware; we load explicitly
# from /etc/vaigai/qat-setup.sh after the rootfs is mounted
# so the correct firmware in /lib/firmware/ is used.
blacklist qat_dh895xcc
blacklist intel_qat
BLEOF

    # ── QAT OpenRC init service ──
    info "Installing QAT init service"
    cat > "$mnt/etc/init.d/qat" << 'QATINITEOF'
#!/sbin/openrc-run
description="QAT firmware and module loader"
depend() { need localmount; before vaigai; }
start() {
    # Only load if a QAT PF is present (vfio-pci passthrough)
    if ! lspci -d 8086:0435 >/dev/null 2>&1; then
        einfo "No QAT PF detected — skipping"
        return 0
    fi
    ebegin "Loading QAT modules (rootfs firmware)"
    if [ -x /etc/vaigai/qat-setup.sh ]; then
        /etc/vaigai/qat-setup.sh
    else
        modprobe intel_qat    2>/dev/null || true
        modprobe qat_dh895xcc 2>/dev/null || true
    fi
    eend $?
}
QATINITEOF
    chmod 755 "$mnt/etc/init.d/qat"
    ln -sf /etc/init.d/qat "$mnt/etc/runlevels/default/qat"

    # ── Finalize ──
    sync
    umount "$mnt"
    rmdir "$mnt"

    ok "QEMU Rootfs: $QEMU_ROOTFS ($(du -h "$QEMU_ROOTFS" | cut -f1))"
}

# ─────────────────────────────────────────────────────────────────────────────
#  VM-specific initramfs (avoids host initramfs bloat / stale firmware)
# ─────────────────────────────────────────────────────────────────────────────
build_vm_initramfs() {
    banner "Step 5c: VM Initramfs"
    local kver
    kver=$(uname -r)
    local out="$FC_DIR/initramfs-vm.img"

    info "Building minimal VM initramfs for kernel $kver"
    dracut --no-hostonly --force \
        --modules "base rootfs-block kernel-modules" \
        --omit "plymouth rdma multipath iscsi fcoe fcoe-uefi nfs nbd lunmask crypt dm lvm btrfs network network-manager" \
        --drivers "virtio_blk virtio_pci virtio_net virtio_scsi ext4 vfio-pci vfio vfio_iommu_type1 mlx5_core mlx5_ib i40e" \
        --no-hostonly-cmdline \
        "$out" "$kver"

    ok "VM Initramfs: $out ($(du -h "$out" | cut -f1))"
}

# ══════════════════════════════════════════════════════════════════════════════
# ══════════════════════════════════════════════════════════════════════════════
#
#  QAT crypto offload on both sides:
#
#  ┌────────────────────────────────────────────────────────────────────┐
#  │                     QAT Offload Architecture                       │
#  │                                                                    │
#  │  vaigAI side (traffic generator):                                  │
#  │    DPDK crypto_qat PMD  →  QAT PF/VF via vfio-pci                │
#  │    vaigai links against librte_common_qat.so                      │
#  │    Activated by: -a <QAT_PCI_BDF> on the DPDK EAL command line   │
#  │                                                                    │
#  │  Server side (QEMU VM):                                           │
#  │    QAT VF passthrough  →  vfio-pci in VM  →  qat_dh895xccvf      │
#  │    OpenSSL afalg engine  →  AF_ALG  →  kernel crypto  →  QAT VF  │
#  │    No userspace QATEngine build required                          │
#  │                                                                    │
#  │  Prerequisites:                                                    │
#  │    1. QAT PF bound to kernel driver (qat_dh895xcc)                │
#  │    2. SR-IOV VFs created (sriov_numvfs)                           │
#  │    3. VF bound to vfio-pci for passthrough                        │
#  │    4. Host IOMMU enabled (intel_iommu=on)                         │
#  └────────────────────────────────────────────────────────────────────┘
#
#  The test scripts (tls_nic.sh, https_nic.sh) handle VF creation and
#  vfio-pci binding automatically at runtime.  This step just verifies
#  the host has the right kernel modules.
#
setup_qat_host() {
    banner "Step 6: QAT Host Verification"

    # Check IOMMU
    if ! grep -q "intel_iommu=on" /proc/cmdline; then
        warn "intel_iommu=on not in kernel cmdline"
        warn "Add 'intel_iommu=on' to GRUB_CMDLINE_LINUX in /etc/default/grub"
        warn "Then: grub2-mkconfig -o /boot/grub2/grub.cfg && reboot"
    else
        ok "IOMMU enabled"
    fi

    # Check vfio-pci module
    modprobe vfio-pci 2>/dev/null && ok "vfio-pci module loaded" \
        || warn "vfio-pci module not available"

    # Check QAT kernel modules
    local qat_mods="intel_qat qat_dh895xcc qat_dh895xccvf"
    for mod in $qat_mods; do
        if modprobe "$mod" 2>/dev/null; then
            ok "Module $mod loaded"
        else
            warn "Module $mod not available (QAT offload will not work)"
        fi
    done

    # Discover QAT PFs
    local pf_count
    pf_count=$(lspci -d 8086:0435 2>/dev/null | wc -l)
    if [[ "$pf_count" -gt 0 ]]; then
        ok "QAT DH895XCC PFs found: $pf_count"
        lspci -d 8086:0435 2>/dev/null | while read -r line; do
            info "  PF: $line"
        done
    else
        warn "No QAT DH895XCC PFs detected (device 8086:0435)"
        warn "QAT tests will fall back to software crypto"
        info ""
        info "Without QAT hardware, run tests in software-only mode:"
        info "  VAIGAI_CRYPTO=sw SERVER_CRYPTO=sw bash tests/tls_nic.sh"
        info "  VAIGAI_CRYPTO=sw SERVER_CRYPTO=sw bash tests/https_nic.sh"
    fi

    # Discover QAT VFs (if any PFs have SR-IOV enabled)
    local vf_count
    vf_count=$(lspci -d 8086:0443 2>/dev/null | wc -l)
    if [[ "$vf_count" -gt 0 ]]; then
        ok "QAT VFs found: $vf_count"
    else
        info "No QAT VFs yet (test scripts create them on demand)"
    fi

    # Check KVM
    if [[ -c /dev/kvm ]]; then
        ok "KVM available"
    else
        warn "/dev/kvm not found — QEMU tests will be slow (no hardware accel)"
    fi

    info ""
    info "QAT offload summary:"
    info "  vaigai side:  DPDK crypto_qat PMD (librte_common_qat)"
    info "                Activated via: -a <QAT_PCI> on EAL cmdline"
    info "  server side:  QAT VF passthrough → kernel qat_dh895xccvf"
    info "                OpenSSL afalg engine → AF_ALG → kernel crypto → QAT"
    info "  no build:     afalg is built into OpenSSL — no QATEngine needed"
}

# ══════════════════════════════════════════════════════════════════════════════
#  STEP 7: Build vaigAI
# ══════════════════════════════════════════════════════════════════════════════
#
#  Links against libdpdk (which brings in crypto_qat), OpenSSL, readline,
#  jansson, libmicrohttpd.  All features auto-detected via meson feature flags.
#
build_vaigai() {
    banner "Step 7: Build vaigAI"

    cd "$VAIGAI_DIR"

    info "Configuring (meson)"
    if [[ -d build ]]; then
        meson setup --wipe build 2>&1 | tail -20
    else
        meson setup build 2>&1 | tail -20
    fi

    info "Building (ninja)"
    ninja -C build -j"$(nproc)" 2>&1 | tail -5

    [[ -x build/vaigai ]] || die "vaigai binary not found after build"
    ok "Binary: $VAIGAI_DIR/build/vaigai"
    ok "  $(file build/vaigai | sed 's|.*: ||')"
}

# ══════════════════════════════════════════════════════════════════════════════
#  STEP 8: Verify Environment
# ══════════════════════════════════════════════════════════════════════════════
verify_env() {
    banner "Step 8: Verification"

    local pass=0 fail=0
    check() {
        if eval "$2" &>/dev/null; then
            ok "$1"; pass=$((pass + 1))
        else
            warn "MISSING: $1"; fail=$((fail + 1))
        fi
    }

    check "DPDK installed"            "pkg-config --exists libdpdk"
    check "DPDK crypto_qat built"     "test -f '$DPDK_DIR/builddir/drivers/librte_common_qat.a'"
    check "DPDK net_i40e built"       "find '$DPDK_DIR/builddir/drivers' -name '*i40e*' | grep -q ."
    check "Firecracker binary"        "command -v firecracker"
    check "Guest kernel"              "test -f '$FC_DIR/vmlinux'"
    check "Rootfs"                    "test -f '$ROOTFS'"
    check "QEMU Rootfs"               "test -f '$QEMU_ROOTFS'"
    check "VM initramfs"              "test -f '$FC_DIR/initramfs-vm.img'"
    check "Host vmlinuz"              "test -f '/boot/vmlinuz-$(uname -r)'"
    check "QEMU"                      "command -v qemu-system-x86_64"
    check "KVM"                       "test -c /dev/kvm"
    check "vfio-pci module"           "modprobe vfio-pci"
    check "IOMMU enabled"             "grep -q intel_iommu=on /proc/cmdline"
    check "vaigai binary"             "test -x '$VAIGAI_DIR/build/vaigai'"
    check "openssl"                   "command -v openssl"
    check "Hugepages (2MB)"           "test $(cat /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages 2>/dev/null || echo 0) -gt 0"

    echo ""
    info "NIC pairs (i40e, link UP):"
    for iface in $(ls /sys/class/net/ 2>/dev/null); do
        local drv state pci
        drv=$(basename "$(readlink /sys/class/net/$iface/device/driver 2>/dev/null)" 2>/dev/null || true)
        state=$(cat /sys/class/net/$iface/operstate 2>/dev/null || true)
        pci=$(readlink /sys/class/net/$iface/device 2>/dev/null | sed 's|.*/||' || true)
        if [[ "$drv" == "i40e" ]]; then
            info "  $iface  $pci  $state  ($drv)"
        fi
    done

    echo ""
    info "QAT devices:"
    local qat_pf qat_vf
    qat_pf=$(lspci -d 8086:0435 2>/dev/null | wc -l)
    qat_vf=$(lspci -d 8086:0443 2>/dev/null | wc -l)
    info "  PFs (DH895XCC): $qat_pf    VFs: $qat_vf"
    lspci -d 8086:0435 2>/dev/null | while read -r l; do info "    $l"; done

    echo ""
    info "══════════════════════════════════════════════════════"
    info "  Verification: $pass passed, $fail missing"
    info "══════════════════════════════════════════════════════"

    if [[ $fail -gt 0 ]]; then
        warn "Some components missing — not all tests will pass"
    fi

    echo ""
    info "Quick-start commands:"
    info ""
    info "  # Set up hugepages (one time after boot)"
    info "  echo 256 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages"
    info ""
    info "  # ICMP (no NIC needed)"
    info "  bash tests/ping_veth.sh"
    info ""
    info "  # TCP over Firecracker"
    info "  bash tests/tcp_tap.sh"
    info ""
    info "  # TLS over NIC loopback (SW crypto)"
    info "  NIC_PCI_VAIGAI=0000:83:00.0 NIC_PCI_VM=0000:83:00.1 NIC_IFACE_VM=ens23f1np1 \\"
    info "    VAIGAI_CRYPTO=sw SERVER_CRYPTO=sw bash tests/tls_nic.sh"
    info ""
    info "  # HTTPS over NIC loopback (SW crypto)"
    info "  NIC_PCI_VAIGAI=0000:83:00.0 NIC_PCI_VM=0000:83:00.1 NIC_IFACE_VM=ens23f1np1 \\"
    info "    VAIGAI_CRYPTO=sw SERVER_CRYPTO=sw bash tests/https_nic.sh"
    info ""
    info "  # TLS with QAT offload (requires QAT hardware)"
    info "  NIC_PCI_VAIGAI=0000:83:00.0 NIC_PCI_VM=0000:83:00.1 NIC_IFACE_VM=ens23f1np1 \\"
    info "    VAIGAI_CRYPTO=qat SERVER_CRYPTO=qat bash tests/tls_nic.sh"
}

# ══════════════════════════════════════════════════════════════════════════════
#  Run
# ══════════════════════════════════════════════════════════════════════════════
should_run "host-packages" && install_host_packages
should_run "dpdk"          && build_dpdk
should_run "firecracker"   && install_firecracker
should_run "kernel"        && build_guest_kernel
should_run "rootfs"        && build_rootfs
should_run "qemu-rootfs"   && build_qemu_rootfs
should_run "vm-initramfs" && build_vm_initramfs
should_run "qat-host"      && setup_qat_host
should_run "vaigai"        && build_vaigai
should_run "verify"        && verify_env

echo ""
ok "test-env.sh complete"
