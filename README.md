# Error Injector

A Linux command-line tool for injecting errors into live network streams (SRT, UDP unicast, UDP multicast, RTP). Designed for testing the resilience of media processing pipelines, monitoring systems, and downstream receivers.

## Features

- **Input protocols**: SRT (listener/caller), UDP unicast, UDP multicast (IGMP join), RTP
- **Output protocols**: SRT (caller), UDP unicast, UDP multicast
- **Error injection**:
  - Periodic delay injection (1–15 seconds, configurable cycle period and burst window)
  - Packet dropping (1–500 packets/sec count mode, or 1–100% percentage mode) — creates MPEG-TS CC errors
  - Packet corruption (1–95% of bytes per packet)
  - MPEG-TS PID nullification (up to 16 PIDs, replaces with null packets to preserve alignment)
- **IP/port filtering**: Apply injection only to traffic matching source or destination IP and port
- **Live console display**: Real-time bitrate, packet counts, injection stats, refreshed every 500 ms
- **Interactive CLI**: Adjust all parameters on-the-fly without restarting

## Build

### Prerequisites

```bash
# Ubuntu / Debian
sudo apt update
sudo apt install build-essential pkg-config libsrt-dev

# RHEL / CentOS / Fedora
sudo dnf install gcc make pkgconfig srt-devel
```

### Compile

```bash
make
```

For a debug build with symbols:

```bash
make debug
```

### Install (optional)

```bash
sudo make install
```

This copies the `error-injector` binary to `/usr/local/bin/`.

## Usage

### Basic Examples

**UDP relay with packet dropping (50 packets/sec):**

```bash
./error-injector \
  --input-type udp \
  --input-port 5000 \
  --output-type udp \
  --output-addr 10.0.0.2 \
  --output-port 6000 \
  --drop-count 50
```

**Multicast input → unicast output with PID dropping:**

```bash
./error-injector \
  --input-type multicast \
  --input-addr 239.1.1.1 \
  --input-port 5000 \
  --input-iface eth0 \
  --output-type udp \
  --output-addr 10.0.0.2 \
  --output-port 6000 \
  --drop-pid 256 \
  --drop-pid 512
```

**SRT input → multicast output with corruption:**

```bash
./error-injector \
  --input-type srt \
  --input-port 9000 \
  --srt-mode listener \
  --srt-latency 200 \
  --output-type multicast \
  --output-addr 239.2.2.2 \
  --output-port 5000 \
  --output-iface eth1 \
  --output-ttl 10 \
  --corrupt 10
```

**RTP input with source filter and periodic delay:**

```bash
./error-injector \
  --input-type rtp \
  --input-port 5004 \
  --input-iface eth0 \
  --output-type udp \
  --output-addr 10.0.0.3 \
  --output-port 5004 \
  --filter-src-ip 10.0.0.1 \
  --delay 5 \
  --delay-period 30 \
  --delay-burst 10
```

### CLI Options

```
Input:
  --input-type <udp|multicast|rtp|srt>  Stream type (default: udp)
  --input-addr <addr>                    Multicast group or SRT target address
  --input-port <port>                    Port to bind/listen
  --input-iface <iface>                  Network interface (e.g. eth0)
  --srt-mode <listener|caller>           SRT mode (default: listener)
  --srt-latency <ms>                     SRT latency (default: 120)

Output:
  --output-type <udp|multicast|srt>      Stream type (default: udp)
  --output-addr <addr>                   Destination address
  --output-port <port>                   Destination port
  --output-iface <iface>                 Output interface
  --output-ttl <ttl>                     Multicast TTL (default: 5)

Filter:
  --filter-src-ip <ip>                   Source IP filter
  --filter-src-port <port>               Source port filter
  --filter-dst-ip <ip>                   Destination IP filter
  --filter-dst-port <port>               Destination port filter

Error injection (initial values, adjustable live):
  --delay <1-15>                         Delay in seconds
  --delay-period <seconds>               Cycle length (default: 30)
  --delay-burst <seconds>                Burst window (default: 10)
  --drop-count <1-500>                   Packets to drop per second
  --drop-percent <1-100>                 Percentage of packets to drop
  --corrupt <1-95>                       Corruption % (bytes per packet)
  --drop-pid <pid>                       MPEG-TS PID to nullify (repeatable)
```

### Interactive Commands

Once running, type commands at the `>` prompt to adjust parameters live:

| Command | Description |
|---|---|
| `delay <1-15> [period <s>] [burst <s>]` | Set delay injection |
| `delay off` | Disable delay |
| `drop count <1-500>` | Drop N packets/second |
| `drop percent <1-100>` | Drop N% of packets |
| `drop off` | Disable packet drop |
| `corrupt <1-95>` | Corrupt N% of bytes per packet |
| `corrupt off` | Disable corruption |
| `droppid <pid>` | Add MPEG-TS PID to drop list |
| `droppid remove <pid>` | Remove PID from list |
| `droppid off` | Disable PID drop |
| `filter src <ip>[:<port>]` | Set source IP/port filter |
| `filter dst <ip>[:<port>]` | Set destination IP/port filter |
| `filter clear` | Clear all filters |
| `status` | Show current settings |
| `stop` | Disable all error injection |
| `reset` | Reset statistics counters |
| `help` | List available commands |
| `quit` | Graceful shutdown |

## Architecture

```
[Input Stream] ──→ [Receiver Thread] ──→ [Ring Buffer] ──→ [Injector Thread] ──→ [Output Stream]
                                                                  │
                                                           ┌──────┴──────┐
                                                           │  1. Drop    │
                                                           │  2. Corrupt │
                                                           │  3. PID Null│
                                                           │  4. Delay   │
                                                           └─────────────┘
```

- **Receiver thread** — reads packets from the input socket or SRT connection and enqueues them into a lock-free ring buffer.
- **Injector thread** — dequeues packets, applies the error injection pipeline (drop → corrupt → PID nullify → delay), and forwards via the sender. Delayed packets are held in a timestamped queue and released when due.
- **Console thread** — refreshes the stats display at the top of the terminal every 500 ms using ANSI escape codes.
- **Main thread** — runs the interactive command parser on stdin.

### Thread Safety

- The injection and filter configuration is protected by a `pthread_rwlock_t`. The injector thread acquires a read lock per packet; interactive commands acquire a write lock.
- Statistics counters use C11 `stdatomic` for lock-free updates from any thread.
- The ring buffer between receiver and injector uses a mutex + condition variable.

## Requirements

- Linux (kernel 3.x+)
- GCC with C11 support
- libsrt >= 1.4
- pthreads (glibc)
