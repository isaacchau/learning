#include "msg_client.h"
#include "log_msg.h"

#include <cstdlib>
#include <string>

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <errno.h>

#include <cstring>
#include <cstdio>
#include <algorithm>
#include <chrono>

// ============================================================================
// Configuration Validation
// ============================================================================

std::string MsgClientConfig::validate() const {
    // Host validation
    if (host.empty()) {
        return "Host cannot be empty";
    }
    
    // Port validation
    if (port < Defaults::MIN_PORT || port > Defaults::MAX_PORT) {
        return "Port must be between " + std::to_string(Defaults::MIN_PORT) + 
               " and " + std::to_string(Defaults::MAX_PORT);
    }
    
    // Item name validation (must fit in protocol's reqItem field)
    if (item_name.empty()) {
        return "Item name cannot be empty";
    }
    if (item_name.length() > Defaults::MAX_ITEM_NAME_LEN) {
        return "Item name too long (max " + std::to_string(Defaults::MAX_ITEM_NAME_LEN) + 
               " chars, got " + std::to_string(item_name.length()) + ")";
    }
    
    // Worker thread count validation
    if (worker_thread_count < Defaults::MIN_WORKER_THREADS || 
        worker_thread_count > Defaults::MAX_WORKER_THREADS) {
        return "Worker thread count must be between " + 
               std::to_string(Defaults::MIN_WORKER_THREADS) + " and " + 
               std::to_string(Defaults::MAX_WORKER_THREADS);
    }
    
    // Queue size validation
    if (raw_queue_size < Defaults::MIN_QUEUE_SIZE || 
        raw_queue_size > Defaults::MAX_QUEUE_SIZE) {
        return "Raw queue size must be between " + 
               std::to_string(Defaults::MIN_QUEUE_SIZE) + " and " + 
               std::to_string(Defaults::MAX_QUEUE_SIZE);
    }
    if (decoded_queue_size < Defaults::MIN_QUEUE_SIZE || 
        decoded_queue_size > Defaults::MAX_QUEUE_SIZE) {
        return "Decoded queue size must be between " + 
               std::to_string(Defaults::MIN_QUEUE_SIZE) + " and " + 
               std::to_string(Defaults::MAX_QUEUE_SIZE);
    }
    
    // Reconnect interval validation
    if (reconnect_interval_ms < Defaults::MIN_RECONNECT_MS || 
        reconnect_interval_ms > Defaults::MAX_RECONNECT_MS) {
        return "Reconnect interval must be between " + 
               std::to_string(Defaults::MIN_RECONNECT_MS) + " and " + 
               std::to_string(Defaults::MAX_RECONNECT_MS) + " ms";
    }
    
    // Queue push timeout validation (-1 is valid for no-wait)
    if (queue_push_timeout_ms < Defaults::MIN_QUEUE_PUSH_TIMEOUT_MS || 
        queue_push_timeout_ms > Defaults::MAX_QUEUE_PUSH_TIMEOUT_MS) {
        return "Queue push timeout must be between " + 
               std::to_string(Defaults::MIN_QUEUE_PUSH_TIMEOUT_MS) + " and " + 
               std::to_string(Defaults::MAX_QUEUE_PUSH_TIMEOUT_MS) + " ms";
    }
    
    // All validations passed
    return "";
}

// ============================================================================
// Construction / Destruction
// ============================================================================

MsgClient::MsgClient(const MsgClientConfig& config)
    : config_(config)
    , running_(false)
    , current_reconnect_delay_ms_(config.reconnect_interval_ms)
    , last_recv_time_(std::chrono::steady_clock::now())
    , last_received_seq_(config.starting_seq_num)
{
    // Clamp worker count
    if (config_.worker_thread_count < Defaults::MIN_WORKER_THREADS) {
        config_.worker_thread_count = Defaults::MIN_WORKER_THREADS;
    }
    if (config_.worker_thread_count > Defaults::MAX_WORKER_THREADS) {
        config_.worker_thread_count = Defaults::MAX_WORKER_THREADS;
    }

    // Create memory pool
    if (config_.pool_config.empty()) {
        pool_.reset(new MemoryPool());
    } else {
        pool_.reset(new MemoryPool(config_.pool_config));
    }

    // Create raw queue (IO → decoder, single SPSC)
    raw_queue_.reset(new LockFreeRingBuffer<RawMessage>(config_.raw_queue_size));

    // Create per-worker decoded queues (decoder → worker[i], one SPSC each)
    decoded_queues_.reserve(config_.worker_thread_count);
    for (size_t i = 0; i < config_.worker_thread_count; ++i) {
        decoded_queues_.emplace_back(
            new LockFreeRingBuffer<SubMessage>(config_.decoded_queue_size));
    }
}

MsgClient::~MsgClient() {
    stop();
}

// ============================================================================
// Public API
// ============================================================================

void MsgClient::setMessageHandler(MessageHandler handler) {
    handler_ = std::move(handler);
}

void MsgClient::start() {
    if (running_.load(std::memory_order_relaxed)) return;
    running_.store(true, std::memory_order_release);

    // Launch worker threads first (consumers)
    worker_threads_.reserve(config_.worker_thread_count);
    for (size_t i = 0; i < config_.worker_thread_count; ++i) {
        worker_threads_.emplace_back(&MsgClient::workerLoop, this, i);
    }

    // Launch decoder thread
    decoder_thread_ = std::thread(&MsgClient::decoderLoop, this);

    // Launch IO thread
    io_thread_ = std::thread(&MsgClient::ioLoop, this);
}

void MsgClient::stop() {
    if (!running_.load(std::memory_order_relaxed)) return;
    running_.store(false, std::memory_order_release);

    // Close socket to unblock any blocking recv/poll
    closeSocket();

    // Join threads with a two-phase approach:
    // 1. Try graceful shutdown (wait for threads to notice running_=false)
    // 2. If that fails, force cancellation (Linux only) and join again
    // 3. Never detach - detached threads accessing member variables after
    //    destruction causes use-after-free crashes.
    const int JOIN_TIMEOUT_MS = Defaults::THREAD_JOIN_TIMEOUT_MS;
    const int CANCEL_JOIN_TIMEOUT_MS = 2000;  // Shorter timeout after cancel
    bool io_cancelled = false, decoder_cancelled = false;
    
    if (io_thread_.joinable()) {
        if (!joinWithTimeout(io_thread_, JOIN_TIMEOUT_MS)) {
            LOG_WARN("[MsgClient] IO thread did not stop gracefully within %d ms, "
                     "attempting force cancellation...", JOIN_TIMEOUT_MS);
            io_cancelled = cancelThread(io_thread_);
            if (io_cancelled) {
                // Try to join after cancellation (thread should stop soon)
                if (!joinWithTimeout(io_thread_, CANCEL_JOIN_TIMEOUT_MS)) {
                    LOG_ERR("[MsgClient] CRITICAL: IO thread did not stop even after "
                            "cancellation. This may indicate a bug. Continuing anyway...");
                    // Still don't detach - the thread might stop later
                }
            } else {
                LOG_ERR("[MsgClient] CRITICAL: Could not cancel IO thread (non-Linux "
                        "platform or thread not joinable). Waiting longer...");
                // Last resort: try joining with a very long timeout
                joinWithTimeout(io_thread_, 60000);  // 60 seconds
            }
        }
    }
    
    if (decoder_thread_.joinable()) {
        if (!joinWithTimeout(decoder_thread_, JOIN_TIMEOUT_MS)) {
            LOG_WARN("[MsgClient] Decoder thread did not stop gracefully within %d ms, "
                     "attempting force cancellation...", JOIN_TIMEOUT_MS);
            decoder_cancelled = cancelThread(decoder_thread_);
            if (decoder_cancelled) {
                if (!joinWithTimeout(decoder_thread_, CANCEL_JOIN_TIMEOUT_MS)) {
                    LOG_ERR("[MsgClient] CRITICAL: Decoder thread did not stop even after "
                            "cancellation. This may indicate a bug. Continuing anyway...");
                }
            } else {
                LOG_ERR("[MsgClient] CRITICAL: Could not cancel Decoder thread. "
                        "Waiting longer...");
                joinWithTimeout(decoder_thread_, 60000);
            }
        }
    }
    
    for (auto& t : worker_threads_) {
        if (t.joinable()) {
            if (!joinWithTimeout(t, JOIN_TIMEOUT_MS)) {
                LOG_WARN("[MsgClient] Worker thread did not stop gracefully within %d ms, "
                         "attempting force cancellation...", JOIN_TIMEOUT_MS);
                if (cancelThread(t)) {
                    joinWithTimeout(t, CANCEL_JOIN_TIMEOUT_MS);
                } else {
                    joinWithTimeout(t, 60000);
                }
            }
        }
    }
    worker_threads_.clear();
    
    // Log summary if any threads required cancellation
    if (io_cancelled || decoder_cancelled) {
        LOG_WARN("[MsgClient] Some threads required force cancellation during shutdown. "
                 "Review thread loop logic if this happens frequently.");
    }
}

StatsSnapshot MsgClient::getStats() const {
    StatsSnapshot snap;
    snap.messages_received  = stats_.messages_received.load(std::memory_order_relaxed);
    snap.messages_decoded   = stats_.messages_decoded.load(std::memory_order_relaxed);
    snap.messages_processed = stats_.messages_processed.load(std::memory_order_relaxed);
    snap.messages_dropped   = stats_.messages_dropped.load(std::memory_order_relaxed);
    snap.bytes_received     = stats_.bytes_received.load(std::memory_order_relaxed);
    snap.reconnect_count    = stats_.reconnect_count.load(std::memory_order_relaxed);
    snap.parse_errors       = stats_.parse_errors.load(std::memory_order_relaxed);
    snap.queue_full_errors  = stats_.queue_full_errors.load(std::memory_order_relaxed);
    return snap;
}

std::vector<MemoryPool::Stats> MsgClient::getPoolStats() const {
    return pool_->getStats();
}

bool MsgClient::isRunning() const {
    return running_.load(std::memory_order_relaxed);
}

// ============================================================================
// Connection Helpers
// ============================================================================

bool MsgClient::connectToServer() {
    // Resolve hostname
    struct addrinfo hints, *result = nullptr;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    std::string port_str = std::to_string(config_.port);
    int rc = ::getaddrinfo(config_.host.c_str(), port_str.c_str(), &hints, &result);
    if (rc != 0 || !result) {
        LOG_ERR("[MsgClient] getaddrinfo failed: %s", gai_strerror(rc));
        return false;
    }

    // Create socket
    int fd = ::socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (fd < 0) {
        LOG_ERR("[MsgClient] socket() failed: %s", strerror(errno));
        ::freeaddrinfo(result);
        return false;
    }
    socket_guard_.reset(fd);

    // Helper for reading environment variables as integers
    auto getEnvInt = [](const char* name, int def) -> int {
        const char* val = std::getenv(name);
        if (val) {
            try { return std::stoi(val); } catch (...) {}
        }
        return def;
    };

    // Enable TCP Keepalive
    int keepalive = 1;
    if (::setsockopt(socket_guard_.get(), SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive)) == 0) {
#ifdef __linux__
        int idle  = getEnvInt("TCP_KEEPIDLE", 10);
        int intvl = getEnvInt("TCP_KEEPINTVL", 3);
        int cnt   = getEnvInt("TCP_KEEPCNT", 3);

        ::setsockopt(socket_guard_.get(), IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
        ::setsockopt(socket_guard_.get(), IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
        ::setsockopt(socket_guard_.get(), IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
        
        LOG_INFO("[MsgClient] TCP Keepalive configured: idle=%ds, intvl=%ds, probes=%d", idle, intvl, cnt);
#else
        LOG_INFO("[MsgClient] TCP Keepalive enabled (OS defaults)");
#endif
    } else {
        LOG_WARN("[MsgClient] Failed to enable SO_KEEPALIVE: %s", strerror(errno));
    }

    // Set connect timeout via SO_SNDTIMEO (5 seconds)
    struct timeval tv;
    tv.tv_sec  = 5;
    tv.tv_usec = 0;
    ::setsockopt(socket_guard_.get(), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    // Disable Nagle's algorithm for low latency
    int flag = 1;
    ::setsockopt(socket_guard_.get(), IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    // Set TCP receive buffer size (for Bandwidth-Delay Product)
    // Default: 2MB. High-BDP networks need larger buffers to maximize throughput.
    // BDP = Bandwidth × RTT. Example: 1Gbps × 10ms = 1.25MB
    int rcvbuf = getEnvInt("APP_TCP_SO_RCVBUF", 2097152);  // 2MB default
    if (rcvbuf > 0) {
        socklen_t optlen = sizeof(rcvbuf);
        int old_size = 0;
        ::getsockopt(socket_guard_.get(), SOL_SOCKET, SO_RCVBUF, &old_size, &optlen);
        ::setsockopt(socket_guard_.get(), SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
        ::getsockopt(socket_guard_.get(), SOL_SOCKET, SO_RCVBUF, &rcvbuf, &optlen);
        LOG_INFO("[MsgClient] TCP receive buffer: %d KB (requested: %d KB, system default: %d KB)",
                 rcvbuf / 1024, 2097152 / 1024, old_size / 1024);
    }

    // Connect
    rc = ::connect(socket_guard_.get(), result->ai_addr, result->ai_addrlen);
    ::freeaddrinfo(result);

    if (rc < 0) {
        LOG_ERR("[MsgClient] connect() to %s:%u failed: %s",
                config_.host.c_str(), config_.port, strerror(errno));
        socket_guard_.close();
        return false;
    }

    LOG_INFO("[MsgClient] Connected to %s:%u",
            config_.host.c_str(), config_.port);
    return true;
}

bool MsgClient::sendSubscription() {
    TcpRequest req;
    std::memset(&req, 0, sizeof(req));
    req.reqKey = getMagicKey();
    snprintf(req.reqItem, sizeof(req.reqItem), "%s", config_.item_name.c_str());
    // Use the last received sequence number for resume on reconnection
    uint64_t resume_seq = last_received_seq_.load(std::memory_order_relaxed);
    req.lastRespSeq = resume_seq;
    snprintf(req.clientID, sizeof(req.clientID), "MsgClient");
    
    LOG_INFO("[MsgClient] Sending subscription: item='%s', resume_from_seq=%lu",
             config_.item_name.c_str(), resume_seq);

    ssize_t sent = ::send(socket_guard_.get(), &req, sizeof(req), MSG_NOSIGNAL);
    if (sent != sizeof(req)) {
        LOG_ERR("[MsgClient] Failed to send subscription: %s",
                strerror(errno));
        return false;
    }
    return true;
}

void MsgClient::closeSocket() {
    socket_guard_.close();
}

// ============================================================================
// IO Thread: recv, frame messages, push to raw_queue_
// ============================================================================

void MsgClient::ioLoop() {
    while (running_.load(std::memory_order_relaxed)) {
        // Connect (with exponential backoff reconnection)
        if (!connectToServer()) {
            if (!running_.load(std::memory_order_relaxed)) break;
            LOG_INFO("[MsgClient] Connection failed, retrying in %d ms...", 
                     current_reconnect_delay_ms_);
            std::this_thread::sleep_for(
                std::chrono::milliseconds(current_reconnect_delay_ms_));
            stats_.reconnect_count.fetch_add(1, std::memory_order_relaxed);
            // Exponential backoff: double the delay, cap at max
            current_reconnect_delay_ms_ = std::min(
                static_cast<int>(current_reconnect_delay_ms_ * Defaults::RECONNECT_BACKOFF_MULT),
                Defaults::RECONNECT_MAX_MS);
            continue;
        }

        // Reset backoff and health check timer on successful connection
        current_reconnect_delay_ms_ = config_.reconnect_interval_ms;
        last_recv_time_ = std::chrono::steady_clock::now();

        // Send subscription request
        if (!sendSubscription()) {
            LOG_ERR("[MsgClient] Failed to send subscription, reconnecting...");
            closeSocket();
            std::this_thread::sleep_for(
                std::chrono::milliseconds(current_reconnect_delay_ms_));
            stats_.reconnect_count.fetch_add(1, std::memory_order_relaxed);
            // Exponential backoff for subscription failures too
            current_reconnect_delay_ms_ = std::min(
                static_cast<int>(current_reconnect_delay_ms_ * Defaults::RECONNECT_BACKOFF_MULT),
                Defaults::RECONNECT_MAX_MS);
            continue;
        }

        // Allocate initial receive buffer from pool
        auto recv_buf = pool_->allocate(getRecvBufferSize());
        if (!recv_buf) {
            LOG_ERR("[MsgClient] Failed to allocate receive buffer - memory pool exhausted, "
                     "will retry connection...");
            closeSocket();
            std::this_thread::sleep_for(
                std::chrono::milliseconds(current_reconnect_delay_ms_));
            // Exponential backoff for allocation failures too
            current_reconnect_delay_ms_ = std::min(
                static_cast<int>(current_reconnect_delay_ms_ * Defaults::RECONNECT_BACKOFF_MULT),
                Defaults::RECONNECT_MAX_MS);
            continue;  // Retry outer loop - don't terminate thread
        }
        size_t recv_used = 0;

        // Receive loop
        while (running_.load(std::memory_order_relaxed)) {
            // Poll with timeout so we can check running_ periodically
            struct pollfd pfd;
            std::memset(&pfd, 0, sizeof(pfd));
            pfd.fd     = socket_guard_.get();
            pfd.events = POLLIN;

            int ret = ::poll(&pfd, 1, Defaults::POLL_TIMEOUT_MS); // 100ms timeout
            if (ret < 0) {
                if (errno == EINTR) continue;
                LOG_ERR("[MsgClient] poll() error: %s", strerror(errno));
                break;
            }
            if (ret == 0) {
                // Timeout — check for idle connection (health check)
                if (Defaults::CONN_IDLE_TIMEOUT_MS > 0) {
                    auto idle_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - last_recv_time_).count();
                    if (idle_ms > Defaults::CONN_IDLE_TIMEOUT_MS) {
                        LOG_ERR("[MsgClient] Connection idle for %ld ms (timeout: %d ms), forcing reconnect",
                                idle_ms, Defaults::CONN_IDLE_TIMEOUT_MS);
                        break; // Force reconnect
                    }
                }
                continue; // re-check running_
            }

            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                LOG_ERR("[MsgClient] Socket error event");
                break;
            }

            // Receive data
            size_t space = recv_buf->capacity - recv_used;
            if (space == 0) {
                // Buffer full but no complete message — protocol error
                LOG_WARN("[MsgClient] Recv buffer full, no complete message");
                stats_.parse_errors.fetch_add(1, std::memory_order_relaxed);
                recv_buf = pool_->allocate(getRecvBufferSize());
                if (!recv_buf) {
                    LOG_ERR("[MsgClient] Failed to allocate receive buffer - memory pool exhausted");
                    break;
                }
                recv_used = 0;
                continue;
            }

            ssize_t n = ::recv(socket_guard_.get(), recv_buf->data + recv_used, space, 0);
            if (n <= 0) {
                if (n == 0) {
                    LOG_INFO("[MsgClient] Server closed connection");
                } else {
                    LOG_ERR("[MsgClient] recv() error: %s", strerror(errno));
                }
                break;
            }

            // Update last receive time for connection health check
            last_recv_time_ = std::chrono::steady_clock::now();

            recv_used += static_cast<size_t>(n);
            stats_.bytes_received.fetch_add(static_cast<uint64_t>(n),
                                            std::memory_order_relaxed);

            // Parse complete messages from the buffer
            size_t parse_pos = 0;
            bool parse_error = false;

            while (parse_pos + sizeof(TcpResponse) <= recv_used) {
                // Read TcpResponse header
                TcpResponse resp;
                std::memcpy(&resp, recv_buf->data + parse_pos, sizeof(TcpResponse));

                // Validate message length
                // NOTE: The > MAX_MSG_LEN check is currently redundant since respLen
                // is uint16_t (max 65535), but it's kept for future-proofing. If the
                // protocol changes to uint32_t, this check becomes meaningful.
                // Modern compilers optimize away the always-false branch anyway.
                if (resp.respLen < MIN_MSG_LEN || resp.respLen > MAX_MSG_LEN) {
                    LOG_ERR("[MsgClient] Invalid msgLen: %u", resp.respLen);
                    stats_.parse_errors.fetch_add(1, std::memory_order_relaxed);
                    parse_error = true;
                    break;
                }

                // Check if we have the complete message
                if (parse_pos + resp.respLen > recv_used) {
                    break; // Incomplete — wait for more data
                }

                // Complete message — create RawMessage
                RawMessage raw;
                raw.buffer  = recv_buf;  // shared_ptr copy, ref +1
                raw.offset  = parse_pos;
                raw.length  = resp.respLen;
                raw.seq_num = resp.respSeq;

                // Track the highest sequence number received for reconnection resume
                // (TCP guarantees in-order delivery, so this is the latest)
                last_received_seq_.store(resp.respSeq, std::memory_order_relaxed);

                if (!raw_queue_->push_wait(std::move(raw), config_.queue_push_timeout_ms)) {
                    stats_.queue_full_errors.fetch_add(1, std::memory_order_relaxed);
                    stats_.messages_dropped.fetch_add(1, std::memory_order_relaxed);
                    LOG_WARN("[MsgClient] Message dropped: raw queue full (seq=%lu)", raw.seq_num);
                }

                stats_.messages_received.fetch_add(1, std::memory_order_relaxed);
                parse_pos += resp.respLen;
            }

            if (parse_error) {
                // Reset buffer on protocol error
                recv_buf = pool_->allocate(getRecvBufferSize());
                if (!recv_buf) {
                    LOG_ERR("[MsgClient] Failed to allocate receive buffer - memory pool exhausted");
                    break;
                }
                recv_used = 0;
                continue;
            }

            // Handle remaining partial data
            size_t remaining = recv_used - parse_pos;
            if (remaining > 0) {
                // Allocate a new buffer and copy the partial data
                // (can't memmove in-place because existing RawMessages reference this buffer)
                auto new_buf = pool_->allocate(getRecvBufferSize());
                if (!new_buf) {
                    LOG_ERR("[MsgClient] Failed to allocate buffer for partial data - memory pool exhausted");
                    break;
                }
                std::memcpy(new_buf->data, recv_buf->data + parse_pos, remaining);
                recv_buf = std::move(new_buf);
                recv_used = remaining;
            } else {
                // All data consumed — fresh buffer for next recv
                recv_buf = pool_->allocate(getRecvBufferSize());
                if (!recv_buf) {
                    LOG_ERR("[MsgClient] Failed to allocate fresh receive buffer - memory pool exhausted");
                    break;
                }
                recv_used = 0;
            }
        }

        // Disconnected — clean up and retry with exponential backoff
        closeSocket();

        if (running_.load(std::memory_order_relaxed)) {
            LOG_INFO("[MsgClient] Disconnected, reconnecting in %d ms...",
                    current_reconnect_delay_ms_);
            std::this_thread::sleep_for(
                std::chrono::milliseconds(current_reconnect_delay_ms_));
            stats_.reconnect_count.fetch_add(1, std::memory_order_relaxed);
            // Exponential backoff
            current_reconnect_delay_ms_ = std::min(
                static_cast<int>(current_reconnect_delay_ms_ * Defaults::RECONNECT_BACKOFF_MULT),
                Defaults::RECONNECT_MAX_MS);
        }
    }
}

// ============================================================================
// Decoder Thread: parse raw → SubMessage, round-robin to per-worker queues
// ============================================================================

void MsgClient::decoderLoop() {
    size_t worker_idx = 0;
    RawMessage raw;

    while (running_.load(std::memory_order_relaxed)) {
        if (!raw_queue_->pop_wait(raw, Defaults::QUEUE_POP_TIMEOUT_MS)) {
            continue; // Timeout — re-check running_
        }

        // Validate minimum size for TcpResponse + MsgHdr
        if (raw.length < sizeof(TcpResponse) + sizeof(MsgHdr)) {
            stats_.parse_errors.fetch_add(1, std::memory_order_relaxed);
            raw.buffer.reset();
            continue;
        }

        const char* msg_data = raw.buffer->data + raw.offset;

        // Parse MsgHdr (immediately after TcpResponse)
        MsgHdr hdr;
        std::memcpy(&hdr, msg_data + sizeof(TcpResponse), sizeof(MsgHdr));

        // Create SubMessage (zero-copy: shares the buffer)
        SubMessage sub;
        sub.buffer      = raw.buffer; // shared_ptr copy, keeps buffer alive
        sub.seq_num     = hdr.msgSeqNum;
        sub.timestamp   = hdr.timestamp;
        sub.flags       = hdr.flags;
        sub.body        = msg_data + sizeof(TcpResponse) + sizeof(MsgHdr);
        sub.body_length = raw.length - sizeof(TcpResponse) - sizeof(MsgHdr);

        // Release raw message's reference (buffer stays alive via sub.buffer)
        raw.buffer.reset();

        // Round-robin push to worker queues
        if (!decoded_queues_[worker_idx]->push_wait(std::move(sub), config_.queue_push_timeout_ms)) {
            stats_.queue_full_errors.fetch_add(1, std::memory_order_relaxed);
            stats_.messages_dropped.fetch_add(1, std::memory_order_relaxed);
            LOG_WARN("[MsgClient] Message dropped: worker queue full (seq=%lu, worker=%zu)", 
                     sub.seq_num, worker_idx);
        }

        stats_.messages_decoded.fetch_add(1, std::memory_order_relaxed);
        worker_idx = (worker_idx + 1) % config_.worker_thread_count;
    }
}

// ============================================================================
// Worker Thread: pop from per-worker queue, invoke handler
// ============================================================================

void MsgClient::workerLoop(size_t worker_index) {
    SubMessage msg;

    while (running_.load(std::memory_order_relaxed)) {
        if (!decoded_queues_[worker_index]->pop_wait(msg, Defaults::QUEUE_POP_TIMEOUT_MS)) {
            continue; // Timeout — re-check running_
        }

        if (handler_) {
            handler_(msg, worker_index);
        }

        stats_.messages_processed.fetch_add(1, std::memory_order_relaxed);

        // Release buffer reference
        msg.buffer.reset();
    }
}
