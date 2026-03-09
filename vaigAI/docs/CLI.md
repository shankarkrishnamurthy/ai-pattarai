# vaigai CLI Reference

Interactive command-line interface for the vaigai traffic generator.
Launch with `vaigai` and type commands at the `vaigai>` prompt.

---

## Command-Line Arguments

vaigai accepts DPDK EAL arguments followed by `--` and then app-level arguments:

```
./build/vaigai <EAL args> -- <app args>
```

### App-Level Options

| Flag | Short | Description |
|------|-------|-------------|
| `--src-ip <addr>` | `-I` | Source IPv4 address for all ports |
| `--sslkeylog <path>` | `-K` | Write TLS session keys in NSS Key Log format |
| `--num-worker-cores <N>` | `-W` | Number of worker lcores (0 = auto) |
| `--num-mgmt-cores <N>` | `-M` | Number of management lcores (0 = auto) |
| `--core-assignment-policy <p>` | `-P` | Core assignment policy (`auto` or `manual`) |
| `--rx-descs <N>` | `-r` | RX ring descriptor count |
| `--tx-descs <N>` | `-t` | TX ring descriptor count |
| `--pipeline-depth <N>` | `-d` | Pipeline depth for mempool sizing |
| `--max-chain-depth <N>` | `-C` | mbuf chain depth |
| `--max-conn <N>` | `-X` | Max concurrent connections per worker |
| `--rest-port <port>` | `-R` | REST API listen port (0 = disabled) |

### Special Modes

| Flag | Description |
|------|-------------|
| `--attach [path]` | Connect to a running vaigai as a remote CLI client (no EAL init) |

### Environment Variables

| Variable | Description |
|----------|-------------|
| `VAIGAI_CONFIG` | Path to JSON config file loaded at startup |
| `SSLKEYLOGFILE` | TLS key log file path (fallback if `--sslkeylog` is not set) |

### `--sslkeylog` — TLS Key Logging

Writes TLS session keys in
[NSS Key Log format](https://developer.mozilla.org/en-US/docs/Mozilla/Projects/NSS/Key_Log_Format)
so that Wireshark or tshark can decrypt captured TLS traffic. The file is
opened in append mode and flushed after every key line.

If `--sslkeylog` is not specified, vaigai checks the `SSLKEYLOGFILE`
environment variable. The command-line flag takes precedence.

**Example — via command-line flag:**

```bash
./build/vaigai -l 14-15 -n 4 -a 0000:95:00.0 -- -I 10.0.0.1 --sslkeylog /tmp/keys.log
```

**Example — via environment variable:**

```bash
SSLKEYLOGFILE=/tmp/keys.log ./build/vaigai -l 14-15 -n 4 -a 0000:95:00.0 -- -I 10.0.0.1
```

**Decrypting with tshark:**

```bash
tshark -r capture.pcapng -o tls.keylog_file:/tmp/keys.log
```

---

## help

Show all available commands.

```
vaigai> help
```

---

## ping

Send ICMP echo requests to a destination.

```
ping <dst_ip> [count] [size] [interval_ms]
```

| Parameter     | Default | Description                      |
|---------------|---------|----------------------------------|
| `dst_ip`      | —       | Destination IPv4 address         |
| `count`       | 5       | Number of echo requests to send  |
| `size`        | 56      | Payload size in bytes            |
| `interval_ms` | 1000    | Interval between requests (ms)   |

```
vaigai> ping 10.0.0.2
vaigai> ping 10.0.0.2 10 128 500
```

---

## start

Start traffic generation toward a destination.

```
start --ip <addr> --port <N> --duration <secs> [options]
```

### Required flags

| Flag         | Description                        |
|--------------|------------------------------------|
| `--ip`       | Destination IPv4 address           |
| `--port`     | Destination TCP/UDP port           |
| `--duration` | Test duration in seconds (> 0). Not needed with `--one`. |

### Optional flags

| Flag          | Default | Description                                   |
|---------------|---------|-----------------------------------------------|
| `--proto`     | `tcp`   | Protocol: `tcp`, `http`, `https`, `udp`, `icmp`, `tls` |
| `--rate`      | 0       | Rate limit in packets/sec (0 = unlimited). Mutually exclusive with `--one`. |
| `--one`       | off     | Send exactly one request/handshake/connection and stop. Mutually exclusive with `--duration` and `--rate`. |
| `--size`      | 56      | Payload size in bytes                         |
| `--streams`   | 1       | Number of concurrent streams (max 16)         |
| `--reuse`     | off     | Enable connection reuse (throughput mode)     |
| `--url`       | `/`     | HTTP request path                             |
| `--host`      | `--ip`  | HTTP Host header value                        |
| `--tls`       | off     | Enable TLS encryption                         |

### Examples

```
# TCP SYN flood for 10 seconds
vaigai> start --ip 10.0.0.2 --port 5000 --duration 10

# HTTP GET at 100 req/s for 5 seconds
vaigai> start --ip 10.0.0.2 --port 80 --proto http --duration 5 --rate 100

# HTTPS throughput test with 4 streams
vaigai> start --ip 10.0.0.2 --port 443 --proto https --duration 30 --reuse --streams 4

# UDP flood with 1024-byte packets
vaigai> start --ip 10.0.0.2 --port 5001 --proto udp --size 1024 --duration 5

# ICMP flood for 3 seconds
vaigai> start --ip 10.0.0.2 --port 0 --proto icmp --duration 3

# Single HTTP request (curl equivalent)
vaigai> start --ip 10.0.0.2 --port 80 --proto http --one --url /index.html

# Single TLS handshake
vaigai> start --ip 10.0.0.2 --port 4433 --proto tls --one

# Single HTTPS request
vaigai> start --ip 10.0.0.2 --port 443 --proto https --one --url /
```

---

## stop

Stop active traffic generation immediately.

```
vaigai> stop
```

---

## reset

Reset all TCP state: sends RST for active connections, clears TCB stores,
frees port pools, and resets metrics. Traffic generation must be stopped first.

```
vaigai> reset
```

---

## stat

Unified statistics command with sub-commands and shared flags.

```
stat [cpu|mem|net|port] [--rate] [--watch] [--core N]
```

Without a sub-command, `stat` prints a brief summary of all domains.

### Shared Flags

| Flag | Description |
|------|-------------|
| `--rate` | Take two snapshots 1 second apart and show rates (pps, Mbps, %) |
| `--watch` | Continuous refresh every 1 second (implies `--rate`; Ctrl+C stops) |
| `--core N` | Filter output to a single worker (W*N*) |

### stat cpu

Per-core CPU utilisation breakdown. Shows what fraction of cycles each worker
spends in RX, TX generation, timer ticks, and idle polling.

```
vaigai> stat cpu
──────────────────────────────────────────────────────────────────
Core   Lcore  Socket  Role     Busy%   RX%    TX%   Timer%  Idle%
──────────────────────────────────────────────────────────────────
W0     1      0       worker   62.3    45.1   12.8   4.4    37.7
W1     2      0       worker   58.9    40.2   14.3   4.4    41.1
──────────────────────────────────────────────────────────────────

vaigai> stat cpu --rate           # 1-second sample
vaigai> stat cpu --core 0         # single worker
vaigai> stat cpu --rate --watch   # live monitor
```

### stat mem

Memory usage: packet buffers (mbufs), DPDK heap, TCP connections, hugepages.

```
vaigai> stat mem
--- packet buffers ---
Pool         Total    In-Use   Avail    Use%
pool_w0      8192     1247     6945     15.2%
pool_w1      8192     892      7300     10.9%

--- dpdk heap ---
Socket   Heap Size    Allocated    Free         Use%
0        512.0 MB     127.3 MB     384.7 MB     24.9%

--- connections ---
Worker   Active   Capacity   Use%
W0       423      1000000     0.0%

--- hugepages ---
Size     Total   Free   In-Use   Use%
2 MB     256     128    128      50.0%

vaigai> stat mem --core 0         # single worker pools & connections
vaigai> stat mem --rate           # mbuf churn and conn delta/s
```

### stat net

Network packet counters. Same content as the old `stats` command.

```
vaigai> stat net                  # JSON counter dump (aggregate)
vaigai> stat net --core 0         # per-worker breakdown + TCP state dist
vaigai> stat net --rate           # pps, Mbps, conn/s over 1-second window
```

With `--core N`, also shows TCP connection state distribution:

```
vaigai> stat net --core 0
--- worker 0 (lcore 1) ---
  tx_pkts: 542301       tx_bytes: 45.3 MB
  ...
--- tcp connections (W0) ---
State          Count
ESTABLISHED    312
SYN_SENT       88
TIME_WAIT      23
TOTAL          423 / 1000000
```

### stat port

Per-NIC hardware statistics from the DPDK driver.

```
vaigai> stat port
─────────────────────────────────────────────────────────────────────
Port  Driver     Link       RX pkts     RX bytes   RX miss  RX err
                            TX pkts     TX bytes   TX err
─────────────────────────────────────────────────────────────────────
0     net_tap    UP 10G     892301      33.1 MB    0        0
                            1.2M        45.3 MB    0
─────────────────────────────────────────────────────────────────────

vaigai> stat port --rate          # live pps and Mbps per port
vaigai> stat port --rate --watch  # continuous refresh
```

### Backward Compatibility

The old `stats` command continues to work as an alias for `stat net`.

---

## Remote CLI Attach

Connect additional CLI terminals to a running vaigai process using Unix
domain sockets.

```bash
# From another terminal while vaigai is running:
$ vaigai --attach
Connected to vaigai via /var/run/vaigai/vaigai.sock
vaigai(remote)> stat cpu --rate
...
vaigai(remote)> stat mem
...
vaigai(remote)> disconnect
Disconnected.
```

The `--attach` flag is detected **before** DPDK EAL initialisation, so
no hugepages or NIC bindings are needed on the client side.

- Default socket path: `/var/run/vaigai/vaigai.sock`
- Fallback: `/tmp/vaigai.sock`
- Specify a path: `vaigai --attach /tmp/vaigai.sock`
- Max concurrent clients: 8
- `quit` / `exit` / `disconnect` closes the remote session (vaigai keeps running)

---

## trace

Packet capture using DPDK pcapng. Captures are streamed directly to disk
with no limit on the number of packets.

### trace start

Start capturing packets to a file.

```
trace start <file.pcapng> [port] [queue]
```

| Parameter      | Default | Description                    |
|----------------|---------|--------------------------------|
| `file.pcapng`  | —       | Output file path (required)    |
| `port`         | 0       | DPDK port ID to capture on     |
| `queue`        | 0       | Queue index to capture on      |

```
vaigai> trace start capture.pcapng
vaigai> trace start /tmp/debug.pcapng 0 0
```

### trace stop

Stop an active capture. Remaining packets are flushed and the file is closed.

```
vaigai> trace stop
```

---

## show interface

Display DPDK interface details: driver, MAC, IP, link status, offloads,
and packet statistics.

```
show interface [port_id]
```

Without `port_id`, shows all ports.

```
vaigai> show interface
vaigai> show interface 0
```

### Output fields

| Field       | Description                                    |
|-------------|------------------------------------------------|
| Driver      | DPDK PMD driver name                           |
| MAC         | Hardware MAC address                           |
| IP          | Configured source IPv4 address                 |
| Link        | UP/DOWN, speed (Mbps), duplex                  |
| NUMA socket | NUMA node the port is on                       |
| Mgmt TX Q   | Dedicated management TX queue index            |
| Offloads    | Hardware offload capabilities                  |
| Statistics  | RX/TX packet and byte counters, errors/missed  |

---

## quit / exit

Gracefully stop all workers and shut down vaigai.

```
vaigai> quit
```
