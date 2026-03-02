# TLS NIC Test — Intel XXV710 Loopback + QEMU + QAT Crypto

> **Scope:** Raw TLS handshake and bulk-encryption performance over physical NIC
> hardware with optional Intel QAT DH895XCC crypto offload.
> Server: `openssl s_server` inside QEMU VM.

---

## At a Glance

| | |
|---|---|
| **Script** | `tests/tls_nic.sh` |
| **Transport** | Physical NIC loopback (XXV710 i40e 25 Gbps) |
| **Peer** | QEMU VM: `openssl s_server` (:4433) |
| **Crypto** | QAT DH895XCC hw offload ↔ OpenSSL software (2×2 matrix) |
| **Tests** | T1: TLS TPS · T2: Bulk throughput · T3: Latency p50/p95/p99 · T4: Crypto matrix · T5: Concurrency |
| **Code exercised** | `tls_engine.c` · `cryptodev.c` · `cert_mgr.c` · `tcp_fsm.c` · `histogram.c` |

---

## 1. Topology

```
┌──────────────────────────────── Host ─────────────────────────────────┐
│                                                                       │
│  vaigAI (DPDK i40e PMD)                       QEMU/KVM VM            │
│  ┌────────────────────┐                 ┌──────────────────────┐      │
│  │  lcore 14: mgmt    │                 │  Alpine Linux 3.23   │      │
│  │  lcore 15: worker  │                 │                      │      │
│  │                    │                 │  openssl s_server    │      │
│  │  ┌──────────────┐ │                 │    :4433 (TLS)       │      │
│  │  │ DPDK crypto  │ │                 │  socat discard :5001 │      │
│  │  │ crypto_qat   │ │                 │                      │      │
│  │  └──────┬───────┘ │                 └──────────┬───────────┘      │
│  └─────────┼──────────┘                           │                   │
│            │                                      │                   │
│    ┌───────┴──────┐    loopback cable    ┌────────┴─────────┐        │
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
| `TLS_PORT` | `4433` | openssl s_server listen port |
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

SW:  OpenSSL software AES-GCM (default hot path)
QAT: DPDK crypto_qat PMD (vaigai) / qatengine (server)
```

### VM Spec

| Component | Detail |
|-----------|--------|
| Hypervisor | QEMU 9.2 + KVM, `q35` machine |
| NIC | vfio-pci passthrough of XXV710 port 1 |
| QAT | vfio-pci passthrough (when `SERVER_CRYPTO=qat`) |
| OS | Alpine Linux 3.23 (host kernel + initramfs) |
| Rootfs | ext4 image, COW-copied per run |
| TLS server | `openssl s_server -accept 4433 -cert ... -cipher $TLS_CIPHER` |

---

## 2. Tests

### Connectivity Check — ARP + ICMP

```
vaigai>  ping 10.0.0.2 5 56 500
```

Retries up to 5 times (VM NIC may take time to link up after PCI passthrough).

| Metric | Pass |
|--------|------|
| `Reply from` count | ≥ 1 |

---

### T1: TLS Handshake TPS

Measures connection-establishment rate: TCP 3WHS + full TLS handshake per
connection. Each handshake exercises key exchange, certificate validation,
and symmetric cipher negotiation.

```
              vaigAI                        VM (openssl s_server :4433)
                │                                    │
                │──── SYN ──────────────────────────►│
                │◄─── SYN-ACK ──────────────────────│
                │──── ACK ──────────────────────────►│  ← TCP ESTABLISHED
                │                                    │
                │──── ClientHello ──────────────────►│
                │◄─── ServerHello + Certificate ────│
                │◄─── ServerHelloDone ──────────────│
                │──── ClientKeyExchange ───────────►│
                │──── ChangeCipherSpec ────────────►│
                │──── Finished ────────────────────►│
                │◄─── ChangeCipherSpec ─────────────│
                │◄─── Finished ─────────────────────│  ← TLS ESTABLISHED
                │                                    │
                │──── RST (close) ─────────────────►│
                ×  repeat at wire rate / capped      ×
```

#### T1a — Unlimited Flood (peak discovery)

```
vaigai>  tps 10.0.0.2 10 0 56 4433
```

| Metric | Pass |
|--------|------|
| `tls_handshake_ok` | > 0 |
| `tls_handshake_fail` | = 0 |
| **Peak TPS** | Reported prominently |

#### T1b — Rate-Limited

```
vaigai>  tps 10.0.0.2 10 5000 56 4433
```

| Metric | Pass |
|--------|------|
| `tls_handshake_ok` | > 0 |
| Measured TPS | within ±50% of `TARGET_CPS` |

**Exercises:** `tls_engine.c` handshake · `cert_mgr.c` cert load · `cryptodev.c` AES-GCM ·
`tcp_fsm.c` connect · `tcp_options.c` SYN · `tx_gen.c` SYN builder · rate token bucket

---

### T2: TLS Bulk Throughput

Measures sustained encrypted data transfer rate. vaigAI opens parallel TLS
streams and pumps encrypted payload data continuously.

```
              vaigAI                        VM (openssl s_server :4433)
                │                                    │
                │──── TLS Handshake ───────────────►│  ← (as T1)
                │                                    │
                │──── [TLS Record: 16 KB] ─────────►│
                │──── [TLS Record: 16 KB] ─────────►│
                │◄─── ACK ──────────────────────────│
                │          ... (10 seconds) ...      │
                │                                    │
                │──── FIN+ACK ─────────────────────►│
                │◄─── FIN+ACK ──────────────────────│
                │──── ACK ─────────────────────────►│  ← TIME_WAIT
```

```
vaigai>  throughput tx 10.0.0.2 4433 10 4
```

| Metric | Pass |
|--------|------|
| `tcp_payload_tx` | > 0 |
| `tls_records_tx` | > 0 |
| `tls_handshake_fail` | = 0 |
| `tcp_retransmit` | = 0 (warn if > 0) |
| **Peak Throughput (Mbps)** | Reported prominently |

**Exercises:** `tls_engine.c` record encrypt · `cryptodev.c` bulk AES-GCM offload ·
`tcp_fsm.c` send/close · `tcp_congestion.c` slow start → avoidance · delayed ACK

---

### T3: TLS Handshake Latency — p50 / p95 / p99

Rate-limited handshake test with latency percentile capture.
Uses half the `TARGET_CPS` rate to avoid saturation effects.

```
vaigai>  tps 10.0.0.2 10 2500 56 4433
```

| Metric | Pass |
|--------|------|
| `tls_handshake_ok` | > 0 |
| **p50 / p95 / p99 (µs)** | Reported prominently |

**Exercises:** `histogram.c` latency recording · `tls_engine.c` handshake timing ·
all T1 code paths under controlled load

---

### T4: Crypto Acceleration Matrix (2×2)

Compares all combinations of QAT and software crypto on vaigai and server sides.
Each permutation performs a short TPS flood and throughput test.

```
┌────────────┬────────────────┬────────────────┐
│            │  Server: SW    │  Server: QAT   │
├────────────┼────────────────┼────────────────┤
│ vaigai: SW │  TPS=___       │  TPS=___       │
│            │  Mbps=___      │  Mbps=___      │
├────────────┼────────────────┼────────────────┤
│ vaigai: QAT│  TPS=___       │  TPS=___       │
│            │  Mbps=___      │  Mbps=___      │
└────────────┴────────────────┴────────────────┘
```

Each cell runs: (1) stop vaigai/QEMU, (2) rebind QAT devices, (3) restart
with new crypto mode, (4) connectivity check, (5) 5s TPS flood, (6) 5s
throughput test. Results printed as a table with QAT acceleration factor.

**Exercises:** Full lifecycle restart · `cryptodev_init()` probe/skip · QAT
vfio-pci bind/unbind · `tls_engine.c` engine switching

---

### T5: Concurrent Connection Scaling

Sweeps `max_concurrent` through [256, 1024, 4096, 16384, 65536] and measures
peak TPS at each level.

```
     TPS
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
| Peak TPS | > 0 |
| All levels complete | ✓ |

**Exercises:** `tcb_store.c` capacity scaling · `tcp_port_pool.c` exhaustion ·
`mempool.c` buffer pressure · worker poll loop throughput under load

---

## 3. Code Coverage

| Source File | T1 | T2 | T3 | T4 | T5 |
|---|:---:|:---:|:---:|:---:|:---:|
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
| `cli.c` — reset | ● | ● | ● | ● | ● |

---

## 4. Lifecycle

```
┌──────────────────────────────────────────────────────────────────────┐
│  PRE-FLIGHT                                                          │
│  ✓ root  ✓ vaigai binary  ✓ qemu-system-x86_64  ✓ openssl          │
│  ✓ vmlinuz  ✓ initramfs  ✓ rootfs ext4  ✓ PCI NICs exist           │
│  ✓ vfio-pci module  ✓ QAT devices (if VAIGAI_CRYPTO=qat)           │
│  ✓ QAT_PCI_VAIGAI ≠ QAT_PCI_SERVER                                 │
└─────────────────────────────┬────────────────────────────────────────┘
                              │
┌─────────────────────────────▼────────────────────────────────────────┐
│  SETUP                                                               │
│                                                                      │
│  1. Generate ephemeral EC TLS cert (prime256v1, 1-day validity)     │
│  2. Bind NIC_PCI_VM to vfio-pci  (save original driver)            │
│  3. Bind QAT device(s) to vfio-pci if crypto=qat                   │
│  4. COW-copy rootfs → inject cert, start-tls.sh, network config    │
│  5. Boot QEMU: KVM + q35 + vfio-pci NIC [+ QAT] + host kernel     │
│     Wait for "vaigAI TLS test services started" in serial (90s)     │
│  6. Start vaigai with DPDK crypto_qat PMD (if qat) on NIC_VAIGAI   │
│     FIFO-based stdin, log capture, wait for CLI prompt               │
└─────────────────────────────┬────────────────────────────────────────┘
                              │
┌─────────────────────────────▼────────────────────────────────────────┐
│  RUN                                                                 │
│                                                                      │
│  Connectivity ─► ping 10.0.0.2 (retry ×5)                           │
│  T1a ──────────► tps ... 0 56 4433           (unlimited — peak TPS) │
│  T1b ──────────► tps ... 5000 56 4433        (rate-limited)         │
│  T2  ──────────► throughput tx ... 4433 10 4 (bulk encrypted data)  │
│  T3  ──────────► tps ... 2500 56 4433        (latency percentiles)  │
│  T4  ──────────► 2×2 matrix (restart per combo, 5s flood + 5s tput) │
│  T5  ──────────► concurrency sweep [256..65536] (5s flood each)     │
│                                                                      │
│  After each command: stats → JSON parse → assert → peak report      │
└─────────────────────────────┬────────────────────────────────────────┘
                              │
┌─────────────────────────────▼────────────────────────────────────────┐
│  TEARDOWN  (EXIT trap — always runs)                                 │
│                                                                      │
│  • vaigai: send "quit" via FIFO, wait, kill -9 if needed            │
│  • QEMU: kill, wait, remove COW rootfs + serial log                  │
│  • vfio-pci: unbind NIC + QAT, restore original drivers             │
│  • Clean temp files: FIFO, cert, key, JSON config, logs             │
└──────────────────────────────────────────────────────────────────────┘
```

---

## 5. Usage

```bash
# Run all tests (default: QAT vaigai, SW server)
bash tests/tls_nic.sh

# Run only T1 (TPS)
bash tests/tls_nic.sh --test 1

# Run only T4 (crypto matrix)
bash tests/tls_nic.sh --test 4

# Keep topology alive for debugging
bash tests/tls_nic.sh --keep

# Override cipher and crypto mode
TLS_CIPHER=AES256-GCM-SHA384 VAIGAI_CRYPTO=sw bash tests/tls_nic.sh

# Use second NIC pair
NIC_PCI_VAIGAI=0000:83:00.0 NIC_PCI_VM=0000:83:00.1 \
  NIC_IFACE_VM=ens23f1np1 bash tests/tls_nic.sh

# Software-only (no QAT)
VAIGAI_CRYPTO=sw SERVER_CRYPTO=sw bash tests/tls_nic.sh
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
| Alpine rootfs ext4 | Guest filesystem (openssl, socat pre-installed) |
| OpenSSL 3.x | Certificate generation + s_server |
| IOMMU enabled | `intel_iommu=on` in kernel cmdline |
| vfio-pci module | PCI passthrough for NIC + QAT |
| qatengine (in rootfs) | Server-side QAT crypto (`SERVER_CRYPTO=qat`) |
| Root privileges | Hugepages, vfio-pci bind, QEMU/KVM |

---

## 7. Known Issues & Notes

| Issue | Detail |
|---|---|
| **QAT device sharing** | A single QAT device cannot be shared between vaigai and VM simultaneously. The script validates `QAT_PCI_VAIGAI ≠ QAT_PCI_SERVER`. |
| **Per-worker TCB store** | RSS may route SYN-ACKs to a different worker than the TCB creator. Limit to 1 worker for TLS tests. |
| **cryptodev fallback** | If no QAT device is probed, `cryptodev_init()` returns non-fatally and the TLS engine uses OpenSSL software AES-GCM. |
| **VM NIC link-up delay** | PCI passthrough NIC may take 5–10s to link up. Connectivity check retries 5× with 3s intervals. |
| **Ephemeral certs** | Certificates are generated fresh each run (EC P-256, 1-day validity). No CA chain validation is performed. |
| **T4 restart overhead** | Each crypto matrix permutation restarts vaigai + QEMU, adding ~60s per cell. Total T4 time: ~5 min. |
| **NUMA alignment** | NICs on NUMA node 1 default to lcores 14-15. Override via `DPDK_LCORES` env var. |
