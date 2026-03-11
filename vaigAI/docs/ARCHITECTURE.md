# vaigAI Architecture

> A DPDK-based traffic generator built from scratch in C17.
> Every packet is crafted in user-space; the kernel is never in the data path.

---

## Table of Contents

1. [System Architecture](#1-system-architecture) — block diagram, module map, build, startup, core assignment, memory
2. [Life of a Packet](#2-life-of-a-packet) — RX/TX paths, worker loop, protocol stack, HTTP parser, TLS, TCP flow control
3. [Telemetry System](#3-telemetry-system) — metrics ownership, counters, histogram, export, logging
4. [Control Plane / Data Plane](#4-control-plane--data-plane-segregation) — mgmt core scheduling, IPC, CLI, REST API, config, shared state
5. [Test Infrastructure](#5-test-infrastructure) — scripts, topology tiers, coverage

---

## 1. System Architecture

### 1.1 High-Level Block Diagram

```
┌─────────────────────────────────────────────────────────────────────────┐
│                           vaigAI  Process                              │
│                                                                        │
│   ┌─────────────── Control Plane ──────────────┐  ┌── Data Plane ───┐ │
│   │                                             │  │                 │ │
│   │  ┌─────────┐  ┌─────────┐  ┌────────────┐  │  │  ┌───────────┐ │ │
│   │  │  CLI    │  │  REST   │  │  Config    │  │  │  │ Worker 0  │ │ │
│   │  │  REPL   │  │  API    │  │  Manager   │  │  │  │ poll loop │ │ │
│   │  └────┬────┘  └────┬────┘  └─────┬──────┘  │  │  └─────┬─────┘ │ │
│   │       │             │             │         │  │        │       │ │
│   │       └─────────────┼─────────────┘         │  │  ┌─────┴─────┐ │ │
│   │                     │                       │  │  │ Worker 1  │ │ │
│   │              ┌──────▼───────┐               │  │  │ poll loop │ │ │
│   │              │  IPC Rings   │───────────────────▶  └─────┬─────┘ │ │
│   │              │  (SPSC)      │◀──────ACK─────────  ┌─────┴─────┐ │ │
│   │              └──────────────┘               │  │  │ Worker N  │ │ │
│   │                                             │  │  │ poll loop │ │ │
│   │  ┌─────────┐  ┌───────────┐  ┌──────────┐  │  │  └───────────┘ │ │
│   │  │  ARP    │  │ Telemetry │  │  Packet  │  │  │                 │ │
│   │  │  Mgr    │  │ Snapshot  │  │  Trace   │  │  │                 │ │
│   │  └─────────┘  └───────────┘  └──────────┘  │  │                 │ │
│   └─────────────────────────────────────────────┘  └─────────────────┘ │
│                                                                        │
│   ┌─────────────── Infrastructure ──────────────────────────────────┐  │
│   │  EAL Init · Core Assign · Mempool Factory · Port Init · SoftNIC│  │
│   └─────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────┘
         │                                             │
    ┌────▼────┐                                   ┌────▼────┐
    │  NIC 0  │                                   │  NIC 1  │
    └─────────┘                                   └─────────┘
```

### 1.2 Module Map

```
src/
├── main.c                     # Process lifecycle, lcore launch, signal handling
│
├── common/
│   ├── types.h                # Constants, enums (MAX_WORKERS=124, burst=32)
│   └── util.h/c               # TSC calibration, IP/MAC parsers, PRNG
│
├── core/                      ── Execution engine ──
│   ├── eal_init.h/c           # DPDK EAL bootstrap + custom arg parsing
│   ├── core_assign.h/c        # Auto-scaling lcore→role mapping (6 tiers)
│   ├── mempool.h/c            # Per-worker rte_mempool (NUMA-aware, 3-tier fallback)
│   ├── ipc.h/c                # SPSC rte_ring IPC (mgmt→worker + ACK path)
│   ├── worker_loop.h/c        # RX→classify→TX gen→TX drain→timer poll loop
│   └── tx_gen.h/c             # Protocol-extensible packet generator + token bucket
│
├── port/                      ── NIC abstraction ──
│   ├── port_init.h/c          # Port probe, RSS, queue setup, offload negotiation
│   └── soft_nic.h/c           # Driver detection (mlx5, af_packet, tap, etc.)
│
├── net/                       ── Protocol stack ──
│   ├── ethernet.h/c           # L2 framing, 802.1Q VLAN tag handling
│   ├── arp.h/c                # ARP cache (rte_hash), request/reply, hold queue
│   ├── ipv4.h/c               # IPv4 validate/strip (RX) + push header (TX)
│   ├── lpm.h/c                # Longest-prefix-match routing (rte_lpm wrapper)
│   ├── icmp.h/c               # ICMP echo reply, ping, unreachable
│   ├── udp.h/c                # UDP RX rings, checksum validation
│   ├── tcp_tcb.h/c            # TCB store (pre-alloc flat array + hash table)
│   ├── tcp_fsm.h/c            # Full TCP state machine (RFC 793/7323/6298/6928)
│   │                          #   IW10, effective MSS, half-open receive,
│   │                          #   TAP PMD l2_len offload, flow-controlled send
│   ├── tcp_options.h/c        # MSS, WScale, SACK-Permitted, Timestamps
│   ├── tcp_timer.h/c          # RTO retransmit (RFC 6298), TIME_WAIT expiry, delayed ACK
│   ├── tcp_congestion.h/c     # New Reno congestion control (RFC 5681; no TX retransmit)
│   ├── tcp_port_pool.h/c      # Ephemeral port bitmap [10000–59999] + reset API
│   └── tcp_checksum.h         # HW/SW checksum inline helpers
│
├── tls/                       ── Encryption ──
│   ├── tls_engine.h/c         # OpenSSL BIO-pair TLS engine
│   ├── tls_session.h/c        # Per-lcore TLS session store
│   ├── cert_mgr.h/c           # Certificate load / hot-reload
│   └── cryptodev.h/c          # DPDK Cryptodev AES-GCM offload
│
├── app/                       ── Application layer ──
│   └── http11.h/c             # HTTP/1.1 request builder + response parser
│
├── mgmt/                      ── Management plane ──
│   ├── mgmt_loop.h/c         # Cooperative run-to-completion event loop
│   ├── cli.h/c                # CLI command handlers + stat dispatcher
│   ├── cli_server.h/c         # Unix domain socket server for remote CLI attach
│   ├── cli_client.c           # Thin readline client for `vaigai --attach`
│   ├── rest.h/c               # libmicrohttpd REST API (JSON)
│   └── config_mgr.h/c         # Runtime config state (populated from CLI args)
│
└── telemetry/                 ── Observability ──
    ├── metrics.h/c            # Per-worker lock-free counter slabs
    ├── cpu_stats.h/c          # Per-worker TSC cycle accounting (RX/TX/timer/idle)
    ├── mem_stats.h/c          # Mempool, DPDK heap, TCB, hugepage queries
    ├── export.h/c             # JSON + text export (cpu/mem/net/port formatters)
    ├── histogram.h/c          # HDR-style latency histogram (log₂ buckets)
    ├── pktrace.h/c            # Per-packet capture trace buffer
    └── log.h/c                # Structured log macros (wraps rte_log)
```

### 1.3 Build System

```
┌─────────────────────────────────────────────────────────────┐
│  meson.build                                                │
│                                                             │
│  vaigai (C17, -march=native, -Wall -Werror)                │
│                                                             │
│  Required ──────────────────────────────────────────────── │
│  │  libdpdk ≥ 24.11    Data-plane I/O + PMDs               │
│  │  threads (pthreads)  POSIX threading                    │
│  │  libm               Math library                        │
│                                                             │
│  Optional (auto-detected) ─────────────────────────────── │
│  │  openssl ≥ 1.1   → HAVE_OPENSSL  (TLS engine)          │
│  │  readline         → HAVE_READLINE (interactive CLI)     │
│  │  libbpf           → HAVE_AF_XDP   (AF_XDP PMD)         │
│  │  jansson ≥ 2.14   ┐                                    │
│  │  libmicrohttpd    ┘→ HAVE_REST    (REST API + JSON)    │
│                                                             │
│  Output ── single "vaigai" binary                          │
└─────────────────────────────────────────────────────────────┘
```

```bash
meson setup build && ninja -C build    # build
meson install -C build                 # install
```

### 1.4 Startup Sequence

```
main()
  │
  ├─1── tgen_eal_init()              Parse args, init DPDK EAL, calibrate TSC
  ├─2── tgen_core_assign_init()      Map lcores → worker / mgmt roles
  ├─3── tgen_mempool_create_all()    One mempool per worker (NUMA-aware)
  ├─4── tgen_ports_init()            Configure ports, RSS, queues, start NICs
  ├─5── arp_init() / icmp_init()     Protocol subsystem init
  │     udp_init() / pktrace_init()
  │     Apply --src-ip to all ports  Set g_arp[p].local_ip for ARP + TX
  │     Apply --gateway/--netmask   Set g_arp[p].gateway_ip/netmask for next-hop routing
  ├─6── tgen_ipc_init()              Create SPSC rings (cmd + ACK)
  ├─7── g_config population         Populate runtime config from CLI args
  │                                  (max_concurrent, rest_port, tls settings)
  ├─8── cert_mgr_init()              TLS client/server SSL contexts
  │     tls_session_store_init()
  │     tls_keylog_enable()          SSLKEYLOG via --sslkeylog or $SSLKEYLOGFILE
  ├─9── tcb_stores_init()            Per-worker TCB stores (+ tcb_store_reset())
  │     tcp_port_pool_init()         Ephemeral port bitmaps (+ tcp_port_pool_reset())
  ├─10─ cryptodev_init()             HW crypto (non-fatal; falls back to SW)
  ├─11─ rte_eal_remote_launch()      Launch all worker lcores
  ├─12─ rest_server_start()          Start REST API (libmicrohttpd thread)
  ├─13─ cli_run()               ◄─── Blocks here until "quit"
  └─14─ tgen_cleanup()               Stop workers, free everything, EAL cleanup
```

### 1.5 Core Assignment

The system auto-scales management overhead based on available cores:

| Available Lcores | Mgmt Cores | Mgmt Roles Assigned                            |
|------------------|------------|------------------------------------------------|
| 2 – 4            | 1          | PRIMARY_MGMT                                   |
| 5 – 16           | 1          | PRIMARY_MGMT                                   |
| 17 – 32          | 2          | PRIMARY_MGMT + TELEMETRY                       |
| 33 – 64          | 2          | PRIMARY_MGMT + TELEMETRY                       |
| 65 – 128         | 3          | PRIMARY_MGMT + TELEMETRY + CLI_API             |
| 129+             | 4          | PRIMARY_MGMT + TELEMETRY + CLI_API + WATCHDOG  |

All remaining lcores become **workers**. Workers are distributed to ports round-robin
on the same NUMA socket. Each worker gets one RX queue and one TX queue per port,
**but only if the port has a free queue**. A worker whose position in the port's
worker list equals or exceeds the port's RX queue count is **not assigned** to that
port — this prevents multiple workers from polling the same RX queue (a DPDK
requirement: each RX queue must be read by exactly one lcore).

#### Worker-to-Queue Mapping (3 cases)

**Rule:** Worker at position `W` in a port's worker list gets `RX queue W`.
If `W >= max_rx_queues`, that worker skips the port entirely.

```
Case 1: Workers < Queues  (2 workers, 1 port with 8 queues)
─────────────────────────────────────────────────────────────
  Port 0 (8 RX queues)
  ├── RX Q0 ← Worker 0  ✅  (position 0 < 8)
  ├── RX Q1 ← Worker 1  ✅  (position 1 < 8)
  ├── RX Q2    (idle)
  ├── RX Q3    (idle)
  ├── RX Q4    (idle)
  ├── RX Q5    (idle)
  ├── RX Q6    (idle)
  └── RX Q7    (idle)

  → Queues 2–7 unused.  Add more workers (-l 0-8) to use them.

Case 2: Workers = Queues  (4 workers, 1 port with 4 queues)
─────────────────────────────────────────────────────────────
  Port 0 (4 RX queues)
  ├── RX Q0 ← Worker 0  ✅  (position 0 < 4)
  ├── RX Q1 ← Worker 1  ✅  (position 1 < 4)
  ├── RX Q2 ← Worker 2  ✅  (position 2 < 4)
  └── RX Q3 ← Worker 3  ✅  (position 3 < 4)

  → Perfect 1:1 mapping.  All queues active, no idle workers.

Case 3: Workers > Queues  (3 workers, 2 AF_PACKET ports with 1 queue each)
───────────────────────────────────────────────────────────────────────────
  Port 0 (1 RX queue)              Port 1 (1 RX queue)
  ├── RX Q0 ← Worker 0  ✅         ├── RX Q0 ← Worker 0  ✅
  ├──        Worker 1  ❌ skip     ├──        Worker 1  ❌ skip
  └──        Worker 2  ❌ skip     └──        Worker 2  ❌ skip
        (pos 1 >= 1)                      (pos 1 >= 1)

  → Workers 1 and 2 have no ports — they spin idle.
    AF_PACKET/TAP always have 1 queue; use fewer workers or
    add more ports.

Case 3b: Workers > Queues  (3 workers, 2 NIC ports with 2 queues each)
──────────────────────────────────────────────────────────────────────
  Port 0 (2 RX queues)             Port 1 (2 RX queues)
  ├── RX Q0 ← Worker 0  ✅         ├── RX Q0 ← Worker 0  ✅
  ├── RX Q1 ← Worker 1  ✅         ├── RX Q1 ← Worker 1  ✅
  └──        Worker 2  ❌ skip     └──        Worker 2  ❌ skip
        (pos 2 >= 2)                      (pos 2 >= 2)

  → Worker 2 idle.  Each port fully served by 2 workers.
```

### 1.6 Memory Layout

| Resource          | Scope       | Sizing                                                       |
|-------------------|-------------|--------------------------------------------------------------|
| **Mempools**      | Per-worker  | `next_pow2((rx_desc + tx_desc + pipeline) × 2 × queues)` mbufs; min 512 |
| **TCB stores**    | Per-worker  | Pre-allocated flat array + open-addressing hash table        |
| **ARP cache**     | Per-port    | `rte_hash` (1024 entries) + `rte_rwlock` (workers read, mgmt writes) |
| **Port pools**    | Per-worker  | Bitmap over [10000, 59999] + TIME_WAIT FIFO ring; reset preserves cursor |
| **Metric slabs**  | Per-worker  | Cache-line aligned; no cross-core writes                     |
| **IPC rings**     | Per-worker  | SPSC `rte_ring`, `max(64, next_pow2(pipeline_depth × 2))` entries |

Mempool allocation uses a 3-tier NUMA fallback: worker's socket with 1 GB pages →
`SOCKET_ID_ANY` with 2 MB pages → `SOCKET_ID_ANY` with 4 KB pages.

**Key Constants** (`common/types.h`):

| Constant | Value | Purpose |
|----------|-------|---------|
| `TGEN_MAX_PORTS` | 16 | Maximum physical/virtual ports |
| `TGEN_MAX_LCORES` | 128 | Maximum logical cores |
| `TGEN_MAX_WORKERS` | 124 | Max workers (128 − 4 mgmt) |
| `TGEN_MAX_CONNECTIONS` | 1,000,000 | TCB store capacity |
| `TGEN_MAX_TX/RX_BURST` | 32 | Packets per burst |
| `TGEN_DEFAULT_RX/TX_DESC` | 2048 | Ring descriptor count |
| `TGEN_MBUF_DATA_SZ` | 2176 | mbuf data room (2048+128) |
| `TGEN_ARP_CACHE_SZ` | 1024 | ARP hash entries |
| `TGEN_TIMEWAIT_DEFAULT_MS` | 4000 | TIME_WAIT duration |

---

## 2. Life of a Packet

### 2.1 Receive Path — From Wire to Protocol Handler

```
                       NIC Hardware
                           │
                    RSS (symmetric Toeplitz)
                    hash on IP + TCP + UDP
                           │
                    ┌──────▼──────┐
                    │  RX Queue i │  (one queue per worker per port)
                    └──────┬──────┘
                           │
               ┌───────────▼───────────┐
               │   rte_eth_rx_burst()  │  ← Worker lcore N (step 2)
               │   up to 32 mbufs     │
               └───────────┬───────────┘
                           │
               ┌───────────▼───────────┐
               │  classify_and_process │
               │  peek at ether_type   │
               │  (handle 802.1Q tag)  │
               └──┬────────────────┬───┘
                  │                │
          ether_type           ether_type
          = 0x0806             = 0x0800
          (ARP)                (IPv4)
                  │                │
         ┌────────▼────────┐  ┌───▼──────────────────────┐
         │  arp_input()    │  │  ipv4_validate_and_strip  │
         │  enqueue to     │  │  • version = 4, IHL ≥ 5  │
         │  g_arp_rings[p] │  │  • checksum (HW or SW)   │
         │  (→ mgmt core)  │  │  • drop fragments        │
         └─────────────────┘  │  • destination match      │
                              │  • strip IP header        │
                              └───┬──────────┬──────────┬─┘
                                  │          │          │
                            IPPROTO=1   IPPROTO=17  IPPROTO=6
                             (ICMP)      (UDP)       (TCP)
                                  │          │          │
                          ┌───────▼───┐ ┌────▼────┐ ┌──▼────────────┐
                          │icmp_input │ │udp_input│ │tcp_fsm_input  │
                          │echo reply │ │enqueue  │ │full state     │
                          │→ reply buf│ │to ring  │ │machine on     │
                          │           │ │(→ mgmt) │ │worker core    │
                          └───────────┘ └─────────┘ └───────────────┘
```

**Function trace — IPv4/ICMP echo reply:**

| Step | Function                     | File                | Core     |
|------|------------------------------|---------------------|----------|
| 1    | `rte_eth_rx_burst()`         | *(DPDK)*            | Worker   |
| 2    | `classify_and_process()`     | `worker_loop.c`     | Worker   |
| 3    | `ipv4_validate_and_strip()`  | `ipv4.c`            | Worker   |
| 4    | `icmp_input()`               | `icmp.c`            | Worker   |
| 5    | Return reply mbuf to loop    | `worker_loop.c`     | Worker   |
| 6    | `rte_eth_tx_burst()`         | *(DPDK)*            | Worker   |

**Function trace — ARP reply:**

| Step | Function                     | File                | Core     |
|------|------------------------------|---------------------|----------|
| 1    | `rte_eth_rx_burst()`         | *(DPDK)*            | Worker   |
| 2    | `classify_and_process()`     | `worker_loop.c`     | Worker   |
| 3    | `arp_input()`                | `arp.c`             | Worker   |
| 4    | `rte_ring_enqueue()` to ring | `arp.c`             | Worker   |
| 5    | `arp_mgmt_tick()`            | `arp.c`             | **Mgmt** |
| 6    | `arp_mgmt_process()`         | `arp.c`             | **Mgmt** |
| 7    | ARP reply TX on queue 0      | `arp.c`             | **Mgmt** |

**Next-hop routing — `arp_nexthop()`:**

All ARP lookups go through `arp_nexthop(port_id, dst_ip)` which determines
the L2 next-hop for a given destination:

```
  dst_ip
    │
    ▼
  gateway or netmask set?
    │             │
   NO            YES
    │             │
    ▼             ▼
  return       (dst_ip & mask) == (local_ip & mask)?
  dst_ip         │                    │
  (direct)      YES                  NO
                 │                    │
                 ▼                    ▼
              return               return
              dst_ip               gateway_ip
              (on-link)            (off-link)
```

- **On-link:** ARP resolves the destination directly (e.g. 10.10.10.10 on same /24)
- **Off-link:** ARP resolves the gateway, IP header keeps the real destination
- **No gateway (`0.0.0.0`):** All traffic treated as on-link (same-subnet direct-link)

Callers: `icmp_ping_start()`, `build_echo_reply()`, `tcp_fsm.c` (SYN + RST).

**Function trace — TCP data:**

| Step | Function                     | File                | Core     |
|------|------------------------------|---------------------|----------|
| 1    | `rte_eth_rx_burst()`         | *(DPDK)*            | Worker   |
| 2    | `classify_and_process()`     | `worker_loop.c`     | Worker   |
| 3    | `ipv4_validate_and_strip()`  | `ipv4.c`            | Worker   |
| 4    | `tcp_fsm_input()`            | `tcp_fsm.c`         | Worker   |
| 5    | TCB lookup by 4-tuple        | `tcp_tcb.c`         | Worker   |
| 6    | State machine transition     | `tcp_fsm.c`         | Worker   |
| 7    | `tcp_send_segment()` (ACK)   | `tcp_fsm.c`         | Worker   |
|      | sets `m->l2_len` for TAP PMD TX checksum offload              |          |
| 8    | `rte_eth_tx_burst()`         | *(DPDK)*            | Worker   |

**Notes:**
- `tcp_send_segment()` sets `m->l2_len = sizeof(struct rte_ether_hdr)` so the TAP PMD can compute L4 checksums via `RTE_MBUF_F_TX_TCP_CKSUM`.
- FIN_WAIT_1 and FIN_WAIT_2 accept incoming data (half-open receive) — required for echo servers that flush buffered data after receiving FIN.
- RST processing is skipped for TIME_WAIT and already-freed TCBs to avoid spurious `reset_rx` counts.
- **CLOSE_WAIT auto-close:** When a peer sends FIN while in ESTABLISHED, the FSM transitions to CLOSE_WAIT, ACKs the FIN, and immediately calls `tcp_fsm_close()` to send our own FIN (→ LAST_ACK). The traffic generator has no pending data, so lingering in CLOSE_WAIT is unnecessary.
- **RST for unknown connections (RFC 793 §3.4):** Segments arriving with no matching TCB trigger `tcp_send_rst_no_tcb()`, which constructs a RST reply using the RFC 793 §3.4 sequence number rules (ACK-bearing → `SEQ = ACK`; non-ACK → `SEQ = 0, ACK = SEQ + seg_len`). Incoming RSTs are never replied to.
- **`tcp_fsm_reset_all()`:** Iterates all in-use TCBs in a worker's store, sends RST+ACK to each peer, detaches any TLS state via `tls_detach_if_needed()`, then calls `tcb_store_reset()` to free all entries. Used by the `reset` CLI command.

### 2.2 Transmit Path — TX Generation Engine

The TX generator produces synthetic traffic from worker cores, controlled by the
management plane via IPC.

```
  CLI: "start --proto udp --ip 10.0.0.1 --duration 3 --rate 1000 --size 64 --port 9"
       │
       ▼
  cmd_start()                                         ── mgmt core ──
  ├── ARP-resolve destination MAC (3 s timeout)
  ├── Build tx_gen_config_t
  ├── metrics_reset()
  └── tgen_ipc_broadcast(CFG_CMD_START, config)
       │
       │  ┌──── SPSC ring per worker ────┐
       │  │  config_update_t (256 bytes) │
       └──▶  cmd = CFG_CMD_START         │
          │  payload = tx_gen_config_t   │
          └──────────┬───────────────────┘
                     │
                     ▼
  tgen_worker_loop → tgen_ipc_recv()                  ── worker core ──
  ├── tx_gen_configure(&ctx->tx_gen, &cfg)
  └── tx_gen_start(&ctx->tx_gen)
       │
       ▼
  ┌────────────────────────────────────────────┐
  │          tx_gen_burst()  (every iteration) │
  │                                            │
  │  1. Deadline check:                        │
  │     if now ≥ deadline_tsc → self-disarm    │
  │                                            │
  │  2. Token bucket refill:                   │
  │     new = elapsed × rate_pps / tsc_hz      │
  │     tokens = min(tokens + new, 32)         │
  │     (rate_pps=0 → unlimited, always 32)    │
  │                                            │
  │  3. Build packets:                         │
  │     for i in 0..to_send:                   │
  │       build_packet() → protocol dispatch   │
  │         ├─ ICMP → build_icmp_echo()        │
  │         └─ UDP  → build_udp_datagram()     │
  │                                            │
  │  NOTE: TCP data is NOT sent via tx_gen.    │
  │  TCP connections are FSM-driven:           │
  │    tcp_fsm_connect() → tcp_fsm_send()      │
  │  with flow control and error handling      │
  │  (see §2.5 TCP Flow Control below).        │
  │                                            │
  │  4. rte_eth_tx_burst(port, queue, burst)   │
  │                                            │
  │  5. Metrics: tx_pkts, tx_bytes,            │
  │     icmp_echo_tx / udp_tx                  │
  └────────────────────────────────────────────┘
```

**Packet construction — UDP datagram (inside `build_udp_datagram`):**

```
  rte_pktmbuf_alloc(mempool)
       │
       ▼
  ┌──────────────────────────────────────────────┐
  │ Ethernet Header (14 bytes)                   │
  │   dst_mac ← config.dst_mac                  │
  │   src_mac ← config.src_mac                  │
  │   ether_type = 0x0800                        │
  ├──────────────────────────────────────────────┤
  │ IPv4 Header (20 bytes)                       │
  │   src = config.src_ip                        │
  │   dst = config.dst_ip                        │
  │   protocol = 17 (UDP)                        │
  │   total_length, TTL=64, checksum             │
  ├──────────────────────────────────────────────┤
  │ UDP Header (8 bytes)                         │
  │   src_port = config.src_port                 │
  │   dst_port = config.dst_port                 │
  │   length, checksum (RFC 768)                 │
  ├──────────────────────────────────────────────┤
  │ Payload (pkt_size - 42 bytes)                │
  │   memset(0xBE)                               │
  └──────────────────────────────────────────────┘
```

### 2.3 Worker Poll Loop — Complete Iteration

Every worker lcore runs a tight, non-blocking loop:

```
  while (g_run) {
      ┌─ Step 1 ─── IPC Drain ─────────────────────────┐
      │  while (tgen_ipc_recv(worker_idx, &msg)):       │
      │    switch (msg.cmd):                            │
      │      CFG_CMD_START    → tx_gen_configure/start  │
      │      CFG_CMD_STOP     → tx_gen_stop             │
      │      CFG_CMD_SHUTDOWN → return                  │
      │    tgen_ipc_ack(worker_idx, msg.seq, rc)        │
      └─────────────────────────────────────────────────┘
                            │
      ┌─ Step 2 ─── RX + Classify + Flush ─────────────┐
      │  for each (port, queue):                        │
      │    nb = rte_eth_rx_burst(port, queue, bufs, 32) │
      │    for each mbuf:                               │
      │      reply = classify_and_process(mbuf)         │
      │      if reply → add to tx_buf                   │
      │  rte_eth_tx_burst(port, queue, tx_buf, nb_tx)   │
      │  free unsent mbufs                              │
      └─────────────────────────────────────────────────┘
                            │
      ┌─ Step 3 ─── TX Generation ──────────────────────┐
      │  if (ctx->tx_gen.active):                       │
      │    tx_gen_burst(&ctx->tx_gen, mempool, port)    │
      └─────────────────────────────────────────────────┘
                            │
      ┌─ Step 4 ─── Timer Tick ─────────────────────────┐
      │  tcp_timer_tick(worker_idx)                     │
      │    • RTO retransmit (RFC 6298 backoff)          │
      │      — arm on first unACKed; restart on ACK    │
      │      — disarm when snd_una == snd_nxt          │
      │    • TIME_WAIT expiry (conn_close counted once) │
      │    • Delayed ACK flush                          │
      └─────────────────────────────────────────────────┘
                            │
      ┌─ Step 5 ─── Port Pool Tick ────────────────────┐
      │  tcp_port_pool_tick(worker_idx)                 │
      │    • Drain TIME_WAIT FIFO ring                 │
      │    • Reclaim expired ephemeral ports            │
      │    • Update internal TSC timestamp              │
      └────────────────────────────────────────────────┘
  }
```

### 2.4 Protocol Stack Summary

```
  Demux key:  ether_type          ip.protocol
              ─────────           ───────────
              0x0800 → IPv4       1  → ICMP
              0x0806 → ARP        6  → TCP
                                  17 → UDP

  ┌───────────────┐
  │  HTTP/1.1     │
  │  (http11.c)   │
  ├───────────────┤
  │  TLS 1.2/1.3  │
  │ (tls_engine.c)│
  ├───────┬───────┴───────┬───────────┐
  │  TCP  │      UDP      │   ICMP    │
  │tcp_fsm│    udp.c /    │  icmp.c   │
  │  .c   │   tx_gen.c    │           │
  ├───────┴───────────────┴───────────┤
  │          IPv4  (ipv4.c)           │        ARP  (arp.c)
  │          ether_type 0x0800        │        ether_type 0x0806
  ├───────────────────────────────────┴──────────────────────┐
  │                   Ethernet  (ethernet.c)                 │
  │           classify_and_process() demux on ether_type     │
  ├──────────────────────────────────────────────────────────┤
  │                   DPDK PMD  (port_init.c)                │
  └──────────────────────────────────────────────────────────┘
```

**Reading the diagram:**
- Ethernet dispatches by `ether_type`: IPv4 (`0x0800`) and ARP (`0x0806`) are **siblings** — independent L3 protocols.
- IPv4 dispatches by `protocol` field: TCP (6), UDP (17), and ICMP (1) are **siblings** — all encapsulated in IP.
- TLS and HTTP stack only on top of TCP, not UDP or ICMP.

### 2.5 HTTP/1.1 Parser — RX State Machine

The HTTP response parser in `http11.c` is a single-pass, zero-copy state machine:

```
  tcp_fsm_input() delivers payload
         │
  ┌──────▼──────┐     HTTP/1.1 200 OK\r\n     ┌────────────────┐
  │    IDLE     │ ──────────────────────────► │  WAIT_HEADERS  │
  │  (waiting   │   parse status line:       │  scan for      │
  │   for resp) │   version, code, reason    │  \r\n\r\n       │
  └─────────────┘                            └───────┬────────┘
                                                     │
                          ┌──────────────────────────┘
                          │ blank line found
                          ▼
            ┌─── Content-Length? ───┬─── Transfer-Encoding: chunked?
            │                      │
     ┌──────▼──────┐        ┌──────▼──────┐
     │  WAIT_BODY  │        │ WAIT_CHUNK  │
     │  count down │        │  parse hex  │
     │  remaining  │        │  size lines │
     └──────┬──────┘        └──────┬──────┘
            │ len == 0             │ chunk_sz == 0
            └──────────┬───────────┘
                       ▼
               ┌──────────────┐
               │     DONE     │──► fire callback
               │  reset for   │    update metrics:
               │  keep-alive  │    http_rsp_2xx etc.
               └──────────────┘
```

**Supported HTTP methods (TX):** `GET`, `POST`, `PUT`, `DELETE`, `HEAD`
— built by `http_build_request()` with `Host`, `Content-Type`, `Content-Length` headers.

**Pipelining:** `pipeline_depth` tracks in-flight requests per connection.

### 2.6 TLS Integration

TLS sits between TCP and HTTP, using OpenSSL memory BIOs for zero-copy,
non-blocking operation:

```
  ┌──────────────────────────────────────────────────────┐
  │              Application (http11.c)                   │
  │                 plaintext ↕                           │
  ├──────────────────────────────────────────────────────┤
  │              TLS Engine (tls_engine.c)                │
  │                                                      │
  │   SSL_write(plain) ──► wbio ──► ciphertext out       │
  │   SSL_read(cipher) ◄── rbio ◄── ciphertext in        │
  │                                                      │
  │   Handshake states: PENDING → IN_PROGRESS → DONE     │
  │   Cipher suites: ECDHE + AES-GCM (TLS 1.2/1.3)     │
  │   SNI support for virtual hosting                    │
  │                                                      │
  │   SSLKEYLOG: SSL_CTX_set_keylog_callback() writes    │
  │   session keys in NSS Key Log format for Wireshark   │
  │   decryption (--sslkeylog / $SSLKEYLOGFILE)          │
  ├──────────────────────────────────────────────────────┤
  │   ┌─────────────┐  ┌────────────────────────────┐   │
  │   │ cert_mgr.c  │  │ cryptodev.c                │   │
  │   │ cert load / │  │ DPDK crypto_qat AES-GCM    │   │
  │   │ hot-reload  │  │ (fallback → OpenSSL SW)    │   │
  │   └─────────────┘  └────────────────────────────┘   │
  ├──────────────────────────────────────────────────────┤
  │              TCP FSM (tcp_fsm.c)                      │
  │   app_state flags coordinate TCP→TLS→HTTP layering   │
  │   tls_hs_start_tsc records handshake start for       │
  │   latency histogram                                  │
  └──────────────────────────────────────────────────────┘
```

### 2.7 TCP Flow Control & Data Transmission

TCP data transfer is driven by the FSM, not `tx_gen_burst()`. The mgmt core
calls `tcp_fsm_connect()` + `tcp_fsm_send()` on behalf of CLI commands like
`start --reuse --streams`.

```
  tcp_fsm_send(worker_idx, tcb, data, len)
  │
  ├── Window calculation:
  │     wnd        = min(cwnd, snd_wnd)       // receiver + congestion window
  │     in_flight  = snd_nxt - snd_una        // bytes unACKed
  │     avail      = wnd - in_flight          // bytes allowed to send
  │
  ├── Effective MSS:
  │     eff_mss = mss_remote - (timestamps ? 12 : 0)
  │     // 12-byte timestamp option overhead subtracted to stay within MTU
  │     // Without this, frames exceed TAP MTU=1500 → silently dropped
  │
  ├── Segmentation loop:
  │     while (sent < len && avail > 0):
  │       seg_len = min(eff_mss, remaining, avail)
  │       rc = tcp_send_segment(...)
  │       if (rc != 0) break     // TX failure → stop, don't advance snd_nxt
  │       snd_nxt += seg_len
  │       avail   -= seg_len
  │
  └── RTO arming (RFC 6298):
        if (rto_deadline_tsc == 0)   // only when timer not running
          arm_rto(initial_rto)
```

**Key design decisions:**
- `snd_nxt` only advances on successful `rte_eth_tx_burst()` return.
- Initial cwnd = 10 × MSS (RFC 6928 IW10), matching `tcp_fsm_connect()`.
- RTO armed once per flight; restarted on ACK with unacked data; disarmed on full ACK.
- No TX buffer — lost segments cannot be retransmitted. Fast retransmit adjusts congestion state only.
- FIN_WAIT_1/2 states accept incoming data (half-open receive) for compatibility with echo servers.
- RFC 7323 §2.2: SYN-ACK window is NOT scaled — initial `snd_wnd` uses the raw window field.
- RTT is measured from the SYN round-trip (timestamp echo in SYN-ACK), calibrating RTO before any data segment is sent.
- **Throughput mode bypass:** connections marked with `app_ctx == (void*)1` (throughput pump) skip congestion control entirely — cwnd is set to UINT32_MAX and `congestion_on_ack`/`congestion_on_rto`/`congestion_fast_retransmit` return early. The receiver's advertised window (`snd_wnd`) is the only flow-control limit.
- **HTTP response timeout:** connections in `app_state == 5` (HTTP response pending) are RST'd after **5 s** (`TCP_HTTP_RSP_TIMEOUT_US`) if no data arrives. This allows for large responses and slow servers.
- **`--one` passive close:** for single-request mode (`graceful_close` path), after the HTTP/TLS response headers are received, the FSM clears `app_state` and waits for the server to send its FIN (passive close). This mirrors `curl` behaviour: the full response body is received before vaigai initiates its own half-close. The done condition is `http_rsp_rx >= 1 && tcp_conn_close >= 1`.
- **Initial RTO:** `TCP_INITIAL_RTO_US` is 200 ms, consistent with the minimum RTO enforced by `update_rtt()` after measurement.
- RST-no-TCB (RFC 793 §3.4) is disabled to prevent RST storms from overwhelming the peer.
- **AF_PACKET kernel RST interference:** when using `net_af_packet` on an interface whose IP is also assigned to the Linux kernel, the kernel TCP stack will receive a copy of every SYN-ACK and generate a RST (no matching socket). Run `sudo bash scripts/setup.sh --suppress-rst` before starting vaigai to install an nftables rule that drops kernel-generated RSTs on vaigai's ephemeral port range (10000–59999). Remove with `--clear-rst`. Alternatively, remove the kernel IP from the interface so it is exclusively managed by vaigai.

---

## 3. Telemetry System

### 3.1 Ownership & Data Flow

**Source of truth: each worker core owns its slab.  Management core only reads.**

There is no push, no periodic timer, no background log-file writer.
The management core **pulls on demand** — only when a human or HTTP client asks.

```
                    WHO WRITES              WHO READS / WHEN
                    ──────────              ────────────────
  Worker 0 ─────► g_metrics[0]  ◄──┐
                   (++ only)        │
  Worker 1 ─────► g_metrics[1]  ◄──┤
                   (++ only)        ├── memcpy ── metrics_snapshot()
  Worker N ─────► g_metrics[N]  ◄──┘       │
                                           │  on-demand pull
                                           │  (never pushed)
                                           ▼
                               ┌───────────────────────┐
                               │  Management Core      │
                               │                       │
                               │  Trigger      Action  │
                               │  ─────────────────────│
                               │  CLI "stat"   snapshot │
                               │  GET /stats   snapshot │
                               │  start loop   1 Hz    │
                               └───────┬───────────────┘
                                       │ render
                          ┌────────────┴────────────┐
                          ▼                         ▼
                   export_json                 cli table
                   /api/v1/stats                 stdout
```

**Key rule**: workers never call `metrics_snapshot()`.
Management never writes to `g_metrics[]`. Ownership is strict and one-directional.

### 3.2 When Does the Pull Happen?

| Trigger | Who runs it | Frequency |
|---------|-------------|-----------|
| CLI `stat` command | mgmt core (readline thread) | once per keystroke |
| CLI `stat --watch` | mgmt core in loop | 1 Hz continuous |
| REST `GET /api/v1/stats/*` | libmicrohttpd thread on mgmt core | once per HTTP request |
| `start` live progress | mgmt core in sleep loop | 1 Hz for the start duration |
| Remote CLI `stat` | mgmt core via socket poll | once per remote command |

There is **no periodic background scrape** and **no metric log file**.
If nobody asks, nobody reads — worker slabs accumulate silently.

### 3.3 Counter Slabs — Zero-Overhead Recording

```c
worker_metrics_t g_metrics[TGEN_MAX_WORKERS] __rte_cache_aligned;
```

Each worker owns one cache-line-aligned slab. **42 counters** per worker:

| Layer | Counters |
|-------|----------|
| L2/L3 | `tx_pkts`, `tx_bytes`, `rx_pkts`, `rx_bytes` |
| IP | `ip_bad_cksum`, `ip_frag_dropped`, `ip_not_for_us` |
| ARP | `arp_reply_tx`, `arp_request_tx`, `arp_miss` |
| ICMP | `icmp_echo_tx`, `icmp_bad_cksum`, `icmp_unreachable_tx` |
| UDP | `udp_tx`, `udp_rx`, `udp_bad_cksum` |
| TCP | `tcp_conn_open/close`, `tcp_syn_sent`, `tcp_retransmit`, `tcp_reset_rx/sent`, `tcp_bad_cksum`, `tcp_syn_queue_drops`, `tcp_ooo_pkts`, `tcp_duplicate_acks`, `tcp_payload_tx/rx` |
| TLS | `tls_handshake_ok/fail`, `tls_records_tx/rx` |
| HTTP | `http_req_tx`, `http_rsp_rx`, `http_rsp_1xx/../5xx`, `http_parse_err` |

Recording compiles to one `INC` instruction — no atomics, no locks.
Safe because each slab has exactly one writer (its worker) and never crosses cache lines.

### 3.4 Snapshot Aggregation

`metrics_snapshot()` runs on the management core:

1. `memcpy` each `g_metrics[i]` → `snapshot.per_worker[i]`
2. `ACC(field)` macro sums all N slabs into `snapshot.total`
3. Returns stack-allocated snapshot (~25 KB)

Reads are **racy** — a worker may be mid-increment during the copy.
Max error: one burst (≤ 32 packets). Acceptable for monitoring.

### 3.5 HDR Histogram

Latency uses a lock-free histogram with 64 power-of-2 buckets:

```
Bucket 0: [0, 2) µs     index = 63 - clzll(us | 1)
Bucket 1: [2, 4) µs     single writer (worker core)
Bucket 2: [4, 8) µs     single reader via snapshot
…
Bucket 63: [2⁶³, ∞) µs  percentile: walk until seen ≥ p% × count
```

### 3.6 Export Format

Endpoints:

| Endpoint | Format | Content |
|----------|--------|---------|
| `/api/v1/stats` | JSON | Aggregate packet/protocol counters + latency |
| `/api/v1/stats/cpu` | Text | Per-core CPU cycle breakdown |
| `/api/v1/stats/mem` | Text | Mempool, DPDK heap, TCB, hugepage usage |
| `/api/v1/stats/port` | Text | Per-NIC hardware stats |

CLI `stat net` calls `export_json()`. CLI `stat cpu|mem|port` call their
respective `export_*_text()` functions. All formatters are in `export.c`.

### 3.7 CPU Cycle Accounting

```c
cpu_stats_t g_cpu_stats[TGEN_MAX_WORKERS] __rte_cache_aligned;
```

Each worker accumulates TSC cycles per poll-loop phase using 5 × `rte_rdtsc()`
calls per iteration (~10 ns overhead):

| Phase | Field | What it measures |
|-------|-------|-----------------|
| IPC drain | `cycles_ipc` | Processing commands from mgmt core |
| RX + classify | `cycles_rx` | Receive burst, protocol classification, replies |
| TX generation | `cycles_tx` | Packet generation bursts |
| Timer tick | `cycles_timer` | TCP timers + port pool tick |
| Idle detection | `cycles_idle` | Iterations with no RX and no active TX gen |

`Busy% = (1 - idle/total) × 100`. Useful because DPDK poll loops always
show 100% in `top`. The management core reads these via `cpu_stats_snapshot()`.

### 3.8 Memory Stats (no per-worker instrumentation)

Read-only queries with no per-worker instrumentation:

| Source | API | Info |
|--------|-----|------|
| Packet buffers | `rte_mempool_avail_count()` / `rte_mempool_in_use_count()` | Free vs in-flight mbufs per worker |
| DPDK heap | `rte_malloc_get_socket_stats()` | Allocated/free/total per NUMA socket |
| TCP connections | `g_tcb_stores[w].count` / `.capacity` | Active TCBs per worker |
| Hugepages | `/sys/kernel/mm/hugepages/` | System hugepage counters |

### 3.9 Structured Logging

Eight log domains mapped to `RTE_LOGTYPE_USER1..8`:

| Domain | Covers |
|--------|--------|
| `TGEN_LOG_MAIN` | Startup, shutdown, lifecycle |
| `TGEN_LOG_PORT` | Port init, driver hooks |
| `TGEN_LOG_CC` | Congestion control events |
| `TGEN_LOG_PP` | Packet processing |
| `TGEN_LOG_SYN` | TCP connection setup |
| `TGEN_LOG_HTTP` | HTTP request/response |
| `TGEN_LOG_TLS` | TLS handshake, record layer |
| `TGEN_LOG_MGMT` | CLI, REST, config |

Each macro adds `[function:line]` prefix automatically.

---

## 4. Control Plane / Data Plane Segregation

### 4.1 Design Principle

The system strictly separates fast-path packet processing (data plane) from
slow-path management operations (control plane). They share **no locks** in the
hot path. All cross-plane communication uses **lock-free SPSC rings**.

```
┌─────────────────────────────────────────────────────────────────────┐
│                        vaigAI Process                               │
│                                                                     │
│  ┌─── Control Plane (mgmt lcores) ────┐ ┌── Data Plane (workers) ─┐│
│  │                                     │ │                         ││
│  │  • CLI REPL (interactive commands)  │ │  • RX burst + classify  ││
│  │  • REST API (libmicrohttpd thread)  │ │  • Protocol processing  ││
│  │  • Config load/save/push            │ │    (ICMP/UDP/TCP/TLS)   ││
│  │  • ARP cache management             │ │  • TX packet generation ││
│  │  • ARP request/reply TX             │ │  • TX burst drain       ││
│  │  • UDP mgmt drain                   │ │  • TCP timer tick       ││
│  │  • Telemetry snapshot               │ │  • Metrics increment    ││
│  │  • Packet trace dump                │ │                         ││
│  │                                     │ │  NEVER:                 ││
│  │  NEVER:                             │ │  • blocks              ││
│  │  • touches RX/TX queues directly    │ │  • allocates memory    ││
│  │    (except ARP reply on queue 0)    │ │  • does syscalls       ││
│  │  • modifies worker state directly   │ │  • reads CLI input      ││
│  │                                     │ │  • does JSON parsing    ││
│  └─────────────────────────────────────┘ └─────────────────────────┘│
│                    │                              ▲                  │
│                    │  IPC Commands                 │  IPC ACKs        │
│                    ▼                              │                  │
│           ┌────────────────────────────────────────┐                 │
│           │        SPSC rte_ring (per worker)      │                 │
│           │                                        │                 │
│           │  g_ipc_rings[w]  mgmt ──► worker       │                 │
│           │  g_ack_rings[w]  worker ──► mgmt       │                 │
│           │                                        │                 │
│           │  Protocol:                             │                 │
│           │  config_update_t { cmd, seq, payload } │                 │
│           │  ipc_ack_t { worker_idx, seq, rc }     │                 │
│           └────────────────────────────────────────┘                 │
└─────────────────────────────────────────────────────────────────────┘
```

### 4.2 The Two Loops — Side by Side

Both cores run an identical **cooperative run-to-completion poll loop** — the
same structural pattern, different payloads. Neither loop may ever block.

```
╔═════════════════════════════════════════════════════════════════════════════════════╗
║                    vaigAI — Cooperative Two-Loop Architecture                       ║
╠═════════════════════════════════════════════════════════════════════════════════════╣
║  Inter-core:  MGMT ──[ipc_cmd_t]──► WORKER  ·  WORKER ──[ipc_ack_t]──► MGMT       ║
║               WORKER ──[ARP/ICMP pkts via pkt_ring]──────────────────► MGMT       ║
╠══════════════════════════════════════════╦══════════════════════════════════════════╣
║   WORKER LCORE(s)  ·  data plane         ║   MANAGEMENT LCORE  ·  control plane     ║
║   always spinning  ·  never blocks       ║   yields when idle  ·  never blocks      ║
╠══════════════════════════════════════════╬══════════════════════════════════════════╣
║                                          ║                                          ║
║  ┌────────────────────────────────────┐  ║  ┌────────────────────────────────────┐  ║
║  │ ➊  ipc_recv()                      │  ║  │ ➊  cli_stdin_poll()                │  ║
║  │    recv CMD · dispatch to FSM      │  ║  │ ➋  cli_server_poll()               │  ║
║  │    send ACK back to mgmt           │  ║  │    (remote --attach clients)       │  ║
║  └─────────────────┬──────────────────┘  ║  └─────────────────┬──────────────────┘  ║
║                    │                     ║                    │                     ║
║  ┌─────────────────▼──────────────────┐  ║  ┌─────────────────▼──────────────────┐  ║
║  │ ➋  rte_eth_rx_burst()              │  ║  │ ➌  arp_mgmt_tick()                 │  ║
║  │    classify → protocol FSMs        │  ║  │ ➍  icmp_mgmt_tick()                │  ║
║  │    · TCP / UDP / ICMP              │  ║  │ ➎  ipc_ack_drain()                 │  ║
║  │    · ARP/ICMP → mgmt pkt ring      │  ║  └─────────────────┬──────────────────┘  ║
║  └─────────────────┬──────────────────┘  ║                    │                     ║
║                    │                     ║  ┌─────────────────▼──────────────────┐  ║
║  ┌─────────────────▼──────────────────┐  ║  │ ➏  pktrace_flush()                 │  ║
║  │ ➌  tx_gen_burst()                  │  ║  │ ➐  traffic_gen_tick()              │  ║
║  │    token-bucket-paced TX           │  ║  │    (--duration · --one monitor)    │  ║
║  │    rte_eth_tx_burst()              │  ║  └─────────────────┬──────────────────┘  ║
║  └─────────────────┬──────────────────┘  ║                    │                     ║
║                    │                     ║  ┌─────────────────▼──────────────────┐  ║
║  ┌─────────────────▼──────────────────┐  ║  │ ➑  cpu_accounting()                │  ║
║  │ ➍  tcp_timer_tick()                │  ║  │    busy → rte_pause()  [spin]      │  ║
║  │    · RTO retransmit (RFC 6298)     │  ║  │    idle → poll(stdin, 1 ms)        │  ║
║  │    · TIME_WAIT expiry              │  ║  └────────────────────────────────────┘  ║
║  │    · delayed-ACK flush             │  ║                                          ║
║  ├────────────────────────────────────┤  ║                                          ║
║  │ ➎  tcp_port_pool_tick()            │  ║                                          ║
║  │    reclaim ephemeral ports         │  ║                                          ║
║  ├────────────────────────────────────┤  ║                                          ║
║  │ ➏  cpu_accounting()                │  ║                                          ║
║  │    rte_pause()  [always spin]      │  ║                                          ║
║  └─────────────────┬──────────────────┘  ║                                          ║
║                    └─────────────────────╫──────────────────── ↺ loop ──────────────║
╚══════════════════════════════════════════╩══════════════════════════════════════════╝
```

**Key rules:**
- Workers own NIC RX/TX queues — the management core never touches them directly (except ARP reply on queue 0).
- Management core owns CLI, ARP cache, telemetry — workers never read CLI or write to shared state.
- Priority 1 (CLI) is checked first every iteration so keystrokes are never delayed by background work.
- `stat --watch` from a remote `--attach` client falls back to a single `--rate` sample (detects memstream via `fileno(stdout) < 0`).

#### Activity Reference

| Step | Function | Core | What it does |
|------|----------|------|-------------|
| W➊ | `ipc_recv()` | Worker | Receive CMD from mgmt; send ACK |
| W➋ | `rte_eth_rx_burst()` / `classify_and_process()` | Worker | RX 32 mbufs, run protocol FSMs, enqueue ARP/ICMP to mgmt ring |
| W➌ | `tx_gen_burst()` | Worker | Token-bucket paced packet generation (ICMP/UDP/TCP) |
| W➍ | `tcp_timer_tick()` | Worker | RTO retransmit, TIME_WAIT expiry, delayed ACK |
| W➎ | `tcp_port_pool_tick()` | Worker | Drain TIME_WAIT FIFO, reclaim ephemeral ports |
| W➏ | `cpu_accounting()` / `rte_pause()` | Worker | TSC cycle accounting; always spins |
| M➊ | `cli_stdin_poll()` | Mgmt | Non-blocking readline (`rl_callback_read_char()`) |
| M➋ | `cli_server_poll()` | Mgmt | Accept + dispatch remote CLI clients (Unix socket) |
| M➌ | `arp_mgmt_tick()` | Mgmt | Drain ARP rings, send replies, probe stale entries |
| M➍ | `icmp_mgmt_tick()` | Mgmt | Drain ICMP rings, send replies |
| M➎ | `ipc_ack_drain()` | Mgmt | Drain worker→mgmt ACK rings, log errors |
| M➏ | `pktrace_flush()` | Mgmt | Write buffered pcapng captures to disk |
| M➐ | `traffic_gen_tick()` | Mgmt | Monitor `--duration` / `--one`; print progress |
| M➑ | `cpu_accounting()` + yield | Mgmt | TSC accounting; `rte_pause()` if busy, `poll(stdin,1ms)` if idle |

#### CPU Cycle Breakdown (`stat cpu`)

| Phase | Worker counter | Mgmt counter | Measures |
|-------|---------------|-------------|---------|
| RX | `cycles_rx` | — | rx_burst + classify + FSM |
| TX | `cycles_tx` | — | tx_gen_burst + tx_drain |
| Timer | `cycles_timer` | — | tcp_timer_tick + port_pool_tick |
| CLI | — | `cycles_cli` | stdin poll + cli_server_poll |
| Protocol | — | `cycles_proto` | ARP + ICMP + IPC ACK drain |
| Background | — | `cycles_bg` | pktrace flush + traffic_gen_tick |
| Idle | `cycles_idle` | `cycles_idle` | rte_pause iters (worker) / poll(1ms) time (mgmt) |

### 4.3 IPC Protocol

Commands flow from management to workers via `config_update_t` messages (256 bytes,
8-byte aligned). Workers acknowledge with `ipc_ack_t`.

```
  config_update_t (256 bytes)         ipc_ack_t
  ┌────────────────────────┐          ┌─────────────────┐
  │ cmd        (4 bytes)   │          │ worker_idx (4B) │
  │ seq        (4 bytes)   │          │ seq        (4B) │
  │ payload  (248 bytes)   │          │ rc         (4B) │
  │   (e.g. tx_gen_config) │          └─────────────────┘
  └────────────────────────┘
```

**Command set:**

| Command            | Payload                | Effect on Worker                          |
|--------------------|------------------------|-------------------------------------------|
| `CFG_CMD_START`    | `tx_gen_config_t`      | Configure and start TX generator          |
| `CFG_CMD_STOP`     | *(none)*               | Stop TX generator                         |
| `CFG_CMD_SET_RATE` | rate value             | Adjust token bucket rate                  |
| `CFG_CMD_SET_PROFILE` | `flow_cfg_t`        | Update flow profile                       |
| `CFG_CMD_SHUTDOWN` | *(none)*               | Exit worker loop                          |

**Management-local commands** (not sent via IPC):

| CLI Command | Effect |
|-------------|--------|
| `reset` | RST all active TCBs, `tcb_store_reset()`, `tcp_port_pool_reset()` (cursor preserved), `metrics_reset()` |

**Delivery guarantees:**
- Sender spins up to 100 µs if ring is full; drops + logs on timeout
- Heap-allocated messages (`rte_malloc`) — ring transports pointers
- Worker frees message after processing
- Broadcast sends to all workers; returns success count

### 4.4 CLI Commands

```
vaigai> help
```

| Command | Syntax | Description |
|---------|--------|-------------|
| `help` | `help` | List available commands |
| `stat` | `stat [cpu\|mem\|net\|port] [--rate] [--watch] [--core N]` | Unified statistics (see CLI.md) |
| `stats` | `stats` | Alias for `stat net` (backward compat) |
| `ping` | `ping <ip> [count] [size] [interval_ms]` | ICMP echo request |
| `start` | `start --proto <proto> --ip <ip> --duration <s> [--rate <pps>] [--size <bytes>] [--port <port>] [--tls] [--reuse] [--streams <n>]` | Start traffic generation |
| `stop` | `stop` | Stop active traffic generation |
| `reset` | `reset` | RST all TCBs, reset port pools + metrics |
| `trace` | `trace start <file.pcapng> [port] [queue]` | Start packet capture |
| | `trace stop` | Stop capture |
| `show` | `show interface [port_id]` | Show DPDK interface details |
| `quit` | `quit` | Graceful shutdown |

### 4.5 REST API

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/v1/stats` | `GET` | Metrics snapshot (same JSON as CLI `stat net`) |
| `/api/v1/stats/cpu` | `GET` | CPU cycle breakdown (text) |
| `/api/v1/stats/mem` | `GET` | Memory usage (text) |
| `/api/v1/stats/port` | `GET` | Per-port NIC stats (text) |
| `/api/v1/config` | `GET` | Current configuration |
| `/api/v1/start` | `POST` | Start traffic generation |
| `/api/v1/stop` | `POST` | Stop traffic generation |

### 4.6 Remote CLI Attach

Multiple CLI clients can connect to a running vaigai process via Unix
domain socket:

```
vaigai process (mgmt core)
├── stdin local CLI (readline or fgets)
└── Unix socket listener (/var/run/vaigai/vaigai.sock)
    ├── Remote client 1 (vaigai --attach)
    ├── Remote client 2
    └── ... (max 8)
```

- Server: `cli_server_init()` creates the socket; `cli_server_poll()` is
  called in the mgmt tick loop (readline event hook, fgets poll, daemon loop)
- Output capture: `open_memstream()` + temporary `stdout` redirect
- Wire protocol: command `\n` → response `\0` (NUL-terminated)
- Client: `vaigai --attach [path]` — runs without DPDK EAL init

All responses are JSON with CORS headers (`Access-Control-Allow-Origin: *`).
REST server runs on the management core via `libmicrohttpd`.

### 4.7 Runtime Configuration

All configuration is supplied via CLI flags at startup or the `set ip` / `start` commands at runtime.
The `tgen_config_t` struct (`config_mgr.h`) holds the live state populated from parsed args:

| Field | Source | Description |
|-------|--------|-------------|
| `tls_enabled` | `--tls` / cert detection | TLS on/off |
| `rest_port` | `--rest-port` | REST API listen port (0 = disabled) |
| `max_concurrent` | `--max-conn` | Max concurrent TCP connections per worker |
| `cert` | `--cert` / `--key` | TLS certificate and key paths |

Per-port IP / gateway / netmask is held in `g_arp[port]` and set via
`--src-ip` / `--gateway` / `--netmask` at startup or `set ip` at runtime.

### 4.8 How They Work Together — Concrete Scenarios

#### Scenario 1: `start --proto udp --ip 10.0.0.1 --duration 3 --rate 1000 --size 64 --port 9`

```
  Time ──────────────────────────────────────────────────────────►

  Mgmt Core                         Worker 0        Worker 1
  ─────────                         ────────        ────────
  cmd_start() parses args
  arp_request(10.0.0.1)
  arp_mgmt_tick() ← poll ARP ring
  [ARP reply arrives] →             arp_input()
                                    enqueue to ring ─┐
  arp_mgmt_process() ← dequeue ◄───────────────────┘
  MAC resolved!
  metrics_reset()
  tgen_ipc_broadcast(START, cfg)
    → enqueue to ring[0] ─────────► ipc_recv()
    → enqueue to ring[1] ──────────────────────────► ipc_recv()
                                    tx_gen_start()   tx_gen_start()
  poll(3s) with progress             ┌─── poll loop ─┐ ┌─ poll loop ─┐
    + arp_mgmt_tick() each iter      │tx_gen_burst()  │ │tx_gen_burst│
                                     │1000 pps each   │ │1000 pps    │
                                     │token bucket    │ │token bucket│
                                     └────────────────┘ └────────────┘
  [3s elapsed]                       deadline hit →     deadline hit →
                                     self-disarm        self-disarm
  TCP close grace: poll TCB state
    for TIME_WAIT up to 5 s
  metrics_snapshot()
  export_json() → print stats
```

#### Scenario 2: REST API `GET /api/v1/stats`

```
  libmicrohttpd thread              Worker cores
  ──────────────────                ────────────
  HTTP request arrives
  metrics_snapshot()
    memcpy(slab[0]) ◄─── racy read ─── slab[0] (worker writing)
    memcpy(slab[1]) ◄─── racy read ─── slab[1] (worker writing)
    aggregate totals
  export_json(snapshot)
  HTTP 200 + JSON body
```

No IPC needed. No lock acquired. The management thread directly reads the
per-worker slabs. The worst-case error is a partially-updated counter for
one burst cycle (32 packets).

#### Scenario 3: ARP Resolution (split-core processing)

```
  Mgmt Core                         Worker Core
  ─────────                         ───────────
  arp_request(dst_ip)
    insert PENDING entry in hash
    build ARP request frame
    rte_eth_tx_burst() on queue 0
                                    RX: ARP reply from peer
                                    arp_input() → enqueue to g_arp_rings[p]
  arp_mgmt_tick()
    drain g_arp_rings[p]
    arp_mgmt_process()
      update entry → RESOLVED
      flush held packets
```

ARP is the clearest example of split-core processing: workers receive ARP frames
but never process them. They forward to the management core via a per-port SPSC
ring. The management core handles all cache mutations, reply generation, and
expiry probing. Workers read the cache via `arp_lookup()` under a read-lock.

### 4.9 Why This Design

| Concern                   | How It's Addressed                                          |
|---------------------------|-------------------------------------------------------------|
| **No locks in hot path**  | Workers never contend; each owns its queues, metrics, TCBs  |
| **No syscalls on workers** | All I/O via DPDK PMD poll; no `send()`/`recv()`           |
| **Config changes at runtime** | IPC ring delivers updates without stopping workers     |
| **Observability without overhead** | Workers do single `++`; mgmt does the heavy export |
| **ARP without blocking**  | Workers enqueue, mgmt processes — no worker stall          |
| **Graceful shutdown**      | `CFG_CMD_SHUTDOWN` via IPC + `g_run` flag; ordered teardown |

---

### 4.10 Shared State Inventory

The following global state is accessed by both planes. Each is designed to be
safe without mutual exclusion in the fast path:

| State | Writer | Reader | Mechanism |
|---|---|---|---|
| `g_metrics[w]` | Worker `w` | Mgmt | Exclusive write; racy read |
| `g_arp[p].table` | Mgmt | Workers | `rte_rwlock` (read-side only on workers) |
| `g_arp_rings[p]` | Workers | Mgmt | SPSC `rte_ring` |
| `g_udp_rings[p]` | Workers | Mgmt | SPSC `rte_ring` |
| `g_ipc_rings[w]` | Mgmt | Worker `w` | SPSC `rte_ring` |
| `g_ack_rings[w]` | Worker `w` | Mgmt | SPSC `rte_ring` |
| `g_run` | Signal handler | All | `volatile int` — process lifecycle |
| `g_traffic` | REST API | Workers | `volatile int` — traffic pause/resume |
| `g_worker_ctx[w].tx_gen` | Worker `w` | Worker `w` | Single owner (configured via IPC copy) |

---

## 5. Test Infrastructure

### 5.1 Test Script Overview

```
tests/
├── ping_veth.sh            # ICMP over veth — smoke test (no VM)
├── udp_veth.sh             # UDP over veth — datagram validation
├── arp_test.sh             # ARP resolution + cache lifecycle
├── tcp_tap.sh              # TCP SYN/data/FIN over TAP + Firecracker
├── http_nic.sh             # HTTP RPS + throughput over NIC + QEMU
├── tls_nic.sh              # TLS handshake/throughput over NIC + QEMU + QAT
├── https_nic.sh            # HTTPS (nginx SSL) over NIC + QEMU + QAT
└── manual-tests.sh         # Cut-and-paste reference for all topologies
```

### 5.2 Topology Tiers

| Tier | Transport | VM | NIC | Script(s) |
|------|-----------|-----|------|----------|
| **Kernel** | veth / TAP | None | Virtual | `ping_veth.sh`, `udp_veth.sh`, `arp_test.sh` |
| **TAP + Firecracker** | TAP + bridge | Firecracker microVM | `net_tap` PMD | `tcp_tap.sh` |
| **NIC + QEMU** | Physical loopback | QEMU/KVM + vfio-pci | HW PMD (mlx5/i40e) | `http_nic.sh`, `tls_nic.sh`, `https_nic.sh` |

### 5.3 TLS / HTTPS Test Architecture

The TLS and HTTPS tests add crypto acceleration testing to the NIC + QEMU tier:

```
┌─────── vaigAI (host) ─────────┐       ┌──────── QEMU VM ────────────┐
│                                │       │                             │
│  DPDK i40e PMD                │  NIC  │  Kernel NIC driver          │
│       │                        │ ═════ │       │                     │
│  TCP/IP stack (net/)          │       │  openssl s_server (TLS)     │
│       │                        │       │  nginx + SSL    (HTTPS)    │
│  TLS engine (tls/)            │       │       │                     │
│       │                        │       │  OpenSSL / qatengine       │
│  ┌────▼────────┐               │       │                             │
│  │ cryptodev   │  QAT          │       │  QAT (vfio-pci passthru)   │
│  │ crypto_qat  │◄─── PCI ──── │       │◄─── PCI passthru ──────── │
│  └─────────────┘               │       │                             │
└────────────────────────────────┘       └─────────────────────────────┘
```

**Crypto parameterization** — Both scripts accept `VAIGAI_CRYPTO=qat|sw` and
`SERVER_CRYPTO=qat|sw` environment variables, enabling a 2×2 test matrix:

| | Server: SW | Server: QAT |
|---|---|---|
| **vaigai: SW** | Baseline (pure software) | Server-side offload only |
| **vaigai: QAT** | Client-side offload (typical) | Full hardware offload |

**Intel QAT DH895XCC** devices are auto-detected and bound to `vfio-pci`.
vaigai uses the DPDK `crypto_qat` PMD; the VM server uses OpenSSL `qatengine`
with the QAT device passed through via vfio-pci.

### 5.4 Common Test Infrastructure

All NIC-tier scripts share a common pattern:

| Component | Implementation |
|-----------|---------------|
| **FIFO lifecycle** | `mkfifo` → `vaigai < fifo > log`, async command dispatch |
| **Stats collection** | `stats` command → JSON parse via `json_val()` grep |
| **VFIO bind/unbind** | Save original driver, bind to `vfio-pci`, restore on teardown |
| **VM lifecycle** | COW rootfs copy → patch config → QEMU boot → serial monitor |
| **Teardown** | EXIT trap: quit vaigai, kill QEMU, restore PCI drivers, clean temp files |
| **Pass/fail** | Per-assertion counters, non-zero exit on any failure |

See [tls-test.md](tls-test.md), [https-test.md](https-test.md),
[http-test.md](http-test.md), and [tcp-test.md](tcp-test.md) for
detailed test plans and code coverage matrices.


