#ifndef MSG_CLIENT_H
#define MSG_CLIENT_H

#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <functional>
#include <cstdint>

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

// Join a thread with timeout (milliseconds)
// Returns true if thread joined successfully, false if timeout
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

// ============================================================================
// Configuration Constants
// ============================================================================

// Default configuration values
namespace Defaults {
    constexpr const char*   HOST                 = "127.0.0.1";
    constexpr uint16_t      PORT                 = 8888;
    constexpr const char*   ITEM_NAME            = "default";
    constexpr uint64_t      STARTING_SEQ_NUM     = 0;
    
    constexpr size_t        IO_THREAD_COUNT      = 1;     // Fixed: single IO thread
    constexpr size_t        DECODER_THREAD_COUNT = 1;     // Fixed: single decoder thread
    constexpr size_t        WORKER_THREAD_COUNT  = 2;     // Configurable: 1-64
    constexpr size_t        MIN_WORKER_THREADS   = 1;
    constexpr size_t        MAX_WORKER_THREADS   = 64;
    
    constexpr size_t        RAW_QUEUE_SIZE       = 8192;  // SPSC queue: IO → decoder
    constexpr size_t        DECODED_QUEUE_SIZE   = 8192;  // Per-worker SPSC queue
    
    constexpr int           RECONNECT_INTERVAL_MS= 3000;
    constexpr int           STATS_INTERVAL_SEC   = 5;
    
    // Timeouts
    constexpr int           POLL_TIMEOUT_MS      = 100;   // poll() wait time
    constexpr int           QUEUE_POP_TIMEOUT_MS = 100;   // Queue pop wait time
    constexpr int           THREAD_JOIN_TIMEOUT_MS = 5000; // Thread join timeout
    
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
}

// ============================================================================
// Configuration
// ============================================================================

struct MsgClientConfig {
    std::string host                    = Defaults::HOST;
    uint16_t    port                    = Defaults::PORT;
    std::string item_name               = Defaults::ITEM_NAME;
    uint64_t    starting_seq_num        = Defaults::STARTING_SEQ_NUM;

    size_t io_thread_count              = Defaults::IO_THREAD_COUNT;
    size_t decoder_thread_count         = Defaults::DECODER_THREAD_COUNT;
    size_t worker_thread_count          = Defaults::WORKER_THREAD_COUNT;

    size_t raw_queue_size               = Defaults::RAW_QUEUE_SIZE;
    size_t decoded_queue_size           = Defaults::DECODED_QUEUE_SIZE;

    int    reconnect_interval_ms        = Defaults::RECONNECT_INTERVAL_MS;
    
    // Queue push timeout in milliseconds (0 = wait forever, -1 = don't wait)
    // See Defaults::QUEUE_PUSH_TIMEOUT_MS comment for design rationale
    int    queue_push_timeout_ms        = Defaults::QUEUE_PUSH_TIMEOUT_MS;

    std::vector<SizeClassConfig> pool_config; // Empty = use defaults
};

// ============================================================================
// Statistics (atomic counters, relaxed ordering)
// ============================================================================

struct MsgClientStats {
    std::atomic<uint64_t> messages_received{0};
    std::atomic<uint64_t> messages_decoded{0};
    std::atomic<uint64_t> messages_processed{0};
    std::atomic<uint64_t> messages_dropped{0};  // Dropped due to full queue (see design note)
    std::atomic<uint64_t> bytes_received{0};
    std::atomic<uint64_t> reconnect_count{0};
    std::atomic<uint64_t> parse_errors{0};
    std::atomic<uint64_t> queue_full_errors{0};  // Deprecated: use messages_dropped
};

struct StatsSnapshot {
    uint64_t messages_received;
    uint64_t messages_decoded;
    uint64_t messages_processed;
    uint64_t messages_dropped;  // Dropped due to full queue
    uint64_t bytes_received;
    uint64_t reconnect_count;
    uint64_t parse_errors;
    uint64_t queue_full_errors;  // Deprecated: use messages_dropped
};

// ============================================================================
// Message handler signature: (message, worker_thread_index)
// ============================================================================

using MessageHandler = std::function<void(const SubMessage& msg, size_t worker_index)>;

// ============================================================================
// MsgClient — Three-stage lock-free pipeline TCP client
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

    // Check if the client is running
    bool isRunning() const;

private:
    // Thread entry points
    void ioLoop();
    void decoderLoop();
    void workerLoop(size_t worker_index);

    // Connection helpers
    bool connectToServer();
    void sendSubscription();
    void closeSocket();

    // Configuration
    MsgClientConfig config_;

    // Memory pool (must outlive queues and threads)
    std::unique_ptr<MemoryPool> pool_;

    // Lock-free queues
    // IO thread → decoder thread (single SPSC)
    std::unique_ptr<LockFreeRingBuffer<RawMessage>> raw_queue_;

    // Decoder thread → worker threads (one SPSC per worker)
    std::vector<std::unique_ptr<LockFreeRingBuffer<SubMessage>>> decoded_queues_;

    // Threads
    std::thread io_thread_;
    std::thread decoder_thread_;
    std::vector<std::thread> worker_threads_;

    // Message handler
    MessageHandler handler_;

    // Socket (RAII wrapped)
    SocketGuard socket_guard_;

    // Control flag
    std::atomic<bool> running_;

    // Statistics
    MsgClientStats stats_;
};

#endif // MSG_CLIENT_H
