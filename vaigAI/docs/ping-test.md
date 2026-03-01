# Test Architecture

## ping_veth — ICMP over veth pair

### Topology

```
 Host (root netns)                        Container netns
┌──────────────────────┐                ┌──────────────────────┐
│                      │                │                      │
│  vaigAI (DPDK)       │    veth pair   │  Alpine (--net=none) │
│  ┌────────────────┐  │                │  ┌────────────────┐  │
│  │  lcore 0: main │  │                │  │  kernel ICMP    │  │
│  │  lcore 1: worker│  │                │  │  echo reply     │  │
│  └───────┬────────┘  │                │  └───────┬────────┘  │
│          │           │                │          │           │
│    ┌─────┴─────┐     │                │    ┌─────┴─────┐    │
│    │ veth-vaigai│◄───────────────────────►│ veth-tpeer │    │
│    │ (host end) │     │                │    │ (peer end) │    │
│    └───────────┘     │                │    └───────────┘    │
│    192.168.200.1     │                │    192.168.200.2/24 │
│                      │                │                      │
└──────────────────────┘                └──────────────────────┘
```

### Data Path

```
 AF_PACKET mode (default)                AF_XDP mode (--xdp)
┌─────────────────────────┐            ┌─────────────────────────┐
│ vaigai worker           │            │ vaigai worker           │
│   │                     │            │   │                     │
│   ▼                     │            │   ▼                     │
│ rte_eth_tx_burst()      │            │ rte_eth_tx_burst()      │
│   │                     │            │   │                     │
│   ▼                     │            │   ▼                     │
│ net_af_packet PMD       │            │ net_af_xdp PMD          │
│   │  raw socket         │            │   │  XDP socket (UMEM)  │
│   ▼                     │            │   ▼                     │
│ ┌─────────────────────┐ │            │ ┌─────────────────────┐ │
│ │ kernel: full stack  │ │            │ │ kernel: XDP program  │ │
│ │ socket → netdev     │ │            │ │ (bypass socket layer)│ │
│ └─────────┬───────────┘ │            │ └─────────┬───────────┘ │
│           ▼             │            │           ▼             │
│   veth_xmit → peer ns  │            │   veth_xmit → peer ns  │
└─────────────────────────┘            └─────────────────────────┘
```

### Interface Names

| Mode | Host-side veth | Peer-side veth | DPDK vdev |
|------|---------------|----------------|-----------|
| AF_PACKET | `veth-vaigai` | `veth-tpeer` | `net_af_packet0,iface=veth-vaigai` |
| AF_XDP | `veth-xdp` | `veth-xdppeer` | `net_af_xdp0,iface=veth-xdp` |

### Test Modes

**Interval** (default) — Sends 5 ICMP echo requests, 56 bytes each, 1 s apart.
Pass criterion: all 5 replies received, 0% loss.

**Flood** (`--flood N`) — Sends ICMP at line rate for N seconds.
Pass criterion: transmitted packet count > 0. Prints throughput (pps) on completion.

### Usage

```bash
# AF_PACKET — interval
bash tests/ping_veth.sh

# AF_PACKET — flood 5s
bash tests/ping_veth.sh --flood 5

# AF_XDP — interval
bash tests/ping_veth.sh --xdp

# AF_XDP — flood 5s
bash tests/ping_veth.sh --xdp --flood 5
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
│  2. ip link add veth-vaigai type veth peer veth-tpeer       │
│     ip link set veth-vaigai promisc on && up                │
│     [XDP only] ethtool -L veth-xdp combined 1              │
│                                                             │
│  3. ip link set veth-tpeer netns <container>                │
│     nsenter: ip addr add 192.168.200.2/24                   │
│                                                             │
│  4. Write JSON config → /tmp/vaigai_ping_XXXXXX.json        │
└──────────────────────────┬──────────────────────────────────┘
                           │
┌──────────────────────────▼──────────────────────────────────┐
│  RUN                                                        │
│                                                             │
│  stdin ──► "ping 192.168.200.2 5 56 1000"  ──┐             │
│            or "flood icmp 192.168.200.2 Ns"   ├──► vaigai  │
│            "quit"                            ──┘             │
│                                                 │           │
│  stdout ◄── capture all output ◄────────────────┘           │
└──────────────────────────┬──────────────────────────────────┘
                           │
┌──────────────────────────▼──────────────────────────────────┐
│  ASSERT                                                     │
│                                                             │
│  Interval: grep "5 transmitted, 5 received, 0% loss"       │
│  Flood:    grep "<N> packets transmitted" where N > 0       │
│                                                             │
│  ──► PASS or FAIL                                           │
└──────────────────────────┬──────────────────────────────────┘
                           │
┌──────────────────────────▼──────────────────────────────────┐
│  TEARDOWN  (EXIT trap — always runs)                        │
│                                                             │
│  rm /tmp/vaigai_ping_*.json                                 │
│  ip link del veth-vaigai                                    │
│  podman stop + rm vaigai-test-ping                          │
└─────────────────────────────────────────────────────────────┘
```
