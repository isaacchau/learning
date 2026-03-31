# TCP Message Streaming Client - Overview

## What This Program Does

This is a **high-performance TCP client** that connects to one or more servers and receives continuous streams of messages. Think of it like a specialized web client that:

1. Connects to multiple servers (up to 64)
2. Subscribes to named "items" on each connection (like joining multiple chat rooms)
3. Receives messages in real-time from all connections
4. Processes them using multiple worker threads

## Real-World Analogy

Imagine a **multi-market stock ticker** or **live sports score feed aggregator**:
- You connect to multiple data providers (NYSE, NASDAQ, etc.)
- You subscribe to specific stocks or games on each provider
- You receive updates from all sources as they happen
- You process them quickly to display to users

## Key Features

| Feature | Purpose |
|---------|---------|
| **Multi-connection support** | Connect to up to 64 different servers/markets simultaneously |
| **Lock-free queues** | Pass data between threads without slowing down |
| **Memory pool** | Reuse memory instead of constantly allocating/freeing |
| **Multiple worker threads** | Process messages in parallel |
| **Per-connection auto-reconnection** | If any connection drops, reconnect automatically with independent backoff |
| **Sequence numbers** | Detect if any messages were missed, resume per-connection |

## Two Programs Included

### 1. `msg_client` (The Main Program)
The client that connects to servers and receives messages.

```bash
# Single connection
./msg_client --host 127.0.0.1 --port 8888 --item default

# Multiple connections to different markets
./msg_client \
  --host nyse.server --port 8888 --item AAPL --client-id ClientA \
  --host nasdaq.server --port 8889 --item MSFT --client-id ClientB
```

### 2. `msg_test_server` (For Testing)
A simple server that generates fake messages for testing the client.

```bash
./msg_test_server --port 8888 --msg-rate 1000
```

## Basic Flow

```
┌─────────────────┐     TCP Socket 1    ┌─────────────────┐
│   Test Server 1 │ ◄──────────────────►│                 │
│  (sends data A) │                     │                 │
└─────────────────┘                     │                 │
                                        │    msg_client   │
┌─────────────────┐     TCP Socket 2    │   (receives     │
│   Test Server 2 │ ◄──────────────────►│    from all)    │
│  (sends data B) │                     │                 │
└─────────────────┘                     └────────┬────────┘
                                                  │
                       ┌──────────────────────────┼──────────┐
                       │                          │          │
                       ▼                          ▼          ▼
                 ┌─────────┐              ┌─────────────┐ ┌─────────────┐
                 │  Worker │              │   Worker    │ │   Worker    │
                 │    #1   │              │     #2      │ │     #3      │
                 └─────────┘              └─────────────┘ └─────────────┘
```

1. **IO Thread**: Manages all TCP sockets using poll(), receives raw bytes from all connections
2. **Decoder Thread**: Parses bytes from all connections into structured messages
3. **Worker Threads**: Process messages from all connections (handler receives connection_id)

## Why Is This Code Complex?

This is **high-performance systems programming**. Unlike a simple script, it:
- Handles millions of messages per second across multiple connections
- Uses zero-copy techniques (shares data instead of copying)
- Avoids locks that would slow down processing
- Manages memory carefully to avoid garbage collection pauses
- Supports independent reconnection for each connection

If you're coming from languages like Python, JavaScript, or older C++, some syntax will look unfamiliar. The next documents explain the modern C++ features used.
