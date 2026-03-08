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
| `--duration` | Test duration in seconds (> 0)     |

### Optional flags

| Flag          | Default | Description                                   |
|---------------|---------|-----------------------------------------------|
| `--proto`     | `tcp`   | Protocol: `tcp`, `http`, `https`, `udp`, `icmp`, `tls` |
| `--rate`      | 0       | Rate limit in packets/sec (0 = unlimited)     |
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

## stats

Print a JSON telemetry snapshot of all counters (TX/RX packets, TCP, HTTP,
TLS, latency percentiles).

```
vaigai> stats
```

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
