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
            fd_ = other.release();
        }
        return *this;
    }
    
    // Reset with new fd
    void reset(int fd = -1) {
        if (fd_ != fd) {
            close();
            fd_ = fd;
        }
    }
    
    // Release ownership without closing
    int release() noexcept {
        int tmp = fd_;
        fd_ = -1;
        return tmp;
    }
    
    // Close the socket
    void close() {
        if (fd_ >= 0) {
            ::shutdown(fd_, SHUT_RDWR);
            ::close(fd_);
            fd_ = -1;
        }
    }
    
    // Get the fd
    int get() const noexcept { return fd_; }
    
    // Check if valid
    bool valid() const noexcept { return fd_ >= 0; }
    
    // Boolean conversion
    explicit operator bool() const noexcept { return valid(); }
    
private:
    int fd_;
};

// ============================================================================
// Thread Join with Timeout Helper
// ============================================================================

#include <chrono>
#include <future>

#ifdef __linux__
#include <pthread.h>
#endif

// Join a thread with timeout (milliseconds).
// Returns true if thread joined successfully, false if timeout.
// NOTE: This function does NOT detach the thread on timeout - doing so would
// risk use-after-free when the destructor runs while detached threads still
// access member variables.
inline bool joinWithTimeout(std::thread& t, int timeout_ms) {
    if (!t.joinable()) return true;
    
    // Use async to wait with timeout
    auto future = std::async(std::launch::async, [&t]() {
        t.join();
    });
    
    if (future.wait_for(std::chrono::milliseconds(timeout_ms)) == std::future_status::timeout) {
        return false;  // Timeout - thread did not join
    }
    return true;
}

// Platform-specific thread cancellation (Linux only).
// This is a last-resort measure for threads that won't stop gracefully.
// NOTE: Cancellation is asynchronous and dangerous - the thread may be in
// an inconsistent state. Only use during shutdown when you're about to exit.
// Returns true if cancellation was attempted, false on non-Linux platforms.
inline bool cancelThread(std::thread& t) {
    if (!t.joinable()) return false;
    
#ifdef __linux__
    // pthread_cancel requests cancellation at the next cancellation point
    // The thread should exit if it's blocking on I/O, sleeping, or at other
    // cancellation points. This is safer than detaching because:
    // 1. Thread will stop (eventually)
    // 2. No use-after-free risk since thread stops
    int result = pthread_cancel(t.native_handle());
    if (result == 0) {
        return true;
    }
#endif
    // On non-Linux platforms, we cannot safely force-stop a thread
    return false;
}

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
// Per-Connection Configuration
// ============================================================================

struct ConnectionConfig {
    std::string host                    = Defaults::HOST;
    uint16_t    port                    = Defaults::PORT;
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
    
    ConnectionState(const ConnectionConfig& cfg);
    
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
    std::atomic<uint64_t> queue_full_errors{0};  // Deprecated: use messages_dropped
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
    uint64_t queue_full_errors;
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
    bool connectToServer(size_t conn_idx);
    bool sendSubscription(size_t conn_idx);
    void closeConnection(size_t conn_idx);
    void closeAllSockets();

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
};

#endif // MSG_CLIENT_H
