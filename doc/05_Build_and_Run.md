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
- `--workers`: Number of worker threads (default: 2)
- `--raw-queue`: Size of raw queue (default: 8192)
- `--dec-queue`: Size of decoded queue per worker (default: 8192)
- `--reconnect`: Reconnect interval in ms (default: 3000)
- `--stats-interval`: Statistics print interval in seconds (default: 5)
- `--log-dir`: Directory for log files (default: ./log)

### Using Environment Variables

All options can also be set via environment variables:

```bash
export APP_TCP_CLIENT_HOST="192.168.1.100"
export APP_TCP_CLIENT_PORT="9999"
export APP_TCP_CLIENT_ITEM="mydata"
export APP_TCP_CLIENT_WORKERS="4"

./msg_client  # Uses environment settings
```

Environment variables take precedence over defaults but command-line arguments override environment variables.

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
1000) bytes=1280000(2.05 Mbps) reconnects=0 parse_err=0 q_full=0
```

- `recv`: Total messages received (+ since last report)
- `decoded`: Total messages successfully parsed
- `proc`: Total messages processed by workers
- `bytes`: Total bytes received (+ throughput)
- `reconnects`: Number of reconnections
- `parse_err`: Protocol parse errors
- `q_full`: Times queue was full (message dropped)

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

To check for issues:
```bash
make check
```

This runs cppcheck (static analyzer) if installed.

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
