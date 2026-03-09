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

## Structure

Each script is fully self-contained — no shared files. You can copy-paste
individual commands from any script directly into a terminal.
