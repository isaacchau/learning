# Architecture Deep Dive

## The Three-Stage Pipeline

This program uses a **pipeline pattern** - like an assembly line in a factory. Each stage does one thing and passes the result to the next stage.

```
┌──────────┐    ┌──────────┐    ┌──────────┐    ┌──────────┐
│ Network  │───►│   Raw    │───►│ Decoded  │───►│  Worker  │
│  Socket  │    │  Queue   │    │  Queue   │    │ Threads  │
└──────────┘    └──────────┘    └──────────┘    └──────────┘
     │               │               │               │
  IO Thread     Decoder Thread                  Worker Threads
                                                    (1-N)
```

## Stage 1: IO Thread (Network → Raw Queue)

**Job:** Read bytes from the TCP socket.

```cpp
void ioLoop() {
    while (running) {
        // 1. Connect to server (with auto-reconnect)
        connectToServer();
        
        // 2. Send subscription request
        sendSubscription();  // "I want item 'default' starting from seq 0"
        
        // 3. Receive loop
        while (running) {
            n = recv(socket, buffer, size);  // Read from network
            
            // Parse complete messages from buffer
            while (haveCompleteMessage) {
                RawMessage raw;
                raw.buffer = shared_buffer;  // Share the buffer (zero-copy)
                raw.offset = message_start;
                raw.length = message_length;
                
                // Push with timeout - drops message if queue is full
                if (!raw_queue_->push_wait(std::move(raw), timeout_ms)) {
                    stats_.messages_dropped++;  // Queue full, message dropped
                }
            }
        }
    }
}
```

**Key concept: Zero-copy**
Instead of copying message data, we share a reference to the buffer using `shared_ptr`. Multiple messages can reference different parts of the same buffer.

**Drop Strategy vs Backpressure**
When queues are full, this system uses a **drop strategy** rather than backpressure:
- **Backpressure** would stop receiving, causing TCP buffers to fill up
- **Drop strategy** discards messages when queues are full
- This protects the server from slow clients that could cause memory exhaustion
- Appropriate for real-time data where freshness matters more than completeness

## Stage 2: Decoder Thread (Raw Queue → Decoded Queues)

**Job:** Parse binary data into structured messages and distribute to workers.

```cpp
void decoderLoop() {
    size_t worker_idx = 0;  // Round-robin index
    RawMessage raw;
    
    while (running) {
        // 1. Get raw message from IO thread
        if (!raw_queue_->pop_wait(raw)) continue;
        
        // 2. Parse the binary data
        MsgHdr hdr;
        memcpy(&hdr, raw.buffer->data + offset, sizeof(MsgHdr));
        
        // 3. Create structured message
        SubMessage sub;
        sub.seq_num = hdr.msgSeqNum;
        sub.timestamp = hdr.timestamp;
        sub.body = raw.buffer->data + body_offset;
        sub.body_length = body_length;
        sub.buffer = raw.buffer;  // Keep buffer alive
        
        // 4. Distribute round-robin to workers
        decoded_queues_[worker_idx]->push_wait(std::move(sub));
        worker_idx = (worker_idx + 1) % num_workers;
    }
}
```

**Why round-robin?**
- Simple load balancing
- Maintains order per worker (messages to same worker are in order)
- No complex scheduling logic

## Stage 3: Worker Threads (Processing)

**Job:** Do actual work with the message.

```cpp
void workerLoop(size_t worker_index) {
    SubMessage msg;
    
    while (running) {
        // 1. Get message from my personal queue
        if (!decoded_queues_[worker_index]->pop_wait(msg)) continue;
        
        // 2. Process (user-defined handler)
        if (handler_) {
            handler_(msg, worker_index);
        }
        
        // 3. Message destroyed here, shared_ptr decrements
        //    Buffer freed when last reference gone
    }
}
```

**Default handler is empty** - you need to set your own:
```cpp
client.setMessageHandler([](const SubMessage &msg, size_t worker_index) {
    // Your business logic here
    std::cout << "Got message #" << msg.seq_num 
              << " with " << msg.body_length << " bytes"
              << " on worker " << worker_index << std::endl;
});
```

## The Lock-Free Queue

This is the secret sauce for high performance.

### Connection Health Check

Beyond TCP keepalive, the client can optionally monitor connection health via an **idle timeout**:

```cpp
// If no data received for N seconds, force reconnect
if (CONN_IDLE_TIMEOUT_MS > 0 && idle_time > CONN_IDLE_TIMEOUT_MS) {
    forceReconnect();
}
```

**Why this might be needed:**
- TCP keepalive can take minutes to detect dead connections (OS-dependent)
- Application-level timeout is faster and configurable
- Protects against "zombie connections" where TCP thinks it's alive but no data flows

**Default: Disabled (0)** - This accommodates busy/quiet periods during the day. To enable, modify `CONN_IDLE_TIMEOUT_MS` in `msg_client.h` (e.g., set to 60000 for 60-second timeout).

### Progressive Backoff Strategy

When a queue operation can't complete immediately, the queue uses progressive backoff instead of busy-waiting:

```
Spin iterations:  0-100     →  yield()
                 100-200    →  sleep 1μs
                 200-300    →  sleep 10μs
                 300-400    →  sleep 100μs
                 400+       →  sleep 1ms
```

This provides a balance between low latency (for short waits) and CPU efficiency (for longer waits).

### Why Lock-Free?

**Traditional approach (with locks):**
```cpp
std::queue<Message> queue;
std::mutex mutex;

// Producer
mutex.lock();
queue.push(msg);      // Other threads wait here
mutex.unlock();

// Consumer
mutex.lock();
msg = queue.front();  // Other threads wait here
queue.pop();
mutex.unlock();
```

Problems:
- Threads waste time waiting
- Cache contention (all CPUs fighting over the lock variable)
- Context switches when threads block

**Lock-free approach:**
```cpp
// Single producer, single consumer
// Uses atomic operations only - no locks!

// Producer
size_t head = head_.load();
buffer_[head & mask] = msg;
head_.store(head + 1);  // Atomic update

// Consumer
size_t tail = tail_.load();
msg = buffer_[tail & mask];
tail_.store(tail + 1);  // Atomic update
```

Benefits:
- No waiting, no blocking
- Deterministic performance
- Cache-friendly (separate cache lines for head/tail)

### How It Works

```
Ring Buffer (size = 8)

  0     1     2     3     4     5     6     7
┌─────┬─────┬─────┬─────┬─────┬─────┬─────┬─────┐
│  A  │  B  │  C  │     │     │     │     │     │
└─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┘
       ▲           ▲
       │           │
     tail         head

head = 3, tail = 1
Size = head - tail = 2 (elements A, B)
```

**Key insight:** With power-of-2 sizing, we can use bitwise AND for modulo:
```cpp
index = position & (capacity - 1);  // Fast!
// Instead of: index = position % capacity;  // Slow division
```

## Memory Pool

**The Problem:**
```cpp
// Allocating/freeing constantly is slow
while (receiving_messages) {
    char* buffer = new char[65536];  // Slow system call
    recv(socket, buffer, 65536);
    process(buffer);
    delete[] buffer;  // Slow system call
}
```

**The Solution - Memory Pool:**
```cpp
// Pre-allocate pools of different sizes
Pool 0: 64 bytes    x 512 buffers
Pool 1: 256 bytes   x 512 buffers
Pool 2: 1 KB        x 512 buffers
...
Pool 7: 256 KB      x 64 buffers

// Allocation is fast - just pop from free list
Buffer* buf = pool.allocate(1000);  // Gets 1KB from Pool 2

// Deallocation returns to pool for reuse
pool.free(buf);  // Goes back to Pool 2 free list
```

**In C++ with smart pointers:**
```cpp
// Custom deleter returns buffer to pool instead of freeing
std::shared_ptr<Buffer> allocate(size_t size) {
    Buffer* buf = getFromPool(size);
    return std::shared_ptr<Buffer>(buf, 
        [this](Buffer* b) { returnToPool(b); }  // Custom deleter
    );
}

// When last shared_ptr is destroyed, buffer returns to pool
// NOT freed to the OS - ready for reuse!
```

## RAII Resource Management

### SocketGuard

A RAII wrapper ensures sockets are always closed properly, even on exceptions:

```cpp
class SocketGuard {
    int fd_;
public:
    explicit SocketGuard(int fd) : fd_(fd) {}
    ~SocketGuard() { 
        if (fd_ >= 0) {
            ::shutdown(fd_, SHUT_RDWR);
            ::close(fd_);
        }
    }
    // Movable but not copyable
    SocketGuard(SocketGuard&& other) noexcept : fd_(other.release()) {}
    SocketGuard(const SocketGuard&) = delete;
};
```

Benefits:
- No resource leaks, even if exceptions occur
- Automatic cleanup when guard goes out of scope
- Move semantics allow ownership transfer

### Thread Lifecycle Management

Threads use a **two-phase shutdown** strategy for safety:

```
Phase 1: Graceful (0-30s)
  ├── Set running_=false
  ├── Close socket (unblocks IO thread)
  └── Wait for threads to exit naturally

Phase 2: Force Cancel (if needed, Linux only)
  ├── pthread_cancel() to request termination
  ├── Wait up to 2s for thread to stop
  └── Thread stops at next cancellation point
```

**Why not detach?** Detached threads that access member variables after destruction cause **use-after-free crashes**.

**Cancellation Points:** Threads stop at blocking operations (poll, recv, sleep) or when checking `running_`. This is safe because:
- IO thread: blocks on `poll()`/`recv()` - cancellation point
- Decoder: blocks on `pop_wait()` - uses sleep (cancellation point)
- Workers: blocks on `pop_wait()` - uses sleep (cancellation point)

On non-Linux platforms, cancellation is not available, so we wait longer (60s) as a last resort.

## Thread Safety Without Locks

| Component | Thread Safety | Mechanism |
|-----------|---------------|-----------|
| `raw_queue_` | IO (producer) + Decoder (consumer) | Lock-free SPSC queue |
| `decoded_queues_[i]` | Decoder (producer) + Worker i (consumer) | Lock-free SPSC queue |
| `stats_` | All threads read/write | Atomic variables |
| `pool_` | All threads allocate | Mutex per size class (acceptable contention) |

### Statistics Counter Overflow

All counters are `uint64_t` (max: 18,446,744,073,709,551,615).

**Overflow behavior:** Counters wrap around to 0 (standard unsigned integer overflow).

**Practical impact:** None for realistic workloads.
- At 10 million messages/second, `messages_received` would take **~58,000 years** to overflow
- For very high rates, monitor for large negative deltas in statistics reporting

**Handling overflow in delta calculations:**
```cpp
// Safe delta calculation (handles overflow correctly)
uint64_t delta = current - previous;  // Unsigned arithmetic wraps correctly
```

## Data Flow Summary

```
Network ──► recv buffer ──► RawMessage ──► raw_queue ──►
    (shared_ptr to buffer)

raw_queue ──► SubMessage ──► decoded_queues[i] ──► handler
    (same buffer shared,    (round-robin          (your code)
     different offsets)      distribution)
```

All stages run in parallel, connected by lock-free queues. No thread waits for another unless a queue is full/empty, and even then it's a spin-wait with backoff, not a mutex lock.
