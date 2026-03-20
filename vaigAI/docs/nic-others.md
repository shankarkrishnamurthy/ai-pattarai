# Other NIC Types for vaigAI Testing

This document covers additional NIC form factors and DPDK PMDs that can be used
to test vaigAI beyond the primary drivers (AF_PACKET, AF_XDP, TAP, SR-IOV).

---

## 1. virtio-net — VM Paravirtual

Tests vaigAI running inside a KVM/QEMU guest against a second VM or host-side
service over a paravirtual NIC.

```
┌─────────────────────────────────┐       ┌─────────────────────────────────┐
│         QEMU/KVM Host           │       │        QEMU Guest VM            │
│                                 │       │                                 │
│  vaigai (DPDK virtio PMD)       │       │   socat / iperf3 / nginx        │
│    ↕                            │       │       ↕                         │
│  virtio-net backend (vhost-net) ├───────┤  virtio-net frontend (eth0)    │
│                                 │       │  IP: 192.168.100.2              │
│  IP: 192.168.100.1              │       │                                 │
└─────────────────────────────────┘       └─────────────────────────────────┘
```

**DPDK vdev flag:**
```
--vdev "net_virtio0"
```

**Use case:** Validate the full protocol stack (TCP/UDP/ICMP/HTTP/TLS) inside a
VM where only paravirtual NICs are available (e.g., cloud instances, CI
environments).

---

## 2. net_ring — Software Loopback

Pure in-process loopback using DPDK ring-based PMD. No kernel, no NIC, no
external process required.

```
┌──────────────────────────────────────────┐
│              vaigai process              │
│                                          │
│   TX Worker ──→ [rte_ring] ──→ RX Worker │
│                                          │
│   Port 0 (net_ring0)  ←──loopback──→    │
│                                          │
│   No kernel, no NIC, pure memory         │
└──────────────────────────────────────────┘
```

**DPDK vdev flag:**
```
--vdev "net_ring0"
```

**Use case:** Unit testing the packet generation pipeline, header construction,
and worker scheduling without any external dependency. Fastest possible test
loop for CI.

---

## 3. net_pcap — Replay / Capture

File-based TX and RX using pcap files. vaigAI reads packets from an input pcap
and writes generated packets to an output pcap.

```
┌──────────────────────────────────────────┐
│              vaigai process              │
│                                          │
│   RX: reads from ──→ input.pcap          │
│                                          │
│   net_pcap PMD                           │
│                                          │
│   TX: writes to  ──→ output.pcap         │
│                                          │
└──────────────────────────────────────────┘
```

**DPDK vdev flag:**
```
--vdev "net_pcap0,rx_pcap=input.pcap,tx_pcap=output.pcap"
```

**Use case:** Deterministic regression testing using pre-recorded traffic.
Replay known-good captures and diff the output pcap to verify correctness after
protocol stack changes.

---

## 4. net_null — Benchmark / Overhead Measurement

A black-hole PMD: TX discards all packets, RX returns empty bursts. Used to
isolate vaigAI's CPU overhead from NIC I/O cost.

```
┌──────────────────────────────────────────┐
│              vaigai process              │
│                                          │
│   TX Worker ──→ net_null0 ──→ /dev/null  │
│                   (all packets dropped)  │
│                                          │
│   RX Worker ←── net_null0 ←── (nothing)  │
│                   (dummy empty bursts)   │
│                                          │
│   Measures: CPU overhead, scheduling,    │
│             code-path latency            │
└──────────────────────────────────────────┘
```

**DPDK vdev flag:**
```
--vdev "net_null0"
```

**Use case:** Measure pure TX generation throughput (Mpps) and worker loop
overhead independent of NIC speed or external back-pressure.

---

## 5. net_memif — Shared Memory (Container-to-Container)

Zero-copy shared memory channel between two processes or containers via a Unix
socket. No kernel network stack involved.

```
┌─────────────────────────┐  shared memory   ┌─────────────────────────┐
│      Container A        │   (memif socket)  │      Container B        │
│                         │                   │                         │
│  vaigai (DPDK)          │       mmap'd      │  socat / iperf3         │
│    ↕                    │    ┌─────────┐    │       ↕                 │
│  net_memif0 (master) ───┼───→│ /tmp/   │←───┼─── net_memif0 (slave)  │
│                         │    │ memif.  │    │   or libmemif app       │
│  IP: 10.0.0.1           │    │ sock    │    │  IP: 10.0.0.2           │
└─────────────────────────┘    └─────────┘    └─────────────────────────┘
            zero-copy, no kernel involved
```

**DPDK vdev flag (master side):**
```
--vdev "net_memif0,role=master,socket=/tmp/memif.sock"
```

**Use case:** Test vaigAI in containerized environments (Podman/Docker) where
two DPDK-capable containers communicate via shared memory with minimal overhead.

---

## 6. vhost-user — VM Backend / OVS-DPDK

vaigAI acts as a vhost-user backend, driving a virtio-net frontend inside a
QEMU guest. Optionally places OVS-DPDK as a software switch in the path.

```
┌──────────────────────────────────────────────────────────────────┐
│                          Host                                    │
│                                                                  │
│  vaigai (DPDK)                        QEMU Guest VM              │
│    ↕                                 ┌──────────────────┐        │
│  vhost-user port ──→ unix socket ──→ │ virtio-net (eth0)│        │
│  (--vdev                  ↕          │    ↕              │        │
│   net_vhost0,   /tmp/vhost.sock      │ socat / iperf3   │        │
│   iface=...)                         │ IP: 192.168.50.2 │        │
│                                      └──────────────────┘        │
│  IP: 192.168.50.1                                                │
│                                                                  │
│  ┌──────────────────────────────────────┐                        │
│  │  Optional: OVS-DPDK switch in path  │                        │
│  │  vaigai ↔ OVS ↔ vhost-user ↔ VM    │                        │
│  └──────────────────────────────────────┘                        │
└──────────────────────────────────────────────────────────────────┘
```

**DPDK vdev flag:**
```
--vdev "net_vhost0,iface=/tmp/vhost.sock"
```

**Use case:** NFV and cloud dataplane testing where vaigAI generates traffic
into a VM via the vhost-user fast path, bypassing the kernel entirely.

---

## Summary

| PMD | Kernel bypass | External peer | Best for |
|-----|:---:|:---:|---------|
| `net_ring` | ✅ | ❌ | Unit tests, CI, pipeline validation |
| `net_null` | ✅ | ❌ | CPU/throughput benchmarking |
| `net_pcap` | ✅ | ❌ (file) | Deterministic regression, pcap replay |
| `net_memif` | ✅ | container | Container-to-container, zero-copy |
| `virtio-net` | ✅ | VM guest | VM guest testing, cloud instances |
| `vhost-user` | ✅ | VM guest | NFV dataplane, OVS-DPDK integration |

**Complexity order (low → high):**
```
net_null  <  net_ring  <  net_pcap  <  net_memif  <  virtio-net  <  vhost-user
```
