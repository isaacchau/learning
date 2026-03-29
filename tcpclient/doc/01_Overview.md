# TCP Message Streaming Client - Overview

## What This Program Does

This is a **high-performance TCP client** that connects to a server and receives a continuous stream of messages. Think of it like a specialized web client that:

1. Connects to a server
2. Subscribes to a named "item" (like joining a chat room or following a hashtag)
3. Receives messages in real-time
4. Processes them using multiple worker threads

## Real-World Analogy

Imagine a **stock ticker** or **live sports score feed**:
- You connect to a data provider
- You subscribe to specific stocks or games (the "item")
- You receive updates as they happen
- You process them quickly to display to users

## Key Features

| Feature | Purpose |
|---------|---------|
| **Lock-free queues** | Pass data between threads without slowing down |
| **Memory pool** | Reuse memory instead of constantly allocating/freeing |
| **Multiple worker threads** | Process messages in parallel |
| **Automatic reconnection** | If connection drops, reconnect automatically |
| **Sequence numbers** | Detect if any messages were missed |

## Two Programs Included

### 1. `msg_client` (The Main Program)
The client that connects to a server and receives messages.

```bash
./msg_client --host 127.0.0.1 --port 8888 --item default
```

### 2. `msg_test_server` (For Testing)
A simple server that generates fake messages for testing the client.

```bash
./msg_test_server --port 8888 --msg-rate 1000
```

## Basic Flow

```
┌─────────────────┐     TCP Socket      ┌─────────────────┐
│   Test Server   │ ◄─────────────────► │    msg_client   │
│  (sends data)   │                     │ (receives data) │
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

1. **IO Thread**: Receives raw bytes from network
2. **Decoder Thread**: Parses bytes into structured messages
3. **Worker Threads**: Process the messages (you define what happens here)

## Why Is This Code Complex?

This is **high-performance systems programming**. Unlike a simple script, it:
- Handles millions of messages per second
- Uses zero-copy techniques (shares data instead of copying)
- Avoids locks that would slow down processing
- Manages memory carefully to avoid garbage collection pauses

If you're coming from languages like Python, JavaScript, or older C++, some syntax will look unfamiliar. The next documents explain the modern C++ features used.
