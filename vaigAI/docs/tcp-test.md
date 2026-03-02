# TCP Test Plan — Dual TAP + Bridge + Firecracker

> **Scope:** TCP-only correctness and throughput. HTTP and TLS tests are covered separately.

---

## At a Glance

| | |
|---|---|
| **Script** | `tests/tcp_tap.sh` |
| **Transport** | TAP PMD → bridge → Firecracker microVM |
| **Peer** | Alpine VM: socat echo (:5000) · discard (:5001) · chargen (:5002) |
| **Tests** | T1: SYN flood CPS · T2: Full lifecycle · T3: Bidirectional throughput |
| **Code exercised** | `tcp_fsm.c` · `tcp_tcb.c` · `tcp_congestion.c` · `tcp_timer.c` · `tcp_port_pool.c` |

---

## 1. Topology

```
┌─────────────────────────────── Host ──────────────────────────────────┐
│                                                                       │
│  vaigAI                                                               │
│    └─► DPDK net_tap0                                                  │
│          │                                                            │
│   ┌──────┴──────┐     ┌────────────┐     ┌────────────┐              │
│   │ tap-vaigai  │◄═══►│ br-vaigai  │◄═══►│  tap-fc0   │              │
│   └─────────────┘     └────────────┘     └─────┬──────┘              │
│                                                 │                     │
│                                    ┌────────────▼──────────────┐      │
│                                    │   Firecracker microVM     │      │
│                                    │   Alpine · eth0=tap-fc0   │      │
│                                    │   192.168.204.2           │      │
│                                    │                           │      │
│                                    │   socat echo   :5000      │      │
│                                    │   socat discard :5001     │      │
│                                    │   socat chargen :5002     │      │
│                                    └───────────────────────────┘      │
│                                                                       │
│   vaigAI IP = 192.168.204.1                                          │
└───────────────────────────────────────────────────────────────────────┘
```

| Parameter     | Value                          |
|---------------|--------------------------------|
| Bridge        | `br-vaigai`                    |
| vaigAI TAP    | `tap-vaigai`                   |
| FC TAP        | `tap-fc0`                      |
| Subnet        | `192.168.204.0/24`             |
| vaigAI IP     | `192.168.204.1`                |
| VM IP         | `192.168.204.2`                |
| VM MAC        | `AA:FC:00:00:00:01`            |

### VM Spec

1 vCPU, 128 MB RAM, Alpine rootfs with `socat` pre-installed.
Init script starts three listeners:

| Port | socat mode | Purpose |
|------|-----------|---------|
| 5000 | `PIPE` (echo) | T1 SYN accepts, T2 data echo |
| 5001 | `/dev/null` (discard) | T3 TX throughput sink |
| 5002 | `dd if=/dev/zero bs=64k count=16384` (chargen) | T3 RX throughput source |

---

## 2. Tests

### T1: TCP SYN Flood — CPS

Benchmark connections per second.

**CLI:** `tps 192.168.204.2 10 0 56 5000`

**Exercises:** `tcp_fsm_connect` · SYN option encoding · SYN_SENT→ESTABLISHED ·
`tcb_alloc` hash insert · `tcp_port_alloc` churn · ARP resolution

| Metric | Pass |
|--------|------|
| `tcp_syn_sent` | > 0 |
| `tcp_conn_open` | > 0 |
| `tcp_reset_rx` | 0 |
| `tcp_syn_queue_drops` | 0 |

---

### T2: TCP Full Lifecycle — SYN → DATA → FIN

Complete connection with bidirectional data via echo server.

**CLI:** `throughput tx 192.168.204.2 5000 5 1`

vaigAI connects, sends data, receives echoed data, closes.

**Exercises:** Full FSM walk (SYN_SENT→ESTABLISHED→FIN_WAIT→TIME_WAIT) ·
`tcp_fsm_send` · `tcp_fsm_close` · delayed ACK · TIME_WAIT expiry ·
`congestion_on_ack` (slow start) · Timestamp options on data segments

| Metric | Pass |
|--------|------|
| `tcp_conn_open` | > 0 |
| `tcp_conn_close` | = `tcp_conn_open` |
| `tcp_retransmit` | 0 |
| `tcp_reset_rx` | 0 |

---

### T3: TCP Data Throughput — Forward & Reverse

Sustained TCP throughput measurement (iperf3 equivalent).

#### T3a: Forward (vaigAI → VM)

vaigAI opens connections to discard server and pumps data.

**CLI:** `throughput tx 192.168.204.2 5001 10 4`

**VM:** `socat TCP-L:5001,fork,reuseaddr /dev/null` — reads and drops; kernel ACKs.

**Exercises:** `tcp_fsm_send` in tight loop · MSS chunking ·
slow start → congestion avoidance · `snd_wnd` tracking · window scale

| Metric | Pass |
|--------|------|
| `tcp_payload_tx` | > 0 |
| `tcp_retransmit` | 0 |
| Throughput (Mbps) | Reported |

#### T3b: Reverse (VM → vaigAI)

VM chargen server connects to vaigAI listener and sends 1 GB.

**CLI:** `throughput rx 5001 10`

**VM:** `socat TCP:192.168.204.1:5001 SYSTEM:'dd if=/dev/zero bs=64k count=16384'`

**Exercises:** `tcp_fsm_listen` (passive accept) · `tcp_fsm_input` data receive ·
`rcv_nxt` advancement · delayed ACK coalescing · passive close (CLOSE_WAIT→LAST_ACK)

| Metric | Pass |
|--------|------|
| `tcp_payload_rx` | > 0 |
| `tcp_retransmit` | 0 |
| Throughput (Mbps) | Reported |

---

## 3. Coverage

| Source file | T1 | T2 | T3 |
|---|---|---|---|
| `tcp_fsm.c` — connect | ● | ● | ● |
| `tcp_fsm.c` — send | | ● | ● |
| `tcp_fsm.c` — input (data/ACK) | | ● | ● |
| `tcp_fsm.c` — close/FIN | | ● | ● |
| `tcp_fsm.c` — listen (passive open) | | | ● |
| `tcp_fsm.c` — delayed ACK | | ● | ● |
| `tcp_tcb.c` — alloc/lookup/free | ● | ● | ● |
| `tcp_options.c` — SYN + data | ● | ● | ● |
| `tcp_congestion.c` — on_ack | | ● | ● |
| `tcp_timer.c` — TIME_WAIT | | ● | |
| `tcp_port_pool.c` | ● | ● | ● |
| `tcp_checksum.h` (SW) | ● | ● | ● |
| `tx_gen.c` — SYN builder | ● | | |
| `arp.c` | ● | ● | ● |
| `metrics.c` | ● | ● | ● |

---

## 4. Prerequisites

| Dependency | Purpose |
|------------|---------|
| `firecracker` v1.x + `vmlinux` | microVM runtime |
| Alpine rootfs ext4 with `socat` | Guest filesystem |
| DPDK 24.11+ | TAP PMD |
| `iproute2` | Bridge management |
