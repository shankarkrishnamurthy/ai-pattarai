# vaigAI Architecture

> A DPDK-based traffic generator built from scratch in C17.
> Every packet is crafted in user-space; the kernel is never in the data path.

---

## Table of Contents

1. [System Architecture](#1-system-architecture)
2. [Life of a Packet](#2-life-of-a-packet)
3. [Telemetry System](#3-telemetry-system)
4. [Control Plane / Data Plane Segregation](#4-control-plane--data-plane-segregation)

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
│   ├── tcp_fsm.h/c            # Full TCP state machine (RFC 793 / 7323)
│   ├── tcp_options.h/c        # MSS, WScale, SACK-Permitted, Timestamps
│   ├── tcp_timer.h/c          # RTO retransmit, TIME_WAIT expiry, delayed ACK
│   ├── tcp_congestion.h/c     # New Reno congestion control (RFC 5681)
│   ├── tcp_port_pool.h/c      # Ephemeral port bitmap [10000–59999]
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
│   ├── cli.h/c                # readline-based interactive CLI
│   ├── rest.h/c               # libmicrohttpd REST API (JSON + Prometheus)
│   └── config_mgr.h/c         # JSON config load/save/validate/push
│
└── telemetry/                 ── Observability ──
    ├── metrics.h/c            # Per-worker lock-free counter slabs
    ├── export.h/c             # JSON and Prometheus text export
    ├── histogram.h/c          # HDR-style latency histogram (log₂ buckets)
    ├── pktrace.h/c            # Per-packet capture trace buffer
    └── log.h/c                # Structured log macros (wraps rte_log)
```

### 1.3 Startup Sequence

```
main()
  │
  ├─1── tgen_eal_init()              Parse args, init DPDK EAL, calibrate TSC
  ├─2── tgen_core_assign_init()      Map lcores → worker / mgmt roles
  ├─3── tgen_mempool_create_all()    One mempool per worker (NUMA-aware)
  ├─4── tgen_ports_init()            Configure ports, RSS, queues, start NICs
  ├─5── arp_init() / icmp_init()     Protocol subsystem init
  │     udp_init() / pktrace_init()
  ├─6── tgen_ipc_init()              Create SPSC rings (cmd + ACK)
  ├─7── config_load_json()           Load JSON config from $VAIGAI_CONFIG
  │     config_push_to_workers()     Push flow profiles to ARP subsystem
  ├─8── cert_mgr_init()              TLS client/server SSL contexts
  │     tls_session_store_init()
  ├─9── tcb_stores_init()            Per-worker TCB stores
  │     tcp_port_pool_init()         Ephemeral port bitmaps
  ├─10─ cryptodev_init()             HW crypto (non-fatal; falls back to SW)
  ├─11─ rte_eal_remote_launch()      Launch all worker lcores
  ├─12─ rest_server_start()          Start REST API (libmicrohttpd thread)
  ├─13─ cli_run()               ◄─── Blocks here until "quit"
  └─14─ tgen_cleanup()               Stop workers, free everything, EAL cleanup
```

### 1.4 Core Assignment

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
on the same NUMA socket. Each worker gets one RX queue and one TX queue per port.

### 1.5 Memory Layout

| Resource          | Scope       | Sizing                                                       |
|-------------------|-------------|--------------------------------------------------------------|
| **Mempools**      | Per-worker  | `next_pow2((rx_desc + tx_desc + pipeline) × 2 × queues)` mbufs; min 512 |
| **TCB stores**    | Per-worker  | Pre-allocated flat array + open-addressing hash table        |
| **ARP cache**     | Per-port    | `rte_hash` (1024 entries) + `rte_rwlock` (workers read, mgmt writes) |
| **Port pools**    | Per-worker  | Bitmap over [10000, 59999] + TIME_WAIT FIFO ring             |
| **Metric slabs**  | Per-worker  | Cache-line aligned; no cross-core writes                     |
| **IPC rings**     | Per-worker  | SPSC `rte_ring`, `max(64, next_pow2(pipeline_depth × 2))` entries |

Mempool allocation uses a 3-tier NUMA fallback: worker's socket with 1 GB pages →
`SOCKET_ID_ANY` with 2 MB pages → `SOCKET_ID_ANY` with 4 KB pages.

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
| 8    | `rte_eth_tx_burst()`         | *(DPDK)*            | Worker   |

### 2.2 Transmit Path — TX Generation Engine

The TX generator produces synthetic traffic from worker cores, controlled by the
management plane via IPC.

```
  CLI: "flood udp 10.0.0.1 3 1000 64 9"
       │
       ▼
  cmd_flood()                                         ── mgmt core ──
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
  │         ├─ UDP  → build_udp_datagram()     │
  │         ├─ TCP  → (future)                 │
  │         └─ HTTP → (future)                 │
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

### 2.3 Worker Core — The Poll Loop

Each worker lcore is pinned to a CPU and runs a **single, non-blocking loop** that
never yields, never sleeps, and never makes a syscall. The loop body has five
distinct phases, executed in fixed order every iteration:

```
  ┌──────────────────────────────────────────────────────────────────────┐
  │                     Worker Lcore N  (pinned CPU)                    │
  │                                                                      │
  │  while (g_run) {                                                     │
  │                                                                      │
  │    ┌──── Phase 1: IPC Drain ──────────────────────────────────────┐  │
  │    │                                                              │  │
  │    │  Dequeue ALL pending commands from mgmt → worker ring.       │  │
  │    │  This is how the control plane talks to us without locks.    │  │
  │    │                                                              │  │
  │    │  ┌─ CFG_CMD_START ────► tx_gen_configure() + tx_gen_start()  │  │
  │    │  │                     arms deadline_tsc, resets token bucket│  │
  │    │  ├─ CFG_CMD_STOP ─────► tx_gen_stop()                       │  │
  │    │  ├─ CFG_CMD_SHUTDOWN ─► return (exit loop)                  │  │
  │    │  └─ ACK sent back ────► tgen_ipc_ack(seq, rc)               │  │
  │    │                                                              │  │
  │    └──────────────────────────────────────────────────────────────┘  │
  │                                 │                                    │
  │                                 ▼                                    │
  │    ┌──── Phase 2: RX Burst + Classify ────────────────────────────┐  │
  │    │                                                              │  │
  │    │  Poll each assigned (port, queue) pair:                      │  │
  │    │                                                              │  │
  │    │    nb_rx = rte_eth_rx_burst(port, queue, bufs, 32)           │  │
  │    │                                                              │  │
  │    │    for each received mbuf:                                   │  │
  │    │      ┌─ ARP?  ──► arp_input()  → enqueue to mgmt ring      │  │
  │    │      ├─ IPv4?                                               │  │
  │    │      │   ├─ ICMP ──► icmp_input()  → reply mbuf returned    │  │
  │    │      │   ├─ UDP  ──► udp_input()   → enqueue to mgmt ring  │  │
  │    │      │   └─ TCP  ──► tcp_fsm_input() → ACK/data inline     │  │
  │    │      └─ other ──► drop                                      │  │
  │    │                                                              │  │
  │    │    Response mbufs (ICMP replies, TCP ACKs) accumulate        │  │
  │    │    in a local tx_buf[] array for Phase 4.                    │  │
  │    │                                                              │  │
  │    └──────────────────────────────────────────────────────────────┘  │
  │                                 │                                    │
  │                                 ▼                                    │
  │    ┌──── Phase 3: TX Packet Generation ───────────────────────────┐  │
  │    │                                                              │  │
  │    │  Runs ONLY when tx_gen.active == true (armed by Phase 1).   │  │
  │    │  This is the synthetic traffic engine — flood, rate-limit.   │  │
  │    │                                                              │  │
  │    │  tx_gen_burst() does THREE things in one call:               │  │
  │    │                                                              │  │
  │    │    ① Deadline check                                         │  │
  │    │       now = rte_rdtsc()                                     │  │
  │    │       if (now ≥ deadline_tsc) → self-disarm, return 0       │  │
  │    │       Duration is enforced HERE, not by the mgmt core.      │  │
  │    │       Worker autonomously stops when time is up.             │  │
  │    │                                                              │  │
  │    │    ② Token bucket refill                                    │  │
  │    │       elapsed_tsc = now − last_refill_tsc                   │  │
  │    │       new_tokens = elapsed × rate_pps / tsc_hz              │  │
  │    │       tokens = min(tokens + new_tokens, 32)                 │  │
  │    │       ┌──────────────────────────────────────┐              │  │
  │    │       │ rate_pps = 0  → unlimited (burst 32) │              │  │
  │    │       │ rate_pps > 0  → tokens gate TX count │              │  │
  │    │       └──────────────────────────────────────┘              │  │
  │    │                                                              │  │
  │    │    ③ Build + transmit                                       │  │
  │    │       for i in 0..tokens:                                   │  │
  │    │         mbuf = build_packet(proto, cfg, mempool)            │  │
  │    │       rte_eth_tx_burst(port, queue, burst, n)  ◄── own TX   │  │
  │    │       increment: tx_pkts, tx_bytes, icmp_echo_tx/udp_tx    │  │
  │    │                                                              │  │
  │    │  NOTE: TX gen uses its OWN rte_eth_tx_burst call,           │  │
  │    │  separate from the Phase 4 drain of RX-triggered replies.   │  │
  │    │                                                              │  │
  │    └──────────────────────────────────────────────────────────────┘  │
  │                                 │                                    │
  │                                 ▼                                    │
  │    ┌──── Phase 4: TX Drain (RX-triggered responses) ──────────────┐  │
  │    │                                                              │  │
  │    │  Flush any reply mbufs accumulated during Phase 2:           │  │
  │    │    rte_eth_tx_burst(port, queue, tx_buf, nb_tx)              │  │
  │    │    free any mbufs the NIC couldn't accept                   │  │
  │    │                                                              │  │
  │    │  These are ICMP echo replies, TCP ACKs/SYN-ACKs, etc.       │  │
  │    │  This is a separate TX path from Phase 3's generated pkts.  │  │
  │    │                                                              │  │
  │    └──────────────────────────────────────────────────────────────┘  │
  │                                 │                                    │
  │                                 ▼                                    │
  │    ┌──── Phase 5: Timer Tick ─────────────────────────────────────┐  │
  │    │                                                              │  │
  │    │  tcp_timer_tick(worker_idx):                                 │  │
  │    │    • RTO expiry    → retransmit with exponential backoff    │  │
  │    │    • TIME_WAIT     → free TCB after 4 s                     │  │
  │    │    • Delayed ACK   → flush pending ACKs past deadline       │  │
  │    │                                                              │  │
  │    └──────────────────────────────────────────────────────────────┘  │
  │                                 │                                    │
  │                                 └──────────── loop back ──────►     │
  │  }                                                                   │
  └──────────────────────────────────────────────────────────────────────┘
```

**Two separate TX paths — why?**

```
  Phase 2 (RX)                    Phase 3 (TX Gen)
  ────────────                    ────────────────
  NIC → mbufs → classify         token bucket → build_packet()
       │                                │
       ▼                                ▼
  reply mbufs (e.g. ICMP echo)    generated mbufs (e.g. UDP flood)
       │                                │
       ▼                                ▼
  ┌─ Phase 4 ─────────┐          ┌─ Phase 3 (inline) ─┐
  │ rte_eth_tx_burst() │          │ rte_eth_tx_burst()  │
  │ (response traffic) │          │ (generated traffic) │
  └────────────────────┘          └─────────────────────┘
```

Phase 3 transmits its own packets immediately inside `tx_gen_burst()`.
Phase 4 flushes response packets that were buffered during Phase 2's classify step.
Both use `rte_eth_tx_burst()` but at different points in the loop and for
fundamentally different traffic: **reactive** (replies) vs **proactive** (generated).

**Duration/deadline lifecycle:**

```
  CLI: "flood udp 10.0.0.1 3 ..."
       │
       ▼
  ┌─ Mgmt core ─────────────────┐     ┌─ Worker core ──────────────────────┐
  │                              │     │                                     │
  │  Build config:               │     │  Phase 1: receive CFG_CMD_START    │
  │    duration_s = 3            │     │    tx_gen_configure():             │
  │    rate_pps = 1000           │     │      deadline_tsc = now + 3×tsc_hz│
  │                              │     │      tokens = rate_pps (seed)     │
  │  IPC broadcast ──────────────────► │    tx_gen_start():                │
  │                              │     │      active = true                 │
  │  sleep(3s) ← progress bar   │     │                                     │
  │                              │     │  Loop iterations 1..N:             │
  │                              │     │    Phase 3: tx_gen_burst()         │
  │                              │     │      ① rdtsc() < deadline? yes    │
  │                              │     │      ② refill tokens              │
  │                              │     │      ③ build + send               │
  │                              │     │                                     │
  │                              │     │  Loop iteration N+1:               │
  │                              │     │    Phase 3: tx_gen_burst()         │
  │                              │     │      ① rdtsc() ≥ deadline!        │
  │  [3s elapsed]                │     │         active = false  (self-stop)│
  │  metrics_snapshot()          │     │         return 0                   │
  │  print throughput            │     │                                     │
  │                              │     │  Subsequent iterations:            │
  └──────────────────────────────┘     │    Phase 3: skipped (active=false) │
                                       └─────────────────────────────────────┘
```

The worker **self-disarms** — the mgmt core does not need to send a STOP command
when using a timed flood. The TSC deadline is checked at the **start** of every
`tx_gen_burst()` call, ensuring sub-microsecond accuracy.

### 2.4 Protocol Stack Summary

```
  ┌────────────────────────────────┐
  │       Application Layer        │
  │   HTTP/1.1  (http11.c)        │
  ├────────────────────────────────┤
  │       Security Layer           │
  │   TLS 1.2/1.3 (tls_engine.c) │
  ├───────────┬────────────────────┤
  │   TCP     │   UDP              │
  │  tcp_fsm  │  udp.c / tx_gen.c │
  ├───────────┴────────────────────┤
  │       ICMP  (icmp.c)          │
  ├────────────────────────────────┤
  │       IPv4  (ipv4.c)          │
  ├────────────────────────────────┤
  │     ARP     (arp.c)           │
  ├────────────────────────────────┤
  │   Ethernet  (ethernet.c)      │
  ├────────────────────────────────┤
  │   DPDK PMD  (port_init.c)     │
  └────────────────────────────────┘
```

---

## 3. Telemetry System

### 3.1 Architecture Overview

```
  Worker 0           Worker 1           Worker N
  ┌──────────┐       ┌──────────┐       ┌──────────┐
  │ metrics  │       │ metrics  │       │ metrics  │
  │ slab[0]  │       │ slab[1]  │       │ slab[N]  │
  │          │       │          │       │          │
  │ tx_pkts  │       │ tx_pkts  │       │ tx_pkts  │
  │ rx_pkts  │       │ rx_pkts  │       │ rx_pkts  │
  │ udp_tx   │       │ udp_tx   │       │ udp_tx   │
  │ tcp_*    │       │ tcp_*    │       │ tcp_*    │
  │ tls_*    │       │ tls_*    │       │ tls_*    │
  │ http_*   │       │ http_*   │       │ http_*   │
  │ ...      │       │ ...      │       │ ...      │
  └────┬─────┘       └────┬─────┘       └────┬─────┘
       │                   │                   │
       │           single writer per slab      │
       │           (no atomics needed)         │
       │                   │                   │
       └───────────────────┼───────────────────┘
                           │  memcpy (racy reads — tolerable for monitoring)
                           ▼
                  ┌────────────────────┐
                  │  metrics_snapshot  │  ← management core
                  │                    │
                  │  per_worker[0..N]  │
                  │  total (aggregate) │
                  └────────┬───────────┘
                           │
              ┌────────────┼────────────┐
              │            │            │
       ┌──────▼──────┐ ┌──▼──────────┐ ┌▼──────────────┐
       │export_json()│ │export_prom()│ │cli_print_stats│
       │  flat JSON  │ │ text format │ │  table view   │
       └──────┬──────┘ └──┬──────────┘ └┬──────────────┘
              │            │             │
       ┌──────▼──────┐ ┌──▼──────────┐  │
       │ REST API    │ │ REST API    │  stdout
       │ /stats JSON │ │ /metrics   │
       └─────────────┘ └─────────────┘
```

### 3.2 Counter Slabs — Zero-Overhead Recording

Each worker owns a dedicated `worker_metrics_t` struct, cache-line aligned to
avoid false sharing. The global array:

```c
worker_metrics_t g_metrics[TGEN_MAX_WORKERS] __rte_cache_aligned;
```

**38 counters per worker** covering every protocol layer:

| Category | Counters                                                              |
|----------|-----------------------------------------------------------------------|
| L2/L3    | `tx_pkts`, `tx_bytes`, `rx_pkts`, `rx_bytes`                         |
| IP       | `ip_bad_cksum`, `ip_frag_dropped`, `ip_not_for_us`                   |
| ARP      | `arp_reply_tx`, `arp_request_tx`, `arp_miss`                         |
| ICMP     | `icmp_echo_tx`, `icmp_bad_cksum`, `icmp_unreachable_tx`              |
| UDP      | `udp_tx`, `udp_rx`, `udp_bad_cksum`                                 |
| TCP      | `tcp_conn_open/close`, `tcp_syn_sent`, `tcp_retransmit`,             |
|          | `tcp_reset_rx/sent`, `tcp_bad_cksum`, `tcp_syn_queue_drops`,         |
|          | `tcp_ooo_pkts`, `tcp_duplicate_acks`                                 |
| TLS      | `tls_handshake_ok/fail`, `tls_records_tx/rx`                         |
| HTTP     | `http_req_tx`, `http_rsp_rx`, `http_rsp_1xx/../5xx`, `http_parse_err`|

**Recording is a single `++` — no atomics, no locks, no function call overhead.**
Each increment compiles to one `INC` or `ADD` instruction. This is safe because
each worker exclusively writes to its own slab; no other core ever writes to it.

### 3.3 Snapshot Aggregation

When the management plane needs a read (CLI `stats`, REST `/api/v1/stats`), it calls
`metrics_snapshot()`:

1. `memcpy` each worker's slab into `snapshot.per_worker[i]`
2. Accumulate all 38 fields into `snapshot.total` with the `ACC(field)` macro
3. Return the snapshot (stack-allocated, ~25 KB)

The reads are deliberately **racy** — a worker might be mid-increment during the
copy. This is acceptable for monitoring data; the error is bounded to one burst
(≤ 32 packets).

### 3.4 HDR Histogram

Latency measurements use a lock-free histogram with 64 power-of-2 buckets:

```
Bucket 0:  [0,   2)  µs        Bucket index = 63 - clzll(us | 1)
Bucket 1:  [2,   4)  µs        Single writer (worker core)
Bucket 2:  [4,   8)  µs        Single reader via hist_copy() snapshot
...                             Percentiles: walk buckets until
Bucket 63: [2⁶³, 2⁶⁴) µs        seen ≥ target = p% × total_count
```

### 3.5 Export Formats

**JSON** (`/api/v1/stats`): Flat object with all aggregate counters:
```json
{"tx_pkts": 1000000, "rx_pkts": 500000, "udp_tx": 1000000, ...}
```

**Prometheus** (`/api/v1/metrics`): One gauge per counter with `vaigai_` prefix:
```
# HELP vaigai_tx_pkts Total packets transmitted
# TYPE vaigai_tx_pkts gauge
vaigai_tx_pkts 1000000
```

### 3.6 Structured Logging

Eight log domains mapped to `RTE_LOGTYPE_USER1..8`:

| Domain           | Covers                          |
|------------------|---------------------------------|
| `TGEN_LOG_MAIN`  | Startup, shutdown, lifecycle    |
| `TGEN_LOG_PORT`  | Port init, driver hooks         |
| `TGEN_LOG_CC`    | Congestion control events       |
| `TGEN_LOG_PP`    | Packet processing               |
| `TGEN_LOG_SYN`   | TCP connection setup            |
| `TGEN_LOG_HTTP`  | HTTP request/response           |
| `TGEN_LOG_TLS`   | TLS handshake, record layer     |
| `TGEN_LOG_MGMT`  | CLI, REST, config               |

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

### 4.2 IPC Protocol

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

**Delivery guarantees:**
- Sender spins up to 100 µs if ring is full; drops + logs on timeout
- Heap-allocated messages (`rte_malloc`) — ring transports pointers
- Worker frees message after processing
- Broadcast sends to all workers; returns success count

### 4.3 How They Work Together — Concrete Scenarios

#### Scenario 1: `flood udp 10.0.0.1 3 1000 64 9`

```
  Time ──────────────────────────────────────────────────────────►

  Mgmt Core                         Worker 0        Worker 1
  ─────────                         ────────        ────────
  cmd_flood() parses args
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
  sleep(3s) with progress           ┌─── poll loop ─┐ ┌─ poll loop ─┐
                                    │tx_gen_burst()  │ │tx_gen_burst│
                                    │1000 pps each   │ │1000 pps    │
                                    │token bucket    │ │token bucket│
                                    └────────────────┘ └────────────┘
  [3s elapsed]                      deadline hit →     deadline hit →
                                    self-disarm        self-disarm
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

### 4.4 Why This Design

| Concern                   | How It's Addressed                                          |
|---------------------------|-------------------------------------------------------------|
| **No locks in hot path**  | Workers never contend; each owns its queues, metrics, TCBs  |
| **No syscalls on workers** | All I/O via DPDK PMD poll; no `send()`/`recv()`           |
| **Config changes at runtime** | IPC ring delivers updates without stopping workers     |
| **Observability without overhead** | Workers do single `++`; mgmt does the heavy export |
| **ARP without blocking**  | Workers enqueue, mgmt processes — no worker stall          |
| **Graceful shutdown**      | `CFG_CMD_SHUTDOWN` via IPC + `g_run` flag; ordered teardown |

### 4.5 Shared State Inventory

The following global state is accessed by both planes. Each is designed to be
safe without mutual exclusion in the fast path:

| State                      | Writer        | Reader        | Mechanism                  |
|----------------------------|---------------|---------------|----------------------------|
| `g_metrics[w]`             | Worker `w`    | Mgmt          | Exclusive write; racy read |
| `g_arp[p].table`           | Mgmt          | Workers       | `rte_rwlock` (read-side only on workers) |
| `g_arp_rings[p]`           | Workers       | Mgmt          | SPSC `rte_ring`            |
| `g_udp_rings[p]`           | Workers       | Mgmt          | SPSC `rte_ring`            |
| `g_ipc_rings[w]`           | Mgmt          | Worker `w`    | SPSC `rte_ring`            |
| `g_ack_rings[w]`           | Worker `w`    | Mgmt          | SPSC `rte_ring`            |
| `g_run`                    | Signal handler| All           | `volatile int`             |
| `g_traffic`                | REST API      | Workers       | `volatile int`             |
| `g_worker_ctx[w].tx_gen`   | Worker `w`    | Worker `w`    | Single owner (configured via IPC copy) |
