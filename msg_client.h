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

// ============================================================================
// Configuration
// ============================================================================

struct MsgClientConfig {
    std::string host;
    uint16_t    port                 = 0;
    std::string item_name;
    uint64_t    starting_seq_num     = 0;

    size_t io_thread_count           = 1;    // Fixed: single IO thread
    size_t decoder_thread_count      = 1;    // Fixed: single decoder thread
    size_t worker_thread_count       = 2;    // Configurable: 0-64

    size_t raw_queue_size            = 8192; // SPSC queue: IO → decoder
    size_t decoded_queue_size        = 8192; // Per-worker SPSC queue

    int    reconnect_interval_ms     = 3000;

    std::vector<SizeClassConfig> pool_config; // Empty = use defaults
};

// ============================================================================
// Statistics (atomic counters, relaxed ordering)
// ============================================================================

struct MsgClientStats {
    std::atomic<uint64_t> messages_received{0};
    std::atomic<uint64_t> messages_decoded{0};
    std::atomic<uint64_t> messages_processed{0};
    std::atomic<uint64_t> bytes_received{0};
    std::atomic<uint64_t> reconnect_count{0};
    std::atomic<uint64_t> parse_errors{0};
    std::atomic<uint64_t> queue_full_errors{0};
};

struct StatsSnapshot {
    uint64_t messages_received;
    uint64_t messages_decoded;
    uint64_t messages_processed;
    uint64_t bytes_received;
    uint64_t reconnect_count;
    uint64_t parse_errors;
    uint64_t queue_full_errors;
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

    // Socket
    int socket_fd_;

    // Control flag
    std::atomic<bool> running_;

    // Statistics
    MsgClientStats stats_;
};

#endif // MSG_CLIENT_H
