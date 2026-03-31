# Building and Running

## Prerequisites

You need:
- **g++** (GCC) version 5.0 or higher (for C++14 support)
- **Make** utility
- **Linux** (RHEL 8/9, Ubuntu, etc.) - uses `epoll` for high-performance I/O

**Note:** This program uses Linux-specific `epoll` API for efficient multi-connection handling. It will not compile on macOS or Windows.

Check your g++ version:
```bash
g++ --version
```

## Building

### Release Build (Optimized)

```bash
make
```

This creates:
- `msg_client` - The main client program
- `msg_test_server` - The test server

### Debug Build

```bash
make debug
```

This creates:
- `msg_client_debug` - Client with debug symbols, no optimization

### Clean Build Files

```bash
make clean
```

## Running

### Step 1: Start the Test Server(s)

For single connection testing:
```bash
./msg_test_server --port 8888 --msg-rate 1000 --msg-size 256
```

For multi-connection testing (two servers on different ports):
```bash
# Terminal 1
./msg_test_server --port 8888 --msg-rate 1000

# Terminal 2
./msg_test_server --port 8889 --msg-rate 1000
```

Server options:
- `--port`: Port to listen on (default: 8888)
- `--msg-rate`: Messages per second, 0 = unlimited (default: 1000)
- `--msg-size`: Body size in bytes (default: 256)
- `--msg-count`: Total messages to send, 0 = infinite (default: 0)

### Step 2: Run the Client

#### Single Connection (Backward Compatible)

```bash
./msg_client --host 127.0.0.1 --port 8888 --item default --workers 2
```

#### Multiple Connections

**Different servers, same item:**
```bash
./msg_client \
  --host server1 --port 8888 --item AAPL --client-id Client1 \
  --host server2 --port 8889 --item AAPL --client-id Client2 \
  --workers 4
```

**Same server, different items:**
```bash
./msg_client \
  --host 127.0.0.1 --port 8888 --item AAPL --client-id ClientA \
  --host 127.0.0.1 --port 8888 --item MSFT --client-id ClientB \
  --workers 4
```

**Multiple markets with different starting sequences:**
```bash
./msg_client \
  --host nyse.primary --port 8888 --item AAPL --seq 1000000 \
  --host nyse.backup  --port 8889 --item AAPL --seq 1000000 \
  --host nasdaq.feed  --port 8890 --item MSFT --seq 500000 \
  --workers 8
```

### Connection Options

| Option | Description | Per-Connection | Default |
|--------|-------------|----------------|---------|
| `--host` | Server hostname/IP | ✅ Yes | 127.0.0.1 |
| `--port` | Server port | ✅ Yes | 8888 |
| `--item` | Subscription item name | ✅ Yes | "default" |
| `--client-id` | Client identifier | ✅ Yes | "MsgClient" |
| `--seq` | Starting sequence number | ✅ Yes | 0 |
| `--workers` | Worker thread count | ❌ Global | 2 |
| `--raw-queue` | Raw queue size | ❌ Global | 16384 |
| `--dec-queue` | Decoded queue per worker | ❌ Global | 16384 |
| `--reconnect` | Reconnect interval (ms) | ❌ Global | 3000 |
| `--queue-timeout` | Queue push timeout (ms) | ❌ Global | 5 |
| `--stats-interval` | Stats print interval (s) | ❌ Global | 5 |
| `--log-dir` | Log directory | ❌ Global | ./log |

**How it works:** When you specify `--host`, `--port`, `--item`, `--client-id`, or `--seq`, you start a new connection specification. The next `--host` (or end of arguments) completes the previous connection and starts a new one.

### Configuration File (Recommended for Production)

For complex setups with many connections, use a JSON configuration file:

```bash
# View configuration file format
./msg_client --config-help

# Run with configuration file
./msg_client --config production.json

# Override specific settings from config file
./msg_client --config production.json --workers 8
```

**Example configuration file (`config.json`):**

```json
{
  "global": {
    "workers": 4,
    "raw_queue_size": 32768,
    "decoded_queue_size": 32768,
    "reconnect_interval_ms": 3000,
    "queue_push_timeout_ms": 5
  },
  "connections": [
    {
      "host": "nyse.primary",
      "port": 8888,
      "item": "AAPL",
      "client_id": "ClientA",
      "starting_seq": 0
    },
    {
      "host": "nyse.backup",
      "port": 8889,
      "item": "AAPL",
      "client_id": "ClientA",
      "starting_seq": 0
    },
    {
      "host": "nasdaq.feed",
      "port": 8890,
      "item": "MSFT",
      "client_id": "ClientB",
      "starting_seq": 0
    }
  ],
  "memory_pool": {
    "class_5": {
      "initial": 512,
      "max_free": 1024,
      "max_total": 8192
    }
  }
}
```

**Configuration Precedence** (highest to lowest):

1. Command-line arguments
2. Environment variables  
3. Configuration file
4. Hardcoded defaults

This means you can set defaults in the config file and override specific values via environment variables or CLI args.

### Using Environment Variables

**Global settings:**
```bash
export APP_TCP_CLIENT_WORKERS="4"
export APP_TCP_CLIENT_RAW_QUEUE="16384"
export APP_TCP_CLIENT_DEC_QUEUE="16384"
export APP_TCP_CLIENT_RECONNECT="3000"
export APP_TCP_CLIENT_QUEUE_TIMEOUT="5"
export APP_TCP_CLIENT_STATS_INTERVAL="5"
```

**Default connection settings (used when not specified on command line):**
```bash
export APP_TCP_CLIENT_HOST="192.168.1.100"
export APP_TCP_CLIENT_PORT="9999"
export APP_TCP_CLIENT_ITEM="mydata"
export APP_TCP_CLIENT_CLIENT_ID="MyClient"
export APP_TCP_CLIENT_SEQ="0"
```

**Logging settings:**
```bash
export APP_LOG_DIR="./log"
export APP_LOG_STDOUT="6"   # 0-7, default: 6 (INFO)
export APP_LOG_FILE="7"     # 0-7, default: 7 (DEBUG)
export APP_LOG_SYSLOG="5"   # 0-7, default: 5 (NOTICE)
```

**Server settings (for test server):**
```bash
export APP_TCP_SERVER_PORT="8888"
export APP_TCP_SERVER_MSG_SIZE="256"
export APP_TCP_SERVER_MSG_RATE="1000"
export APP_TCP_SERVER_MSG_COUNT="0"
export APP_TCP_SERVER_SNDBUF="2097152"   # Send buffer size in bytes (default: 2MB)
```

**TCP Socket Buffer settings:**
```bash
export APP_TCP_SO_RCVBUF="2097152"  # TCP receive buffer size (default: 2MB)
```

**Why 2MB?** For high-bandwidth networks, the Bandwidth-Delay Product (BDP) determines the optimal buffer size:
- **BDP = Bandwidth × Round-Trip Time**
- Example: 1 Gbps × 10ms RTT = 1.25MB
- Example: 10 Gbps × 1ms RTT = 1.25MB

If the buffer is smaller than BDP, TCP cannot fully utilize the bandwidth. Increase this for WAN/high-latency links; decrease for memory-constrained LAN environments.

**TCP Keepalive settings (Linux only):**
```bash
export TCP_KEEPIDLE="10"    # Seconds before starting keepalive probes
export TCP_KEEPINTVL="3"    # Seconds between keepalive probes
export TCP_KEEPCNT="3"      # Number of keepalive probes before giving up
```

**Protocol customization (testing/advanced):**
```bash
export APP_TCP_MAGIC_KEY="0xDEADBEEF"    # Override protocol magic key
export APP_TCP_RECV_BUFFER_SIZE="131072" # Override application receive buffer size (default: 64KB, min: 64KB)
```

### Per-Connection Reconnection Behavior

Each connection has **independent auto-reconnection** with exponential backoff:
- Initial delay: `--reconnect` value (default: 3000ms)
- Backoff multiplier: 2x each failure
- Maximum delay: 60 seconds
- Resets to initial delay on successful connection
- Each connection tracks its own retry count and delay

Example sequence for a single connection: 3s → 6s → 12s → 24s → 48s → 60s (cap) → 60s...

When Connection 0 drops, Connection 1 continues unaffected:
```
Time ──────────────────────────────────────────────►

Conn 0:  CONNECTED ──► DROP ──► retry 3s ──► retry 6s ──► CONNECTED
                          ▲
                          │ (independent)
Conn 1:  CONNECTED ───────┴──────────────────────────────► CONNECTED
```

### Per-Connection Sequence Number Resume

When reconnecting, each connection tells the server its **last received sequence number**, so the server can resume from where it left off:

```
Connection 0:
  1. Receives messages 1-1000
  2. Connection drops
  3. Reconnects automatically
  4. Sends: "I have up to seq 1000"
  5. Server resumes from seq 1001

Connection 1:
  1. Receives messages 1-500
  2. Connection drops
  3. Reconnects automatically
  4. Sends: "I have up to seq 500"
  5. Server resumes from seq 501
```

This is automatic and per-connection - no action needed.

### Log Levels

| Level | Value | Description |
|-------|-------|-------------|
| CRITICAL | 2 | System critical errors |
| ERROR | 3 | Error conditions |
| WARNING | 4 | Warning conditions |
| NOTICE | 5 | Normal but significant |
| INFO | 6 | Informational messages |
| DEBUG | 7 | Debug-level messages |

## Example Scenarios

### High-Throughput Single Connection

```bash
# Terminal 1: Server at max speed
./msg_test_server --port 8888 --msg-rate 0 --msg-size 1024

# Terminal 2: Client with many workers
./msg_client --host 127.0.0.1 --port 8888 --workers 8
```

### Multi-Market Data Aggregation

```bash
# Terminal 1: NYSE feed simulator
./msg_test_server --port 8888 --msg-rate 10000

# Terminal 2: NASDAQ feed simulator
./msg_test_server --port 8889 --msg-rate 10000

# Terminal 3: Client aggregating both feeds
./msg_client \
  --host 127.0.0.1 --port 8888 --item AAPL --client-id ClientA \
  --host 127.0.0.1 --port 8889 --item MSFT --client-id ClientB \
  --workers 8 --stats-interval 2
```

### Simulating Network Issues

```bash
# Start two servers
./msg_test_server --port 8888 --msg-rate 100 &
SERVER1=$!
./msg_test_server --port 8889 --msg-rate 100 &
SERVER2=$!

# Client with 1-second reconnect interval
./msg_client \
  --host 127.0.0.1 --port 8888 --item A --reconnect 1000 \
  --host 127.0.0.1 --port 8889 --item B --reconnect 1000

# Kill one server - watch the other continue
kill $SERVER1

# Restart the server - watch it reconnect automatically
./msg_test_server --port 8888 --msg-rate 100
```

### Testing Per-Connection Sequence Numbers

```bash
# Server
./msg_test_server --port 8888 --msg-rate 100

# Client starting from different sequences per connection
./msg_client \
  --host 127.0.0.1 --port 8888 --item A --seq 1000 \
  --host 127.0.0.1 --port 8888 --item B --seq 500
```

## Running in Background (Production Deployment)

When you run a program via SSH and disconnect, it receives **SIGHUP** (hangup signal) and terminates by default. Here are three ways to keep it running:

### Quick Comparison

| Tool | When to Use | Terminal Detached? | Survives Logout? |
|------|-------------|-------------------|------------------|
| `nohup` | Before starting | No | ✅ Yes |
| `disown` | After starting | No | ✅ Yes* |
| `setsid` | Before starting | ✅ Yes | ✅ Yes |

\* Only if program handles/ignores SIGHUP, or you used `nohup`

### Option 1: `nohup` (Simplest)

**Use when:** You want a simple way to run something before you logout.

```bash
# Start with nohup - ignores SIGHUP automatically
nohup ./msg_client \
    --host server1 --port 8888 --item A \
    --host server2 --port 8889 --item B \
    --workers 4 > client.log 2>&1 &

# Output goes to nohup.out by default (or your redirect)
# Safe to logout
```

**What it does:**
1. Sets SIGHUP handler to "ignore"
2. Redirects output to file (or `nohup.out`)
3. Process stays in your session but survives logout

### Option 2: `disown` (Forgot to use nohup)

**Use when:** You already started the process and need to logout.

```bash
# Oops, already started!
./msg_client --host server --workers 4 &
[1] 12345

# Remove from shell's job table
jobs          # See running jobs
disown %1     # Remove job 1 from table (or just 'disown')

# Now safe to logout
exit
```

**What it does:**
- Removes job from shell's tracking
- Shell won't send SIGHUP when you exit
- ⚠️ **Caveat:** If the program doesn't handle SIGHUP itself, it may still die

### Option 3: `setsid` (True Daemon)

**Use when:** You want a completely detached process (production deployment).

```bash
# Create new session - fully detached from terminal
setsid ./msg_client \
    --host primary.server --port 8888 --item marketdata --client-id Primary \
    --host backup.server --port 8889 --item marketdata --client-id Backup \
    --workers 16 \
    > /var/log/msg_client/stdout.log \
    2> /var/log/msg_client/stderr.log \
    < /dev/null &

# Process is now in its own session
# Not affected by ANY terminal signals
```

**What it does:**
1. Creates a new session (new Session ID)
2. New process group (becomes session leader)
3. No controlling terminal at all
4. Can't receive SIGHUP even if the kernel tried to send it

### Visual Difference

```
nohup:                           setsid:
┌──────────────┐                ┌──────────────┐
│ SSH Session  │                │ SSH Session  │──SIGHUP──▶X
│ Session 100  │                │ Session 100  │
│              │                │              │
│ ┌──────────┐ │                │ ┌──────────┐ │
│ │ bash     │ │                │ │ bash     │ │
│ │          │ │                │ │          │ │
│ │ ┌──────┐ │ │                │ │ │(done)│ │ │
│ │ │nohup │─┼─┤                │ │ └──────┘ │ │
│ │ │prog  │ │ │                │ └──────────┘ │
│ │ └──────┘ │ │                └──────────────┘
│ └──────────┘ │                         │
└──────────────┘                         │
      │                                  ▼
      │ SIGHUP sent...          ┌──────────────┐
      ▼ ignored!                │ NEW Session  │
   ┌────────┐                   │ Session 101  │◀── no SIGHUP!
   │program │                   │ ┌────────┐   │
   │survives│                   │ │program │   │
   └────────┘                   │ │survives│   │
                                 │ └────────┘   │
                                 └──────────────┘
```

### Using `screen` or `tmux` (Interactive Sessions)

**Use when:** You want to detach and reattach later interactively.

```bash
# Using screen
screen -S msgclient
./msg_client --host server1 --port 8888 --item A --host server2 --port 8889 --item B
# Press Ctrl+A, then D to detach
# Logout, come back later
screen -r msgclient  # Reattach

# Using tmux
tmux new -s msgclient
./msg_client --host server1 --port 8888 --item A --host server2 --port 8889 --item B
# Press Ctrl+B, then D to detach
tmux attach -t msgclient  # Reattach
```

**What they do:**
- Create a persistent virtual terminal
- You can detach and reattach from anywhere
- Great for debugging and monitoring

### Decision Guide

| Scenario | Recommended Tool |
|----------|------------------|
| Simple background task | `nohup ./cmd &` |
| Already started, need to leave | `disown` |
| Production service/daemon | `setsid ./cmd >log 2>&1 </dev/null &` |
| Need to monitor/reattach | `screen` or `tmux` |

### Full Production Example

```bash
#!/bin/bash
# start_production.sh

LOG_DIR="/var/log/msgclient"
mkdir -p "$LOG_DIR"

# Fully detached daemon with multiple market feeds
setsid ./msg_client \
    --host nyse.primary --port 8888 --item AAPL --client-id ClientA \
    --host nyse.backup --port 8889 --item AAPL --client-id ClientA \
    --host nasdaq.feed --port 8890 --item MSFT --client-id ClientB \
    --workers 16 \
    --raw-queue 65536 \
    --dec-queue 65536 \
    --reconnect 1000 \
    > "$LOG_DIR/stdout.log" \
    2> "$LOG_DIR/stderr.log" \
    < /dev/null &

echo "Client started, PID: $!"
```

## Understanding Output

### Client Statistics

```
[Stats] recv=5000(+1000) decoded=5000 proc=5000(+1000) dropped=0 bytes=1280000(2.05 Mbps) reconnects=0 parse_err=0 conns=2
[Conn 0] nyse.primary:8888 item='AAPL' recv=2500 bytes=640000 reconnects=0 (connected)
[Conn 1] nasdaq.feed:8890 item='MSFT' recv=2500 bytes=640000 reconnects=0 (connected)
```

- `recv`: Total messages received from all connections (+ since last report)
- `decoded`: Total messages successfully parsed
- `proc`: Total messages processed by workers
- `dropped`: Messages dropped due to full queues
- `bytes`: Total bytes received (+ throughput in Mbps)
- `reconnects`: Total reconnections across all connections
- `parse_err`: Protocol parse errors
- `conns`: Number of configured connections
- Per-connection stats show: endpoint, item, messages received, bytes, reconnects, connection status

### Server Statistics

```
[Server] sent=10000 rate=980 msgs/s
```

Shows how many messages sent and current rate.

## Troubleshooting

### "Connection refused"
- Server not running
- Wrong host/port
- Firewall blocking connection

### "Bind failed: Address already in use"
- Previous server process still running
- Wait a few seconds or use different port

### High CPU usage
- Normal under high load (spin-waiting in queues)
- Reduce `--msg-rate` or increase queue sizes
- Ensure you have enough workers for your message rate

### Messages being dropped
- Increase queue sizes: `--raw-queue 65584 --dec-queue 65584`
- Add more workers: `--workers 8`
- Check if handler is too slow (use profiling)
- Check per-connection stats to see if one connection is overwhelming others

### One connection not receiving data
- Check per-connection stats: `[Conn N]` lines in logs
- Verify server is sending to that item name
- Check network connectivity: `telnet host port`
- Check firewall rules

## Performance Tuning

### For Maximum Throughput

1. **Use release build** (`make`, not `make debug`)
2. **Disable logging** or set high log levels
3. **Increase queue sizes** to handle bursts: `--raw-queue 65536 --dec-queue 65536`
4. **Match workers to CPU cores** (but leave one for IO)
5. **Use larger message sizes** (less overhead per byte)
6. **Multiple connections**: Spread load across connections if single connection is saturated

### For Minimum Latency

1. **Keep queues small** (less buffering = lower latency)
2. **Use single worker** (no context switching) if processing is simple
3. **Set CPU affinity** (not implemented here, but common optimization)
4. **Disable Nagle's algorithm** (already done in code via `TCP_NODELAY`)
5. **Fewer connections**: Each connection adds slight overhead in poll()

### For Market Data (Tick-by-Tick)

```bash
# Recommended configuration for low-latency market data
./msg_client \
  --host feed1 --port 8888 --item AAPL \
  --host feed2 --port 8889 --item AAPL \
  --workers 4 \
  --raw-queue 16384 \
  --dec-queue 16384 \
  --reconnect 1000 \
  --queue-timeout 0  # Wait forever (don't drop market data)
```

## Code Analysis

### Basic Static Analysis
```bash
make check
```
This runs cppcheck if installed.

### Advanced Analysis (Optional Tools)

For deeper analysis, use the separate `Makefile.analysis` (requires additional tools):

```bash
# Install tools (Ubuntu)
sudo apt install clang clang-tidy valgrind

# Build with AddressSanitizer (memory leak/overflow detection)
make -f Makefile.analysis asan
./msg_client_asan --host 127.0.0.1 --port 8888

# Build with ThreadSanitizer (race condition detection for lock-free code)
make -f Makefile.analysis tsan
./msg_client_tsan --host 127.0.0.1 --port 8888

# Run under Valgrind (detailed memory leak analysis)
make -f Makefile.analysis valgrind

# Run clang-tidy (modern C++ static analysis)
make -f Makefile.analysis tidy
```

### When to Use Each Tool

| Tool | Use For | Impact |
|------|---------|--------|
| **AddressSanitizer** | Memory leaks, buffer overflows | Fast runtime overhead |
| **ThreadSanitizer** | Data races in lock-free queue | 5-15x slowdown |
| **Valgrind** | Detailed memory analysis | 10-50x slowdown |
| **clang-tidy** | Code style, modern C++ checks | Compile-time only |

**Note:** Analysis tools are kept in a separate Makefile so the main build works on all systems.

## Next Steps

To customize the client for your needs:

1. Edit `main.cpp` to set your own message handler:
```cpp
client.setMessageHandler([](const SubMessage &msg, size_t worker_index, size_t connection_id) {
    // Your processing code here
    // msg.body points to the data
    // msg.body_length tells you how much data
    // msg.seq_num is the sequence number
    // msg.connection_id tells you which connection (0, 1, 2, ...)
    
    // Example: Route by connection
    switch (connection_id) {
        case 0: process_nyse(msg); break;
        case 1: process_nasdaq(msg); break;
    }
});
```

2. Rebuild: `make`

3. Run against your own servers (that implement the same protocol)
