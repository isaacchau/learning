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
  - WHY comments on `MessageHandler` signature (worker_index, connection_id).
  - WHY comment on `SocketGuard` move-assignment memory ordering.
- Enhanced inline documentation in `msg_client.cpp`:
  - Connection helper rationale (DNS resolution caching, failover).
  - Socket option rationale (keepalive timing, NODELAY, SO_RCVBUF kernel doubling).
  - IO loop: explanation of why no idle-timeout disconnect is used.
  - IO loop: WHY comments on epoll event ordering (ERR before IN, RDHUP handling).
  - `computeEpollTimeoutMs`: WHY dynamic timeout beats fixed polling.
  - `processRecvData`: buffer rotation strategy and tail-copy rationale.
  - `stop()`: detailed producer-to-consumer thread join ordering rationale.
  - Decoder loop: note on drop behavior when worker queues are full.
  - Worker loop: buffer lifetime documentation and custom deleter behavior.
- Enhanced inline documentation in `lockfree_ringbuffer.h`:
  - WHY `size()` uses acquire ordering and its concurrency semantics.
  - WHY `nextPowerOf2()` uses bit-twiddling instead of loops/log2.
- Enhanced inline documentation in `shared_ptr_pool.h`:
  - WHY linear scan in `findSizeClass()` (small fixed array, branch prediction).
  - WHY two-phase allocation (lock-free-list check, then OS alloc outside lock).
- Enhanced inline documentation in `protocol.h`:
  - WHY `RawMessage` and `SubMessage` use `shared_ptr` (zero-copy, buffer lifetime).
- Enhanced inline documentation in `market_data/message_types.h`:
  - WHY fixed-size char arrays instead of `std::string` (zero-copy, deterministic layout).
  - Security note on non-null-terminated char arrays.
- Enhanced file headers for `main.cpp`, `log_msg.cpp`, `config_parser.cpp`,
  `msg_test_server.cpp`, `metrics.hpp`, `metrics_demo.cpp`, and `tests/test_main.cpp`
  with additional WHY comments and design overviews.
- Added file header to `Makefile` explaining WHY GNU Make is used.
- Added "Source File Guide" table to `doc/README.md` mapping each source file
  to its responsibility (now includes `Makefile` and `Makefile.analysis`).

### Changed
- `doc/README.md`: added Source File Guide table for quick navigation.
- `tests/test_main.cpp`: updated header title to match file path.

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
