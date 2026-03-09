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

```bash
# Run as root
sudo bash tests/manual/1e-native-afpacket.sh

# Each script prints traffic commands after setup.
# For daemon-mode tests (1C), use a second terminal:
./build/vaigai --attach
```

## Structure

Each script is fully self-contained — no shared files. You can copy-paste
individual commands from any script directly into a terminal.
