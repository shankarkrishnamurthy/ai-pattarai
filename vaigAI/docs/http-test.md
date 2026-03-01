# HTTP NIC Test — Mellanox ConnectX-4 Loopback + QEMU Passthrough

> **Scope:** HTTP-layer performance over real NIC hardware.
> Tests TCP SYN rate (≈ HTTP RPS) and TCP data throughput using
> a physical Mellanox loopback cable and a QEMU VM with PCI passthrough.

---

## 1. Topology

```
┌─────────────────────────────── Host ──────────────────────────────────┐
│                                                                       │
│  vaigAI (DPDK mlx5 PMD)                          QEMU/KVM VM         │
│  ┌────────────────────┐                    ┌──────────────────────┐   │
│  │  lcore 14: mgmt    │                    │  Alpine Linux 3.23   │   │
│  │  lcore 15: worker  │                    │                      │   │
│  └────────┬───────────┘                    │  nginx        :80    │   │
│           │                                │  socat echo   :5000  │   │
│    ┌──────┴──────┐    loopback cable       │  socat discard:5001  │   │
│    │ ens30f0np0  │◄══════════════════════►│  socat chargen:5002  │   │
│    │ 0000:95:00.0│    50 Gbps ConnectX-4   │                      │   │
│    │ DPDK mlx5   │                         │  ens30f1np1 (eth0)   │   │
│    │ bifurcated  │                         │  0000:95:00.1        │   │
│    └─────────────┘                         │  vfio-pci passthru   │   │
│    10.0.0.1                                │  10.0.0.2            │   │
│                                            └──────────────────────┘   │
└───────────────────────────────────────────────────────────────────────┘
```

### Parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `NIC_PCI_VAIGAI` | `0000:95:00.0` | PCI BDF — vaigAI side (DPDK mlx5 bifurcated) |
| `NIC_PCI_VM` | `0000:95:00.1` | PCI BDF — VM side (vfio-pci passthrough) |
| `VAIGAI_IP` | `10.0.0.1` | vaigAI source IP |
| `VM_IP` | `10.0.0.2` | VM target IP |
| `DPDK_LCORES` | `14-15` | NUMA-aligned lcores (1 mgmt + 1 worker) |
| `VM_MEM` / `VM_CPUS` | `1024` / `2` | VM resources |

> **Why 2 lcores?** TCP connections are stored in per-worker TCB stores.
> With multiple workers, RSS may route SYN-ACKs to a different worker
> than the one that created the TCB, causing silent connection failure.

### VM Spec

| Component | Detail |
|-----------|--------|
| Hypervisor | QEMU 9.2 + KVM, `q35` machine |
| NIC | vfio-pci passthrough of ConnectX-4 port 1 |
| OS | Alpine Linux 3.23 (host kernel + initramfs) |
| Rootfs | ext4 image, COW-copied per run, IP patched at boot |

---

## 2. Tests

### Connectivity Check — ARP + ICMP

```
vaigai>  ping 10.0.0.2 5 56 500
```

Retries up to 5 times (VM NIC may take time to link up after passthrough).

| Metric | Pass |
|--------|------|
| `Reply from` count | ≥ 1 |

---

### T1: HTTP RPS — TCP SYN Flood

Measures connection-establishment rate. Each SYN triggers a 3-way handshake
with nginx — for HTTP/1.0 connection-per-request patterns, **CPS ≈ RPS**.

```
                vaigAI                          VM (nginx :80)
                  │                                  │
                  │──── SYN ────────────────────────►│
                  │◄─── SYN-ACK ────────────────────│
                  │──── RST (flood — no data) ─────►│
                  │                                  │
                  ×  repeat at wire rate / capped    ×
```

#### T1a — Unlimited Flood

```
vaigai>  flood tcp 10.0.0.2 10 0 56 80
```

| Metric | Pass |
|--------|------|
| `tx_pkts` | > 0 |
| Reported | Flood CPS (SYN packets / duration) |

#### T1b — Rate-Limited

```
vaigai>  flood tcp 10.0.0.2 10 5000 56 80
```

| Metric | Pass |
|--------|------|
| `tx_pkts` | > 0 |
| Measured CPS | within ±50% of `TARGET_CPS` |

**Exercises:**  `tx_gen.c` SYN builder · `tcp_fsm_connect` · `tcp_port_alloc` churn ·
`tcp_options_write_syn` · ARP resolution · rate token bucket

---

### T2: HTTP Throughput — TCP Data Transfer

Measures sustained TCP payload throughput to the VM's discard server.
vaigAI opens 4 parallel TCP streams and pumps zero-fill data continuously.

```
                vaigAI                       VM (socat discard :5001)
                  │                                  │
                  │──── SYN ────────────────────────►│
                  │◄─── SYN-ACK ────────────────────│
                  │──── ACK ────────────────────────►│  ← ESTABLISHED
                  │                                  │
                  │──── PSH+ACK [1460 B] ──────────►│
                  │──── PSH+ACK [1460 B] ──────────►│
                  │◄─── ACK ────────────────────────│
                  │          ... (10 seconds) ...    │
                  │                                  │
                  │──── FIN+ACK ───────────────────►│
                  │◄─── FIN+ACK ────────────────────│
                  │──── ACK ────────────────────────►│  ← TIME_WAIT
```

```
vaigai>  throughput tx 10.0.0.2 5001 10 4
```

| Metric | Pass |
|--------|------|
| `tcp_payload_tx` | > 0 |
| `tcp_conn_open` | > 0 (= streams) |
| `tcp_retransmit` | 0 (warn if > 0) |
| Reported | Throughput in Mbps |

**Exercises:**  Full FSM walk SYN_SENT → ESTABLISHED → FIN_WAIT → TIME_WAIT ·
`tcp_fsm_send` with MSS chunking · congestion control (slow start → avoidance) ·
`snd_wnd` / `cwnd` flow control · TCP timestamp options · delayed ACK ·
`tcp_checksum_set` (HW offload with pseudo-header) · `tcp_fsm_close` · graceful shutdown

---

## 3. Code Coverage

| Source File | Connectivity | T1 (RPS) | T2 (Throughput) |
|---|:---:|:---:|:---:|
| `tcp_fsm.c` — connect | | ● | ● |
| `tcp_fsm.c` — send / input | | | ● |
| `tcp_fsm.c` — close / FIN | | | ● |
| `tcp_fsm.c` — RTO retransmit | | ● | ● |
| `tcp_tcb.c` — alloc / lookup / free | | ● | ● |
| `tcp_options.c` — SYN + data | | ● | ● |
| `tcp_congestion.c` | | | ● |
| `tcp_timer.c` — RTO + TIME_WAIT | | ● | ● |
| `tcp_port_pool.c` | | ● | ● |
| `tcp_checksum.h` (HW offload) | | ● | ● |
| `tx_gen.c` — SYN flood builder | | ● | |
| `arp.c` — resolve + reply | ● | ● | ● |
| `icmp.c` — echo request/reply | ● | | |
| `ipv4.c` — validate + strip | ● | ● | ● |
| `port_init.c` — mlx5 descriptor negotiation | ● | ● | ● |
| `metrics.c` — snapshot | ● | ● | ● |
| `cli.c` — reset (TCB store clear) | | ● | ● |

---

## 4. Lifecycle

```
┌──────────────────────────────────────────────────────────────────────┐
│  PRE-FLIGHT                                                          │
│  ✓ root  ✓ vaigai binary  ✓ qemu-system-x86_64  ✓ vmlinuz          │
│  ✓ initramfs  ✓ rootfs ext4  ✓ PCI devices exist  ✓ vfio-pci       │
│  ✓ IOMMU group isolation check                                       │
└─────────────────────────────┬────────────────────────────────────────┘
                              │
┌─────────────────────────────▼────────────────────────────────────────┐
│  SETUP                                                               │
│                                                                      │
│  1. Bind NIC_PCI_VM to vfio-pci  (save original driver for restore) │
│  2. COW-copy rootfs → patch /etc/network/interfaces with VM_IP      │
│  3. Boot QEMU: KVM + vfio-pci passthrough + host kernel             │
│     Wait for "nginx started" in serial console (up to 90s)          │
│  4. Start vaigai on NIC_PCI_VAIGAI via DPDK mlx5 bifurcated driver  │
│     FIFO-based stdin, log capture, wait for CLI prompt               │
└─────────────────────────────┬────────────────────────────────────────┘
                              │
┌─────────────────────────────▼────────────────────────────────────────┐
│  RUN                                                                 │
│                                                                      │
│  Connectivity ─► ping 10.0.0.2 (retry ×5)                           │
│  T1a ──────────► flood tcp 10.0.0.2 10 0 56 80     (unlimited)      │
│  reset                                                               │
│  T1b ──────────► flood tcp 10.0.0.2 10 5000 56 80  (rate-limited)   │
│  reset                                                               │
│  T2 ───────────► throughput tx 10.0.0.2 5001 10 4                    │
│                                                                      │
│  After each command: stats → JSON parse → assert                     │
└─────────────────────────────┬────────────────────────────────────────┘
                              │
┌─────────────────────────────▼────────────────────────────────────────┐
│  TEARDOWN  (EXIT trap — always runs)                                 │
│                                                                      │
│  • vaigai: send "quit" via FIFO, wait, kill -9 if needed            │
│  • QEMU: kill, wait, remove COW rootfs + serial log                  │
│  • vfio-pci: unbind NIC_PCI_VM, restore original driver              │
│  • Clean temp files: FIFO, JSON config, logs                         │
└──────────────────────────────────────────────────────────────────────┘
```

---

## 5. Usage

```bash
# Run all tests
bash tests/http_nic.sh

# Run only T1 (RPS)
bash tests/http_nic.sh --test 1

# Run only T2 (Throughput)
bash tests/http_nic.sh --test 2

# Keep topology alive for debugging
bash tests/http_nic.sh --keep

# Override parameters
FLOOD_DURATION=30 TARGET_CPS=10000 bash tests/http_nic.sh
```

---

## 6. Prerequisites

| Dependency | Purpose |
|---|---|
| QEMU ≥ 6.0 + KVM | VM with vfio-pci passthrough |
| Mellanox ConnectX-4/5/6 | Two-port NIC with loopback cable |
| DPDK 24.11+ | mlx5 PMD (bifurcated driver) |
| Host vmlinuz + initramfs | Shared kernel for QEMU direct boot |
| Alpine rootfs ext4 | Guest filesystem (nginx, socat pre-installed) |
| IOMMU enabled | `intel_iommu=on` or `amd_iommu=on` in kernel cmdline |
| vfio-pci module | Kernel module for PCI passthrough |
| Root privileges | DPDK hugepages, vfio-pci bind, QEMU/KVM |

---

## 7. Known Issues & Notes

| Issue | Detail |
|---|---|
| ConnectX-4 DevX | Firmware ≤ 12.x does not support DevX. DPDK mlx5 falls back to Verbs mode; `log_max_wq_sz` defaults to 13 (8192 WQEs). |
| Per-worker TCB store | RSS distributes packets across workers. A TCB created on worker *W* is invisible to other workers. Limit to 1 worker for connection-oriented tests. |
| TCB hash tombstones | `tcb_free()` leaves tombstone markers (`-2`). After SYN floods, `cmd_reset` calls `tcb_store_reset()` to clear the hash table entirely. |
| VM NIC link-up delay | PCI passthrough NIC may take 5–10s to link up. Connectivity check retries 5× with 3s intervals. |
| Rootfs IP override | Alpine's `/etc/network/interfaces` is patched in the COW copy before boot to match `VM_IP`. |
