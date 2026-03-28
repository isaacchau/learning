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
                
                raw_queue_->push_wait(std::move(raw));  // Pass to decoder
            }
        }
    }
}
```

**Key concept: Zero-copy**
Instead of copying message data, we share a reference to the buffer using `shared_ptr`. Multiple messages can reference different parts of the same buffer.

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

## Thread Safety Without Locks

| Component | Thread Safety | Mechanism |
|-----------|---------------|-----------|
| `raw_queue_` | IO (producer) + Decoder (consumer) | Lock-free SPSC queue |
| `decoded_queues_[i]` | Decoder (producer) + Worker i (consumer) | Lock-free SPSC queue |
| `stats_` | All threads read/write | Atomic variables |
| `pool_` | All threads allocate | Mutex per size class (acceptable contention) |

## Data Flow Summary

```
Network ──► recv buffer ──► RawMessage ──► raw_queue ──►
    (shared_ptr to buffer)

raw_queue ──► SubMessage ──► decoded_queues[i] ──► handler
    (same buffer shared,    (round-robin          (your code)
     different offsets)      distribution)
```

All stages run in parallel, connected by lock-free queues. No thread waits for another unless a queue is full/empty, and even then it's a spin-wait with backoff, not a mutex lock.
