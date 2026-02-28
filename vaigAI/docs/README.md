# vaigAI — DPDK L4–L7 Traffic Generator

High-performance traffic generator for IPv4 TCP/HTTP/TLS workloads built on DPDK 24.11 LTS.

## Features

| Layer | Capability |
|-------|-----------|
| L2    | Ethernet, 802.1Q VLAN, ARP (RFC 826) |
| L3    | IPv4, ICMP, LPM routing |
| L4    | Full TCP stack (RFC 793/7323): New Reno CC, SACK, Timestamps, Window Scale |
| L5/6  | TLS 1.2/1.3 (OpenSSL BIO-pair, optional hardware Cryptodev offload) |
| L7    | HTTP/1.1 client (keep-alive, pipelining, chunked transfer) |
| Mgmt  | CLI (readline), REST API (libmicrohttpd + jansson) |
| Telem | Per-worker lock-free counters, latency histograms, JSON export |

## Quick Start

### 1. Install dependencies

**Fedora:**
```bash
sudo dnf install -y meson ninja-build pkgconf-pkg-config numactl-devel \
    openssl-devel readline-devel jansson-devel libmicrohttpd-devel
```

### 2. Allocate hugepages (one-time, per boot)

```bash
# 4 × 1 GB minimum — 16 recommended for sustained load
echo 4 | sudo tee /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages
grep -i hugepage /proc/meminfo
```

### 3. Bind the client-side NIC (i40e only — skip for mlx5)

```bash
sudo modprobe vfio-pci
sudo dpdk-devbind.py --bind=vfio-pci 0000:81:00.0   # replace with your PCI address
dpdk-devbind.py --status-dev 0000:81:00.0
```

mlx5 (ConnectX) uses the bifurcated driver model — no binding step is required.

### 4. Build

```bash
meson setup build -Dtls=enabled -Dcli=enabled -Drest=enabled -Daf_xdp=disabled
ninja -C build
```

### 5. Run

```bash
# Intel X710 / i40e (bound to vfio-pci)
sudo ./build/vaigai -l 0-3 -n 4 -a 0000:81:00.0 -- --config /path/to/your-flow.json

# Mellanox ConnectX / mlx5 (bifurcated — no bind step)
sudo ./build/vaigai -l 0-7 -n 4 -a 0000:95:00.0 -- --config /path/to/your-flow.json
```

## Build Options

| Option   | Values        | Default | Description              |
|----------|---------------|---------|--------------------------|
| `tls`    | auto/enabled/disabled | auto | OpenSSL TLS support  |
| `cli`    | auto/enabled/disabled | auto | readline CLI             |
| `rest`   | auto/enabled/disabled | auto | libmicrohttpd REST API   |
| `af_xdp` | auto/enabled/disabled | auto | AF_XDP soft NIC          |

## Configuration

Minimal HTTP flow config:

```json
{
  "flows": [
    {
      "src_ip_lo":  "10.22.0.10",
      "src_ip_hi":  "10.22.0.50",
      "dst_ip":     "10.22.0.2",
      "dst_port":   80,
      "vlan_id":    0,
      "enable_tls": false,
      "http_url":   "/",
      "http_host":  "test.local"
    }
  ],
  "load": {
    "target_cps":     1000,
    "target_rps":     10000,
    "ramp_up_secs":   5,
    "ramp_down_secs": 5,
    "duration_secs":  60,
    "max_concurrent": 5000
  },
  "tls": null
}
```

| Field | Description |
|---|---|
| `src_ip_lo` / `src_ip_hi` | Source IP range; cycled per new connection |
| `dst_ip` / `dst_port` | Target server address |
| `vlan_id` | 0 = untagged; any non-zero value inserts an 802.1Q header |
| `enable_tls` | Wraps the TCP stream in TLS 1.2/1.3 when true |
| `target_cps` | New TCP connections per second |
| `target_rps` | HTTP requests per second (across all active connections) |
| `max_concurrent` | Maximum simultaneous open TCP connections |

## REST API

```
GET  /api/v1/stats     → JSON snapshot of all worker counters
GET  /api/v1/config    → currently loaded configuration (JSON)
PUT  /api/v1/config    → apply a partial config patch (JSON body)
POST /api/v1/start     → signal all workers to begin generating traffic
POST /api/v1/stop      → signal all workers to drain and stop
```

### Key metrics in `/api/v1/stats`

| Field | Meaning |
|---|---|
| `tcp_syn_sent` | SYN packets transmitted |
| `tcp_synack_rcvd` | Completed 3-way handshakes |
| `http_req_tx` | HTTP requests sent |
| `http_rsp_rx` | HTTP responses received — this is your effective TPS |
| `total_tx_bytes` / `total_rx_bytes` | Cumulative throughput |
| `active_conns` | Currently open TCP sessions |
| `p50_latency_us` / `p99_latency_us` | Response latency percentiles (µs) |

## CLI Commands

```
vaigai> help                           # list all registered commands
vaigai> stats                          # print current metrics to stdout
vaigai> load /path/to/your-flow.json  # load a config file
vaigai> save /tmp/current.json         # save running config to file
vaigai> set-cps 5000                   # change target connections/sec live
vaigai> quit                           # graceful shutdown
```

## Supported NICs

**Physical (verified):**

| NIC | DPDK PMD | Setup |
|---|---|---|
| Mellanox ConnectX-5/6 (mlx5) | `net_mlx5` | Bifurcated — kernel driver stays; no `vfio-pci` needed |
| Intel X710 / XL710 (i40e) | `net_i40e` | Bind to `vfio-pci` before launching |
| Intel 82599 / X520 (ixgbe) | `net_ixgbe` | Bind to `vfio-pci` before launching |

**Soft / Virtual:**

| PMD | Use case |
|---|---| 
| `net_af_packet` | Kernel socket; no hugepages required |
| `net_af_xdp` | XDP zero-copy where the NIC driver supports it |
| `net_tap` | Kernel TAP; useful for packet capture and debugging |
| `net_virtio` | KVM/QEMU guest NIC |
| `net_vhost` | virtio-net host backend |
| `net_null` | TX blackhole; use for pipeline benchmarking without a target |
| `net_ring` | In-process SPSC loopback; use for RX path testing |
| `net_bonding` | LACP / active-backup NIC bonding |

## Target Server (HTTP / HTTPS)

vaigAI speaks **HTTP/1.1 only**, **TLS 1.2/1.3** only, and buffers up to **1 MB** per response body. Run the target in an Alpine container on the same Fedora host, reachable via `net_tap` or `net_af_packet`.

| Server | Alpine pkg | Best for |
|---|---|---|
| **nginx** | `nginx` | Throughput baseline; serve static files of varied sizes |
| **Apache httpd** | `apache2 apache2-ssl` | CGI-driven dynamic sizes; POST/PUT verb testing |
| **Flask** | `python3` + `flask` | Fully programmable responses; dynamic size via URL param (`/body/<n>`) |

## Requirements

| Requirement | Minimum |
|---|---|
| Linux kernel | ≥ 5.15 with VFIO/IOMMU enabled in BIOS and kernel cmdline |
| DPDK | ≥ 24.11 LTS |
| Compiler | GCC ≥ 11 or Clang ≥ 14 (C17) |
| Build system | Meson ≥ 0.58 + Ninja |
| Hugepages | 4 × 1 GB minimum; 16 × 1 GB recommended for sustained load |
| OpenSSL | ≥ 1.1 (only required when `tls=enabled`) |

## Test Coverage Status

| Area | Status | Notes |
|---|---|---|
| Config loading / validation | ⚠️ Pending | To be added |
| REST API (all 6 endpoints) | ⚠️ Pending | To be added |
| CLI commands | ⚠️ Pending | To be added |
| Metrics JSON export | ⚠️ Pending | To be added |
| TX pipeline / SYN generation | ⚠️ Pending | To be added |
| Physical NIC link (mlx5 + i40e) | ✅ Link verified | Phase 1 ping tests passed on all 4 cable pairs |
| TCP 3-way handshake | ⚠️ Not yet tested | Requires real server target |
| HTTP request / response | ⚠️ Not yet tested | Requires real server target |
| TLS handshake | ⚠️ Not yet tested | Requires real server target |
| ICMP echo reply | ⚠️ Not yet tested | Requires vaigAI running against live peer |
| VLAN (802.1Q) end-to-end | ⚠️ Not yet tested | Config support is implemented |
| Loopback (net_ring) | ⚠️ Not yet tested | Set `TGEN_LOOPBACK=1` to enable |

## License

BSD 3-Clause License — see source file headers.
