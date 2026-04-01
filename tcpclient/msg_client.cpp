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
#include <sys/epoll.h>
#include <signal.h>
#include <errno.h>

#include <cstring>
#include <cstdio>
#include <algorithm>
#include <chrono>

// ============================================================================
// ConnectionConfig Validation
// ============================================================================

std::string ConnectionConfig::validate() const {
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
    
    // Client ID validation
    if (client_id.empty()) {
        return "Client ID cannot be empty";
    }
    if (client_id.length() > Defaults::MAX_CLIENT_ID_LEN) {
        return "Client ID too long (max " + std::to_string(Defaults::MAX_CLIENT_ID_LEN) + 
               " chars, got " + std::to_string(client_id.length()) + ")";
    }
    
    return "";
}

// ============================================================================
// MsgClientConfig Implementation
// ============================================================================

void MsgClientConfig::addConnection(const ConnectionConfig& conn) {
    connections.push_back(conn);
}

void MsgClientConfig::addConnection(const std::string& host, uint16_t port, 
                                     const std::string& item,
                                     const std::string& client_id,
                                     uint64_t start_seq) {
    ConnectionConfig conn;
    conn.host = host;
    conn.port = port;
    conn.item_name = item;
    conn.client_id = client_id;
    conn.starting_seq_num = start_seq;
    connections.push_back(conn);
}

std::string MsgClientConfig::validate() const {
    // Must have at least one connection
    if (connections.empty()) {
        return "At least one connection must be configured";
    }
    
    // Maximum connections
    if (connections.size() > Defaults::MAX_CONNECTIONS) {
        return "Too many connections (max " + std::to_string(Defaults::MAX_CONNECTIONS) + 
               ", got " + std::to_string(connections.size()) + ")";
    }
    
    // Validate each connection
    for (size_t i = 0; i < connections.size(); ++i) {
        std::string err = connections[i].validate();
        if (!err.empty()) {
            return "Connection[" + std::to_string(i) + "]: " + err;
        }
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
    
    return "";
}

// ============================================================================
// ConnectionState Implementation
// ============================================================================

ConnectionState::ConnectionState(const ConnectionConfig& cfg)
    : config(cfg)
    , current_reconnect_delay_ms_(Defaults::RECONNECT_INTERVAL_MS)
    , last_recv_time_(std::chrono::steady_clock::now())
    , last_received_seq_(cfg.starting_seq_num)
    , messages_received_(0)
    , bytes_received_(0)
    , reconnect_count_(0)
{
}

// ============================================================================
// Construction / Destruction
// ============================================================================

MsgClient::MsgClient(const MsgClientConfig& config)
    : config_(config)
    , running_(false)
    , epoll_fd_(-1)
{
    // Cache environment-derived constants once at construction
    auto getEnvInt = [](const char* name, int def) -> int {
        const char* val = std::getenv(name);
        if (val) { try { return std::stoi(val); } catch (...) {} }
        return def;
    };
    recv_buffer_size_ = getRecvBufferSize();
    tcp_keepidle_     = getEnvInt("TCP_KEEPIDLE", 10);
    tcp_keepintvl_    = getEnvInt("TCP_KEEPINTVL", 3);
    tcp_keepcnt_      = getEnvInt("TCP_KEEPCNT", 3);
    tcp_rcvbuf_       = getEnvInt("APP_TCP_SO_RCVBUF", 2097152);
    // Clamp worker count
    if (config_.worker_thread_count < Defaults::MIN_WORKER_THREADS) {
        config_.worker_thread_count = Defaults::MIN_WORKER_THREADS;
    }
    if (config_.worker_thread_count > Defaults::MAX_WORKER_THREADS) {
        config_.worker_thread_count = Defaults::MAX_WORKER_THREADS;
    }

    // Create connection states
    connections_.reserve(config_.connections.size());
    for (const auto& conn_config : config_.connections) {
        connections_.emplace_back(new ConnectionState(conn_config));
    }

    // Create memory pool
    if (config_.pool_config.empty()) {
        pool_.reset(new MemoryPool());
    } else {
        pool_.reset(new MemoryPool(config_.pool_config));
    }

    // Create raw queue (IO → decoder, single SPSC for all connections)
    raw_queue_.reset(new LockFreeRingBuffer<RawMessage>(config_.raw_queue_size));

    // Create per-worker decoded queues (decoder → worker[i], one SPSC each)
    decoded_queues_.reserve(config_.worker_thread_count);
    for (size_t i = 0; i < config_.worker_thread_count; ++i) {
        decoded_queues_.emplace_back(
            new LockFreeRingBuffer<SubMessage>(config_.decoded_queue_size));
    }
    
    // Create epoll instance for efficient multi-connection I/O
    epoll_fd_.store(::epoll_create1(EPOLL_CLOEXEC));
    if (epoll_fd_.load() < 0) {
        LOG_ERR("[MsgClient] Failed to create epoll instance: %s", strerror(errno));
        // We'll fall back to the non-epoll path in ioLoop if epoll_fd_ < 0
    } else {
        LOG_INFO("[MsgClient] Epoll instance created (fd=%d)", epoll_fd_.load());
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
    if (running_.load()) return;

    // Resolve hostnames on the main thread before any worker threads start.
    for (size_t i = 0; i < connections_.size(); ++i) {
        if (!resolveHost(i)) {
            LOG_ERR("[MsgClient] Failed to resolve host for connection %zu (%s:%u). "
                    "Client will retry during reconnection loop.",
                    i, connections_[i]->config.host.c_str(), connections_[i]->config.port);
        }
    }

    running_.store(true);

    // Launch worker threads first (consumers)
    worker_threads_.reserve(config_.worker_thread_count);
    for (size_t i = 0; i < config_.worker_thread_count; ++i) {
        worker_threads_.emplace_back(&MsgClient::workerLoop, this, i);
    }

    // Launch decoder thread
    decoder_thread_ = std::thread(&MsgClient::decoderLoop, this);

    // Launch IO thread
    io_thread_ = std::thread(&MsgClient::ioLoop, this);

    LOG_INFO("[MsgClient] Started with %zu connection(s), %zu worker thread(s)",
             connections_.size(), config_.worker_thread_count);
}

void MsgClient::stop() {
    if (!running_.load()) return;
    running_.store(false);

    // Close all sockets to unblock any blocking recv/poll
    closeAllSockets();

    // Close epoll_fd to wake up epoll_wait immediately (it will return EBADF)
    int epoll_fd_local = epoll_fd_.exchange(-1);
    if (epoll_fd_local >= 0) {
        ::close(epoll_fd_local);
    }

    // Join all threads gracefully.  All blocking syscalls now have timeouts
    // (SO_SNDTIMEO on connect, epoll_wait timeout, queue pop timeouts).
    if (io_thread_.joinable()) {
        io_thread_.join();
    }
    if (decoder_thread_.joinable()) {
        decoder_thread_.join();
    }
    for (auto& t : worker_threads_) {
        if (t.joinable()) {
            t.join();
        }
    }
    worker_threads_.clear();
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

    // Per-connection stats
    snap.connection_stats.reserve(connections_.size());
    for (size_t i = 0; i < connections_.size(); ++i) {
        ConnectionStats cs;
        cs.connection_id = i;
        cs.messages_received = connections_[i]->messages_received_.load(std::memory_order_relaxed);
        cs.bytes_received = connections_[i]->bytes_received_.load(std::memory_order_relaxed);
        cs.reconnect_count = connections_[i]->reconnect_count_.load(std::memory_order_relaxed);
        cs.last_seq_num = connections_[i]->last_received_seq_.load(std::memory_order_relaxed);
        cs.connected = connections_[i]->socket_guard.valid();
        cs.endpoint = connections_[i]->config.host + ":" + std::to_string(connections_[i]->config.port);
        cs.item_name = connections_[i]->config.item_name;
        snap.connection_stats.push_back(cs);
    }
    
    return snap;
}

std::vector<MemoryPool::Stats> MsgClient::getPoolStats() const {
    return pool_->getStats();
}

bool MsgClient::isRunning() const {
    return running_.load();
}

// ============================================================================
// Connection Helpers
// ============================================================================

bool MsgClient::resolveHost(size_t conn_idx) {
    if (conn_idx >= connections_.size()) return false;

    ConnectionState& conn = *connections_[conn_idx];

    struct addrinfo hints, *result = nullptr;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    std::string port_str = std::to_string(conn.config.port);
    int rc = ::getaddrinfo(conn.config.host.c_str(), port_str.c_str(), &hints, &result);
    if (rc != 0 || !result) {
        LOG_ERR("[MsgClient][Conn %zu] getaddrinfo failed: %s", conn_idx, gai_strerror(rc));
        return false;
    }

    std::memcpy(&conn.resolved_addr, result->ai_addr, result->ai_addrlen);
    conn.resolved_addr_len = result->ai_addrlen;
    ::freeaddrinfo(result);
    return true;
}

bool MsgClient::connectToServer(size_t conn_idx) {
    if (conn_idx >= connections_.size()) return false;

    ConnectionState& conn = *connections_[conn_idx];

    if (conn.resolved_addr_len == 0) {
        LOG_ERR("[MsgClient][Conn %zu] No resolved address available", conn_idx);
        return false;
    }

    // Create socket
    int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        LOG_ERR("[MsgClient][Conn %zu] socket() failed: %s", conn_idx, strerror(errno));
        return false;
    }
    conn.socket_guard.reset(fd);

    // Enable TCP Keepalive
    int keepalive = 1;
    if (::setsockopt(conn.socket_guard.get(), SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive)) == 0) {
#ifdef __linux__
        ::setsockopt(conn.socket_guard.get(), IPPROTO_TCP, TCP_KEEPIDLE, &tcp_keepidle_, sizeof(tcp_keepidle_));
        ::setsockopt(conn.socket_guard.get(), IPPROTO_TCP, TCP_KEEPINTVL, &tcp_keepintvl_, sizeof(tcp_keepintvl_));
        ::setsockopt(conn.socket_guard.get(), IPPROTO_TCP, TCP_KEEPCNT, &tcp_keepcnt_, sizeof(tcp_keepcnt_));

        LOG_INFO("[MsgClient][Conn %zu] TCP Keepalive configured: idle=%ds, intvl=%ds, probes=%d",
                 conn_idx, tcp_keepidle_, tcp_keepintvl_, tcp_keepcnt_);
#else
        LOG_INFO("[MsgClient][Conn %zu] TCP Keepalive enabled (OS defaults)", conn_idx);
#endif
    } else {
        LOG_WARN("[MsgClient][Conn %zu] Failed to enable SO_KEEPALIVE: %s",
                 conn_idx, strerror(errno));
    }

    // Set connect timeout via SO_SNDTIMEO (5 seconds)
    struct timeval tv;
    tv.tv_sec  = 5;
    tv.tv_usec = 0;
    ::setsockopt(conn.socket_guard.get(), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    // Disable Nagle's algorithm for low latency
    int flag = 1;
    ::setsockopt(conn.socket_guard.get(), IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    // Set TCP receive buffer size
    if (tcp_rcvbuf_ > 0) {
        socklen_t optlen = sizeof(tcp_rcvbuf_);
        int old_size = 0;
        ::getsockopt(conn.socket_guard.get(), SOL_SOCKET, SO_RCVBUF, &old_size, &optlen);
        ::setsockopt(conn.socket_guard.get(), SOL_SOCKET, SO_RCVBUF, &tcp_rcvbuf_, sizeof(tcp_rcvbuf_));
        int actual_rcvbuf = tcp_rcvbuf_;
        ::getsockopt(conn.socket_guard.get(), SOL_SOCKET, SO_RCVBUF, &actual_rcvbuf, &optlen);
        LOG_INFO("[MsgClient][Conn %zu] TCP receive buffer: %d KB (requested: %d KB, system default: %d KB)",
                 conn_idx, actual_rcvbuf / 1024, tcp_rcvbuf_ / 1024, old_size / 1024);
    }

    // Connect using pre-resolved address
    int rc = ::connect(conn.socket_guard.get(), reinterpret_cast<struct sockaddr*>(&conn.resolved_addr), conn.resolved_addr_len);

    if (rc < 0) {
        LOG_ERR("[MsgClient][Conn %zu] connect() to %s:%u failed: %s",
                conn_idx, conn.config.host.c_str(), conn.config.port, strerror(errno));
        conn.socket_guard.close();
        return false;
    }

    LOG_INFO("[MsgClient][Conn %zu] Connected to %s:%u",
             conn_idx, conn.config.host.c_str(), conn.config.port);

    // Add socket to epoll for efficient I/O multiplexing
    int epoll_fd_local = epoll_fd_.load();
    if (epoll_fd_local >= 0) {
        struct epoll_event ev;
        std::memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN | EPOLLPRI | EPOLLERR | EPOLLHUP | EPOLLRDHUP;
        ev.data.u32 = static_cast<uint32_t>(conn_idx);  // Store connection index
        if (::epoll_ctl(epoll_fd_local, EPOLL_CTL_ADD, conn.socket_guard.get(), &ev) < 0) {
            LOG_ERR("[MsgClient][Conn %zu] Failed to add socket to epoll: %s",
                    conn_idx, strerror(errno));
            conn.socket_guard.close();
            return false;
        }
        LOG_DEBUG("[MsgClient][Conn %zu] Socket added to epoll", conn_idx);
    }

    return true;
}

bool MsgClient::sendSubscription(size_t conn_idx) {
    if (conn_idx >= connections_.size()) return false;
    
    ConnectionState& conn = *connections_[conn_idx];
    
    TcpRequest req;
    std::memset(&req, 0, sizeof(req));
    req.reqKey = getMagicKey();
    snprintf(req.reqItem, sizeof(req.reqItem), "%s", conn.config.item_name.c_str());
    uint64_t resume_seq = conn.last_received_seq_.load(std::memory_order_relaxed);
    req.lastRespSeq = resume_seq;
    snprintf(req.clientID, sizeof(req.clientID), "%s", conn.config.client_id.c_str());
    
    LOG_INFO("[MsgClient][Conn %zu] Sending subscription: item='%s', clientID='%s', resume_from_seq=%lu",
             conn_idx, conn.config.item_name.c_str(), conn.config.client_id.c_str(), resume_seq);

    ssize_t sent = ::send(conn.socket_guard.get(), &req, sizeof(req), MSG_NOSIGNAL);
    if (sent != sizeof(req)) {
        LOG_ERR("[MsgClient][Conn %zu] Failed to send subscription: %s",
                conn_idx, strerror(errno));
        return false;
    }
    return true;
}

void MsgClient::closeConnection(size_t conn_idx) {
    if (conn_idx >= connections_.size()) return;
    int epoll_fd_local = epoll_fd_.load();
    int fd = connections_[conn_idx]->socket_guard.get();
    if (epoll_fd_local >= 0 && fd >= 0) {
        ::epoll_ctl(epoll_fd_local, EPOLL_CTL_DEL, fd, nullptr);
    }
    connections_[conn_idx]->socket_guard.close();
}

void MsgClient::closeAllSockets() {
    for (size_t i = 0; i < connections_.size(); ++i) {
        closeConnection(i);
    }
}

// ============================================================================
// IO Thread: manage multiple connections with poll()
// ============================================================================

void MsgClient::ioLoop() {
    // Per-connection receive state
    struct RecvState {
        std::shared_ptr<Buffer> recv_buf;
        size_t recv_used = 0;
    };
    std::vector<RecvState> recv_states(connections_.size());
    
    // Track which connections are currently connected
    std::vector<bool> connected(connections_.size(), false);
    
    // Initialize all connections as needing to connect
    for (size_t i = 0; i < connections_.size(); ++i) {
        connections_[i]->current_reconnect_delay_ms_ = config_.reconnect_interval_ms;
    }

    while (running_.load()) {
        // Try to connect any disconnected connections
        for (size_t i = 0; i < connections_.size(); ++i) {
            if (!connected[i] && running_.load()) {
                ConnectionState& conn = *connections_[i];
                
                if (!connectToServer(i)) {
                    LOG_INFO("[MsgClient][Conn %zu] Connection failed, retrying in %d ms...", 
                             i, conn.current_reconnect_delay_ms_);
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(conn.current_reconnect_delay_ms_));
                    conn.reconnect_count_.fetch_add(1, std::memory_order_relaxed);
                    stats_.reconnect_count.fetch_add(1, std::memory_order_relaxed);
                    conn.current_reconnect_delay_ms_ = std::min(
                        static_cast<int>(conn.current_reconnect_delay_ms_ * Defaults::RECONNECT_BACKOFF_MULT),
                        Defaults::RECONNECT_MAX_MS);
                    continue;
                }

                // Reset backoff and health check timer on successful connection
                conn.current_reconnect_delay_ms_ = config_.reconnect_interval_ms;
                conn.last_recv_time_ = std::chrono::steady_clock::now();

                // Send subscription request
                if (!sendSubscription(i)) {
                    LOG_ERR("[MsgClient][Conn %zu] Failed to send subscription, reconnecting...", i);
                    closeConnection(i);
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(conn.current_reconnect_delay_ms_));
                    conn.reconnect_count_.fetch_add(1, std::memory_order_relaxed);
                    stats_.reconnect_count.fetch_add(1, std::memory_order_relaxed);
                    conn.current_reconnect_delay_ms_ = std::min(
                        static_cast<int>(conn.current_reconnect_delay_ms_ * Defaults::RECONNECT_BACKOFF_MULT),
                        Defaults::RECONNECT_MAX_MS);
                    continue;
                }

                // Allocate receive buffer
                recv_states[i].recv_buf = pool_->allocate(recv_buffer_size_);
                if (!recv_states[i].recv_buf) {
                    LOG_ERR("[MsgClient][Conn %zu] Failed to allocate receive buffer, reconnecting...", i);
                    closeConnection(i);
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(conn.current_reconnect_delay_ms_));
                    conn.current_reconnect_delay_ms_ = std::min(
                        static_cast<int>(conn.current_reconnect_delay_ms_ * Defaults::RECONNECT_BACKOFF_MULT),
                        Defaults::RECONNECT_MAX_MS);
                    continue;
                }
                recv_states[i].recv_used = 0;
                connected[i] = true;
            }
        }

        // Check if we have any connected sockets
        bool has_connected = false;
        for (size_t i = 0; i < connections_.size(); ++i) {
            if (connected[i] && connections_[i]->socket_guard.valid()) {
                has_connected = true;
                break;
            }
        }

        if (!has_connected) {
            // No connected sockets, wait a bit before retrying
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // Wait for events using epoll
        struct epoll_event events[Defaults::MAX_CONNECTIONS];
        int ret = ::epoll_wait(epoll_fd_.load(), events, Defaults::MAX_CONNECTIONS, Defaults::POLL_TIMEOUT_MS);
        
        if (ret < 0) {
            if (errno == EINTR) continue;
            if (errno == EBADF && !running_.load()) continue;  // shutting down
            LOG_ERR("[MsgClient] epoll_wait() error: %s", strerror(errno));
            continue;
        }

        // Process each event (ret = number of ready sockets)
        for (int eidx = 0; eidx < ret; ++eidx) {
            size_t conn_idx = events[eidx].data.u32;
            if (conn_idx >= connections_.size()) continue;

            ConnectionState& conn = *connections_[conn_idx];
            RecvState& recv_state = recv_states[conn_idx];
            bool should_close = false;

            // Check for hard errors first
            if (events[eidx].events & (EPOLLERR | EPOLLHUP)) {
                should_close = true;
            }

            if (events[eidx].events & EPOLLIN) {
                // Receive data
                size_t space = recv_state.recv_buf->capacity - recv_state.recv_used;
                if (space == 0) {
                    LOG_WARN("[MsgClient][Conn %zu] Recv buffer full, no complete message", conn_idx);
                    stats_.parse_errors.fetch_add(1, std::memory_order_relaxed);
                    recv_state.recv_buf = pool_->allocate(recv_buffer_size_);
                    if (!recv_state.recv_buf) {
                        LOG_ERR("[MsgClient][Conn %zu] Failed to allocate receive buffer", conn_idx);
                        should_close = true;
                    } else {
                        recv_state.recv_used = 0;
                    }
                } else {
                    ssize_t n = ::recv(conn.socket_guard.get(),
                                       recv_state.recv_buf->data + recv_state.recv_used, space, 0);
                    if (n <= 0) {
                        if (n == 0) {
                            LOG_INFO("[MsgClient][Conn %zu] Server closed connection", conn_idx);
                        } else {
                            LOG_ERR("[MsgClient][Conn %zu] recv() error: %s", conn_idx, strerror(errno));
                        }
                        should_close = true;
                    } else {
                        // Update last receive time for connection health check
                        conn.last_recv_time_ = std::chrono::steady_clock::now();

                        recv_state.recv_used += static_cast<size_t>(n);
                        conn.bytes_received_.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
                        stats_.bytes_received.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);

                        // Parse complete messages from the buffer
                        size_t parse_pos = 0;
                        bool parse_error = false;

                        while (parse_pos + sizeof(TcpResponse) <= recv_state.recv_used) {
                            // Read TcpResponse header
                            TcpResponse resp;
                            std::memcpy(&resp, recv_state.recv_buf->data + parse_pos, sizeof(TcpResponse));

                            // Validate message length
                            if (resp.respLen < MIN_MSG_LEN || resp.respLen > MAX_MSG_LEN) {
                                LOG_ERR("[MsgClient][Conn %zu] Invalid msgLen: %u", conn_idx, resp.respLen);
                                stats_.parse_errors.fetch_add(1, std::memory_order_relaxed);
                                parse_error = true;
                                break;
                            }

                            // Check if we have the complete message
                            if (parse_pos + resp.respLen > recv_state.recv_used) {
                                break; // Incomplete — wait for more data
                            }

                            // Complete message — create RawMessage
                            RawMessage raw;
                            raw.buffer       = recv_state.recv_buf;
                            raw.offset       = parse_pos;
                            raw.length       = resp.respLen;
                            raw.seq_num      = resp.respSeq;
                            raw.connection_id = conn_idx;

                            // Track the highest sequence number received for this connection
                            conn.last_received_seq_.store(resp.respSeq, std::memory_order_relaxed);

                            if (!raw_queue_->push_wait(std::move(raw), config_.queue_push_timeout_ms)) {
                                stats_.messages_dropped.fetch_add(1, std::memory_order_relaxed);
                                LOG_WARN("[MsgClient][Conn %zu] Message dropped: raw queue full (seq=%lu)",
                                         conn_idx, raw.seq_num);
                            }

                            conn.messages_received_.fetch_add(1, std::memory_order_relaxed);
                            stats_.messages_received.fetch_add(1, std::memory_order_relaxed);
                            parse_pos += resp.respLen;
                        }

                        if (parse_error) {
                            // Reset buffer on protocol error
                            recv_state.recv_buf = pool_->allocate(recv_buffer_size_);
                            if (!recv_state.recv_buf) {
                                LOG_ERR("[MsgClient][Conn %zu] Failed to allocate receive buffer", conn_idx);
                                should_close = true;
                            } else {
                                recv_state.recv_used = 0;
                            }
                        } else {
                            // Handle remaining partial data
                            size_t remaining = recv_state.recv_used - parse_pos;
                            if (remaining > 0) {
                                auto new_buf = pool_->allocate(recv_buffer_size_);
                                if (!new_buf) {
                                    LOG_ERR("[MsgClient][Conn %zu] Failed to allocate buffer for partial data", conn_idx);
                                    should_close = true;
                                } else {
                                    std::memcpy(new_buf->data, recv_state.recv_buf->data + parse_pos, remaining);
                                    recv_state.recv_buf = std::move(new_buf);
                                    recv_state.recv_used = remaining;
                                }
                            } else {
                                recv_state.recv_buf = pool_->allocate(recv_buffer_size_);
                                if (!recv_state.recv_buf) {
                                    LOG_ERR("[MsgClient][Conn %zu] Failed to allocate fresh receive buffer", conn_idx);
                                    should_close = true;
                                } else {
                                    recv_state.recv_used = 0;
                                }
                            }
                        }
                    }
                }
            }

            if (events[eidx].events & EPOLLRDHUP) {
                should_close = true;
            }

            if (should_close) {
                connected[conn_idx] = false;
                closeConnection(conn_idx);
            }
        }
        
        // Check idle timeout for all connected connections
        // (epoll_wait timeout means no data on any socket)
        if (Defaults::CONN_IDLE_TIMEOUT_MS > 0) {
            auto now = std::chrono::steady_clock::now();
            for (size_t i = 0; i < connections_.size(); ++i) {
                if (connected[i]) {
                    auto idle_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - connections_[i]->last_recv_time_).count();
                    if (idle_ms > Defaults::CONN_IDLE_TIMEOUT_MS) {
                        LOG_ERR("[MsgClient][Conn %zu] Connection idle for %ld ms (timeout: %d ms), forcing reconnect",
                                i, idle_ms, Defaults::CONN_IDLE_TIMEOUT_MS);
                        connected[i] = false;
                        closeConnection(i);
                    }
                }
            }
        }
    }
}

// ============================================================================
// Decoder Thread: parse raw → SubMessage, round-robin to per-worker queues
// ============================================================================

void MsgClient::decoderLoop() {
    size_t worker_idx = 0;
    RawMessage raw;

    while (running_.load()) {
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

        // Save connection_id before we reset raw
        size_t conn_id = raw.connection_id;

        // Create SubMessage (zero-copy: shares the buffer)
        SubMessage sub;
        sub.buffer       = std::move(raw.buffer); // move shared_ptr, no atomic ops
        sub.seq_num      = hdr.msgSeqNum;
        sub.timestamp    = hdr.timestamp;
        sub.flags        = hdr.flags;
        sub.body         = msg_data + sizeof(TcpResponse) + sizeof(MsgHdr);
        sub.body_length  = raw.length - sizeof(TcpResponse) - sizeof(MsgHdr);
        sub.connection_id = conn_id;

        // Round-robin push to worker queues
        if (!decoded_queues_[worker_idx]->push_wait(std::move(sub), config_.queue_push_timeout_ms)) {
            stats_.messages_dropped.fetch_add(1, std::memory_order_relaxed);
            LOG_WARN("[MsgClient] Message dropped: worker queue full (seq=%lu, worker=%zu, conn=%zu)", 
                     sub.seq_num, worker_idx, conn_id);
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

    while (running_.load()) {
        if (!decoded_queues_[worker_index]->pop_wait(msg, Defaults::QUEUE_POP_TIMEOUT_MS)) {
            continue; // Timeout — re-check running_
        }

        // Invoke handler with connection_id
        if (handler_) {
            handler_(msg, worker_index, msg.connection_id);
        }

        stats_.messages_processed.fetch_add(1, std::memory_order_relaxed);

        // Release buffer reference
        msg.buffer.reset();
    }
}
