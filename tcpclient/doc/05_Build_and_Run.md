# Building and Running

## Prerequisites

You need:
- **g++** (GCC) version 5.0 or higher (for C++14 support)
- **Make** utility
- Linux environment (uses `poll`, `socket`, `epoll`-style APIs)

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

### Step 1: Start the Test Server

In one terminal:
```bash
./msg_test_server --port 8888 --msg-rate 1000 --msg-size 256
```

Options:
- `--port`: Port to listen on (default: 8888)
- `--msg-rate`: Messages per second, 0 = unlimited (default: 1000)
- `--msg-size`: Body size in bytes (default: 256)
- `--msg-count`: Total messages to send, 0 = infinite (default: 0)

### Step 2: Run the Client

In another terminal:
```bash
./msg_client --host 127.0.0.1 --port 8888 --item default --workers 2
```

Options:
- `--host`: Server address (default: 127.0.0.1)
- `--port`: Server port (default: 8888)
- `--item`: Item to subscribe to (default: "default")
- `--seq`: Starting sequence number (default: 0)
- `--workers`: Number of worker threads (default: 2, range: 1-64)
- `--raw-queue`: Size of raw queue (default: 8192, range: 64-1048576)
- `--dec-queue`: Size of decoded queue per worker (default: 8192, range: 64-1048576)
- `--reconnect`: Reconnect interval in ms (default: 3000, range: 100-300000)
- `--queue-timeout`: Queue push timeout in ms (default: 5, -1=no wait, 0=wait forever)
- `--stats-interval`: Statistics print interval in seconds (default: 5)
- `--log-dir`: Directory for log files (default: ./log)
- `--log-stdout`: STDOUT log level (default: 6/INFO, 0-7)
- `--log-file`: File log level (default: 7/DEBUG, 0-7)
- `--log-syslog`: Syslog log level (default: 5/NOTICE, 0-7)

### Using Environment Variables

All options can also be set via environment variables:

**Client connection settings:**
```bash
export APP_TCP_CLIENT_HOST="192.168.1.100"
export APP_TCP_CLIENT_PORT="9999"
export APP_TCP_CLIENT_ITEM="mydata"
export APP_TCP_CLIENT_SEQ="0"
export APP_TCP_CLIENT_WORKERS="4"
export APP_TCP_CLIENT_RAW_QUEUE="8192"
export APP_TCP_CLIENT_DEC_QUEUE="8192"
export APP_TCP_CLIENT_RECONNECT="3000"
export APP_TCP_CLIENT_QUEUE_TIMEOUT="5"
export APP_TCP_CLIENT_STATS_INTERVAL="5"
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
```

**TCP Keepalive settings (Linux only):**
```bash
export TCP_KEEPIDLE="10"    # Seconds before starting keepalive probes
export TCP_KEEPINTVL="3"    # Seconds between keepalive probes
export TCP_KEEPCNT="3"      # Number of keepalive probes before giving up
```

**Protocol customization (testing/advanced):**
```bash
export APP_TCP_MAGIC_KEY="0xDEADBEEF"    # Override protocol magic key
export APP_TCP_RECV_BUFFER_SIZE="131072" # Override receive buffer size (bytes)
```

### Reconnection Behavior

The client uses **exponential backoff** for reconnection attempts:
- Initial delay: `--reconnect` value (default: 3000ms)
- Backoff multiplier: 2x each failure
- Maximum delay: 60 seconds
- Resets to initial delay on successful connection

Example sequence: 3s → 6s → 12s → 24s → 48s → 60s (cap) → 60s...

### Sequence Number Resume

When reconnecting, the client tells the server the **last received sequence number**, so the server can resume from where it left off instead of resending everything:

```
1. Client receives messages 1-1000
2. Connection drops
3. Client reconnects automatically
4. Client sends: "I have up to seq 1000"
5. Server resumes from seq 1001 (no duplicates!)
```

This is automatic - no action needed. The client tracks the highest sequence number received and uses it for all reconnection attempts.

Environment variables take precedence over defaults but command-line arguments override environment variables.

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

### High-Throughput Test

```bash
# Terminal 1: Server at max speed
./msg_test_server --port 8888 --msg-rate 0 --msg-size 1024

# Terminal 2: Client with many workers
./msg_client --host 127.0.0.1 --port 8888 --workers 8
```

### Simulating Network Issues

```bash
# Start server, send 10000 messages at 100/sec
./msg_test_server --port 8888 --msg-rate 100 --msg-count 10000

# Client with 1-second reconnect interval
./msg_client --host 127.0.0.1 --port 8888 --reconnect 1000

# Then kill and restart the server to test reconnection
```

### Testing Sequence Numbers

```bash
# Server
./msg_test_server --port 8888 --msg-rate 100

# Client starting from sequence 500 (server should continue from there)
./msg_client --host 127.0.0.1 --port 8888 --seq 500
```

## Understanding Output

### Client Statistics

```
[Stats] recv=5000(+1000) decoded=5000 proc=5000(+
1000) dropped=0 bytes=1280000(2.05 Mbps) reconnects=0 parse_err=0
```

- `recv`: Total messages received (+ since last report)
- `decoded`: Total messages successfully parsed
- `proc`: Total messages processed by workers
- `dropped`: Messages dropped due to full queues
- `bytes`: Total bytes received (+ throughput in Mbps)
- `reconnects`: Number of reconnections
- `parse_err`: Protocol parse errors

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

### Messages being dropped
- Increase queue sizes: `--raw-queue 16384 --dec-queue 16384`
- Add more workers: `--workers 4`
- Check if handler is too slow

## Performance Tuning

### For Maximum Throughput

1. **Use release build** (`make`, not `make debug`)
2. **Disable logging** or set high log levels
3. **Increase queue sizes** to handle bursts
4. **Match workers to CPU cores** (but leave one for IO)
5. **Use larger message sizes** (less overhead per byte)

### For Minimum Latency

1. **Keep queues small** (less buffering = lower latency)
2. **Use single worker** (no context switching)
3. **Set CPU affinity** (not implemented here, but common optimization)
4. **Disable Nagle's algorithm** (already done in code via `TCP_NODELAY`)

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
client.setMessageHandler([](const SubMessage &msg, size_t worker_index) {
    // Your processing code here
    // msg.body points to the data
    // msg.body_length tells you how much data
    // msg.seq_num is the sequence number
});
```

2. Rebuild: `make`

3. Run against your own server (that implements the same protocol)
