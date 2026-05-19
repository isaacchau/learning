// ============================================================================
// shared_ptr_pool.h — Size-class memory pool with custom shared_ptr deleters
// ============================================================================
// Provides zero-copy buffer sharing across the three-stage pipeline.
//
// Why a pool instead of malloc/free per message?
//   - Eliminates allocation overhead on the hot path (IO thread).
//   - Prevents heap fragmentation under sustained high throughput.
//   - Bounded memory usage (max_total_allocated per size class).
//
// Security:
//   - Buffers are zeroed (memset) before returning to the free list.
//   - This prevents stale message data from leaking to future allocations.
//   - The zeroing happens in the worker thread, not the IO thread.
//
// Usage:
//   MemoryPool pool;
//   std::shared_ptr<Buffer> buf = pool.allocate(4096);
//   // ... use buf->data ...
//   // buf returns to pool automatically when last shared_ptr is destroyed
// ============================================================================

#ifndef SHARED_PTR_POOL_H
#define SHARED_PTR_POOL_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

// Buffer with flexible-array-member pattern.
// Allocated as raw bytes: sizeof(size_t) header + data region.
struct Buffer {
  size_t capacity; // allocated data capacity
  char data[1];    // struct hack — actual allocation extends past this

  // Factory: allocate a Buffer with `cap` bytes of data space
  static Buffer *create(size_t cap) {
    size_t alloc = offsetof(Buffer, data) + cap;
    // Note: new[]() with parentheses zero-initializes all bytes
    // This ensures buffers start clean (security - no garbage data)
    char *raw = new char[alloc]();
    Buffer *buf = reinterpret_cast<Buffer *>(raw);
    buf->capacity = cap;
    return buf;
  }

  static void destroy(Buffer *buf) { delete[] reinterpret_cast<char *>(buf); }
};

// Configuration for a single size class in the memory pool.
struct SizeClassConfig {
  size_t block_size;         // data capacity (e.g. 64, 256, 1024, ...)
  size_t initial_count;      // pre-allocated at construction
  size_t max_count;          // max blocks kept in free list
  size_t max_total_allocated;// max total allocations (0 = unlimited)
};

// Size-class memory pool.
// Returns std::shared_ptr<Buffer> with custom deleter that returns
// buffers to the pool instead of freeing them.
class MemoryPool {
public:
  static const size_t NUM_SIZE_CLASSES = 8;

  // Default size classes: 64B, 256B, 1KB, 4KB, 16KB, 64KB, 128KB, 256KB
  // Format: {block_size, initial_count, max_free_list, max_total_allocated}
  // 
  // NOTE: max_total_allocated increased for high-throughput scenarios
  // (70k+ msg/s). Each recv buffer (64KB) stays allocated until message
  // is processed. With 3 pipeline stages, need 3x burst capacity.
  static std::vector<SizeClassConfig> defaultConfig() {
    return {{64,     128, 1024, 4096},   // Small metadata
            {256,    128, 1024, 4096},   // Small messages
            {1024,   128, 1024, 4096},   // 1KB messages
            {4096,   256, 1024, 8192},   // 4KB messages
            {16384,  256, 512,  4096},   // 16KB messages
            {65536,  512, 1024, 8192},   // 64KB - recv buffer size, INCREASED
            {131072, 256, 512,  4096},   // 128KB large messages
            {262144, 128, 256,  2048}};  // 256KB max messages
  }

  explicit MemoryPool(
      const std::vector<SizeClassConfig> &config = defaultConfig()) {
    if (config.size() != NUM_SIZE_CLASSES) {
      throw std::invalid_argument(
          "MemoryPool requires exactly 8 size class configs");
    }
    for (size_t i = 0; i < NUM_SIZE_CLASSES; ++i) {
      classes_[i].block_size = config[i].block_size;
      classes_[i].max_count = config[i].max_count;
      classes_[i].max_total_allocated = config[i].max_total_allocated;
      // Pre-allocate initial buffers
      classes_[i].free_list.reserve(config[i].max_count);
      for (size_t j = 0; j < config[i].initial_count; ++j) {
        classes_[i].free_list.push_back(Buffer::create(config[i].block_size));
        classes_[i].total_allocated.fetch_add(1, std::memory_order_relaxed);
        // Note: current_allocated is NOT incremented here because
        // pre-allocated buffers start in the free list (not in use)
      }
    }
  }

  ~MemoryPool() {
    for (size_t i = 0; i < NUM_SIZE_CLASSES; ++i) {
      std::lock_guard<std::mutex> lock(classes_[i].mutex);
      for (Buffer *buf : classes_[i].free_list) {
        Buffer::destroy(buf);
      }
      classes_[i].free_list.clear();
    }
  }

  MemoryPool(const MemoryPool &) = delete;
  MemoryPool &operator=(const MemoryPool &) = delete;

  // Allocate a buffer with at least `requested_size` bytes of data capacity.
  // Returns shared_ptr with custom deleter that returns buffer to pool.
  // Returns empty shared_ptr if allocation limit reached.
  std::shared_ptr<Buffer> allocate(size_t requested_size) {
    size_t cls = findSizeClass(requested_size);
    Buffer *buf = nullptr;
    bool need_os_alloc = false;

    {
      std::lock_guard<std::mutex> lock(classes_[cls].mutex);
      if (!classes_[cls].free_list.empty()) {
        buf = classes_[cls].free_list.back();
        classes_[cls].free_list.pop_back();
        classes_[cls].current_allocated.fetch_add(1, std::memory_order_relaxed);
      } else {
        size_t max = classes_[cls].max_total_allocated;
        if (max > 0 && classes_[cls].current_allocated.load(std::memory_order_relaxed) >= max) {
          return std::shared_ptr<Buffer>();
        }
        need_os_alloc = true;
        classes_[cls].current_allocated.fetch_add(1, std::memory_order_relaxed);
      }
    }

    if (need_os_alloc) {
      buf = Buffer::create(classes_[cls].block_size);
      classes_[cls].total_allocated.fetch_add(1, std::memory_order_relaxed);
    }

    // Custom deleter returns buffer to pool (or destroys if pool is full)
    return std::shared_ptr<Buffer>(
        buf, [this, cls](Buffer *b) { this->returnBuffer(b, cls); });
  }

  // Statistics snapshot
  struct Stats {
    size_t block_size;
    size_t max_total_allocated;
    uint64_t total_allocated;
    uint64_t total_returned;
    uint64_t current_allocated;
    size_t free_count;
  };

  std::vector<Stats> getStats() const {
    std::vector<Stats> result;
    result.reserve(NUM_SIZE_CLASSES);
    for (size_t i = 0; i < NUM_SIZE_CLASSES; ++i) {
      Stats s;
      s.block_size = classes_[i].block_size;
      s.max_total_allocated = classes_[i].max_total_allocated;
      s.total_allocated =
          classes_[i].total_allocated.load(std::memory_order_relaxed);
      s.total_returned =
          classes_[i].total_returned.load(std::memory_order_relaxed);
      s.current_allocated =
          classes_[i].current_allocated.load(std::memory_order_relaxed);
      {
        std::lock_guard<std::mutex> lock(classes_[i].mutex);
        s.free_count = classes_[i].free_list.size();
      }
      result.push_back(s);
    }
    return result;
  }

private:
  struct SizeClass {
    size_t block_size = 0;
    size_t max_count = 0;
    size_t max_total_allocated = 0;      // 0 = unlimited
    std::vector<Buffer *> free_list;
    mutable std::mutex mutex;
    std::atomic<uint64_t> total_allocated{0};
    std::atomic<uint64_t> total_returned{0};
    std::atomic<uint64_t> current_allocated{0};  // Currently in use
  };

  SizeClass classes_[NUM_SIZE_CLASSES];

  // Find the smallest size class >= requested_size
  size_t findSizeClass(size_t requested_size) const {
    for (size_t i = 0; i < NUM_SIZE_CLASSES; ++i) {
      if (classes_[i].block_size >= requested_size) {
        return i;
      }
    }
    // Requested size exceeds largest class — use largest
    return NUM_SIZE_CLASSES - 1;
  }

  // Return buffer to pool (called by shared_ptr custom deleter)
  void returnBuffer(Buffer *buf, size_t class_idx) {
    classes_[class_idx].total_returned.fetch_add(1, std::memory_order_relaxed);
    classes_[class_idx].current_allocated.fetch_sub(1, std::memory_order_relaxed);

    // Security: Clear buffer contents before returning to pool.
    // This prevents sensitive data from leaking to subsequent allocations.
    // This is done in the worker thread (not IO thread), so it doesn't
    // impact network receive performance.  It is a deliberate trade-off:
    // we burn some memory-bandwidth/CPU in the worker to guarantee that
    // the next recipient of this buffer sees zeros instead of stale data.
    std::memset(buf->data, 0, buf->capacity);

    std::lock_guard<std::mutex> lock(classes_[class_idx].mutex);
    if (classes_[class_idx].free_list.size() < classes_[class_idx].max_count) {
      classes_[class_idx].free_list.push_back(buf);
    } else {
      // Pool full for this class — actually free the memory
      Buffer::destroy(buf);
    }
  }
};

#endif // SHARED_PTR_POOL_H
