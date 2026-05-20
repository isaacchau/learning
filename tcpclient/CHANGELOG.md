# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- Enhanced inline documentation in `msg_client.h`:
  - Design rationale for the three-stage pipeline (why separate I/O, decoder, workers).
  - Queue default sizing notes (16384 entries ≈ 230ms burst absorption at 70k msg/s).
  - Explanation of per-connection sequence tracking for resume-on-reconnect.
  - Memory ordering notes for `running_` control flag.
- Enhanced inline documentation in `msg_client.cpp`:
  - Connection helper rationale (DNS resolution caching, failover).
  - Socket option rationale (keepalive timing, NODELAY, SO_RCVBUF kernel doubling).
  - IO loop: explanation of why no idle-timeout disconnect is used.
  - Decoder loop: note on drop behavior when worker queues are full.
  - Worker loop: buffer lifetime documentation and custom deleter behavior.
- Enhanced file headers for `main.cpp`, `log_msg.cpp`, `config_parser.cpp`,
  `msg_test_server.cpp`, `metrics.hpp`, and `metrics_demo.cpp` with usage
  hints and design overviews.
- Added "Source File Guide" table to `doc/README.md` mapping each source file
  to its responsibility.

### Changed
- `doc/README.md`: added Source File Guide table for quick navigation.

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
