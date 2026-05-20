// ============================================================================
// msg_client.cpp — Core MsgClient implementation
// ============================================================================
// Implements the three-stage lock-free pipeline (IO thread, decoder thread,
// worker threads), connection management with epoll, and reconnection logic.
//
// See msg_client.h for the public API and doc/03_Architecture.md for design.
// ============================================================================

#include "msg_client.h"
#include "log_msg.h"
#include "market_data/message_types.h"

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
// Validates user-supplied connection settings before they are used.
// Returns an empty string on success, or a human-readable error message.

std::string ConnectionConfig::validate() const {
    // Endpoint validation
    if (endpoints.empty()) {
        return "At least one endpoint must be configured";
    }
    for (size_t i = 0; i < endpoints.size(); ++i) {
        if (endpoints[i].host.empty()) {
            return "Endpoint[" + std::to_string(i) + "]: host cannot be empty";
        }
        if (endpoints[i].port < Defaults::MIN_PORT || endpoints[i].port > Defaults::MAX_PORT) {
            return "Endpoint[" + std::to_string(i) + "]: port must be between " +
                   std::to_string(Defaults::MIN_PORT) + " and " +
                   std::to_string(Defaults::MAX_PORT);
        }
    }

    // Max retries validation
    if (max_retries_per_endpoint < 1) {
        return "max_retries_per_endpoint must be >= 1";
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
    conn.endpoints.push_back({host, port});
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
    , next_retry_time_(std::chrono::steady_clock::time_point())
    , last_received_seq_(cfg.starting_seq_num)
    , messages_received_(0)
    , bytes_received_(0)
    , reconnect_count_(0)
    , active_endpoint_idx(0)
    , consecutive_failures_on_endpoint(0)
{
    resolved_endpoints.resize(cfg.endpoints.size());
}

const EndpointConfig& ConnectionState::activeEndpoint() const {
    return config.endpoints[active_endpoint_idx];
}

const ResolvedEndpoint& ConnectionState::activeResolved() const {
    return resolved_endpoints[active_endpoint_idx];
}

bool ConnectionState::hasResolvedEndpoint() const {
    return active_endpoint_idx < resolved_endpoints.size()
        && resolved_endpoints[active_endpoint_idx].resolved;
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
    
    // Initialize aggregation components if enabled
    if (config_.aggregation_config.enabled) {
        disk_writer_.reset(new metrics::DiskWriter(config_.aggregation_config.output_dir));
        disk_writer_->start();

        uint64_t bucket_ns = config_.aggregation_config.window_ms * 1000000ULL;
        size_t num_shards = std::thread::hardware_concurrency();
        if (num_shards == 0) num_shards = 4;

        orders_aggregator_.reset(new metrics::Aggregator(
            "orders", config_.aggregation_config.filename_prefix,
            bucket_ns, num_shards, true, config_.aggregation_config.output_format,
            disk_writer_.get(), config_.aggregation_config.output_dir));

        trades_aggregator_.reset(new metrics::Aggregator(
            "trades", config_.aggregation_config.filename_prefix,
            bucket_ns, num_shards, true, config_.aggregation_config.output_format,
            disk_writer_.get(), config_.aggregation_config.output_dir));

        quotes_aggregator_.reset(new metrics::Aggregator(
            "quotes", config_.aggregation_config.filename_prefix,
            bucket_ns, num_shards, true, config_.aggregation_config.output_format,
            disk_writer_.get(), config_.aggregation_config.output_dir));

        LOG_INFO("[MsgClient] Aggregation enabled: window_ms=%lu, format=%s, output=%s, shards=%zu",
                 config_.aggregation_config.window_ms,
                 config_.aggregation_config.output_format == metrics::OutputFormat::CSV ? "csv" : "influxdb_line",
                 config_.aggregation_config.output_dir.c_str(), num_shards);
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
            LOG_ERR("[MsgClient] Failed to resolve any endpoint for connection %zu. "
                    "Client will retry during reconnection loop.", i);
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
    
    // Disk writer is already started in constructor; aggregators are passive

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

    // Join threads in pipeline order (producers → consumers) so workers
    // finish processing everything already in the queues.
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

    // Flush aggregators AFTER workers finish (no new data can arrive).
    // Stop disk writer only after all data is enqueued.
    if (orders_aggregator_) orders_aggregator_->forceFlush();
    if (trades_aggregator_) trades_aggregator_->forceFlush();
    if (quotes_aggregator_) quotes_aggregator_->forceFlush();
    if (disk_writer_) {
        disk_writer_->stop();
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

    // Per-connection stats
    snap.connection_stats.reserve(connections_.size());
    for (size_t i = 0; i < connections_.size(); ++i) {
        snap.connection_stats.push_back(buildConnectionStats(i));
    }

    return snap;
}

ConnectionStats MsgClient::buildConnectionStats(size_t conn_idx) const {
    ConnectionStats cs;
    cs.connection_id     = conn_idx;
    cs.messages_received = connections_[conn_idx]->messages_received_.load(std::memory_order_relaxed);
    cs.bytes_received    = connections_[conn_idx]->bytes_received_.load(std::memory_order_relaxed);
    cs.reconnect_count   = connections_[conn_idx]->reconnect_count_.load(std::memory_order_relaxed);
    cs.last_seq_num      = connections_[conn_idx]->last_received_seq_.load(std::memory_order_relaxed);
    cs.connected         = connections_[conn_idx]->socket_guard.valid();
    cs.item_name         = connections_[conn_idx]->config.item_name;

    const auto& conn = *connections_[conn_idx];
    if (conn.active_endpoint_idx < conn.config.endpoints.size()) {
        const auto& ep = conn.activeEndpoint();
        cs.endpoint = ep.host + ":" + std::to_string(ep.port);
    } else {
        cs.endpoint = "unknown";
    }

    return cs;
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

bool MsgClient::resolveEndpoint(size_t conn_idx, size_t ep_idx) {
    if (conn_idx >= connections_.size()) return false;
    ConnectionState& conn = *connections_[conn_idx];
    if (ep_idx >= conn.config.endpoints.size()) return false;

    const auto& ep = conn.config.endpoints[ep_idx];
    struct addrinfo hints, *result = nullptr;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    std::string port_str = std::to_string(ep.port);
    int rc = ::getaddrinfo(ep.host.c_str(), port_str.c_str(), &hints, &result);
    if (rc != 0 || !result) {
        LOG_ERR("[MsgClient][Conn %zu][EP %zu] getaddrinfo failed for %s:%u: %s",
                conn_idx, ep_idx, ep.host.c_str(), ep.port, gai_strerror(rc));
        return false;
    }

    conn.resolved_endpoints[ep_idx].addr_len = result->ai_addrlen;
    std::memcpy(&conn.resolved_endpoints[ep_idx].addr, result->ai_addr, result->ai_addrlen);
    conn.resolved_endpoints[ep_idx].resolved = true;
    ::freeaddrinfo(result);
    return true;
}

bool MsgClient::resolveHost(size_t conn_idx) {
    if (conn_idx >= connections_.size()) return false;

    const ConnectionState& conn = *connections_[conn_idx];
    bool any_ok = false;

    for (size_t i = 0; i < conn.config.endpoints.size(); ++i) {
        if (resolveEndpoint(conn_idx, i)) {
            any_ok = true;
        }
    }

    if (!any_ok) {
        LOG_ERR("[MsgClient][Conn %zu] Failed to resolve any endpoint", conn_idx);
    }
    return any_ok;
}

// ============================================================================
// Socket Setup Helpers
// ============================================================================
// These helpers configure a newly-created socket before connect().
// Extracted from connectToServer() to reduce its length and nesting.
// ============================================================================

static void applyKeepalive(int fd, size_t conn_idx, int tcp_keepidle, int tcp_keepintvl, int tcp_keepcnt) {
    int keepalive = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive)) != 0) {
        LOG_WARN("[MsgClient][Conn %zu] Failed to enable SO_KEEPALIVE: %s", conn_idx, strerror(errno));
        return;
    }

#ifdef __linux__
    if (::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &tcp_keepidle, sizeof(tcp_keepidle)) < 0) {
        LOG_WARN("[MsgClient][Conn %zu] Failed to set TCP_KEEPIDLE: %s", conn_idx, strerror(errno));
    }
    if (::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &tcp_keepintvl, sizeof(tcp_keepintvl)) < 0) {
        LOG_WARN("[MsgClient][Conn %zu] Failed to set TCP_KEEPINTVL: %s", conn_idx, strerror(errno));
    }
    if (::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &tcp_keepcnt, sizeof(tcp_keepcnt)) < 0) {
        LOG_WARN("[MsgClient][Conn %zu] Failed to set TCP_KEEPCNT: %s", conn_idx, strerror(errno));
    }
    LOG_INFO("[MsgClient][Conn %zu] TCP Keepalive configured: idle=%ds, intvl=%ds, probes=%d",
             conn_idx, tcp_keepidle, tcp_keepintvl, tcp_keepcnt);
#else
    (void)tcp_keepidle; (void)tcp_keepintvl; (void)tcp_keepcnt;
    LOG_INFO("[MsgClient][Conn %zu] TCP Keepalive enabled (OS defaults)", conn_idx);
#endif
}

static void applyConnectTimeout(int fd, size_t conn_idx) {
    struct timeval tv;
    tv.tv_sec  = 5;
    tv.tv_usec = 0;
    if (::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) {
        LOG_WARN("[MsgClient][Conn %zu] Failed to set SO_SNDTIMEO: %s", conn_idx, strerror(errno));
    }
}

static void applyTcpNoDelay(int fd, size_t conn_idx) {
    int flag = 1;
    if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) < 0) {
        LOG_WARN("[MsgClient][Conn %zu] Failed to set TCP_NODELAY: %s", conn_idx, strerror(errno));
    }
}

static void applyRecvBufferSize(int fd, size_t conn_idx, int requested_size) {
    if (requested_size <= 0) return;

    socklen_t optlen = sizeof(requested_size);
    int old_size = 0;
    ::getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &old_size, &optlen);

    if (::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &requested_size, sizeof(requested_size)) < 0) {
        LOG_WARN("[MsgClient][Conn %zu] Failed to set SO_RCVBUF: %s", conn_idx, strerror(errno));
    }

    int actual_size = requested_size;
    ::getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &actual_size, &optlen);
    LOG_INFO("[MsgClient][Conn %zu] TCP receive buffer: %d KB (requested: %d KB, system default: %d KB)",
             conn_idx, actual_size / 1024, requested_size / 1024, old_size / 1024);
}

static bool addSocketToEpoll(int epoll_fd, int sock_fd, size_t conn_idx) {
    if (epoll_fd < 0) return true;  // epoll not available, skip

    struct epoll_event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLPRI | EPOLLERR | EPOLLHUP | EPOLLRDHUP;
    ev.data.u32 = static_cast<uint32_t>(conn_idx);

    if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock_fd, &ev) < 0) {
        LOG_ERR("[MsgClient][Conn %zu] Failed to add socket to epoll: %s", conn_idx, strerror(errno));
        return false;
    }
    LOG_DEBUG("[MsgClient][Conn %zu] Socket added to epoll", conn_idx);
    return true;
}

bool MsgClient::connectToServer(size_t conn_idx) {
    if (conn_idx >= connections_.size()) return false;

    ConnectionState& conn = *connections_[conn_idx];

    if (conn.active_endpoint_idx >= conn.config.endpoints.size()) {
        LOG_ERR("[MsgClient][Conn %zu] Invalid active endpoint index %zu",
                conn_idx, conn.active_endpoint_idx);
        return false;
    }

    if (!conn.hasResolvedEndpoint()) {
        LOG_ERR("[MsgClient][Conn %zu] No resolved address for active endpoint %zu (%s:%u)",
                conn_idx, conn.active_endpoint_idx,
                conn.activeEndpoint().host.c_str(), conn.activeEndpoint().port);
        return false;
    }

    // Create socket
    int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        LOG_ERR("[MsgClient][Conn %zu] socket() failed: %s", conn_idx, strerror(errno));
        return false;
    }
    conn.socket_guard.reset(fd);

    // Apply socket options
    applyKeepalive(fd, conn_idx, tcp_keepidle_, tcp_keepintvl_, tcp_keepcnt_);
    applyConnectTimeout(fd, conn_idx);
    applyTcpNoDelay(fd, conn_idx);
    applyRecvBufferSize(fd, conn_idx, tcp_rcvbuf_);

    // Connect using pre-resolved address of the active endpoint
    const auto& resolved = conn.activeResolved();
    int rc = ::connect(conn.socket_guard.get(),
                       reinterpret_cast<const struct sockaddr*>(&resolved.addr),
                       resolved.addr_len);

    if (rc < 0) {
        LOG_ERR("[MsgClient][Conn %zu] connect() to %s:%u (ep %zu) failed: %s",
                conn_idx, conn.activeEndpoint().host.c_str(),
                conn.activeEndpoint().port, conn.active_endpoint_idx, strerror(errno));
        conn.socket_guard.close();
        return false;
    }

    LOG_INFO("[MsgClient][Conn %zu] Connected to %s:%u (endpoint %zu/%zu)",
             conn_idx, conn.activeEndpoint().host.c_str(),
             conn.activeEndpoint().port, conn.active_endpoint_idx + 1,
             conn.config.endpoints.size());

    // Add socket to epoll for efficient I/O multiplexing
    if (!addSocketToEpoll(epoll_fd_.load(), conn.socket_guard.get(), conn_idx)) {
        conn.socket_guard.close();
        return false;
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

void MsgClient::advanceToNextEndpoint(size_t conn_idx) {
    if (conn_idx >= connections_.size()) return;
    ConnectionState& conn = *connections_[conn_idx];

    size_t old_idx = conn.active_endpoint_idx;
    conn.active_endpoint_idx = (conn.active_endpoint_idx + 1) % conn.config.endpoints.size();
    conn.consecutive_failures_on_endpoint = 0;
    conn.current_reconnect_delay_ms_ = config_.reconnect_interval_ms;

    const auto& old_ep = conn.config.endpoints[old_idx];
    const auto& new_ep = conn.config.endpoints[conn.active_endpoint_idx];
    LOG_INFO("[MsgClient][Conn %zu] Failover: %s:%u -> %s:%u (ep %zu -> %zu)",
             conn_idx, old_ep.host.c_str(), old_ep.port,
             new_ep.host.c_str(), new_ep.port,
             old_idx + 1, conn.active_endpoint_idx + 1);
}

void MsgClient::handleConnectFailure(size_t conn_idx, const char* reason) {
    ConnectionState& conn = *connections_[conn_idx];
    conn.consecutive_failures_on_endpoint++;
    conn.reconnect_count_.fetch_add(1, std::memory_order_relaxed);
    stats_.reconnect_count.fetch_add(1, std::memory_order_relaxed);

    if (conn.consecutive_failures_on_endpoint >= conn.config.max_retries_per_endpoint
        && conn.config.endpoints.size() > 1) {
        advanceToNextEndpoint(conn_idx);
    } else {
        conn.current_reconnect_delay_ms_ = std::min(
            static_cast<int>(conn.current_reconnect_delay_ms_ * Defaults::RECONNECT_BACKOFF_MULT),
            Defaults::RECONNECT_MAX_MS);
    }

    conn.next_retry_time_ = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds(conn.current_reconnect_delay_ms_);

    LOG_INFO("[MsgClient][Conn %zu] %s on %s:%u (failure %d/%d), retrying in %d ms...",
             conn_idx, reason,
             conn.activeEndpoint().host.c_str(), conn.activeEndpoint().port,
             conn.consecutive_failures_on_endpoint,
             conn.config.max_retries_per_endpoint,
             conn.current_reconnect_delay_ms_);
}

// ============================================================================
// IO Thread: epoll-based multi-connection I/O
// ============================================================================
// The IO thread is the only thread that calls connect(), recv(), and close()
// on sockets.  It maintains a RecvState per connection to handle partial
// messages across multiple recv() calls.
//
// Message framing:
//   1. Read TcpResponse header (10 bytes) to get respLen.
//   2. Validate respLen is within [MIN_MSG_LEN, MAX_MSG_LEN].
//   3. Once respLen bytes are available, create a RawMessage referencing
//      the receive buffer and push it to raw_queue_.
//   4. Any trailing bytes belong to the next message; copy them to a new
//      buffer so the current buffer can be forwarded to the decoder.
//
// Why copy trailing bytes instead of slicing the same buffer?
//   - Each RawMessage needs a contiguous buffer reference.  A shared_ptr
//     cannot represent a sub-range of an array without extra bookkeeping.
//   - Copying the small tail (usually < one message) is cheaper than the
//     complexity of multi-message buffer management.
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
        connections_[i]->next_retry_time_ = std::chrono::steady_clock::time_point();
    }

    while (running_.load()) {
        auto now = std::chrono::steady_clock::now();

        // Try to connect any disconnected connections that are due for retry
        for (size_t i = 0; i < connections_.size(); ++i) {
            if (!connected[i] && running_.load()) {
                ConnectionState& conn = *connections_[i];

                if (now < conn.next_retry_time_) {
                    continue;  // Not yet time to retry
                }

                if (!connectToServer(i)) {
                    handleConnectFailure(i, "Connection failed");
                    continue;
                }

                conn.last_recv_time_ = now;

                if (!sendSubscription(i)) {
                    closeConnection(i);
                    handleConnectFailure(i, "Subscription failed");
                    continue;
                }

                recv_states[i].recv_buf = pool_->allocate(recv_buffer_size_);
                if (!recv_states[i].recv_buf) {
                    LOG_ERR("[MsgClient][Conn %zu] Failed to allocate receive buffer, reconnecting...", i);
                    closeConnection(i);
                    handleConnectFailure(i, "Buffer allocation failed");
                    continue;
                }
                recv_states[i].recv_used = 0;

                conn.consecutive_failures_on_endpoint = 0;
                conn.current_reconnect_delay_ms_ = config_.reconnect_interval_ms;
                connected[i] = true;

                LOG_INFO("[MsgClient][Conn %zu] Stream active on %s:%u",
                         i, conn.activeEndpoint().host.c_str(), conn.activeEndpoint().port);
            }
        }

        // Compute epoll timeout: wake up when the next disconnected connection
        // is due for retry, but don't exceed POLL_TIMEOUT_MS.
        auto next_event = now + std::chrono::milliseconds(Defaults::POLL_TIMEOUT_MS);
        for (size_t i = 0; i < connections_.size(); ++i) {
            if (!connected[i]) {
                if (connections_[i]->next_retry_time_ < next_event) {
                    next_event = connections_[i]->next_retry_time_;
                }
            }
        }
        int epoll_timeout_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(next_event - now).count());
        if (epoll_timeout_ms < 0) epoll_timeout_ms = 0;

        // Wait for events using epoll
        struct epoll_event events[Defaults::MAX_CONNECTIONS];
        int ret = ::epoll_wait(epoll_fd_.load(), events, Defaults::MAX_CONNECTIONS, epoll_timeout_ms);
        
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

            bool should_close = false;

            // Check for hard errors first
            if (events[eidx].events & (EPOLLERR | EPOLLHUP)) {
                should_close = true;
            }

            if (events[eidx].events & EPOLLIN) {
                // Receive data
                {
                    RecvState& recv_state = recv_states[conn_idx];
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
                        ssize_t n = ::recv(connections_[conn_idx]->socket_guard.get(),
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
                            connections_[conn_idx]->last_recv_time_ = std::chrono::steady_clock::now();

                            recv_state.recv_used += static_cast<size_t>(n);
                            connections_[conn_idx]->bytes_received_.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
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
                                connections_[conn_idx]->last_received_seq_.store(resp.respSeq, std::memory_order_relaxed);

                                if (!raw_queue_->push_wait(std::move(raw), config_.queue_push_timeout_ms)) {
                                    stats_.messages_dropped.fetch_add(1, std::memory_order_relaxed);
                                    LOG_WARN("[MsgClient][Conn %zu] Message dropped: raw queue full (seq=%lu)",
                                             conn_idx, raw.seq_num);
                                }

                                connections_[conn_idx]->messages_received_.fetch_add(1, std::memory_order_relaxed);
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
            }

            if (events[eidx].events & EPOLLRDHUP) {
                should_close = true;
            }

            if (should_close) {
                connected[conn_idx] = false;
                closeConnection(conn_idx);
            }
        }
        
        // Note: Intentionally no idle-timeout disconnect here.
        // Market data can legitimately be quiet for extended periods
        // (overnight, holidays, pre-open). Forcing a reconnect would
        // be counter-productive — TCP keepalive handles dead peers.
    }
}

// ============================================================================
// Decoder Thread: parse raw → SubMessage, round-robin to per-worker queues
// ============================================================================
// The decoder thread is the single consumer of raw_queue_ and the single
// producer for all decoded_queues_.  It performs the only parsing of the
// wire format, keeping the IO thread lightweight.
//
// Zero-copy design:
//   - SubMessage holds a shared_ptr to the SAME Buffer as RawMessage.
//   - Only pointers and lengths are copied; no message body data is moved.
//   - The Buffer stays alive until the last worker releases its reference.
//
// Round-robin load balancing:
//   - Simple modulo distribution ensures even spreading across workers.
//   - No need for work-stealing because all messages are independent.
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
// Each worker thread owns one decoded_queues_[worker_index].  There is no
// contention between workers — each has exclusive consume access.
//
// Aggregation pipeline (optional):
//   - If aggregation_config.enabled == true, the worker parses the message
//     body to extract market-data fields and feeds them into the appropriate
//     Aggregator (orders, trades, or quotes).
//   - Aggregators are thread-safe internally (sharded by hash of tags).
//   - Flushing happens asynchronously via DiskWriter.
//
// Handler invocation:
//   - The user-supplied handler_ is called for EVERY message, even when
//     aggregation is enabled.  This gives the user full control.
//   - handler_ receives (SubMessage, worker_index, connection_id).
// ============================================================================

void MsgClient::workerLoop(size_t worker_index) {
    SubMessage msg;

    while (running_.load()) {
        if (!decoded_queues_[worker_index]->pop_wait(msg, Defaults::QUEUE_POP_TIMEOUT_MS)) {
            continue; // Timeout — re-check running_
        }
        
        // Process for aggregation if enabled
        if (orders_aggregator_) {
            MarketDataType msg_type = parseMessageType(msg.body, msg.body_length);
            if (msg_type != MarketDataType::UNKNOWN) {
                const MsgHeader* hdr = reinterpret_cast<const MsgHeader*>(msg.body);
                uint64_t ts_ns = hdr->timestamp_ns;

                switch (msg_type) {
                    case MarketDataType::ORDER_NEW: {
                        const auto* m = castMessage<OrderNewMsg>(msg.body, msg.body_length);
                        if (m) {
                            metrics::TagSet tags;
                            tags.add("Market", m->getMarket());
                            tags.add("Instrument", m->getInstrument());
                            tags.add("Broker", m->getBroker());
                            orders_aggregator_->add(tags, "newOrders", static_cast<int64_t>(1), ts_ns);
                            orders_aggregator_->add(tags, "openOrders", static_cast<int64_t>(1), ts_ns);
                            orders_aggregator_->add(tags, "totalOrderQty", static_cast<int64_t>(m->quantity), ts_ns);
                            orders_aggregator_->onIncomingTimestamp(ts_ns);
                        }
                        break;
                    }
                    case MarketDataType::ORDER_UPDATE: {
                        const auto* m = castMessage<OrderUpdateMsg>(msg.body, msg.body_length);
                        if (m) {
                            metrics::TagSet tags;
                            tags.add("Market", m->getMarket());
                            tags.add("Instrument", m->getInstrument());
                            tags.add("Broker", m->getBroker());
                            orders_aggregator_->add(tags, "modifiedOrders", static_cast<int64_t>(1), ts_ns);
                            orders_aggregator_->onIncomingTimestamp(ts_ns);
                        }
                        break;
                    }
                    case MarketDataType::ORDER_CANCEL: {
                        const auto* m = castMessage<OrderCancelMsg>(msg.body, msg.body_length);
                        if (m) {
                            metrics::TagSet tags;
                            tags.add("Market", m->getMarket());
                            tags.add("Instrument", m->getInstrument());
                            tags.add("Broker", m->getBroker());
                            orders_aggregator_->add(tags, "cancelledOrders", static_cast<int64_t>(1), ts_ns);
                            orders_aggregator_->add(tags, "totalCancelQty", static_cast<int64_t>(m->cancelled_qty), ts_ns);
                            orders_aggregator_->onIncomingTimestamp(ts_ns);
                        }
                        break;
                    }
                    case MarketDataType::TRADE: {
                        const auto* m = castMessage<TradeMsg>(msg.body, msg.body_length);
                        if (m) {
                            metrics::TagSet tags;
                            tags.add("Market", m->getMarket());
                            tags.add("Instrument", m->getInstrument());
                            trades_aggregator_->add(tags, "numTrades", static_cast<int64_t>(1), ts_ns);
                            trades_aggregator_->add(tags, "totalVolume", static_cast<int64_t>(m->quantity), ts_ns);
                            trades_aggregator_->add(tags, "totalValue", m->price * m->quantity, ts_ns);
                            trades_aggregator_->set(tags, "highPrice", m->price, ts_ns);
                            trades_aggregator_->set(tags, "lowPrice", m->price, ts_ns);
                            trades_aggregator_->onIncomingTimestamp(ts_ns);
                        }
                        break;
                    }
                    case MarketDataType::QUOTE_BID: {
                        const auto* m = castMessage<QuoteBidMsg>(msg.body, msg.body_length);
                        if (m) {
                            metrics::TagSet tags;
                            tags.add("Market", m->getMarket());
                            tags.add("Instrument", m->getInstrument());
                            quotes_aggregator_->add(tags, "bidUpdates", static_cast<int64_t>(1), ts_ns);
                            quotes_aggregator_->set(tags, "bestBid", m->price, ts_ns);
                            quotes_aggregator_->set(tags, "bestBidQty", static_cast<int64_t>(m->quantity), ts_ns);
                            quotes_aggregator_->onIncomingTimestamp(ts_ns);
                        }
                        break;
                    }
                    case MarketDataType::QUOTE_ASK: {
                        const auto* m = castMessage<QuoteAskMsg>(msg.body, msg.body_length);
                        if (m) {
                            metrics::TagSet tags;
                            tags.add("Market", m->getMarket());
                            tags.add("Instrument", m->getInstrument());
                            quotes_aggregator_->add(tags, "askUpdates", static_cast<int64_t>(1), ts_ns);
                            quotes_aggregator_->set(tags, "bestAsk", m->price, ts_ns);
                            quotes_aggregator_->set(tags, "bestAskQty", static_cast<int64_t>(m->quantity), ts_ns);
                            quotes_aggregator_->onIncomingTimestamp(ts_ns);
                        }
                        break;
                    }
                    default:
                        break;
                }
            }
        }

        // Invoke handler with connection_id
        if (handler_) {
            handler_(msg, worker_index, msg.connection_id);
        }

        stats_.messages_processed.fetch_add(1, std::memory_order_relaxed);

        // Release buffer reference
        // Explicit reset() here documents the hand-off: the buffer may still be
        // alive if the user's handler captured a copy of msg.buffer.
        msg.buffer.reset();
    }
}
