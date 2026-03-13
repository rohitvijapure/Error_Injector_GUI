# Error Injector v2.0

A Linux command-line tool with an ncurses TUI for injecting errors into live network streams (SRT, UDP unicast, UDP multicast, RTP). Supports **two layers of impairment**: application-level packet manipulation (Layer 1) and kernel-level network emulation via `tc netem` (Layer 2).

## Features

- **Input protocols**: SRT (listener/caller), UDP unicast, UDP multicast (IGMP join), RTP
- **Output protocols**: SRT (caller), UDP unicast, UDP multicast
- **Layer 1 — Application-level error injection**:
  - Periodic delay injection (1–15 seconds, configurable cycle period and burst window)
  - Packet dropping (1–500 packets/sec count mode, or 1–100% percentage mode) — creates MPEG-TS CC errors
  - Packet corruption (1–95% of bytes per packet)
  - MPEG-TS PID nullification (up to 16 PIDs, replaces with null packets to preserve alignment)
- **Layer 2 — Network-level impairment (tc netem)**:
  - Packet loss (0–100%)
  - Delay with optional jitter
  - Packet reordering (0–100%)
  - Packet duplication (0–100%)
  - Bit-level corruption (0–100%)
  - Explicit `netem apply` workflow — stage settings, then apply
  - Auto-clear on exit
- **IP/port filtering**: Apply Layer 1 injection only to traffic matching source or destination IP and port
- **ncurses TUI**: Two-panel display with live stats, action log, and interactive command input
- **Interactive CLI**: Adjust all parameters on-the-fly without restarting

## TUI Layout

```
=== ERROR INJECTOR v2.0 ===  Uptime 01:23:45
Input:  Multicast  239.1.1.212:5724 (eno2)  In:  48.23 Mbps
Output: Multicast  239.200.200.200:6000 (eno2)  Out: 47.01 Mbps
────────────────────────────────────────────────────────────────
 LAYER 1 - Application       │ LAYER 2 - Network (tc netem)
 Pkts In: 1234567  Out: 1200 │ Interface: eno2
 Dropped: 34567  Corrupted: 8│ Loss:      5.0%
 Delayed: 1200   PID-null: 44│ Delay:     100ms (+/- 20ms)
                              │ Reorder:   2.0%
 Delay:   5s every 30s       │ Duplicate: 1.0%
 Drop:    50 pkt/s           │ Corrupt:   0.5%
 Corrupt: 10%                │
 PID-drop: 256,512           │ [ACTIVE]
 Filter:  NONE               │
────────────────────────────────────────────────────────────────
 [14:23:01] delay set to 5s
 [14:23:15] netem applied on eno2
 [14:23:30] drop count set to 50 pkt/s
────────────────────────────────────────────────────────────────
> netem loss 5 _
```

## Build

### Prerequisites

```bash
# Ubuntu / Debian
sudo apt update
sudo apt install build-essential pkg-config libsrt-openssl-dev libssl-dev libncurses-dev

# RHEL / CentOS / Fedora
sudo dnf install gcc make pkgconfig srt-devel ncurses-devel
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
sudo ./error-injector \
  --input-type udp \
  --input-port 5000 \
  --output-type udp \
  --output-addr 10.0.0.2 \
  --output-port 6000 \
  --drop-count 50
```

**Multicast input → unicast output with PID dropping:**

```bash
sudo ./error-injector \
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
sudo ./error-injector \
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
sudo ./error-injector \
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

Once running, type commands in the input bar at the bottom of the TUI.

#### Layer 1 — Application Injection

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

#### Layer 2 — Network Impairment (tc netem)

| Command | Description |
|---|---|
| `netem loss <0-100>` | Set packet loss percentage |
| `netem delay <ms> [jitter_ms]` | Set base delay and optional jitter |
| `netem reorder <0-100>` | Set reorder percentage |
| `netem duplicate <0-100>` | Set duplication percentage |
| `netem corrupt <0-100>` | Set bit corruption percentage |
| `netem iface <name>` | Set target interface |
| `netem apply` | Apply staged netem settings to interface |
| `netem clear` | Remove all netem rules from interface |
| `netem status` | Query and display current tc state |

**Workflow**: `netem` parameter commands (loss, delay, etc.) stage values in memory. Run `netem apply` to execute the tc command. This prevents accidental partial configurations. On exit, the tool automatically clears any netem rules it applied.

#### General

| Command | Description |
|---|---|
| `status` | Show current settings (both layers) |
| `stop` | Disable all Layer 1 injection |
| `reset` | Reset statistics counters |
| `help` | List available commands |
| `quit` / `exit` | Graceful shutdown |

## Architecture

```
[Input Stream] ──→ [Receiver Thread] ──→ [Ring Buffer] ──→ [Injector Thread] ──→ [Output Stream]
                                                                  │                     │
                                                           ┌──────┴──────┐       ┌──────┴──────┐
                                                           │  LAYER 1    │       │  LAYER 2    │
                                                           │  1. Drop    │       │  tc netem   │
                                                           │  2. Corrupt │       │  (kernel)   │
                                                           │  3. PID Null│       └─────────────┘
                                                           │  4. Delay   │
                                                           └─────────────┘
```

- **Receiver thread** — reads packets from the input socket or SRT connection and enqueues them into a lock-free ring buffer.
- **Injector thread** — dequeues packets, applies the Layer 1 error injection pipeline (drop → corrupt → PID nullify → delay), and forwards via the sender. Delayed packets are held in a timestamped queue and released when due.
- **Main thread** — runs the ncurses TUI: refreshes the display every 500 ms and processes interactive commands. Replaces the old console thread and fgets-based input loop.
- **Layer 2 (tc netem)** — managed by the `netem` module, which executes `tc qdisc` commands via `system()`/`popen()`. Applied at the kernel level on the output interface, affecting all outgoing traffic on that interface.

### Thread Safety

- The injection, filter, and netem configuration is protected by a `pthread_rwlock_t`. The injector thread acquires a read lock per packet; interactive commands acquire a write lock.
- Statistics counters use C11 `stdatomic` for lock-free updates from any thread.
- The ring buffer between receiver and injector uses a mutex + condition variable.
- The TUI log ring buffer uses its own mutex for thread-safe logging from any thread.

## Requirements

- Linux (kernel 3.x+)
- GCC with C11 support
- libsrt >= 1.4
- libncurses
- pthreads (glibc)
- Root privileges (required for `SO_BINDTODEVICE`, `tc netem`, and multicast TTL)
