# vaigAI Architecture

## Module Map

```
vaigai/
├── src/
│   ├── main.c                   # Process startup, lcore launch, CLI
│   ├── common/
│   │   ├── types.h              # Enums, constants, macros
│   │   ├── util.h / util.c      # TSC calibration, IP/MAC helpers, PRNG
│   ├── core/
│   │   ├── eal_init.h/c         # EAL initialisation + custom arg parsing
│   │   ├── core_assign.h/c      # 6-tier auto-scaling lcore role assignment
│   │   ├── mempool.h/c          # Per-worker rte_mempool factory
│   │   ├── ipc.h/c              # SPSC rte_ring management→worker IPC
│   │   └── worker_loop.h/c      # RX→classify→TX poll loop
│   ├── port/
│   │   ├── port_init.h/c        # Port capability negotiation, RSS, queues
│   │   └── soft_nic.h/c         # Driver detection + per-driver hooks
│   ├── net/
│   │   ├── ethernet.h/c         # L2 framing, 802.1Q VLAN
│   │   ├── arp.h/c              # ARP cache, request/reply, rate limiting
│   │   ├── ipv4.h/c             # IPv4 TX build + RX validation
│   │   ├── lpm.h/c              # rte_lpm routing table wrapper
│   │   ├── icmp.h/c             # ICMP echo reply, unreachable
│   │   ├── tcp_tcb.h/c          # TCB pre-allocated store, hash table
│   │   ├── tcp_options.h/c      # MSS, WScale, SACK, Timestamps
│   │   ├── tcp_fsm.h/c          # Full TCP state machine (RFC 793/7323)
│   │   ├── tcp_timer.h/c        # RTO / TIME_WAIT timer wheel tick
│   │   ├── tcp_congestion.h/c   # New Reno CC (RFC 5681)
│   │   ├── tcp_port_pool.h/c    # Ephemeral port bitmap pool
│   │   └── tcp_checksum.h       # HW/SW checksum inline helpers
│   ├── tls/
│   │   ├── tls_engine.h/c       # OpenSSL BIO-pair session engine
│   │   ├── tls_session.h/c      # Per-lcore session store
│   │   ├── cert_mgr.h/c         # Certificate load/hot-reload
│   │   └── cryptodev.h/c        # DPDK Cryptodev AES-GCM offload
│   ├── app/
│   │   └── http11.h/c           # HTTP/1.1 request builder + parser
│   ├── mgmt/
│   │   ├── config_mgr.h/c       # Runtime config (JSON load/save/patch)
│   │   ├── cli.h/c              # readline CLI REPL
│   │   └── rest.h/c             # libmicrohttpd REST API
│   └── telemetry/
│       ├── metrics.h/c          # Per-worker lock-free counter slabs
│       ├── log.h/c              # Structured DPDK log wrappers
│       ├── histogram.h/c        # HDR-style latency histograms
│       └── export.h/c           # JSON / Prometheus export
├── scripts/
│   └── setup.sh                 # Hugepage + NIC binding helper
└── docs/
    ├── README.md
    └── ARCHITECTURE.md          # Architecture reference (this file)
```

## Data-Plane Threading Model

```
Management cores (1–4)       Worker cores (N)
┌─────────────────┐          ┌──────────────────────────┐
│ CLI REPL        │  IPC     │ RX burst (rte_eth_rx)    │
│ REST server     │─ring────▶│ → L2/L3/L4 classify      │
│ Config manager  │◀─ack─────│ → TCP FSM (connect/data)  │
│ ARP manager     │          │ → TLS encrypt/decrypt     │
│ Telemetry snap  │          │ → HTTP build/parse        │
│                 │          │ → TX burst (rte_eth_tx)   │
│                 │          │ → Timer tick (1 ms)       │
└─────────────────┘          └──────────────────────────┘
```

## Packet Receive Path

```
NIC → RSS (symmetric Toeplitz) → Worker queue
→ rte_eth_rx_burst()
→ L2: eth_pop_hdr() + optional VLAN strip
→ L3: ipv4_validate_and_strip()
    → ARP: arp_input()
    → ICMP: icmp_input()
    → TCP:  tcp_fsm_input()
        → TLS decrypt (if session active)
        → HTTP parse: http11_rx_data()
→ metrics update
```

## Packet Transmit Path

```
http11_tx_request()
→ tcp_fsm_send() → build Ethernet+IP+TCP headers
    → tcp_checksum_set() (HW offload or SW)
    → TLS encrypt (if enabled) → prepend TLS record header
→ rte_eth_tx_burst()
```

## Memory Layout

- **Mempools**: per-worker, sized `(rx+tx+pipeline)*2*queues` rounded up to power of 2
- **TCB stores**: pre-allocated flat arrays, open-addressing hash table per lcore
- **ARP cache**: `rte_hash` table with `rte_rwlock` — workers read, mgmt writes
- **Port pools**: per-lcore bitmap over [10000, 60000), TIME_WAIT FIFO ring
- **Metric slabs**: cache-line aligned, one per worker — no cross-core writes

## IPC Protocol

```
Management → Worker:
  config_update_t { cmd(4) seq(8) payload[248] } = 256 bytes exactly
  Transported as pointer in SPSC rte_ring (zero-copy of struct pointer)
  Spin-wait ≤100 µs; drop + log mgmt_ring_overflow if full

Worker → Management (ACK):
  ack_msg_t { seq(8) rc(4) worker_idx(4) } via ack_ring
```

## Core Assignment Tiers

| Cores available | Worker | Management | I/O-bound |
|-----------------|--------|------------|-----------|
| 2–4             | N–1    | 1          | shared    |
| 5–16            | N–2    | 1          | 1         |
| 17–32           | N–3    | 2          | 1         |
| 33–64           | N–4    | 3          | 1         |
| 65–128          | N–5    | 3          | 2         |
| >128            | N–6    | 4          | 2         |
