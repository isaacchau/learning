# TCP Message Streaming Client - Documentation

Welcome! This documentation is designed for developers who haven't used C++ in a while (or are new to modern C++).

## Quick Start

1. **Read [01_Overview.md](01_Overview.md)** - Understand what this program does
2. **Read [02_Modern_CPP_Guide.md](02_Modern_CPP_Guide.md)** - Learn the "alien" C++ syntax
3. **Read [05_Build_and_Run.md](05_Build_and_Run.md)** - Build and try it out

## Documentation Index

| Document | What You'll Learn |
|----------|-------------------|
| [01_Overview.md](01_Overview.md) | What the program does, key features, basic flow |
| [02_Modern_CPP_Guide.md](02_Modern_CPP_Guide.md) | Modern C++ features (auto, lambdas, smart pointers, move semantics, atomics) |
| [03_Architecture.md](03_Architecture.md) | Three-stage pipeline, lock-free queues, memory pool, zero-copy |
| [04_Protocol.md](04_Protocol.md) | Binary protocol format, sequence numbers, wire format |
| [05_Build_and_Run.md](05_Build_and_Run.md) | Build instructions, running, troubleshooting, performance tuning, config files |
| [06_Glossary.md](06_Glossary.md) | Terms and definitions |
| [07_Analysis_Tools.md](07_Analysis_Tools.md) | Static analysis, sanitizers, debugging tools |
| [08_Memory_Tuning.md](08_Memory_Tuning.md) | Memory usage estimation, tuning, and optimization |

## Source File Guide

| File | Responsibility |
|------|----------------|
| `msg_client.h` / `msg_client.cpp` | Core client class, connection management, three-stage thread lifecycle, statistics |
| `main.cpp` | CLI parsing, environment-variable overrides, JSON config loading, signal handling, stats printing loop |
| `msg_test_server.cpp` | Interactive test server with keyboard rate controls (`u`/`d`/`o`/`q`) |
| `config_parser.h` / `config_parser.cpp` | JSON configuration file parsing using `json.hpp` |
| `protocol.h` | Packed wire-format structs (`TcpRequest`, `TcpResponse`, `MsgHdr`) and internal pipeline types (`RawMessage`, `SubMessage`) |
| `lockfree_ringbuffer.h` | Templated SPSC lock-free ring buffer with progressive spin-backoff |
| `shared_ptr_pool.h` | Size-class memory pool (`MemoryPool`) and `Buffer` with custom `shared_ptr` deleters |
| `log_msg.h` / `log_msg.cpp` | Singleton logger with syslog-compatible levels and convenience macros (`LOG_INFO`, `LOG_ERR`, etc.) |
| `metrics.hpp` / `metrics_demo.cpp` | Time-bucketed metrics aggregation library with CSV and InfluxDB Line output |
| `market_data/message_types.h` | Market data message structs (orders, trades, quotes) for the aggregation pipeline |
| `tests/test_main.cpp` | Custom minimal unit-test framework and test cases |
| `json.hpp` | Vendored single-header JSON library (nlohmann/json) |

## For the Impatient

```bash
# Build
make

# Terminal 1 - Start server
./msg_test_server --port 8888 --msg-rate 1000

# Terminal 2 - Run client
./msg_client --host 127.0.0.1 --port 8888 --item default
```

## Key Concepts at a Glance

**What is this?**
A high-performance TCP client that receives message streams from a server.

**Why so complex?**
It's optimized for millions of messages per second using:
- Lock-free queues (no waiting)
- Memory pools (no allocation overhead)
- Multiple worker threads (parallel processing)
- Zero-copy (share data, don't copy it)

**What C++ version?**
C++14. Uses modern features like auto, lambdas, smart pointers, move semantics.

## Questions?

If something is unclear, check the glossary or look at the code comments. The code is extensively commented to help with understanding.
