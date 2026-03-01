# Copilot Instructions — vaigAI

## What This Project Is

**vaigAI** is a high-performance DPDK-based traffic generator written in **C17**.
Every packet (Ethernet → IP → TCP/UDP/ICMP) is crafted in user-space; the Linux
kernel is never in the data path. Think of it as a from-scratch iperf3 + hping3
that runs entirely on DPDK poll-mode drivers.

The binary is called `vaigai`. It is a single process that spawns worker lcores
(DPDK threads pinned to CPU cores) for packet TX/RX and a management core for
CLI, REST API, config, and ARP.

## Host Environment

| Item | Value |
|------|-------|
| OS | Fedora 42 (Linux 6.14+) |
| Arch | x86_64 |
| Build system | Meson + Ninja |
| C standard | C17 (`-std=c17`) |
| Compiler flags | `-march=native -Wall -Wextra -Werror` |
| Package manager | `dnf` |

## External Dependencies & Paths

### DPDK 24.11.1 (required)

- **Source**: `/work/dpdk-stable-24.11.1/`
- **Build dir**: `/work/dpdk-stable-24.11.1/builddir/`
- **Installed to**: `/usr/local` (libs in `/usr/local/lib64/`, pkgconfig in `/usr/local/lib64/pkgconfig/`)
- **PMD plugins**: `/usr/local/lib64/dpdk/pmds-25.0/`
- **Enabled drivers**: `net/af_packet, net/af_xdp, net/tap, net/mlx5, net/i40e, common/mlx5, bus/auxiliary, bus/pci, bus/vdev`
- **Build command** (from source):
  ```bash
  cd /work/dpdk-stable-24.11.1
  meson setup builddir -Denable_drivers=net/af_packet,net/af_xdp,net/tap,net/mlx5,net/i40e,common/mlx5,bus/auxiliary,bus/pci,bus/vdev
  ninja -C builddir
  sudo ninja -C builddir install
  sudo ldconfig
  ```
- **Key**: If you change the DPDK build, rebuild vaigai with `meson setup --wipe build && ninja -C build`.

### Firecracker MicroVM (for TCP tests)

- **Binary**: `firecracker` (v1.14.2, must be in `$PATH` or set `$FIRECRACKER`)
- **Guest kernel**: `/work/firecracker/vmlinux` — Linux 6.1.163, built from `/work/firecracker/linux-src/`
  - Minimal config: virtio-net, ext4, 9p, no modules, ELF vmlinux (not bzImage)
- **Root filesystem**: `/work/firecracker/alpine.ext4` — Alpine 3.23, 128 MB ext4 image
  - Contains: busybox, socat, iproute2, iperf3, curl, nginx, openssl, strace
  - Init system: OpenRC with a `vaigai` service that starts TCP listeners:
    - Port 5000: socat echo (PIPE) — for T1 SYN flood & T2 data echo
    - Port 5001: socat discard (/dev/null) — for T3 TX throughput sink
    - Port 5002: socat chargen (dd from /dev/zero) — for T3 RX throughput source
  - The `vaigai_mode` kernel boot arg controls which services start (default: `tcp`)
- **Firecracker repo**: `/work/firecracker/fc-repo/` (v1.14.2 source, used to build the binary)
- **Kernel config reference**: `/work/firecracker/vmlinux-6.1`

### Optional Libraries

| Library | Feature flag | Purpose |
|---------|-------------|---------|
| OpenSSL ≥ 1.1 | `-Dtls=enabled` | TLS 1.2/1.3 engine |
| readline | `-Dcli=enabled` | Interactive CLI |
| libbpf | `-Daf_xdp=enabled` | AF_XDP socket support |
| jansson ≥ 2.14 | `-Drest=enabled` | JSON config & REST API |
| libmicrohttpd | `-Drest=enabled` | REST HTTP server |
| rdma-core-devel | (for mlx5 PMD) | Mellanox ConnectX NIC support |

## Building vaigAI

```bash
cd vaigAI/
meson setup build          # first time
ninja -C build             # incremental
# OR after DPDK / dep changes:
meson setup --wipe build
ninja -C build
```

The binary is at `build/vaigai`. Install with `ninja -C build install`.

## Architecture Overview

Refer to `docs/ARCHITECTURE.md` for the full design. Key points:

### Source Layout

```
src/
├── main.c              # Process lifecycle, lcore launch, signal handling
├── common/             # types.h (constants), util.c (TSC, parsers, PRNG)
├── core/               # EAL init, core assignment, mempool, IPC rings, worker loop, TX gen
├── port/               # NIC port init, soft_nic driver detection
├── net/                # Full protocol stack: ethernet, ARP, IPv4, ICMP, UDP, TCP (FSM + TCB + options + timers + congestion + port pool + checksum)
├── tls/                # OpenSSL BIO-pair TLS engine, session store, cert manager, cryptodev
├── app/                # HTTP/1.1 request builder + response parser
├── mgmt/               # CLI (readline), REST API (libmicrohttpd), config manager (JSON)
└── telemetry/          # Per-worker lock-free metrics, JSON export, HDR histogram, packet trace, structured logging
```

### Control Plane vs Data Plane

- **Data plane** (worker lcores): RX burst → classify → protocol process → TX gen → TX drain → timer tick. Never blocks, never does syscalls, never allocates memory.
- **Control plane** (mgmt lcore): CLI REPL, REST API, config management, ARP cache, telemetry snapshots. Communicates with workers via lock-free SPSC `rte_ring` IPC.
- **No locks in hot path**. Workers own their metrics slabs, TCB stores, and port pools.

### Key Constants (from `src/common/types.h`)

| Constant | Value | Meaning |
|----------|-------|---------|
| `TGEN_MAX_WORKERS` | 124 | Max worker lcores |
| `TGEN_MAX_PORTS` | 16 | Max DPDK ports |
| `TGEN_MAX_RX_BURST` / `TGEN_MAX_TX_BURST` | 32 | Burst size |
| `TGEN_DEFAULT_RX_DESC` / `TGEN_DEFAULT_TX_DESC` | 2048 | Ring descriptor count |
| `TGEN_EPHEMERAL_LO` – `TGEN_EPHEMERAL_HI` | 10000–59999 | TCP ephemeral port range |

### Startup Sequence (simplified)

1. `tgen_eal_init()` — DPDK EAL + TSC calibration
2. `tgen_core_assign_init()` — map lcores to worker/mgmt roles (auto-scales by core count)
3. `tgen_mempool_create_all()` — per-worker NUMA-aware mempools
4. `tgen_ports_init()` — configure NICs, RSS, queues, start ports
5. Protocol subsystem init (ARP, ICMP, UDP, TCP stores, port pools)
6. `tgen_ipc_init()` — SPSC command + ACK rings
7. Config load, TLS init, cryptodev init
8. Launch workers → REST server → CLI REPL (blocks)
9. On "quit": ordered teardown

### CLI Commands

| Command | Example | What it does |
|---------|---------|-------------|
| `flood` | `flood tcp 10.0.0.1 5 0 56 80` | Flood with ICMP/UDP/TCP SYN packets |
| `throughput` | `throughput tx 10.0.0.1 5000 10 4` | TCP data throughput (iperf3-like) |
| `stats` | `stats` | Print telemetry JSON snapshot |
| `quit` | `quit` | Graceful shutdown |

### Telemetry

- 38 per-worker counters (L2 through HTTP), cache-line aligned, single-writer.
- Management core pulls on demand via `metrics_snapshot()` — no periodic scrape.
- Export: flat JSON via `export_json()`, served at `/api/v1/stats` and CLI `stats`.

## Test Infrastructure

### ping_veth.sh — ICMP over AF_PACKET/AF_XDP

Uses a veth pair + Alpine container (podman). No Firecracker needed.
Tests ICMP echo request/reply through the full stack.

```bash
bash tests/ping_veth.sh              # AF_PACKET mode
bash tests/ping_veth.sh --xdp       # AF_XDP mode
bash tests/ping_veth.sh --flood 5   # 5-second flood
```

### tcp_tap.sh — TCP over TAP + Firecracker

Uses dual TAP interfaces + Linux bridge + Firecracker microVM.
The test topology is documented in `docs/tcp-test.md`.

```
vaigAI (DPDK net_tap) ↔ tap-vaigai ↔ br-vaigai ↔ tap-fc0 ↔ Firecracker VM (Alpine)
```

- Network: `192.168.204.0/24`, vaigAI = `.1`, VM = `.2`
- **Important**: DPDK's TAP PMD creates the `tap-vaigai` interface internally. Do NOT pre-create it. The test script attaches it to the bridge after DPDK creates it.
- vaigai runs as a persistent background process; commands are sent via FIFO.

```bash
bash tests/tcp_tap.sh                # all tests
bash tests/tcp_tap.sh --test 1       # T1 only (SYN flood)
bash tests/tcp_tap.sh --test 2       # T2 only (full lifecycle)
bash tests/tcp_tap.sh --test 3       # T3 only (throughput)
```

**Prerequisite paths** (override with env vars `$VMLINUX`, `$ROOTFS`, `$FIRECRACKER`):
- Kernel: `/work/firecracker/vmlinux`
- Rootfs: `/work/firecracker/alpine.ext4`
- Firecracker: in `$PATH`

### udp_veth.sh — UDP over veth

Similar to ping_veth.sh but for UDP flood testing.

## Hugepage Setup

DPDK requires hugepages. For development with TAP/af_packet (no physical NIC):

```bash
# 256 × 2 MB hugepages (512 MB total) — sufficient for TAP testing
echo 256 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages

# For production with physical NICs, use 1 GB hugepages via setup.sh:
bash scripts/setup.sh --hugepages-1g 16 <pci-addr>
```

## Running vaigai Manually

```bash
# TAP mode (no physical NIC needed):
./build/vaigai --lcores 0-1 --no-pci --vdev "net_tap0,iface=tap-vaigai"

# Physical NIC:
./build/vaigai --lcores 0-7 -a 0000:01:00.0

# With config file:
VAIGAI_CONFIG=/path/to/config.json ./build/vaigai --lcores 0-3 -a 0000:01:00.0
```

## Recreating the Environment on a New System

### 1. Install system packages (Fedora/RHEL)

```bash
dnf install -y \
    gcc meson ninja-build pkgconfig \
    numactl-devel rdma-core-devel libibverbs-devel libmlx5 \
    openssl-devel readline-devel libbpf-devel jansson-devel libmicrohttpd-devel \
    python3-pyelftools \
    socat iproute bridge-utils podman
```

### 2. Build and install DPDK

```bash
# Download DPDK 24.11.1
curl -LO https://fast.dpdk.org/rel/dpdk-24.11.1.tar.xz
tar xf dpdk-24.11.1.tar.xz
cd dpdk-stable-24.11.1

meson setup builddir \
    -Denable_drivers=net/af_packet,net/af_xdp,net/tap,net/mlx5,net/i40e,common/mlx5,bus/auxiliary,bus/pci,bus/vdev
ninja -C builddir
sudo ninja -C builddir install
sudo ldconfig
```

### 3. Build vaigai

```bash
cd vaigAI/
meson setup build
ninja -C build
```

### 4. Set up hugepages

```bash
echo 256 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
```

### 5. (Optional) Set up Firecracker for TCP tests

```bash
# Install Firecracker v1.14.2
curl -L https://github.com/firecracker-microvm/firecracker/releases/download/v1.14.2/firecracker-v1.14.2-x86_64.tgz | tar xz
sudo cp release-v1.14.2-x86_64/firecracker-v1.14.2-x86_64 /usr/local/bin/firecracker

# Build guest kernel (Linux 6.1.x, minimal config with virtio-net + ext4)
# See /work/firecracker/linux-src/ for config reference

# Build Alpine rootfs (128 MB ext4 with socat, iproute2, iperf3)
# The rootfs must have an OpenRC init that starts socat listeners on :5000-5002
# Place at /work/firecracker/alpine.ext4

# Verify KVM is available
ls /dev/kvm
```

## Coding Conventions

- **Keep `docs/ARCHITECTURE.md` updated** for any change that impacts the architecture — new modules, changed startup sequence, new IPC commands, protocol additions, modified data flows, or telemetry counter changes. The architecture doc is the canonical design reference; it must stay in sync with the code.
- License: BSD-3-Clause on all source files
- All source is under `src/`, tests under `tests/`, docs under `docs/`
- Header guards: `TGEN_<MODULE>_H`
- Log macros: `RTE_LOG(level, TGEN, ...)` — domains defined in `types.h`
- Metrics: single `++` per counter, no atomics — each worker slab is single-writer
- IPC: `config_update_t` (256 bytes) over SPSC `rte_ring` — never share state directly
- Meson feature flags: `tls`, `cli`, `af_xdp`, `rest` (all default to `auto`)
