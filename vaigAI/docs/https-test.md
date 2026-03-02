# HTTPS NIC Test — Intel XXV710 Loopback + QEMU + nginx SSL + QAT

> **Scope:** HTTPS application-layer performance over physical NIC hardware
> with optional Intel QAT DH895XCC crypto offload.
> Server: `nginx` with SSL inside QEMU VM.

---

## TLS vs HTTPS Test Comparison

| Aspect | `tls_nic.sh` | `https_nic.sh` |
|--------|-------------|----------------|
| Server | `openssl s_server` | `nginx` with SSL |
| Port | 4433 | 443 |
| Protocol | Raw TLS | HTTPS (TLS + HTTP/1.1) |
| Response | TLS echo | Static file (1k/100k/1m) |
| HTTP metrics | No | Yes (`http_req_tx`, `http_rsp_2xx`, etc.) |
| Use case | Crypto engine benchmarking | Real-world HTTPS workload |

---

## 1. Topology

```
┌──────────────────────────────── Host ─────────────────────────────────┐
│                                                                       │
│  vaigAI (DPDK i40e PMD)                       QEMU/KVM VM            │
│  ┌────────────────────┐                 ┌──────────────────────┐      │
│  │  lcore 14: mgmt    │                 │  Alpine Linux 3.23   │      │
│  │  lcore 15: worker  │                 │                      │      │
│  │                    │                 │  nginx SSL :443      │      │
│  │  ┌──────────────┐ │                 │    /1k    (1 KB)     │      │
│  │  │ DPDK crypto  │ │                 │    /100k  (100 KB)   │      │
│  │  │ crypto_qat   │ │                 │    /1m    (1 MB)     │      │
│  │  └──────┬───────┘ │                 │  socat discard :5001 │      │
│  └─────────┼──────────┘                 └──────────┬───────────┘      │
│            │                                       │                  │
│    ┌───────┴──────┐    loopback cable    ┌─────────┴────────┐        │
│    │ ens22f0np0   │◄════════════════════►│ ens22f1np1       │        │
│    │ 0000:81:00.0 │    25 Gbps XXV710    │ 0000:81:00.1     │        │
│    │ DPDK i40e    │                      │ vfio-pci passthru│        │
│    └──────────────┘                      └──────────────────┘        │
│    10.0.0.1                               10.0.0.2                    │
│                                                                       │
│    ┌── QAT DH895XCC ──────────────────────────────────────────┐      │
│    │  0000:0b:00.0  →  vaigai (DPDK crypto_qat PMD)          │      │
│    │  0000:0c:00.0  →  VM (vfio-pci → qatengine)             │      │
│    │  0000:0d-0e,11 →  idle                                   │      │
│    └──────────────────────────────────────────────────────────┘      │
└───────────────────────────────────────────────────────────────────────┘
```

### Parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `NIC_PCI_VAIGAI` | `0000:81:00.0` | PCI BDF — vaigAI side (DPDK bifurcated) |
| `NIC_PCI_VM` | `0000:81:00.1` | PCI BDF — VM side (vfio-pci passthrough) |
| `VAIGAI_IP` | `10.0.0.1` | vaigAI source IP |
| `VM_IP` | `10.0.0.2` | VM target IP |
| `HTTP_RESP_SIZE` | `100k` | nginx static file to request (`1k`, `100k`, `1m`) |
| `TLS_CIPHER` | `AES128-GCM-SHA256` | OpenSSL cipher string |
| `VAIGAI_CRYPTO` | `qat` | vaigai-side crypto engine (`qat` or `sw`) |
| `SERVER_CRYPTO` | `sw` | server-side crypto engine (`qat` or `sw`) |
| `QAT_PCI_VAIGAI` | auto-detect | QAT PCI BDF for vaigai |
| `QAT_PCI_SERVER` | auto-detect | QAT PCI BDF for VM |
| `FLOOD_DURATION` | `10` | Flood test duration (seconds) |
| `TARGET_CPS` | `5000` | Rate-limited CPS target |
| `THROUGHPUT_DUR` | `10` | Throughput test duration (seconds) |
| `THROUGHPUT_STREAMS` | `4` | Parallel TCP streams for throughput |
| `LATENCY_DUR` | `10` | Latency test duration (seconds) |
| `DPDK_LCORES` | `14-15` | NUMA-aligned lcores (auto-detected) |
| `VM_MEM` / `VM_CPUS` | `1024` / `2` | VM resources |

### Crypto Modes

```
             ┌──────────────┬──────────────┐
             │ Server: SW   │ Server: QAT  │
┌────────────┼──────────────┼──────────────┤
│ vaigai: SW │   SW × SW    │   SW × QAT   │
│ vaigai: QAT│  QAT × SW    │  QAT × QAT   │
└────────────┴──────────────┴──────────────┘

SW:  OpenSSL software AES-GCM
QAT: DPDK crypto_qat PMD (vaigai) / qatengine + nginx ssl_engine (server)
```

### VM Spec

| Component | Detail |
|-----------|--------|
| Hypervisor | QEMU 9.2 + KVM, `q35` machine |
| NIC | vfio-pci passthrough of XXV710 port 1 |
| QAT | vfio-pci passthrough (when `SERVER_CRYPTO=qat`) |
| OS | Alpine Linux 3.23 (host kernel + initramfs) |
| Rootfs | ext4 image, COW-copied per run |
| Web server | nginx with `ssl_ciphers $TLS_CIPHER`, `ssl_engine qat` (optional) |
| Static files | `/var/www/html/{1k,100k,1m}` — random data |

---

## 2. Tests

### Connectivity Check — ARP + ICMP

```
vaigai>  ping 10.0.0.2 5 56 500
```

| Metric | Pass |
|--------|------|
| `Reply from` count | ≥ 1 |

---

### T1: HTTPS TPS

Measures full HTTPS transaction rate: TCP 3WHS + TLS handshake + HTTP
GET/response per connection. Unlike raw TLS TPS, this includes HTTP
request serialization and response parsing overhead.

```
              vaigAI                        VM (nginx SSL :443)
                │                                    │
                │──── SYN ──────────────────────────►│
                │◄─── SYN-ACK ──────────────────────│
                │──── ACK ──────────────────────────►│  ← TCP
                │                                    │
                │──── ClientHello ──────────────────►│
                │◄─── ServerHello + Cert ───────────│
                │──── Finished ────────────────────►│
                │◄─── Finished ─────────────────────│  ← TLS
                │                                    │
                │──── GET /100k HTTP/1.1 ──────────►│
                │◄─── HTTP/1.1 200 OK [100 KB] ────│  ← HTTPS
                │                                    │
                │──── RST (close) ─────────────────►│
                ×  repeat at wire rate / capped      ×
```

#### T1a — Unlimited Flood (peak discovery)

```
vaigai>  tps 10.0.0.2 10 0 56 443
```

| Metric | Pass |
|--------|------|
| `tls_handshake_ok` | > 0 |
| `tls_handshake_fail` | = 0 |
| `http_req_tx` | > 0 |
| `http_rsp_2xx` | > 0 |
| `http_rsp_4xx` | = 0 |
| `http_rsp_5xx` | = 0 |
| **Peak HTTPS TPS** | Reported prominently |

#### T1b — Rate-Limited

```
vaigai>  tps 10.0.0.2 10 5000 56 443
```

| Metric | Pass |
|--------|------|
| `tls_handshake_ok` | > 0 |
| Measured TPS | within ±50% of `TARGET_CPS` |

**Exercises:** `http11.c` request builder/response parser · `tls_engine.c` handshake ·
`cert_mgr.c` · `cryptodev.c` · `tcp_fsm.c` connect · `tx_gen.c` SYN builder

---

### T2: HTTPS Throughput

Measures sustained HTTPS download rate. vaigAI opens parallel TLS+HTTP streams
to nginx, repeatedly fetching `/$HTTP_RESP_SIZE`. Throughput includes TLS record
encryption + HTTP framing overhead.

```
              vaigAI                        VM (nginx SSL :443)
                │                                    │
                │──── TLS Handshake ───────────────►│
                │                                    │
                │──── GET /100k HTTP/1.1 ──────────►│
                │◄─── [TLS Record: response data] ──│
                │◄─── [TLS Record: response data] ──│
                │◄─── ACK ──────────────────────────│
                │          ... (10 seconds) ...      │
                │                                    │
                │──── FIN+ACK ─────────────────────►│
                │◄─── FIN+ACK ──────────────────────│
                │──── ACK ─────────────────────────►│
```

```
vaigai>  throughput tx 10.0.0.2 443 10 4
```

| Metric | Pass |
|--------|------|
| `tcp_payload_tx` | > 0 |
| `tls_records_tx` | > 0 |
| `http_req_tx` | > 0 |
| `http_parse_err` | = 0 |
| `tls_handshake_fail` | = 0 |
| `tcp_retransmit` | = 0 (warn if > 0) |
| **Peak HTTPS Throughput (Mbps)** | Reported prominently |

**Exercises:** `http11.c` GET builder · `tls_engine.c` record encrypt ·
`cryptodev.c` bulk AES-GCM · `tcp_fsm.c` send/close · `tcp_congestion.c`

---

### T3: HTTPS Transaction Latency — p50 / p95 / p99

Rate-limited HTTPS transaction test with latency percentile capture.
Measures full round-trip: SYN → TLS → HTTP GET → HTTP 200 → close.

```
vaigai>  tps 10.0.0.2 10 2500 56 443
```

| Metric | Pass |
|--------|------|
| `tls_handshake_ok` | > 0 |
| **p50 / p95 / p99 (µs)** | Reported prominently |

**Exercises:** `histogram.c` latency recording · full T1 code paths under
controlled load · `http11.c` response parse timing

---

### T4: Crypto Acceleration Matrix (2×2)

Compares all combinations of QAT and software crypto on vaigai and server:

```
┌────────────┬─────────────────┬─────────────────┐
│            │  Server: SW     │  Server: QAT    │
│            │  (nginx OpenSSL)│  (nginx qatengine)│
├────────────┼─────────────────┼─────────────────┤
│ vaigai: SW │  HTTPS TPS=___  │  HTTPS TPS=___  │
│            │  HTTPS Mbps=___ │  HTTPS Mbps=___ │
├────────────┼─────────────────┼─────────────────┤
│ vaigai: QAT│  HTTPS TPS=___  │  HTTPS TPS=___  │
│            │  HTTPS Mbps=___ │  HTTPS Mbps=___ │
└────────────┴─────────────────┴─────────────────┘
```

Each cell restarts vaigai + QEMU with the appropriate crypto config.
Reports QAT acceleration factor vs SW-only baseline.

**Exercises:** Full lifecycle restart · nginx `ssl_engine qat` config ·
`cryptodev_init()` probe/skip · QAT vfio-pci bind/unbind

---

### T5: Concurrent Connection Scaling

Sweeps `max_concurrent` through [256, 1024, 4096, 16384, 65536] and measures
peak HTTPS TPS at each level.

```
     HTTPS TPS
      │
  16k ┤                    ◆ ← peak
      │               ◆
   8k ┤          ◆
      │     ◆
   4k ┤◆
      └──┬─────┬─────┬─────┬─────┬──
        256  1024  4096  16384 65536
                 Concurrency
```

| Metric | Pass |
|--------|------|
| Peak HTTPS TPS | > 0 |
| All levels complete | ✓ |

**Exercises:** `tcb_store.c` capacity · `tcp_port_pool.c` exhaustion ·
`http11.c` under concurrent load · nginx connection handling

---

## 3. Code Coverage

| Source File | T1 | T2 | T3 | T4 | T5 |
|---|:---:|:---:|:---:|:---:|:---:|
| `http11.c` — request builder | ● | ● | ● | ● | ● |
| `http11.c` — response parser | ● | ● | ● | ● | ● |
| `tls_engine.c` — handshake | ● | ● | ● | ● | ● |
| `tls_engine.c` — record encrypt | | ● | | ● | |
| `tls_session.c` — session store | ● | ● | ● | ● | ● |
| `cert_mgr.c` — cert load | ● | ● | ● | ● | ● |
| `cryptodev.c` — QAT init/submit/poll | ● | ● | ● | ● | ● |
| `tcp_fsm.c` — connect | ● | ● | ● | ● | ● |
| `tcp_fsm.c` — send / input | | ● | | ● | |
| `tcp_fsm.c` — close / FIN | | ● | | ● | |
| `tcp_tcb.c` — alloc / lookup / free | ● | ● | ● | ● | ● |
| `tcp_options.c` — SYN + data | ● | ● | ● | ● | ● |
| `tcp_congestion.c` | | ● | | ● | |
| `tcp_timer.c` — RTO + TIME_WAIT | ● | ● | ● | ● | ● |
| `tcp_port_pool.c` | ● | ● | ● | ● | ● |
| `tcp_checksum.h` (HW offload) | ● | ● | ● | ● | ● |
| `tx_gen.c` — SYN builder | ● | | ● | ● | ● |
| `arp.c` — resolve + reply | ● | ● | ● | ● | ● |
| `icmp.c` — echo reply | ● | | ● | | |
| `histogram.c` — latency | | | ● | | |
| `metrics.c` — snapshot | ● | ● | ● | ● | ● |
| `config_mgr.c` — JSON push | ● | ● | ● | ● | ● |
| `cli.c` — reset | ● | ● | ● | ● | ● |

---

## 4. Lifecycle

```
┌──────────────────────────────────────────────────────────────────────┐
│  PRE-FLIGHT                                                          │
│  ✓ root  ✓ vaigai binary  ✓ qemu-system-x86_64  ✓ openssl          │
│  ✓ vmlinuz  ✓ initramfs  ✓ rootfs ext4 (nginx pre-installed)       │
│  ✓ vfio-pci module  ✓ QAT devices (if crypto=qat)                  │
│  ✓ QAT_PCI_VAIGAI ≠ QAT_PCI_SERVER                                 │
└─────────────────────────────┬────────────────────────────────────────┘
                              │
┌─────────────────────────────▼────────────────────────────────────────┐
│  SETUP                                                               │
│                                                                      │
│  1. Generate ephemeral EC TLS cert (prime256v1)                     │
│  2. Bind NIC_PCI_VM to vfio-pci                                    │
│  3. Bind QAT device(s) to vfio-pci if crypto=qat                   │
│  4. COW-copy rootfs → inject:                                       │
│     - TLS cert/key → /etc/vaigai/                                   │
│     - Static files → /var/www/html/{1k,100k,1m}                    │
│     - nginx SSL config → /etc/nginx/http.d/vaigai-ssl.conf         │
│     - Start script → /etc/vaigai/start-https.sh                    │
│  5. Boot QEMU: KVM + q35 + vfio-pci NIC [+ QAT] + host kernel     │
│  6. Start vaigai with DPDK crypto_qat PMD on NIC_VAIGAI            │
└─────────────────────────────┬────────────────────────────────────────┘
                              │
┌─────────────────────────────▼────────────────────────────────────────┐
│  RUN                                                                 │
│                                                                      │
│  Connectivity ─► ping 10.0.0.2 (retry ×5)                           │
│  T1a ──────────► tps ... 0 56 443             (unlimited HTTPS TPS) │
│  T1b ──────────► tps ... 5000 56 443          (rate-limited)        │
│  T2a ──────────► throughput tx ... 443 10 4   (unlimited Mbps)      │
│  T2b ──────────► throughput tx ... 443 10 4   (rate-limited)        │
│  T3  ──────────► tps ... 2500 56 443          (latency percentiles) │
│  T4  ──────────► 2×2 matrix (restart per combo)                     │
│  T5  ──────────► concurrency sweep [256..65536]                     │
│                                                                      │
│  After each: stats → JSON parse → assert HTTP metrics → peak report │
└─────────────────────────────┬────────────────────────────────────────┘
                              │
┌─────────────────────────────▼────────────────────────────────────────┐
│  TEARDOWN  (EXIT trap — always runs)                                 │
│                                                                      │
│  • vaigai: quit via FIFO → wait → kill -9                           │
│  • QEMU: kill → remove COW rootfs + serial log                      │
│  • vfio-pci: unbind NIC + QAT → restore original drivers            │
│  • Clean: cert, key, FIFO, config JSON, logs                        │
└──────────────────────────────────────────────────────────────────────┘
```

---

## 5. Usage

```bash
# Run all tests (default: QAT vaigai, SW server)
bash tests/https_nic.sh

# Run only T1 (HTTPS TPS)
bash tests/https_nic.sh --test 1

# Run only T4 (crypto matrix)
bash tests/https_nic.sh --test 4

# Keep topology alive for debugging
bash tests/https_nic.sh --keep

# Small response (1 KB)
HTTP_RESP_SIZE=1k bash tests/https_nic.sh

# Large response (1 MB)
HTTP_RESP_SIZE=1m bash tests/https_nic.sh

# Override cipher and crypto mode
TLS_CIPHER=AES256-GCM-SHA384 VAIGAI_CRYPTO=sw bash tests/https_nic.sh

# Use second NIC pair
NIC_PCI_VAIGAI=0000:83:00.0 NIC_PCI_VM=0000:83:00.1 \
  NIC_IFACE_VM=ens23f1np1 bash tests/https_nic.sh

# Software-only (no QAT)
VAIGAI_CRYPTO=sw SERVER_CRYPTO=sw bash tests/https_nic.sh
```

---

## 6. Prerequisites

| Dependency | Purpose |
|---|---|
| QEMU ≥ 6.0 + KVM | VM with vfio-pci passthrough |
| Intel XXV710 (i40e) | Two-port 25 GbE NIC with loopback cable |
| Intel QAT DH895XCC | Crypto offload (≥ 2 devices for T4 matrix) |
| DPDK 24.11+ | i40e PMD + crypto_qat PMD |
| Host vmlinuz + initramfs | Shared kernel for QEMU direct boot |
| Alpine rootfs ext4 | Guest filesystem (nginx, openssl, socat pre-installed) |
| OpenSSL 3.x | Certificate generation |
| IOMMU enabled | `intel_iommu=on` in kernel cmdline |
| vfio-pci module | PCI passthrough for NIC + QAT |
| qatengine (in rootfs) | Server-side QAT crypto (`SERVER_CRYPTO=qat`) |
| Root privileges | Hugepages, vfio-pci bind, QEMU/KVM |

---

## 7. Known Issues & Notes

| Issue | Detail |
|---|---|
| **nginx worker_processes** | Alpine's default nginx config uses 1 worker. For high TPS, increase `worker_processes auto` in nginx.conf. |
| **HTTP_RESP_SIZE** | Large responses (1m) stress TCP window scaling and congestion control more than TLS. Small responses (1k) stress handshake rate. |
| **QAT + nginx** | `ssl_engine qat` requires qatengine + usdm_drv loaded in the VM. The QAT PCI device must be passed through via vfio-pci. |
| **QAT device sharing** | Single QAT device cannot be shared between vaigai and VM. Script validates `QAT_PCI_VAIGAI ≠ QAT_PCI_SERVER`. |
| **Per-worker TCB store** | Limit to 1 worker for connection-oriented tests (RSS routing issue). |
| **VM NIC link-up delay** | PCI passthrough NIC takes 5–10s to link up. Connectivity check retries 5×. |
| **T4 restart overhead** | Each matrix cell restarts vaigai + QEMU (~60s each). Total T4 time: ~5 min. |
| **HTTPS vs TLS overhead** | HTTPS TPS is lower than raw TLS TPS due to HTTP request/response parsing and nginx application processing. |
