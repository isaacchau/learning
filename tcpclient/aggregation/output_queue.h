#ifndef OUTPUT_QUEUE_H
#define OUTPUT_QUEUE_H

#include "output_record.h"

#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>

namespace aggregation {

// ============================================================================
// Multi-Producer Single-Consumer Output Queue
// Simple mutex-based implementation - adequate for low-frequency output
// ============================================================================

class OutputQueue {
public:
    OutputQueue();
    
    // Push a record (called by multiple worker/flush threads)
    void push(OutputRecord record);
    
    // Pop a record with timeout (called by single writer thread)
    // Returns true if record was popped, false on timeout
    bool pop(OutputRecord& record, int timeout_ms);
    
    // Get current queue size (approximate)
    size_t size() const;
    
    // Check if empty
    bool empty() const;
    
    // Signal shutdown - wakes up waiting consumers
    void shutdown();
    
    // Get dropped count (records dropped due to full queue)
    uint64_t droppedCount() const { return dropped_.load(std::memory_order_relaxed); }
    
    // Reset dropped count
    void resetDroppedCount() { dropped_.store(0, std::memory_order_relaxed); }
    
    // Set max size (0 = unlimited)
    void setMaxSize(size_t max_size) { max_size_ = max_size; }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_pop_;   // For consumer waiting
    std::condition_variable cv_push_;  // For producer backpressure (optional)
    std::queue<OutputRecord> queue_;
    
    std::atomic<bool> shutdown_{false};
    std::atomic<uint64_t> dropped_{0};
    size_t max_size_ = 0;  // 0 = unlimited
};

} // namespace aggregation

#endif // OUTPUT_QUEUE_H
