#include "output_queue.h"

#include <algorithm>

namespace aggregation {

OutputQueue::OutputQueue() = default;

void OutputQueue::push(OutputRecord record) {
    std::unique_lock<std::mutex> lock(mutex_);
    
    // Check if queue is full (if max_size is set)
    if (max_size_ > 0 && queue_.size() >= max_size_) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    
    queue_.push(std::move(record));
    lock.unlock();
    
    cv_pop_.notify_one();
}

bool OutputQueue::pop(OutputRecord& record, int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    
    // Wait until data available or timeout/shutdown
    bool has_data = cv_pop_.wait_for(lock, 
        std::chrono::milliseconds(timeout_ms),
        [this] { return !queue_.empty() || shutdown_.load(); });
    
    if (!has_data || queue_.empty()) {
        return false;
    }
    
    record = std::move(queue_.front());
    queue_.pop();
    return true;
}

size_t OutputQueue::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

bool OutputQueue::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.empty();
}

void OutputQueue::shutdown() {
    shutdown_.store(true, std::memory_order_release);
    cv_pop_.notify_all();
}

} // namespace aggregation
