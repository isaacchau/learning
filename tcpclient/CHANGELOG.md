# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- Comprehensive file-header comments to all core headers (`msg_client.h`, `protocol.h`, `lockfree_ringbuffer.h`, `shared_ptr_pool.h`, `log_msg.h`, `config_parser.h`).
- Inline documentation in `msg_client.cpp` explaining:
  - IO thread message-framing logic and why trailing bytes are copied.
  - Decoder thread zero-copy design and round-robin load balancing.
  - Worker thread aggregation pipeline and handler invocation contract.
  - TCP socket options (keepalive, Nagle, send timeout) and their rationale.
- `CHANGELOG.md` to track project evolution.

### Changed
- `doc/README.md`: removed duplicate `08_Memory_Tuning.md` entry in the index table.

### Fixed
- Minor trailing-whitespace cleanup in `msg_client.h`.

## [1.0.0] – 2025-04-17

### Added
- Initial release of the multi-connection TCP message streaming client.
- Three-stage lock-free pipeline (IO → Decoder → Workers).
- Single-producer-single-consumer ring buffers (`LockFreeRingBuffer`).
- Size-class memory pool (`MemoryPool`) with custom `shared_ptr` deleters.
- RAII socket wrapper (`SocketGuard`) with atomic fd storage.
- Epoll-based I/O multiplexing supporting up to 64 simultaneous connections.
- Endpoint failover with exponential backoff and per-connection retry limits.
- Sequence-number tracking for resume-on-reconnect.
- Message dropping (rather than TCP backpressure) to protect the server.
- JSON configuration file support (`config_parser.h/cpp`).
- Custom minimal unit-test framework (`tests/test_main.cpp`).
- Static analysis Makefile (`Makefile.analysis`) with ASan, TSan, UBSan, clang-tidy, and Valgrind targets.
- Metrics aggregation pipeline (orders, trades, quotes) with CSV and InfluxDB Line output.

### Security
- Buffer zeroing on return to pool prevents sensitive data leakage between allocations.
- `SIGPIPE` ignored globally to prevent crashes on broken TCP connections.
