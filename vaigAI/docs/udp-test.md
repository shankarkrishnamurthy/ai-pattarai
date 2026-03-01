# Test Architecture

## udp_veth — UDP over veth pair

### Topology

```
 Host (root netns)                        Container netns
┌──────────────────────┐                ┌──────────────────────┐
│                      │                │                      │
│  vaigAI (DPDK)       │    veth pair   │  Alpine (--net=none) │
│  ┌────────────────┐  │                │  ┌────────────────┐  │
│  │  lcore 0: main │  │                │  │  kernel UDP     │  │
│  │  lcore 1: worker│  │                │  │  (no listener → │  │
│  └───────┬────────┘  │                │  │   NoPorts++)    │  │
│          │           │                │  └───────┬────────┘  │
│    ┌─────┴─────┐     │                │    ┌─────┴─────┐    │
│    │veth-udptest│◄───────────────────────►│veth-udppeer│    │
│    │ (host end) │     │                │    │ (peer end) │    │
│    └───────────┘     │                │    └───────────┘    │
│    192.168.201.1     │                │    192.168.201.2/24 │
│                      │                │                      │
└──────────────────────┘                └──────────────────────┘
```

### Interface Names

| Mode | Host-side veth | Peer-side veth | DPDK vdev |
|------|---------------|----------------|-----------|
| AF_PACKET | `veth-udptest` | `veth-udppeer` | `net_af_packet0,iface=veth-udptest` |
| AF_XDP | `veth-udpxdp` | `veth-udpxdppeer` | `net_af_xdp0,iface=veth-udpxdp` |

### Test Modes

**Rate-limited** (default) — Sends UDP at 1000 pps for 1 second to port 9 (discard).
Pass criteria: vaigai `udp_tx` is ~1000 (900–1100), and container `/proc/net/snmp` NoPorts delta matches.

**Flood** (`--flood-seconds N`) — Sends UDP at line rate for N seconds.
Pass criteria: vaigai `udp_tx` > 0, and container NoPorts delta matches.

Each invocation runs **one** mode. The container has no UDP listener, so every packet increments the kernel's `NoPorts` counter — this cross-validates vaigai's telemetry.

### Usage

```bash
# AF_PACKET — rate-limited (default)
bash tests/udp_veth.sh

# AF_PACKET — flood 10s
bash tests/udp_veth.sh --flood-seconds 10

# AF_XDP — rate-limited
bash tests/udp_veth.sh --xdp

# AF_XDP — flood 10s
bash tests/udp_veth.sh --xdp --flood-seconds 10
```

### Requirements

| Requirement | AF_PACKET | AF_XDP |
|-------------|-----------|--------|
| Root | yes | yes |
| Kernel | any | >= 5.4 |
| libbpf | no | yes |
| DPDK PMD | `librte_net_af_packet` | `librte_net_af_xdp` |
| podman | yes | yes |

### Lifecycle

```
┌─────────────────────────────────────────────────────────────┐
│  PRE-FLIGHT                                                 │
│  ✓ root?  ✓ vaigai binary?  ✓ podman/ip/nsenter?           │
│  [XDP only] ✓ kernel ≥ 5.4?  ✓ libbpf?                     │
└──────────────────────────┬──────────────────────────────────┘
                           │
┌──────────────────────────▼──────────────────────────────────┐
│  SETUP                                                      │
│                                                             │
│  1. podman run alpine:latest --network none                 │
│                                                             │
│  2. ip link add veth-udptest type veth peer veth-udppeer    │
│     ip link set veth-udptest promisc on && up               │
│     [XDP only] ethtool -L veth-udpxdp combined 1           │
│                                                             │
│  3. ip link set veth-udppeer netns <container>              │
│     nsenter: ip addr add 192.168.201.2/24                   │
│                                                             │
│  4. Write JSON config → /tmp/vaigai_udp_XXXXXX.json         │
└──────────────────────────┬──────────────────────────────────┘
                           │
┌──────────────────────────▼──────────────────────────────────┐
│  RUN (one of two modes)                                     │
│                                                             │
│  Rate-limited (default):                                    │
│  stdin ──► "flood udp 192.168.201.2 1 1000 64 9" ──► vaigai│
│                                                             │
│  Flood (--flood-seconds N):                                 │
│  stdin ──► "flood udp 192.168.201.2 Ns 0 64 9"  ──► vaigai │
│                                                             │
│  Read container NoPorts before & after                      │
└──────────────────────────┬──────────────────────────────────┘
                           │
┌──────────────────────────▼──────────────────────────────────┐
│  ASSERT (2 checks)                                          │
│                                                             │
│  Rate-limited: udp_tx ≈ 1000 (900–1100)                    │
│  Flood:        udp_tx > 0                                  │
│  Both:         NoPorts delta == udp_tx                      │
│                                                             │
│  ──► PASS or FAIL                                           │
└──────────────────────────┬──────────────────────────────────┘
                           │
┌──────────────────────────▼──────────────────────────────────┐
│  TEARDOWN  (EXIT trap — always runs)                        │
│                                                             │
│  rm /tmp/vaigai_udp_*.json                                  │
│  ip link del veth-udptest                                   │
│  podman stop + rm vaigai-test-udp                           │
└─────────────────────────────────────────────────────────────┘
```

### Validation Strategy

```
  vaigAI                      container kernel
┌──────────┐                ┌──────────────────┐
│ udp_tx=N │───── UDP ─────►│ /proc/net/snmp   │
│ (telem.) │    port 9      │ Udp: NoPorts += N│
└──────────┘                └──────────────────┘
      │                              │
      └──────── compare ─────────────┘
              N == delta? → PASS
```
