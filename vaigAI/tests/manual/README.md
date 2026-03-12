# Manual Tests

Individual test scripts for each vaigAI topology. Each script is self-contained:
sets up infrastructure, starts servers, launches vaigai, prints available
commands, and cleans up on exit (Ctrl+C).

## Scripts

| Script | Topology | NIC / PMD | Requirements |
|--------|----------|-----------|-------------|
| `1a-qemu-mlx5.sh` | QEMU VM + loopback cable | Mellanox ConnectX (mlx5) | Physical NIC, QEMU, loopback cable |
| `1b-qemu-i40e.sh` | QEMU VM + loopback cable | Intel XXV710 (i40e) | Physical NIC, QEMU, loopback cable |
| `1c-firecracker-tap.sh` | Firecracker MicroVM + bridge | TAP PMD | Firecracker, kernel, rootfs |
| `1d-container-afxdp.sh` | Podman container + veth | AF_XDP | podman, libbpf, `-Daf_xdp=enabled` |
| `1e-native-afpacket.sh` | Native processes + veth | AF_PACKET | nginx, openssl, socat |
| `1f-net-memif.sh` | Shared memory + testpmd relay | net_memif (master) + net_tap | DPDK with `net/memif` driver |
| `1g-net-ring.sh` | Software loopback (self) | net_ring | None (DPDK built-in) |
| `1h-net-null.sh` | CPU benchmark / null sink | net_null | None (DPDK built-in) |
| `1i-net-pcap.sh` | PCAP replay + capture | net_pcap | libpcap, DPDK with `net/pcap` |
| `1j-qemu-virtio.sh` | QEMU microVM + virtio-net bridge | net_tap + vhost-net | QEMU, kernel, rootfs |
| `1k-vhost-user.sh` | vhost-user ↔ QEMU microVM | net_vhost | QEMU, kernel, rootfs, DPDK with `net/vhost` |

## Usage

Each script supports three modes for split-terminal workflows:

```bash
# All-in-one (original behavior) — run as root
sudo bash tests/manual/1e-native-afpacket.sh

# Split-terminal workflow:
#   Terminal 1: start server infrastructure
sudo bash tests/manual/1e-native-afpacket.sh --server

#   Terminal 2: start vaigai traffic generator
sudo bash tests/manual/1e-native-afpacket.sh --tgen

# Explicit cleanup (if needed after abnormal exit)
sudo bash tests/manual/1e-native-afpacket.sh --cleanup
```

### Options

| Flag | Description |
|------|-------------|
| (none) | Run everything: server + vaigai (original behavior) |
| `--server` | Start server infrastructure only — blocks until Ctrl+C |
| `--tgen` | Start vaigai traffic generator only (assumes `--server` is running) |
| `--cleanup` | Clean up all resources from previous runs |

### Accessing the server

| Script | Access command |
|--------|---------------|
| `1a-qemu-mlx5.sh` | `ssh -o StrictHostKeyChecking=no -p 2222 root@localhost` |
| `1b-qemu-i40e.sh` | `ssh -o StrictHostKeyChecking=no -p 2222 root@localhost` |
| `1c-firecracker-tap.sh` | `tail -f /tmp/vaigai-serial.log` (serial) or `ssh root@192.168.204.2` (if SSH in rootfs) |
| `1d-container-afxdp.sh` | `podman exec -it vaigai-server sh` |
| `1e-native-afpacket.sh` | Native processes on host — no SSH needed |
| `1f-net-memif.sh` | socat on TAP relay — no SSH needed |
| `1g-net-ring.sh` | Self-loopback — no external server |
| `1h-net-null.sh` | Null sink — no external server |
| `1i-net-pcap.sh` | File-based — inspect output.pcap after quit |
| `1j-qemu-virtio.sh` | `tail -f /tmp/vaigai-1j/serial.log` (serial) |
| `1k-vhost-user.sh` | `tail -f /tmp/vaigai-1k/serial.log` (serial) |

### Special startup order

`1k-vhost-user.sh` requires **`--vaigai` before `--server`** because vaigai creates
the vhost-user socket that QEMU connects to:

```bash
# Terminal 1 — creates socket, starts vaigai interactively
sudo bash tests/manual/1k-vhost-user.sh --vaigai

# Terminal 2 — starts QEMU once socket is visible
sudo bash tests/manual/1k-vhost-user.sh --server
```

## Structure

Each script is fully self-contained — no shared files. You can copy-paste
individual commands from any script directly into a terminal.
