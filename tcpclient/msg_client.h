#ifndef MSG_CLIENT_H
#define MSG_CLIENT_H

#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <functional>
#include <cstdint>
#include <unordered_map>

#include "lockfree_ringbuffer.h"
#include "shared_ptr_pool.h"
#include "protocol.h"

// Aggregation support
#include "aggregation/aggregation_config.h"
#include "aggregation/aggregation_manager.h"
#include "aggregation/output_queue.h"
#include "aggregation/output_writer.h"

#include <sys/socket.h>  // For shutdown(), SHUT_RDWR
#include <unistd.h>      // For close()

// ============================================================================
// RAII Socket Wrapper - ensures socket is always closed properly
// ============================================================================

class SocketGuard {
public:
    SocketGuard() : fd_(-1) {}
    explicit SocketGuard(int fd) : fd_(fd) {}

    ~SocketGuard() { close(); }

    // Non-copyable
    SocketGuard(const SocketGuard&) = delete;
    SocketGuard& operator=(const SocketGuard&) = delete;

    // Movable
    SocketGuard(SocketGuard&& other) noexcept : fd_(other.release()) {}
    SocketGuard& operator=(SocketGuard&& other) noexcept {
        if (this != &other) {
            close();
            fd_.store(other.release(), std::memory_order_relaxed);
        }
        return *this;
    }

    // Reset with new fd
    void reset(int fd = -1) {
        if (fd_.load(std::memory_order_relaxed) != fd) {
            close();
            fd_.store(fd, std::memory_order_relaxed);
        }
    }

    // Release ownership without closing
    int release() noexcept {
        return fd_.exchange(-1, std::memory_order_relaxed);
    }

    // Close the socket
    void close() {
        int fd = fd_.exchange(-1, std::memory_order_relaxed);
        if (fd >= 0) {
            ::shutdown(fd, SHUT_RDWR);
            ::close(fd);
        }
    }

    // Get the fd
    int get() const noexcept { return fd_.load(std::memory_order_relaxed); }

    // Check if valid
    bool valid() const noexcept { return fd_.load(std::memory_order_relaxed) >= 0; }

    // Boolean conversion
    explicit operator bool() const noexcept { return valid(); }

private:
    std::atomic<int> fd_;
};

// ============================================================================
// Configuration Constants
// ============================================================================

// Default configuration values
namespace Defaults {
    constexpr const char*   HOST                 = "127.0.0.1";
    constexpr uint16_t      PORT                 = 8888;
    constexpr const char*   ITEM_NAME            = "default";
    constexpr const char*   CLIENT_ID            = "MsgClient";
    constexpr uint64_t      STARTING_SEQ_NUM     = 0;
    
    constexpr size_t        IO_THREAD_COUNT      = 1;     // Fixed: single IO thread
    constexpr size_t        DECODER_THREAD_COUNT = 1;     // Fixed: single decoder thread
    constexpr size_t        WORKER_THREAD_COUNT  = 2;     // Configurable: 1-64
    constexpr size_t        MIN_WORKER_THREADS   = 1;
    constexpr size_t        MAX_WORKER_THREADS   = 64;
    constexpr size_t        MAX_CONNECTIONS      = 64;    // Maximum number of connections
    
    constexpr size_t        RAW_QUEUE_SIZE       = 16384; // SPSC queue: IO → decoder
    constexpr size_t        DECODED_QUEUE_SIZE   = 16384; // Per-worker SPSC queue
    
    constexpr int           RECONNECT_INTERVAL_MS= 3000;
    constexpr int           RECONNECT_MIN_MS     = 1000;  // Exponential backoff min
    constexpr int           RECONNECT_MAX_MS     = 60000; // Exponential backoff max (1 min)
    constexpr double        RECONNECT_BACKOFF_MULT = 2.0; // Backoff multiplier
    constexpr int           STATS_INTERVAL_SEC   = 5;
    
    // Timeouts
    constexpr int           POLL_TIMEOUT_MS      = 100;   // poll() wait time
    constexpr int           QUEUE_POP_TIMEOUT_MS = 100;   // Queue pop wait time
    constexpr int           THREAD_JOIN_TIMEOUT_MS = 30000; // Thread join timeout (30s - very generous)
    
    // Connection health check
    // Default: 0 (disabled) - accommodates busy/quiet periods during the day.
    // If enabled, force reconnect if no data for specified milliseconds.
    constexpr int           CONN_IDLE_TIMEOUT_MS = 0;

    // Failover: max retries on same endpoint before switching to next
    constexpr int           MAX_RETRIES_PER_ENDPOINT = 2;

    // Push wait timeout for queues (when full)
    // NOTE: This uses a "drop" strategy rather than backpressure.
    // 
    // Why drop instead of backpressure?
    // - Backpressure (stopping recv()) causes TCP buffer buildup
    // - This can exhaust server memory when serving multiple clients
    // - A slow/bad client could DoS the server and affect other clients
    // - Dropping messages protects the server from rogue clients
    //
    // This is appropriate for:
    // - Real-time data where freshness matters more than completeness
    // - Pub/sub systems where clients shouldn't affect each other
    // - High-frequency streams where old data has no value
    //
    // Use a longer timeout (or 0 = wait forever) only if you have:
    // - 1:1 client-server relationship, OR
    // - Server has per-client isolation and circuit breakers
    constexpr int           QUEUE_PUSH_TIMEOUT_MS = 5;
    constexpr int           MIN_QUEUE_PUSH_TIMEOUT_MS = -1;  // -1 = no wait
    constexpr int           MAX_QUEUE_PUSH_TIMEOUT_MS = 60000; // 60 seconds max
    
    // Queue size limits
    constexpr size_t        MIN_QUEUE_SIZE       = 64;
    constexpr size_t        MAX_QUEUE_SIZE       = 1048576;  // 1M entries
    
    // Network limits
    constexpr uint16_t      MIN_PORT             = 1;
    constexpr uint16_t      MAX_PORT             = 65535;
    constexpr int           MIN_RECONNECT_MS     = 100;
    constexpr int           MAX_RECONNECT_MS     = 300000;  // 5 minutes
    
    // Protocol limits (from protocol.h)
    constexpr size_t        MAX_ITEM_NAME_LEN    = 32;
    constexpr size_t        MAX_CLIENT_ID_LEN    = 32;
}

// ============================================================================
// Endpoint Configuration (one IP:port pair)
// ============================================================================

struct EndpointConfig {
    std::string host;
    uint16_t    port;
};

// ============================================================================
// Per-Connection Configuration
// ============================================================================

struct ConnectionConfig {
    std::vector<EndpointConfig> endpoints;
    uint16_t    default_port            = Defaults::PORT;  // fallback for host entries without :port
    int         max_retries_per_endpoint= Defaults::MAX_RETRIES_PER_ENDPOINT;
    std::string item_name               = Defaults::ITEM_NAME;
    std::string client_id               = Defaults::CLIENT_ID;
    uint64_t    starting_seq_num        = Defaults::STARTING_SEQ_NUM;

    // Validate this connection configuration
    std::string validate() const;
};

// ============================================================================
// Global Client Configuration
// ============================================================================

struct MsgClientConfig {
    // Multiple connection configurations
    std::vector<ConnectionConfig> connections;
    
    // Thread configuration (shared across all connections)
    size_t worker_thread_count          = Defaults::WORKER_THREAD_COUNT;
    
    // Queue sizes
    size_t raw_queue_size               = Defaults::RAW_QUEUE_SIZE;
    size_t decoded_queue_size           = Defaults::DECODED_QUEUE_SIZE;
    
    // Timing
    int    reconnect_interval_ms        = Defaults::RECONNECT_INTERVAL_MS;
    
    // Queue push timeout in milliseconds (0 = wait forever, -1 = don't wait)
    int    queue_push_timeout_ms        = Defaults::QUEUE_PUSH_TIMEOUT_MS;

    // Memory pool configuration
    std::vector<SizeClassConfig> pool_config; // Empty = use defaults
    
    // Aggregation configuration
    aggregation::AggregationConfig aggregation_config;
    
    // Add a connection (convenience method)
    void addConnection(const ConnectionConfig& conn);
    void addConnection(const std::string& host, uint16_t port, 
                       const std::string& item,
                       const std::string& client_id = Defaults::CLIENT_ID,
                       uint64_t start_seq = Defaults::STARTING_SEQ_NUM);
    
    // Validate entire configuration
    std::string validate() const;
};

// ============================================================================
// Resolved endpoint (cached getaddrinfo result)
// ============================================================================

struct ResolvedEndpoint {
    struct sockaddr_storage addr{};
    socklen_t addr_len = 0;
    bool resolved = false;
};

// ============================================================================
// Per-Connection State
// ============================================================================

struct ConnectionState {
    ConnectionConfig config;                    // Connection configuration
    SocketGuard socket_guard;                   // Socket handle
    int current_reconnect_delay_ms_;            // Current backoff delay
    std::chrono::steady_clock::time_point last_recv_time_; // Last data received
    std::atomic<uint64_t> last_received_seq_;   // Last sequence number received
    std::atomic<uint64_t> messages_received_;   // Messages received on this connection
    std::atomic<uint64_t> bytes_received_;      // Bytes received on this connection
    std::atomic<uint64_t> reconnect_count_;     // Reconnect count for this connection

    // Failover state
    size_t active_endpoint_idx = 0;             // Currently active endpoint
    int consecutive_failures_on_endpoint = 0;   // Failures on current endpoint since last success
    std::vector<ResolvedEndpoint> resolved_endpoints; // One per config endpoint

    ConnectionState(const ConnectionConfig& cfg);

    // Current endpoint accessors
    const EndpointConfig& activeEndpoint() const;
    const ResolvedEndpoint& activeResolved() const;
    bool hasResolvedEndpoint() const;

    // Disable copy and move - stored via unique_ptr
    ConnectionState(const ConnectionState&) = delete;
    ConnectionState& operator=(const ConnectionState&) = delete;
    ConnectionState(ConnectionState&&) = delete;
    ConnectionState& operator=(ConnectionState&&) = delete;
};

// ============================================================================
// Statistics (atomic counters, relaxed ordering)
// ============================================================================

struct MsgClientStats {
    std::atomic<uint64_t> messages_received{0};
    std::atomic<uint64_t> messages_decoded{0};
    std::atomic<uint64_t> messages_processed{0};
    std::atomic<uint64_t> messages_dropped{0};  // Dropped due to full queue
    std::atomic<uint64_t> bytes_received{0};
    std::atomic<uint64_t> reconnect_count{0};
    std::atomic<uint64_t> parse_errors{0};
};

struct ConnectionStats {
    uint64_t connection_id;     // Index into connections vector
    uint64_t messages_received;
    uint64_t bytes_received;
    uint64_t reconnect_count;
    uint64_t last_seq_num;
    bool connected;
    std::string endpoint;       // "host:port"
    std::string item_name;
};

struct StatsSnapshot {
    uint64_t messages_received;
    uint64_t messages_decoded;
    uint64_t messages_processed;
    uint64_t messages_dropped;
    uint64_t bytes_received;
    uint64_t reconnect_count;
    uint64_t parse_errors;
    std::vector<ConnectionStats> connection_stats;
};

// ============================================================================
// Message handler signature: (message, worker_thread_index, connection_id)
// ============================================================================

using MessageHandler = std::function<void(const SubMessage& msg, size_t worker_index, size_t connection_id)>;

// ============================================================================
// MsgClient — Multi-connection Three-stage Lock-Free Pipeline TCP Client
// ============================================================================

class MsgClient {
public:
    explicit MsgClient(const MsgClientConfig& config);
    ~MsgClient();

    // Non-copyable, non-movable
    MsgClient(const MsgClient&) = delete;
    MsgClient& operator=(const MsgClient&) = delete;

    // Set the message handler (must be called before start)
    void setMessageHandler(MessageHandler handler);

    // Start all threads and begin processing
    void start();

    // Stop all threads and disconnect
    void stop();

    // Get a snapshot of current statistics
    StatsSnapshot getStats() const;

    // Get memory pool statistics
    std::vector<MemoryPool::Stats> getPoolStats() const;

    // Check if the client is running
    bool isRunning() const;

private:
    // Thread entry points
    void ioLoop();
    void decoderLoop();
    void workerLoop(size_t worker_index);

    // Connection helpers
    bool resolveHost(size_t conn_idx);              // Resolve ALL endpoints for a connection
    bool resolveEndpoint(size_t conn_idx, size_t ep_idx); // Resolve single endpoint
    bool connectToServer(size_t conn_idx);          // Connect to active endpoint
    bool sendSubscription(size_t conn_idx);
    void closeConnection(size_t conn_idx);
    void closeAllSockets();

    // Failover: advance to next endpoint after max retries
    void advanceToNextEndpoint(size_t conn_idx);

    // Configuration
    MsgClientConfig config_;

    // Memory pool (must outlive queues and threads)
    std::unique_ptr<MemoryPool> pool_;

    // Lock-free queues
    // IO thread → decoder thread (single SPSC, handles all connections)
    std::unique_ptr<LockFreeRingBuffer<RawMessage>> raw_queue_;

    // Decoder thread → worker threads (one SPSC per worker)
    std::vector<std::unique_ptr<LockFreeRingBuffer<SubMessage>>> decoded_queues_;

    // Connection states (one per configured connection)
    std::vector<std::unique_ptr<ConnectionState>> connections_;

    // Threads
    std::thread io_thread_;
    std::thread decoder_thread_;
    std::vector<std::thread> worker_threads_;

    // Message handler
    MessageHandler handler_;

    // Control flag
    std::atomic<bool> running_;

    // Global statistics
    MsgClientStats stats_;
    
    // Epoll instance for efficient multi-connection I/O (Linux only)
    std::atomic<int> epoll_fd_;

    // Cached environment-derived constants (read once at construction)
    size_t recv_buffer_size_ = 0;
    int    tcp_keepidle_ = 0;
    int    tcp_keepintvl_ = 0;
    int    tcp_keepcnt_ = 0;
    int    tcp_rcvbuf_ = 0;
    
    // Aggregation components
    std::unique_ptr<aggregation::AggregationManager> aggregation_manager_;
    std::unique_ptr<aggregation::OutputQueue> output_queue_;
    std::unique_ptr<aggregation::OutputWriter> output_writer_;
};

#endif // MSG_CLIENT_H
