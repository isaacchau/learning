# Additional Test Case Suggestions

These test cases can be added to `tests/test_main.cpp` without modifying the production code.

## 1. Protocol Edge Cases

```cpp
TEST(protocol_malformed_messages) {
    // Test parsing of malformed/corrupted messages
    MemoryPool pool;
    auto buf = pool.allocate(100);
    
    // Write garbage data simulating corrupted network packet
    std::memset(buf->data, 0xFF, 100);
    
    RawMessage raw;
    raw.buffer = buf;
    raw.offset = 0;
    raw.length = 100;
    raw.seq_num = 1;
    
    // The decoder should handle this gracefully (drop the message)
    // We verify the raw message structure works even with garbage
    ASSERT_EQ(100, raw.length);
    ASSERT_EQ(1, raw.seq_num);
}

TEST(protocol_sequence_number_wraparound) {
    // Test that sequence numbers work correctly at uint64_t boundaries
    // This would take forever in reality, but we can verify the type
    SubMessage msg;
    msg.seq_num = UINT64_MAX;  // Max value
    ASSERT_EQ(UINT64_MAX, msg.seq_num);
    
    // Verify overflow behavior (wraps to 0)
    msg.seq_num = msg.seq_num + 1;
    ASSERT_EQ(0, msg.seq_num);  // Unsigned overflow is defined behavior
}
```

## 2. Memory Pool Stress Tests

```cpp
TEST(pool_exhaustion_recovery) {
    // Create pool with very small limits
    std::vector<SizeClassConfig> config = {
        {64, 1, 2, 2},    // Only 2 buffers max
        {256, 0, 0, 0},   // No pre-allocation
        // ... rest default
    };
    
    MemoryPool pool(config);
    
    // Exhaust the pool
    auto buf1 = pool.allocate(64);
    auto buf2 = pool.allocate(64);
    auto buf3 = pool.allocate(64);  // Should fail - limit reached
    
    ASSERT_TRUE(buf1 != nullptr);
    ASSERT_TRUE(buf2 != nullptr);
    ASSERT_FALSE(buf3);  // Should be empty/null
    
    // Release one and try again
    buf1.reset();
    auto buf4 = pool.allocate(64);
    ASSERT_TRUE(buf4 != nullptr);
}

TEST(pool_high_churn) {
    // Simulate high allocation/deallocation rate
    MemoryPool pool;
    const int ITERATIONS = 10000;
    
    for (int i = 0; i < ITERATIONS; ++i) {
        auto buf = pool.allocate(1024);
        ASSERT_TRUE(buf != nullptr);
        // Buffer automatically returns to pool
    }
    
    // After many iterations, pool should still work
    auto final_buf = pool.allocate(1024);
    ASSERT_TRUE(final_buf != nullptr);
}
```

## 3. Queue Stress Tests

```cpp
TEST(ringbuffer_empty_operations) {
    LockFreeRingBuffer<int> rb(16);
    int val = 999;
    
    // Pop from empty should fail
    ASSERT_FALSE(rb.pop(val));
    ASSERT_EQ(999, val);  // val should be unchanged
    ASSERT_TRUE(rb.empty());
    ASSERT_EQ(0, rb.size());
}

TEST(ringbuffer_push_pop_interleaved) {
    LockFreeRingBuffer<int> rb(4);
    int val;
    
    // Interleaved push/pop
    ASSERT_TRUE(rb.push(1));
    ASSERT_TRUE(rb.push(2));
    ASSERT_TRUE(rb.pop(val)); ASSERT_EQ(1, val);
    ASSERT_TRUE(rb.push(3));
    ASSERT_TRUE(rb.pop(val)); ASSERT_EQ(2, val);
    ASSERT_TRUE(rb.push(4));
    ASSERT_TRUE(rb.push(5));
    ASSERT_TRUE(rb.pop(val)); ASSERT_EQ(3, val);
    ASSERT_TRUE(rb.pop(val)); ASSERT_EQ(4, val);
    ASSERT_TRUE(rb.pop(val)); ASSERT_EQ(5, val);
    ASSERT_TRUE(rb.empty());
}
```

## 4. Configuration Validation Tests

```cpp
TEST(config_default_values) {
    // Test that default configuration is valid
    MsgClientConfig config;
    std::string error = config.validate();
    ASSERT_TRUE(error.empty());
    
    // Verify defaults
    ASSERT_EQ(std::string("127.0.0.1"), config.host);
    ASSERT_EQ(8888, config.port);
    ASSERT_EQ(std::string("default"), config.item_name);
}

TEST(config_invalid_port) {
    MsgClientConfig config;
    config.port = 0;  // Invalid
    std::string error = config.validate();
    ASSERT_FALSE(error.empty());
}

TEST(config_invalid_host) {
    MsgClientConfig config;
    config.host = "";  // Empty
    std::string error = config.validate();
    ASSERT_FALSE(error.empty());
}

TEST(config_item_name_too_long) {
    MsgClientConfig config;
    config.item_name = std::string(50, 'x');  // > 32 chars
    std::string error = config.validate();
    ASSERT_FALSE(error.empty());
}
```

## 5. Statistics Counter Tests

```cpp
TEST(stats_counter_overflow) {
    // Test that uint64_t counters handle overflow correctly
    std::atomic<uint64_t> counter{UINT64_MAX};
    
    counter.fetch_add(1, std::memory_order_relaxed);
    
    // Should wrap to 0
    ASSERT_EQ(0, counter.load());
}

TEST(stats_delta_calculation) {
    // Test delta calculation with overflow
    uint64_t prev = UINT64_MAX - 10;
    uint64_t curr = 5;  // Wrapped around
    
    // Unsigned subtraction handles overflow correctly
    uint64_t delta = curr - prev;
    ASSERT_EQ(16, delta);  // 5 - (MAX-10) = 16 (with wrap)
}
```

## 6. Thread Safety Tests

```cpp
TEST(thread_atomic_operations) {
    // Verify atomic operations work correctly across threads
    std::atomic<uint64_t> counter{0};
    const int NUM_THREADS = 8;
    const int ITERATIONS = 10000;
    
    std::vector<std::thread> threads;
    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back([&counter]() {
            for (int j = 0; j < ITERATIONS; ++j) {
                counter.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    ASSERT_EQ(NUM_THREADS * ITERATIONS, counter.load());
}
```

## 7. Environmental Configuration Tests

```cpp
TEST(env_magic_key_override) {
    // Save original
    const char* original = std::getenv("APP_TCP_MAGIC_KEY");
    
    // Set test value
    setenv("APP_TCP_MAGIC_KEY", "0xDEADBEEF", 1);
    ASSERT_EQ(0xDEADBEEF, getMagicKey());
    
    // Reset
    if (original) {
        setenv("APP_TCP_MAGIC_KEY", original, 1);
    } else {
        unsetenv("APP_TCP_MAGIC_KEY");
    }
}

TEST(env_buffer_size_override) {
    const char* original = std::getenv("APP_TCP_RECV_BUFFER_SIZE");
    
    setenv("APP_TCP_RECV_BUFFER_SIZE", "131072", 1);
    ASSERT_EQ(131072, getRecvBufferSize());
    
    // Test minimum enforcement
    setenv("APP_TCP_RECV_BUFFER_SIZE", "10", 1);  // Below MIN_MSG_LEN
    ASSERT_EQ(65536, getRecvBufferSize());  // Falls back to default
    
    // Reset
    if (original) {
        setenv("APP_TCP_RECV_BUFFER_SIZE", original, 1);
    } else {
        unsetenv("APP_TCP_RECV_BUFFER_SIZE");
    }
}
```

## Implementation Priority

**High Priority (test critical logic):**
1. Configuration validation
2. Statistics counter overflow
3. Memory pool exhaustion

**Medium Priority (edge cases):**
4. Protocol edge cases
5. Queue boundary conditions
6. Environmental configuration

**Lower Priority (stress tests):**
7. High churn scenarios
8. Thread safety verification

All these tests can be added to `tests/test_main.cpp` without touching production code!
