#!/usr/bin/env bash
# ============================================================================
#  vaigAI — Manual Test Reference
#
#  Cut-and-paste commands for reproducing every test topology.
#  NOT meant to be run as a single script — pick the section you need.
#
#  Sections:
#    0. Common Pre-Setup  (hugepages, modules — run once per boot)
#    1. Server + vaigai Pairs (5 topologies, pick one)
#    2. Traffic Commands  (common to all topologies)
#    3. Monitoring & Debug Commands (common to all topologies)
#    4. Cleanup
#
#  Conventions:
#    SERVER_IP  = IP of the server (nginx/openssl/socat)
#    CLIENT_IP  = IP vaigai binds to (--src-ip / -I)
#    Variables in <angle brackets> must be replaced with actual values.
# ============================================================================

# ╔══════════════════════════════════════════════════════════════════════════════╗
# ║  0. COMMON PRE-SETUP  (run once per boot)                                  ║
# ╚══════════════════════════════════════════════════════════════════════════════╝

# -- Hugepages (2MB, 512MB total — enough for TAP/veth/NIC testing) --
echo 256 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages

# -- Kernel modules --
modprobe vfio-pci
modprobe vfio_iommu_type1

# -- Build vaigai (if not already built) --
cd vaigAI/
meson setup build
ninja -C build


# ╔══════════════════════════════════════════════════════════════════════════════╗
# ║  1. SERVER + VAIGAI PAIRS                                                   ║
# ║                                                                             ║
# ║  Pick ONE pair. Each starts a server with nginx (:80/:443),                 ║
# ║  openssl s_server (:4433), and socat listeners (:5000-5002),                ║
# ║  then shows the matching vaigai command to pair with it.                    ║
# ║                                                                             ║
# ║  DPDK QAT note: crypto_qat PMD only has VF device ID 0x0443 in its PCI     ║
# ║  table, not PF 0x0435. If vaigai ignores the QAT PF, use a VF instead:     ║
# ║    VF 0b:01.0 (from PF 0b:00.0 — different device from 0d/0e)              ║
# ╚══════════════════════════════════════════════════════════════════════════════╝

# ─────────────────────────────────────────────────────────────────────────────
#  1A. QEMU + Mellanox ConnectX NIC (loopback cable between two ports)
# ─────────────────────────────────────────────────────────────────────────────
#  Topology: vaigai (DPDK mlx5, port0) ←loopback→ QEMU VM (vfio passthrough, port1)
#  Network:  10.0.0.1 (vaigai) ↔ 10.0.0.2 (VM)
#  NICs:     95:00.0 → vaigai  (bifurcated mlx5, stays on mlx5_core)
#            95:00.1 → VM      (PF passthrough, bind to vfio-pci)
#  QAT:     PF 0d:00.0 → VM    (PF passthrough, kernel qat_dh895xcc inside VM)
#           PF 0e:00.0 → vaigai (PF passthrough, DPDK crypto_qat PMD)

# -- Setup: bind VM NIC + QAT PFs to vfio-pci --
NIC_VM=0000:95:00.1
NIC_IFACE=ens30f1np1   # kernel interface name before unbinding
QAT_PF_VM=0000:0d:00.0     # → QEMU VM (server-side crypto)
QAT_PF_VAIGAI=0000:0e:00.0 # → vaigai  (client-side crypto)

ip link set "$NIC_IFACE" down 2>/dev/null
for DEV in $NIC_VM $QAT_PF_VM $QAT_PF_VAIGAI; do
    echo "$DEV" > /sys/bus/pci/devices/$DEV/driver/unbind 2>/dev/null
    echo "vfio-pci" > /sys/bus/pci/devices/$DEV/driver_override
    echo "$DEV" > /sys/bus/pci/drivers/vfio-pci/bind
done

# -- Server: QEMU VM (without QAT) --
qemu-system-x86_64 -machine q35,accel=kvm -cpu host -m 1024M -smp 2 \
    -kernel /boot/vmlinuz-$(uname -r) \
    -initrd /boot/initramfs-$(uname -r).img \
    -append "console=ttyS0,115200 root=/dev/vda rw quiet net.ifnames=0 biosdevname=0 vaigai_mode=all" \
    -drive "file=/work/firecracker/rootfs.ext4,format=raw,if=virtio,cache=unsafe" \
    -nic user,model=virtio,hostfwd=tcp::2222-:22 \
    -device "vfio-pci,host=$NIC_VM" \
    -nographic
# ↳ vaigai: ./build/vaigai -l 14-15 -n 4 -a 0000:95:00.0 -- -I 10.0.0.1

# -- OR: Server: QEMU VM with QAT PF --
qemu-system-x86_64 -machine q35,accel=kvm -cpu host -m 1024M -smp 2 \
    -kernel /boot/vmlinuz-$(uname -r) \
    -initrd /boot/initramfs-$(uname -r).img \
    -append "console=ttyS0,115200 root=/dev/vda rw quiet net.ifnames=0 biosdevname=0 vaigai_mode=all" \
    -drive "file=/work/firecracker/rootfs.ext4,format=raw,if=virtio,cache=unsafe" \
    -nic user,model=virtio,hostfwd=tcp::2222-:22 \
    -device "vfio-pci,host=$NIC_VM" \
    -device "vfio-pci,host=$QAT_PF_VM" \
    -nographic
# ↳ vaigai: ./build/vaigai -l 14-15 -n 4 -a 0000:95:00.0 -a 0000:0e:00.0 -- -I 10.0.0.1
# ↳ fallback (if DPDK ignores PF):
#    ./build/vaigai -l 14-15 -n 4 -a 0000:95:00.0 -a 0000:0b:01.0 -- -I 10.0.0.1

# -- Post-boot setup --
sleep 15
ssh -o StrictHostKeyChecking=no -p 2222 root@localhost 'ip addr add 10.0.0.2/24 dev eth0 && ip link set eth0 up'
ssh -p 2222 root@localhost 'nginx -t && rc-service nginx start; ss -tlnp'
ssh -p 2222 root@localhost 'lspci | grep Co-pro && dmesg | grep -i qat | tail -3'
# SERVER_IP=10.0.0.2


# ─────────────────────────────────────────────────────────────────────────────
#  1B. QEMU + Intel i40e NIC (loopback cable between two ports)
# ─────────────────────────────────────────────────────────────────────────────
#  Topology: vaigai (DPDK i40e, port0) ←loopback→ QEMU VM (vfio passthrough, port1)
#  Network:  10.0.0.1 (vaigai) ↔ 10.0.0.2 (VM)
#  NICs:     83:00.0 → vaigai  (PF passthrough, bind to vfio-pci)
#            83:00.1 → VM      (PF passthrough, bind to vfio-pci)
#  QAT:     PF 0d:00.0 → VM    (PF passthrough, kernel qat_dh895xcc inside VM)
#           PF 0e:00.0 → vaigai (PF passthrough, DPDK crypto_qat PMD)

# -- Setup: bind NICs + QAT PFs to vfio-pci --
NIC_VAIGAI=0000:83:00.0
NIC_VM=0000:83:00.1
QAT_PF_VM=0000:0d:00.0     # → QEMU VM
QAT_PF_VAIGAI=0000:0e:00.0 # → vaigai
for DEV in $NIC_VAIGAI $NIC_VM $QAT_PF_VM $QAT_PF_VAIGAI; do
    echo "$DEV" > /sys/bus/pci/devices/$DEV/driver/unbind 2>/dev/null
    echo "vfio-pci" > /sys/bus/pci/devices/$DEV/driver_override
    echo "$DEV" > /sys/bus/pci/drivers/vfio-pci/bind
done

# -- Server: QEMU VM (without QAT) --
qemu-system-x86_64 -machine q35,accel=kvm -cpu host -m 1024M -smp 2 \
    -kernel /boot/vmlinuz-$(uname -r) \
    -initrd /boot/initramfs-$(uname -r).img \
    -append "console=ttyS0,115200 root=/dev/vda rw quiet net.ifnames=0 biosdevname=0 vaigai_mode=all" \
    -drive "file=/work/firecracker/rootfs.ext4,format=raw,if=virtio,cache=unsafe" \
    -nic user,model=virtio,hostfwd=tcp::2222-:22 \
    -device "vfio-pci,host=$NIC_VM" \
    -nographic
# ↳ vaigai: ./build/vaigai -l 0-1 -n 4 -a 0000:83:00.0 -- -I 10.0.0.1

# -- OR: Server: QEMU VM with QAT PF --
qemu-system-x86_64 -machine q35,accel=kvm -cpu host -m 1024M -smp 2 \
    -kernel /boot/vmlinuz-$(uname -r) \
    -initrd /boot/initramfs-$(uname -r).img \
    -append "console=ttyS0,115200 root=/dev/vda rw quiet net.ifnames=0 biosdevname=0 vaigai_mode=all" \
    -drive "file=/work/firecracker/rootfs.ext4,format=raw,if=virtio,cache=unsafe" \
    -nic user,model=virtio,hostfwd=tcp::2222-:22 \
    -device "vfio-pci,host=$NIC_VM" \
    -device "vfio-pci,host=$QAT_PF_VM" \
    -nographic
# ↳ vaigai: ./build/vaigai -l 0-1 -n 4 -a 0000:83:00.0 -a 0000:0e:00.0 -- -I 10.0.0.1
# ↳ fallback (if DPDK ignores PF):
#    ./build/vaigai -l 0-1 -n 4 -a 0000:83:00.0 -a 0000:0b:01.0 -- -I 10.0.0.1

# -- Post-boot setup --
sleep 15
ssh -o StrictHostKeyChecking=no -p 2222 root@localhost 'ip addr add 10.0.0.2/24 dev eth0 && ip link set eth0 up'
ssh -p 2222 root@localhost 'nginx -t && rc-service nginx start; ss -tlnp'
ssh -p 2222 root@localhost 'lspci | grep Co-pro && dmesg | grep -i qat | tail -3'
# SERVER_IP=10.0.0.2


# ─────────────────────────────────────────────────────────────────────────────
#  1C. Firecracker MicroVM + TAP bridge
# ─────────────────────────────────────────────────────────────────────────────
#  Topology: vaigai (DPDK net_tap) ↔ tap-vaigai ↔ br-vaigai ↔ tap-fc0 ↔ Firecracker VM
#  Network:  192.168.204.1 (vaigai) ↔ 192.168.204.2 (VM) via 192.168.204.0/24
#  No physical NIC needed.

# -- Setup: create bridge + TAP for Firecracker --
BRIDGE=br-vaigai
TAP_FC=tap-fc0
VM_IP=192.168.204.2
BRIDGE_IP=192.168.204.3

ip link add "$BRIDGE" type bridge
ip link set "$BRIDGE" up
ip addr add "$BRIDGE_IP/24" dev "$BRIDGE"

ip tuntap add "$TAP_FC" mode tap
ip link set "$TAP_FC" master "$BRIDGE"
ip link set "$TAP_FC" up

# Disable netfilter on bridge (critical for performance)
sysctl -w net.bridge.bridge-nf-call-iptables=0
sysctl -w net.bridge.bridge-nf-call-ip6tables=0
sysctl -w net.bridge.bridge-nf-call-arptables=0

# -- COW copy of rootfs --
ROOTFS_COW=/tmp/vaigai-fc-rootfs.ext4
cp --reflink=auto /work/firecracker/alpine.ext4 "$ROOTFS_COW"

# -- Server: Firecracker --
FC_SOCKET=/tmp/vaigai-fc.sock
rm -f "$FC_SOCKET"
firecracker --api-sock "$FC_SOCKET" &
FC_PID=$!
sleep 1

curl -s --unix-socket "$FC_SOCKET" -X PUT http://localhost/boot-source \
    -H 'Content-Type: application/json' \
    -d '{"kernel_image_path":"/work/firecracker/vmlinux","boot_args":"console=ttyS0 reboot=k panic=1 pci=off root=/dev/vda rw quiet vaigai_mode=all ip=192.168.204.2::192.168.204.3:255.255.255.0::eth0:off"}'

curl -s --unix-socket "$FC_SOCKET" -X PUT http://localhost/drives/rootfs \
    -H 'Content-Type: application/json' \
    -d "{\"drive_id\":\"rootfs\",\"path_on_host\":\"$ROOTFS_COW\",\"is_root_device\":true,\"is_read_only\":false}"

curl -s --unix-socket "$FC_SOCKET" -X PUT http://localhost/network-interfaces/eth0 \
    -H 'Content-Type: application/json' \
    -d "{\"iface_id\":\"eth0\",\"host_dev_name\":\"$TAP_FC\"}"

curl -s --unix-socket "$FC_SOCKET" -X PUT http://localhost/machine-config \
    -H 'Content-Type: application/json' \
    -d '{"vcpu_count":1,"mem_size_mib":256}'

curl -s --unix-socket "$FC_SOCKET" -X PUT http://localhost/actions \
    -H 'Content-Type: application/json' \
    -d '{"action_type":"InstanceStart"}'

sleep 5
ping -c 1 "$VM_IP"

# ↳ vaigai: ./build/vaigai -l 0-1 --no-pci --vdev "net_tap0,iface=tap-vaigai" -- -I 192.168.204.1
# IMPORTANT: After vaigai starts, attach tap-vaigai to bridge:
#   ip link set tap-vaigai master br-vaigai && ip link set tap-vaigai up
# SERVER_IP=192.168.204.2


# ─────────────────────────────────────────────────────────────────────────────
#  1D. Container (podman) + veth pair (AF_XDP)
# ─────────────────────────────────────────────────────────────────────────────
#  Topology: vaigai (DPDK af_xdp on veth-vaigai) ↔ container (veth-peer)
#  Network:  192.168.200.1 (vaigai) ↔ 192.168.200.2 (container)
#  No physical NIC needed. Requires: libbpf, vaigai built with -Daf_xdp=enabled.

# -- Setup: create veth pair + container --
VETH_HOST=veth-vaigai
VETH_PEER=veth-peer

ip link add "$VETH_HOST" type veth peer name "$VETH_PEER"
ip link set "$VETH_HOST" up
ip addr add 192.168.200.1/24 dev "$VETH_HOST"

podman run -d --name vaigai-server --network=none alpine:latest sleep infinity
CTR_PID=$(podman inspect -f '{{.State.Pid}}' vaigai-server)

ip link set "$VETH_PEER" netns "/proc/$CTR_PID/ns/net" 2>/dev/null || \
    nsenter -t "$CTR_PID" -n ip link set "$VETH_PEER" up  # fallback
nsenter -t "$CTR_PID" -n ip addr add 192.168.200.2/24 dev "$VETH_PEER"
nsenter -t "$CTR_PID" -n ip link set "$VETH_PEER" up

# -- Server: install and start services inside container --
podman exec vaigai-server sh -c '
    apk add --no-cache nginx openssl socat
    mkdir -p /etc/nginx/ssl /var/www/html /run/nginx
    openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
        -keyout /etc/nginx/ssl/server.key -out /etc/nginx/ssl/server.crt \
        -subj "/CN=vaigai-test" 2>/dev/null
    echo "OK" > /var/www/html/index.html
    dd if=/dev/urandom of=/var/www/html/100k.bin bs=1024 count=100 2>/dev/null
    nginx
    openssl s_server -cert /etc/nginx/ssl/server.crt -key /etc/nginx/ssl/server.key \
        -accept 4433 -www -quiet &
    socat TCP-LISTEN:5000,fork,reuseaddr PIPE &
    socat TCP-LISTEN:5001,fork,reuseaddr /dev/null &
'
podman exec vaigai-server ss -tlnp

# ↳ vaigai (AF_XDP): ./build/vaigai -l 0-1 --no-pci --vdev "net_af_xdp0,iface=veth-vaigai" -- -I 192.168.200.1
# SERVER_IP=192.168.200.2


# ─────────────────────────────────────────────────────────────────────────────
#  1E. Native Process (no VM, no container — same host)
# ─────────────────────────────────────────────────────────────────────────────
#  Topology: vaigai (DPDK af_packet on veth-vaigai) ↔ native processes (veth-native)
#  Network:  192.168.201.1 (vaigai) ↔ 192.168.201.2 (native)
#  No physical NIC needed.

# -- Setup: create veth pair --
VETH_HOST=veth-vaigai
VETH_NATIVE=veth-native

ip link add "$VETH_HOST" type veth peer name "$VETH_NATIVE"
ip link set "$VETH_HOST" up
ip link set "$VETH_NATIVE" up
ip addr add 192.168.201.2/24 dev "$VETH_NATIVE"

mkdir -p /tmp/vaigai-native-tls
openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
    -keyout /tmp/vaigai-native-tls/server.key \
    -out /tmp/vaigai-native-tls/server.crt \
    -subj "/CN=vaigai-test" 2>/dev/null

# -- Server: nginx + openssl s_server + socat --
cat > /tmp/vaigai-native-nginx.conf << 'EOF'
worker_processes 1;
events { worker_connections 4096; }
http {
    server {
        listen 192.168.201.2:80;
        location / { root /tmp/vaigai-native-www; }
    }
    server {
        listen 192.168.201.2:443 ssl;
        ssl_certificate     /tmp/vaigai-native-tls/server.crt;
        ssl_certificate_key /tmp/vaigai-native-tls/server.key;
        location / { root /tmp/vaigai-native-www; }
    }
}
EOF
mkdir -p /tmp/vaigai-native-www
echo "OK" > /tmp/vaigai-native-www/index.html
dd if=/dev/urandom of=/tmp/vaigai-native-www/100k.bin bs=1024 count=100 2>/dev/null
nginx -c /tmp/vaigai-native-nginx.conf

openssl s_server -cert /tmp/vaigai-native-tls/server.crt \
    -key /tmp/vaigai-native-tls/server.key \
    -accept 192.168.201.2:4433 -www -quiet &

socat TCP-LISTEN:5000,bind=192.168.201.2,fork,reuseaddr PIPE &
socat TCP-LISTEN:5001,bind=192.168.201.2,fork,reuseaddr /dev/null &

ss -tlnp | grep -E ':(80|443|4433|5000|5001)\b'

# ↳ vaigai: ./build/vaigai -l 0-1 --no-pci --vdev "net_af_packet0,iface=veth-vaigai" -- -I 192.168.201.1
# SERVER_IP=192.168.201.2


# ╔══════════════════════════════════════════════════════════════════════════════╗
# ║  2. TRAFFIC COMMANDS  (run at the vaigai> prompt, common to all topologies) ║
# ║                                                                             ║
# ║  Replace SERVER_IP with the server IP from your chosen topology:            ║
# ║    1A/1B: 10.0.0.2    1C: 192.168.204.2                                    ║
# ║    1D: 192.168.200.2  1E: 192.168.201.2                                    ║
# ╚══════════════════════════════════════════════════════════════════════════════╝

# ── TCP SYN flood (unlimited CPS) ──
start --ip SERVER_IP --port 5000 --proto tcp --duration 30

# ── TCP SYN (rate-limited to 1000 CPS) ──
start --ip SERVER_IP --port 5000 --proto tcp --duration 30 --rate 1000

# ── HTTP GET (unlimited TPS) ──
start --ip SERVER_IP --port 80 --proto http --duration 30 --url /

# ── HTTP GET (rate-limited to 500 req/s) ──
start --ip SERVER_IP --port 80 --proto http --duration 30 --rate 500 --url /index.html

# ── HTTP throughput (connection reuse, 4 streams) ──
start --ip SERVER_IP --port 80 --proto http --duration 30 --reuse --streams 4 --url /100k.bin

# ── HTTPS TPS (unlimited, nginx :443) ──
start --ip SERVER_IP --port 443 --proto https --duration 30 --url /

# ── HTTPS TPS (rate-limited to 200 req/s) ──
start --ip SERVER_IP --port 443 --proto https --duration 30 --rate 200 --url /

# ── HTTPS throughput (connection reuse, 4 streams) ──
start --ip SERVER_IP --port 443 --proto https --duration 30 --reuse --streams 4 --url /100k.bin

# ── TLS handshake TPS (raw TLS, openssl s_server :4433) ──
start --ip SERVER_IP --port 4433 --proto tls --duration 30

# ── TLS handshake (rate-limited to 100 conn/s) ──
start --ip SERVER_IP --port 4433 --proto tls --duration 30 --rate 100

# ── TLS throughput (connection reuse, 4 streams) ──
start --ip SERVER_IP --port 4433 --proto tls --duration 30 --reuse --streams 4

# ── UDP flood (unlimited PPS, 1024-byte payload) ──
start --ip SERVER_IP --port 5001 --proto udp --size 1024 --duration 30

# ── UDP (rate-limited to 10000 PPS) ──
start --ip SERVER_IP --port 5001 --proto udp --size 512 --duration 30 --rate 10000

# ── ICMP flood ──
start --ip SERVER_IP --port 0 --proto icmp --duration 10

# ── curl equivalent (single HTTP GET) ──
start --ip SERVER_IP --port 80 --proto http --duration 1 --rate 1 --url /index.html

# ── Stop active traffic ──
stop

# ── Reset TCP state between tests ──
reset


# ╔══════════════════════════════════════════════════════════════════════════════╗
# ║  3. MONITORING & DEBUG  (run at vaigai> prompt during or after tests)       ║
# ╚══════════════════════════════════════════════════════════════════════════════╝

# ── Stats ──
stat net                    # snapshot of all counters
stat net --rate             # per-second rates (pps, Mbps)
stat net --rate --watch     # live dashboard (Ctrl+C to stop)
stat net --core 0           # single worker core stats
stat cpu                    # CPU utilization per lcore
stat cpu --rate --watch     # live CPU monitor
stat mem                    # mempool usage
stat port                   # NIC port counters
stat port --rate            # NIC port rates

# ── Packet trace (pcapng capture) ──
trace start /tmp/capture.pcapng           # capture port 0 queue 0
trace start /tmp/capture.pcapng 0 0       # explicit port and queue
trace stop                                # stop capture
# Open with: wireshark /tmp/capture.pcapng

# ── Interface info ──
show interface              # NIC driver, MAC, IP, link, offloads, queue stats

# ── ICMP ping (standalone, not a traffic test) ──
ping SERVER_IP              # default 4 pings
ping SERVER_IP 10           # 10 pings
ping SERVER_IP 1 1400       # 1 ping, 1400-byte payload

# ── Quit ──
quit                        # graceful shutdown


# ╔══════════════════════════════════════════════════════════════════════════════╗
# ║  4. CLEANUP  (after tests)                                                  ║
# ╚══════════════════════════════════════════════════════════════════════════════╝

# Kill QEMU
kill $QEMU_PID 2>/dev/null

# Rebind NIC to kernel driver (i40e example)
NIC_VM=0000:83:00.1
echo "$NIC_VM" > /sys/bus/pci/devices/$NIC_VM/driver/unbind 2>/dev/null
echo "" > /sys/bus/pci/devices/$NIC_VM/driver_override
echo "$NIC_VM" > /sys/bus/pci/drivers/i40e/bind 2>/dev/null

# Rebind NIC to kernel driver (mlx5 example)
NIC_VM=0000:95:00.1
echo "$NIC_VM" > /sys/bus/pci/devices/$NIC_VM/driver/unbind 2>/dev/null
echo "" > /sys/bus/pci/devices/$NIC_VM/driver_override
echo "$NIC_VM" > /sys/bus/pci/drivers/mlx5_core/bind 2>/dev/null

# Firecracker cleanup
kill $FC_PID 2>/dev/null
rm -f "$FC_SOCKET" "$ROOTFS_COW"
ip link del tap-fc0 2>/dev/null
ip link del br-vaigai 2>/dev/null

# Container cleanup
podman rm -f vaigai-server 2>/dev/null
ip link del veth-vaigai 2>/dev/null

# Native cleanup
nginx -s stop -c /tmp/vaigai-native-nginx.conf 2>/dev/null
# kill openssl and socat PIDs
ip link del veth-vaigai 2>/dev/null
rm -rf /tmp/vaigai-native-tls /tmp/vaigai-native-www /tmp/vaigai-native-nginx.conf

