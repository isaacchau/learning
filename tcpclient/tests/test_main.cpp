// ============================================================================
// Unit Test Framework
// ============================================================================
// This is a minimal test framework. No external dependencies needed!
// 
// WHAT IS UNIT TESTING?
// ---------------------
// Unit tests are small programs that test individual "units" of code
// (functions, classes) in isolation. They verify that each part works
// correctly on its own.
//
// WHY DO WE NEED THEM?
// --------------------
// 1. Lock-free code is hard to get right - tests catch bugs early
// 2. Refactoring safety - tests verify changes don't break things
// 3. Documentation - tests show how code is supposed to work
// 4. Regression prevention - once a bug is fixed, a test ensures it stays fixed
//
// HOW TO RUN:
// -----------
// make test
// ./test_runner
//
// ============================================================================

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <functional>

// Simple test framework
struct Test {
    const char* name;
    std::function<bool()> func;
};

static std::vector<Test> g_tests;
static int g_passed = 0;
static int g_failed = 0;

// Register a test (called before main)
#define TEST(name) \
    static bool test_##name(); \
    static struct test_##name##_registrar { \
        test_##name##_registrar() { \
            g_tests.push_back({#name, test_##name}); \
        } \
    } test_##name##_instance; \
    static bool test_##name()

// Assertion macros
#define ASSERT_TRUE(expr) \
    do { if (!(expr)) { \
        printf("    ASSERT_TRUE(%s) FAILED at line %d\n", #expr, __LINE__); \
        return false; \
    } } while(0)

#define ASSERT_FALSE(expr) \
    do { if (expr) { \
        printf("    ASSERT_FALSE(%s) FAILED at line %d\n", #expr, __LINE__); \
        return false; \
    } } while(0)

#define ASSERT_EQ(expected, actual) \
    do { if ((expected) != (actual)) { \
        printf("    ASSERT_EQ(%s, %s) FAILED at line %d\n", \
               #expected, #actual, __LINE__); \
        return false; \
    } } while(0)

#define ASSERT_STREQ(expected, actual) \
    do { if (std::string(expected) != std::string(actual)) { \
        printf("    ASSERT_STREQ(%s, %s) FAILED at line %d: expected '%s', got '%s'\n", \
               #expected, #actual, __LINE__, expected, actual.c_str()); \
        return false; \
    } } while(0)

#define ASSERT_NE(expected, actual) \
    do { if ((expected) == (actual)) { \
        printf("    ASSERT_NE(%s, %s) FAILED at line %d\n", #expected, #actual, __LINE__); \
        return false; \
    } } while(0)

// ============================================================================
// Include files under test
// ============================================================================

#include "../lockfree_ringbuffer.h"
#include "../shared_ptr_pool.h"
#include "../protocol.h"

#include <thread>
#include <atomic>
#include <chrono>

// ============================================================================
// Test Cases
// ============================================================================

// ----------------------------------------------------------------------------
// LockFreeRingBuffer Tests
// ----------------------------------------------------------------------------
// These test the single-producer-single-consumer queue that passes messages
// between threads.
// ----------------------------------------------------------------------------

TEST(ringbuffer_basic_push_pop) {
    // Create a small ring buffer
    LockFreeRingBuffer<int> rb(16);
    
    // Initially empty
    ASSERT_TRUE(rb.empty());
    ASSERT_EQ(0, rb.size());
    
    // Push some items
    ASSERT_TRUE(rb.push(42));
    ASSERT_TRUE(rb.push(100));
    ASSERT_EQ(2, rb.size());
    ASSERT_FALSE(rb.empty());
    
    // Pop items
    int val;
    ASSERT_TRUE(rb.pop(val));
    ASSERT_EQ(42, val);
    ASSERT_TRUE(rb.pop(val));
    ASSERT_EQ(100, val);
    ASSERT_EQ(0, rb.size());
    ASSERT_TRUE(rb.empty());
    
    return true;
}

TEST(ringbuffer_capacity_limits) {
    // Create buffer of size 4
    LockFreeRingBuffer<int> rb(4);
    ASSERT_EQ(4, rb.capacity());
    
    // Fill it up
    ASSERT_TRUE(rb.push(1));
    ASSERT_TRUE(rb.push(2));
    ASSERT_TRUE(rb.push(3));
    ASSERT_TRUE(rb.push(4));
    ASSERT_EQ(4, rb.size());
    ASSERT_TRUE(rb.full());
    
    // Push to full buffer should fail (non-blocking)
    ASSERT_FALSE(rb.push(5));
    
    // Pop one, now we can push again
    int val;
    ASSERT_TRUE(rb.pop(val));
    ASSERT_TRUE(rb.push(5));
    
    return true;
}

TEST(ringbuffer_wraparound) {
    // Test that the ring buffer correctly wraps around
    LockFreeRingBuffer<int> rb(4);
    int val;
    
    // Fill, then empty multiple times to exercise wraparound
    for (int cycle = 0; cycle < 10; ++cycle) {
        // Push 4 items
        for (int i = 0; i < 4; ++i) {
            ASSERT_TRUE(rb.push(i + cycle * 100));
        }
        
        // Pop 4 items
        for (int i = 0; i < 4; ++i) {
            ASSERT_TRUE(rb.pop(val));
            ASSERT_EQ(i + cycle * 100, val);
        }
    }
    
    return true;
}

TEST(ringbuffer_spsc_threaded) {
    // Single Producer, Single Consumer test
    // This simulates the actual usage pattern (IO thread -> Decoder thread)
    LockFreeRingBuffer<int> rb(1024);
    const int NUM_ITEMS = 10000;  // Reduced for faster test
    
    std::atomic<int> consumer_count{0};
    std::atomic<int> mismatch_count{0};
    
    // Producer thread
    std::thread producer([&]() {
        for (int i = 0; i < NUM_ITEMS; ++i) {
            while (!rb.push(i)) {
                // Spin-wait (queue full)
                std::this_thread::yield();
            }
        }
    });
    
    // Consumer thread
    std::thread consumer([&]() {
        int val;
        int expected = 0;
        while (expected < NUM_ITEMS) {
            if (rb.pop(val)) {
                if (val != expected) {
                    ++mismatch_count;  // Count mismatches instead of asserting
                }
                ++expected;
                ++consumer_count;
            }
        }
    });
    
    producer.join();
    consumer.join();
    
    ASSERT_EQ(NUM_ITEMS, consumer_count.load());
    ASSERT_EQ(0, mismatch_count.load());  // Verify order was preserved
    
    return true;
}

// ----------------------------------------------------------------------------
// Memory Pool Tests
// ----------------------------------------------------------------------------
// These test the size-class memory pool for buffer allocation.
// ----------------------------------------------------------------------------

TEST(pool_basic_allocation) {
    MemoryPool pool;
    
    // Allocate a buffer
    auto buf = pool.allocate(100);
    ASSERT_TRUE(buf != nullptr);
    ASSERT_TRUE(buf->capacity >= 100);
    
    // Buffer should be usable
    buf->data[0] = 'A';
    buf->data[99] = 'Z';
    ASSERT_EQ('A', buf->data[0]);
    ASSERT_EQ('Z', buf->data[99]);
    
    // Buffer automatically returns to pool when shared_ptr is destroyed
    buf.reset();
    
    // Allocate again - should reuse from pool
    auto buf2 = pool.allocate(100);
    ASSERT_TRUE(buf2 != nullptr);
    
    return true;
}

TEST(pool_size_classes) {
    MemoryPool pool;
    
    // Different sizes should get appropriate capacity
    auto small = pool.allocate(50);    // Gets 64B class
    auto medium = pool.allocate(500);  // Gets 1KB class
    auto large = pool.allocate(50000); // Gets 64KB class
    
    ASSERT_TRUE(small->capacity >= 50);
    ASSERT_TRUE(medium->capacity >= 500);
    ASSERT_TRUE(large->capacity >= 50000);
    
    return true;
}

TEST(pool_shared_ownership) {
    // Test that shared_ptr correctly manages buffer lifetime
    MemoryPool pool;
    
    std::shared_ptr<Buffer> ref1 = pool.allocate(100);
    ref1->data[0] = 'X';
    
    {
        std::shared_ptr<Buffer> ref2 = ref1;  // Copy shared_ptr
        ASSERT_EQ('X', ref2->data[0]);         // Same buffer
        // ref2 goes out of scope, but ref1 keeps buffer alive
    }
    
    // Buffer should still be valid
    ASSERT_EQ('X', ref1->data[0]);
    
    return true;
}

// ----------------------------------------------------------------------------
// Protocol Tests
// ----------------------------------------------------------------------------
// These test the wire format protocol structures.
// ----------------------------------------------------------------------------

TEST(protocol_structure_sizes) {
    // Verify packed structures have correct sizes (no padding)
    ASSERT_EQ(76, sizeof(TcpRequest));    // 4 + 32 + 8 + 32 = 76
    ASSERT_EQ(10, sizeof(TcpResponse));   // 2 + 8 = 10
    ASSERT_EQ(14, sizeof(MsgHdr));        // 8 + 4 + 2 = 14
    
    // Verify minimum message length
    ASSERT_EQ(24, MIN_MSG_LEN);           // 10 + 14 = 24
    
    return true;
}

TEST(protocol_magic_key) {
    // Default magic key
    ASSERT_EQ(0xCAFEBABE, getMagicKey());
    
    return true;
}

TEST(protocol_buffer_size) {
    // Default receive buffer size (64KB)
    ASSERT_EQ(65536, getRecvBufferSize());
    
    return true;
}

TEST(protocol_raw_message) {
    // Test RawMessage structure
    MemoryPool pool;
    auto buf = pool.allocate(100);
    
    RawMessage raw;
    raw.buffer = buf;
    raw.offset = 10;
    raw.length = 50;
    raw.seq_num = 12345;
    
    ASSERT_EQ(10, raw.offset);
    ASSERT_EQ(50, raw.length);
    ASSERT_EQ(12345, raw.seq_num);
    
    return true;
}

TEST(protocol_sub_message) {
    // Test SubMessage structure
    MemoryPool pool;
    auto buf = pool.allocate(100);
    std::memcpy(buf->data, "Hello World", 11);
    
    SubMessage sub;
    sub.buffer = buf;
    sub.seq_num = 999;
    sub.timestamp = 1234567890;
    sub.flags = 0x01;
    sub.body = buf->data;
    sub.body_length = 11;
    
    ASSERT_EQ(999, sub.seq_num);
    ASSERT_EQ(1234567890, sub.timestamp);
    ASSERT_EQ(11, sub.length());
    ASSERT_EQ(sub.body, sub.data());
    
    return true;
}

// ----------------------------------------------------------------------------
// Stress Tests
// ----------------------------------------------------------------------------
// These tests run longer and stress-test the components.
// ----------------------------------------------------------------------------

TEST(stress_ringbuffer_contention) {
    // Heavy contention test with multiple producer/consumer pairs
    // This verifies the lock-free algorithm is correct
    const int NUM_PAIRS = 4;
    const int ITEMS_PER_PAIR = 10000;
    
    std::vector<std::thread> threads;
    std::atomic<int> total_consumed{0};
    
    for (int pair = 0; pair < NUM_PAIRS; ++pair) {
        auto* rb = new LockFreeRingBuffer<int>(256);
        
        // Producer
        threads.emplace_back([rb, pair]() {
            for (int i = 0; i < ITEMS_PER_PAIR; ++i) {
                while (!rb->push(i)) {
                    std::this_thread::yield();
                }
            }
        });
        
        // Consumer
        threads.emplace_back([rb, &total_consumed]() {
            int val;
            int count = 0;
            while (count < ITEMS_PER_PAIR) {
                if (rb->pop(val)) {
                    ++count;
                    ++total_consumed;
                }
            }
            delete rb;
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    ASSERT_EQ(NUM_PAIRS * ITEMS_PER_PAIR, total_consumed.load());
    
    return true;
}

// ----------------------------------------------------------------------------
// Reconnection Sequence Tracking Test
// ----------------------------------------------------------------------------
// This verifies that the client tracks the last received sequence number
// correctly for reconnection resume functionality.
// ----------------------------------------------------------------------------

TEST(sequence_number_tracking) {
    // Simulate tracking sequence numbers as they arrive
    std::atomic<uint64_t> last_received_seq{0};
    
    // Simulate receiving messages with increasing sequence numbers
    for (uint64_t seq = 1; seq <= 1000; ++seq) {
        last_received_seq.store(seq, std::memory_order_relaxed);
    }
    
    // Verify we track the highest (latest) sequence
    ASSERT_EQ(1000, last_received_seq.load());
    
    // Simulate reconnection - we should resume from last known sequence
    uint64_t resume_seq = last_received_seq.load(std::memory_order_relaxed);
    ASSERT_EQ(1000, resume_seq);
    
    // After resume, new messages continue from 1001
    for (uint64_t seq = 1001; seq <= 2000; ++seq) {
        last_received_seq.store(seq, std::memory_order_relaxed);
    }
    
    ASSERT_EQ(2000, last_received_seq.load());
    
    return true;
}

TEST(pool_buffer_zeroing) {
    // Test that buffers are zeroed when returned to pool (security feature)
    MemoryPool pool;
    
    // Allocate and fill with sensitive data
    auto buf = pool.allocate(256);
    ASSERT_TRUE(buf != nullptr);
    std::memset(buf->data, 'X', 256);  // Fill with 'X'
    ASSERT_EQ('X', buf->data[0]);
    ASSERT_EQ('X', buf->data[255]);
    
    // Return to pool
    buf.reset();
    
    // Reallocate - should get the same buffer (from pool)
    auto buf2 = pool.allocate(256);
    ASSERT_TRUE(buf2 != nullptr);
    
    // Buffer should be zeroed (security: no data leakage)
    bool all_zero = true;
    for (int i = 0; i < 256; ++i) {
        if (buf2->data[i] != 0) {
            all_zero = false;
            break;
        }
    }
    ASSERT_TRUE(all_zero);
    
    return true;
}

// ----------------------------------------------------------------------------
// Edge Case Tests
// ----------------------------------------------------------------------------
// Tests for boundary conditions, empty inputs, and maximum sizes.
// ----------------------------------------------------------------------------

TEST(protocol_min_max_message_length) {
    // MIN_MSG_LEN should be 24 (TcpResponse 10 + MsgHdr 14)
    ASSERT_EQ(24, MIN_MSG_LEN);
    // MAX_MSG_LEN should be 65535
    ASSERT_EQ(65535, MAX_MSG_LEN);
    return true;
}

TEST(protocol_empty_item_name) {
    // Item name is a 32-byte char array; verify it can hold an empty string
    TcpRequest req;
    std::memset(&req, 0, sizeof(req));
    req.reqKey = getMagicKey();
    // Empty item name should be all zeros
    bool all_zero = true;
    for (int i = 0; i < 32; ++i) {
        if (req.reqItem[i] != 0) {
            all_zero = false;
            break;
        }
    }
    ASSERT_TRUE(all_zero);
    ASSERT_EQ(0xCAFEBABE, req.reqKey);
    return true;
}

TEST(protocol_max_size_item_name) {
    // Item name max length is 32 bytes (including null terminator)
    TcpRequest req;
    std::memset(&req, 0, sizeof(req));
    req.reqKey = getMagicKey();
    // Fill with exactly 31 chars + null terminator
    std::memcpy(req.reqItem, "ABCDEFGHIJKLMNOPQRSTUVWXYZ12345", 31);
    req.reqItem[31] = '\0';
    ASSERT_EQ('A', req.reqItem[0]);
    ASSERT_EQ('5', req.reqItem[30]);
    ASSERT_EQ('\0', req.reqItem[31]);
    return true;
}

TEST(protocol_max_size_client_id) {
    // Client ID max length is 32 bytes
    TcpRequest req;
    std::memset(&req, 0, sizeof(req));
    std::memcpy(req.clientID, "Client_ABCDEFGHIJKLMNOPQRSTUVWX", 31);
    req.clientID[31] = '\0';
    ASSERT_EQ('C', req.clientID[0]);
    ASSERT_EQ('X', req.clientID[30]);
    ASSERT_EQ('\0', req.clientID[31]);
    return true;
}

TEST(protocol_max_resp_len) {
    // Test that TcpResponse can hold MAX_MSG_LEN
    TcpResponse resp;
    resp.respLen = static_cast<uint16_t>(MAX_MSG_LEN);
    resp.respSeq = 0xFFFFFFFFFFFFFFFF;
    ASSERT_EQ(MAX_MSG_LEN, resp.respLen);
    ASSERT_EQ(0xFFFFFFFFFFFFFFFF, resp.respSeq);
    return true;
}

TEST(protocol_msg_flags_boundary) {
    // Test all bits of flags field
    MsgHdr hdr;
    hdr.msgSeqNum = 1;
    hdr.timestamp = 0;
    hdr.flags = 0xFFFF;
    ASSERT_EQ(0xFFFF, hdr.flags);
    hdr.flags = 0x0000;
    ASSERT_EQ(0x0000, hdr.flags);
    return true;
}

TEST(protocol_raw_message_zero_length) {
    // RawMessage with zero length should be valid
    MemoryPool pool;
    auto buf = pool.allocate(64);
    ASSERT_TRUE(buf != nullptr);
    
    RawMessage raw;
    raw.buffer = buf;
    raw.offset = 0;
    raw.length = 0;  // Zero-length message (edge case)
    raw.seq_num = 0;
    raw.connection_id = 0;
    
    ASSERT_EQ(0, raw.length);
    ASSERT_TRUE(raw.buffer != nullptr);
    return true;
}

TEST(protocol_sub_message_zero_body) {
    // SubMessage with zero body length
    MemoryPool pool;
    auto buf = pool.allocate(64);
    ASSERT_TRUE(buf != nullptr);
    
    SubMessage sub;
    sub.buffer = buf;
    sub.seq_num = 1;
    sub.timestamp = 0;
    sub.flags = 0;
    sub.body = buf->data;
    sub.body_length = 0;  // Zero body (edge case)
    sub.connection_id = 0;
    
    ASSERT_EQ(0, sub.length());
    ASSERT_EQ(sub.body, sub.data());
    return true;
}

TEST(pool_allocation_max_size_class) {
    // Allocate at the max size class boundary (256KB)
    MemoryPool pool;
    auto buf = pool.allocate(262144);
    ASSERT_TRUE(buf != nullptr);
    ASSERT_TRUE(buf->capacity >= 262144);
    
    // Fill and verify
    std::memset(buf->data, 0xAB, 262144);
    ASSERT_EQ(static_cast<char>(0xAB), buf->data[0]);
    ASSERT_EQ(static_cast<char>(0xAB), buf->data[262143]);
    return true;
}

TEST(pool_allocation_oversized) {
    // Request larger than largest class — should still succeed (falls back to largest)
    MemoryPool pool;
    auto buf = pool.allocate(300000);  // Larger than 256KB
    ASSERT_TRUE(buf != nullptr);
    ASSERT_TRUE(buf->capacity >= 262144);
    return true;
}

TEST(pool_allocation_zero) {
    // Zero-byte allocation should still return a valid buffer from smallest class
    MemoryPool pool;
    auto buf = pool.allocate(0);
    ASSERT_TRUE(buf != nullptr);
    ASSERT_TRUE(buf->capacity > 0);  // Should get at least smallest class size
    return true;
}

TEST(pool_exhaustion_and_recovery) {
    // Exhaust a small size class, then verify recovery when buffers are freed
    // Use a pool with very limited capacity for this test
    std::vector<SizeClassConfig> limited_config = {
        {64, 1, 1, 2},    // Only 2 total allocations allowed
        {256, 1, 1, 2},
        {1024, 1, 1, 2},
        {4096, 1, 1, 2},
        {16384, 1, 1, 2},
        {65536, 1, 1, 2},
        {131072, 1, 1, 2},
        {262144, 1, 1, 2}
    };
    MemoryPool pool(limited_config);
    
    // Allocate up to max_total_allocated (2)
    auto buf1 = pool.allocate(64);
    ASSERT_TRUE(buf1 != nullptr);
    auto buf2 = pool.allocate(64);
    ASSERT_TRUE(buf2 != nullptr);
    
    // Third allocation should fail (limit reached)
    auto buf3 = pool.allocate(64);
    ASSERT_TRUE(buf3 == nullptr);
    
    // Free one buffer
    buf1.reset();
    
    // Now allocation should succeed again
    auto buf4 = pool.allocate(64);
    ASSERT_TRUE(buf4 != nullptr);
    
    return true;
}

TEST(ringbuffer_single_element) {
    // Ring buffer with capacity 1
    LockFreeRingBuffer<int> rb(1);
    ASSERT_EQ(1, rb.capacity());
    ASSERT_TRUE(rb.empty());
    
    ASSERT_TRUE(rb.push(42));
    ASSERT_TRUE(rb.full());
    ASSERT_FALSE(rb.push(99));  // Should fail (full)
    
    int val;
    ASSERT_TRUE(rb.pop(val));
    ASSERT_EQ(42, val);
    ASSERT_TRUE(rb.empty());
    
    return true;
}

TEST(ringbuffer_large_capacity) {
    // Ring buffer with large capacity
    LockFreeRingBuffer<int> rb(100000);
    ASSERT_EQ(131072, rb.capacity());  // Next power of 2
    
    for (int i = 0; i < 100000; ++i) {
        ASSERT_TRUE(rb.push(i));
    }
    ASSERT_EQ(100000, rb.size());
    ASSERT_FALSE(rb.empty());
    ASSERT_FALSE(rb.full());
    
    int val;
    for (int i = 0; i < 100000; ++i) {
        ASSERT_TRUE(rb.pop(val));
        ASSERT_EQ(i, val);
    }
    ASSERT_TRUE(rb.empty());
    
    return true;
}

TEST(ringbuffer_push_pop_mixed) {
    // Interleaved push/pop to stress the queue
    LockFreeRingBuffer<int> rb(8);
    int val;
    
    // Push 3, pop 2, push 3, pop 4, etc.
    ASSERT_TRUE(rb.push(1));
    ASSERT_TRUE(rb.push(2));
    ASSERT_TRUE(rb.push(3));
    ASSERT_TRUE(rb.pop(val)); ASSERT_EQ(1, val);
    ASSERT_TRUE(rb.pop(val)); ASSERT_EQ(2, val);
    
    ASSERT_TRUE(rb.push(4));
    ASSERT_TRUE(rb.push(5));
    ASSERT_TRUE(rb.push(6));
    ASSERT_TRUE(rb.pop(val)); ASSERT_EQ(3, val);
    ASSERT_TRUE(rb.pop(val)); ASSERT_EQ(4, val);
    ASSERT_TRUE(rb.pop(val)); ASSERT_EQ(5, val);
    ASSERT_TRUE(rb.pop(val)); ASSERT_EQ(6, val);
    ASSERT_TRUE(rb.empty());
    
    return true;
}

// ----------------------------------------------------------------------------
// Config Parser Tests
// ----------------------------------------------------------------------------
// These test the JSON configuration file parser.
// ----------------------------------------------------------------------------

#include "../config_parser.h"
#include <cstdio>

TEST(config_parser_valid_file) {
    // Create a temporary valid config file
    const char* tmpfile = "/tmp/test_config_valid.json";
    FILE* f = std::fopen(tmpfile, "w");
    ASSERT_TRUE(f != nullptr);
    std::fprintf(f, "{\n");
    std::fprintf(f, "  \"global\": {\n");
    std::fprintf(f, "    \"workers\": 4,\n");
    std::fprintf(f, "    \"raw_queue_size\": 1024,\n");
    std::fprintf(f, "    \"decoded_queue_size\": 1024,\n");
    std::fprintf(f, "    \"reconnect_interval_ms\": 2000,\n");
    std::fprintf(f, "    \"queue_push_timeout_ms\": 10\n");
    std::fprintf(f, "  },\n");
    std::fprintf(f, "  \"connections\": [\n");
    std::fprintf(f, "    {\n");
    std::fprintf(f, "      \"host\": \"127.0.0.1\",\n");
    std::fprintf(f, "      \"port\": 8888,\n");
    std::fprintf(f, "      \"item\": \"TEST_ITEM\",\n");
    std::fprintf(f, "      \"client_id\": \"TestClient\"\n");
    std::fprintf(f, "    }\n");
    std::fprintf(f, "  ]\n");
    std::fprintf(f, "}\n");
    std::fclose(f);
    
    MsgClientConfig config;
    std::string error;
    bool result = parseConfigFile(tmpfile, config, error);
    
    ASSERT_TRUE(result);
    ASSERT_EQ(4, config.worker_thread_count);
    ASSERT_EQ(1024, config.raw_queue_size);
    ASSERT_EQ(1024, config.decoded_queue_size);
    ASSERT_EQ(2000, config.reconnect_interval_ms);
    ASSERT_EQ(10, config.queue_push_timeout_ms);
    ASSERT_EQ(1, config.connections.size());
    ASSERT_STREQ("127.0.0.1", config.connections[0].endpoints[0].host);
    ASSERT_EQ(8888, config.connections[0].endpoints[0].port);
    ASSERT_STREQ("TEST_ITEM", config.connections[0].item_name);
    ASSERT_STREQ("TestClient", config.connections[0].client_id);
    
    std::remove(tmpfile);
    return true;
}

TEST(config_parser_missing_file) {
    MsgClientConfig config;
    std::string error;
    bool result = parseConfigFile("/tmp/nonexistent_config_12345.json", config, error);
    
    ASSERT_FALSE(result);
    ASSERT_TRUE(!error.empty());
    return true;
}

TEST(config_parser_invalid_json) {
    const char* tmpfile = "/tmp/test_config_invalid.json";
    FILE* f = std::fopen(tmpfile, "w");
    ASSERT_TRUE(f != nullptr);
    std::fprintf(f, "this is not json {\n");
    std::fclose(f);
    
    MsgClientConfig config;
    std::string error;
    bool result = parseConfigFile(tmpfile, config, error);
    
    ASSERT_FALSE(result);
    ASSERT_TRUE(!error.empty());
    
    std::remove(tmpfile);
    return true;
}

TEST(config_parser_empty_connections) {
    const char* tmpfile = "/tmp/test_config_empty.json";
    FILE* f = std::fopen(tmpfile, "w");
    ASSERT_TRUE(f != nullptr);
    std::fprintf(f, "{\"connections\": []}\n");
    std::fclose(f);
    
    MsgClientConfig config;
    std::string error;
    bool result = parseConfigFile(tmpfile, config, error);
    
    // Should fail validation because at least one connection is required
    ASSERT_FALSE(result);
    ASSERT_TRUE(!error.empty());
    
    std::remove(tmpfile);
    return true;
}

TEST(config_parser_workers_out_of_range) {
    const char* tmpfile = "/tmp/test_config_workers.json";
    FILE* f = std::fopen(tmpfile, "w");
    ASSERT_TRUE(f != nullptr);
    std::fprintf(f, "{\n");
    std::fprintf(f, "  \"global\": {\"workers\": 100},\n");
    std::fprintf(f, "  \"connections\": [{\"host\":\"127.0.0.1\",\"port\":8888,\"item\":\"x\",\"client_id\":\"c\"}]\n");
    std::fprintf(f, "}\n");
    std::fclose(f);
    
    MsgClientConfig config;
    std::string error;
    bool result = parseConfigFile(tmpfile, config, error);
    
    ASSERT_FALSE(result);
    ASSERT_TRUE(!error.empty());
    
    std::remove(tmpfile);
    return true;
}

TEST(config_parser_port_out_of_range) {
    const char* tmpfile = "/tmp/test_config_port.json";
    FILE* f = std::fopen(tmpfile, "w");
    ASSERT_TRUE(f != nullptr);
    std::fprintf(f, "{\n");
    std::fprintf(f, "  \"connections\": [{\"host\":\"127.0.0.1\",\"port\":99999,\"item\":\"x\",\"client_id\":\"c\"}]\n");
    std::fprintf(f, "}\n");
    std::fclose(f);
    
    MsgClientConfig config;
    std::string error;
    bool result = parseConfigFile(tmpfile, config, error);
    
    ASSERT_FALSE(result);
    ASSERT_TRUE(!error.empty());
    
    std::remove(tmpfile);
    return true;
}

TEST(config_parser_multi_endpoint) {
    const char* tmpfile = "/tmp/test_config_multi.json";
    FILE* f = std::fopen(tmpfile, "w");
    ASSERT_TRUE(f != nullptr);
    std::fprintf(f, "{\n");
    std::fprintf(f, "  \"connections\": [\n");
    std::fprintf(f, "    {\n");
    std::fprintf(f, "      \"endpoints\": [\n");
    std::fprintf(f, "        {\"host\": \"primary.example.com\", \"port\": 8888},\n");
    std::fprintf(f, "        {\"host\": \"backup.example.com\", \"port\": 8889}\n");
    std::fprintf(f, "      ],\n");
    std::fprintf(f, "      \"item\": \"AAPL\",\n");
    std::fprintf(f, "      \"client_id\": \"Client1\"\n");
    std::fprintf(f, "    }\n");
    std::fprintf(f, "  ]\n");
    std::fprintf(f, "}\n");
    std::fclose(f);
    
    MsgClientConfig config;
    std::string error;
    bool result = parseConfigFile(tmpfile, config, error);
    
    ASSERT_TRUE(result);
    ASSERT_EQ(1, config.connections.size());
    ASSERT_EQ(2, config.connections[0].endpoints.size());
    ASSERT_STREQ("primary.example.com", config.connections[0].endpoints[0].host);
    ASSERT_EQ(8888, config.connections[0].endpoints[0].port);
    ASSERT_STREQ("backup.example.com", config.connections[0].endpoints[1].host);
    ASSERT_EQ(8889, config.connections[0].endpoints[1].port);
    
    std::remove(tmpfile);
    return true;
}

TEST(config_parser_memory_pool) {
    const char* tmpfile = "/tmp/test_config_pool.json";
    FILE* f = std::fopen(tmpfile, "w");
    ASSERT_TRUE(f != nullptr);
    std::fprintf(f, "{\n");
    std::fprintf(f, "  \"connections\": [{\"host\":\"127.0.0.1\",\"port\":8888,\"item\":\"x\",\"client_id\":\"c\"}],\n");
    std::fprintf(f, "  \"memory_pool\": {\n");
    std::fprintf(f, "    \"class_0\": {\"initial\": 10, \"max_free\": 20, \"max_total\": 50}\n");
    std::fprintf(f, "  }\n");
    std::fprintf(f, "}\n");
    std::fclose(f);
    
    MsgClientConfig config;
    std::string error;
    bool result = parseConfigFile(tmpfile, config, error);
    
    ASSERT_TRUE(result);
    ASSERT_EQ(8, config.pool_config.size());
    ASSERT_EQ(10, config.pool_config[0].initial_count);
    ASSERT_EQ(20, config.pool_config[0].max_count);
    ASSERT_EQ(50, config.pool_config[0].max_total_allocated);
    
    std::remove(tmpfile);
    return true;
}

TEST(config_parser_aggregation) {
    const char* tmpfile = "/tmp/test_config_agg.json";
    FILE* f = std::fopen(tmpfile, "w");
    ASSERT_TRUE(f != nullptr);
    std::fprintf(f, "{\n");
    std::fprintf(f, "  \"connections\": [{\"host\":\"127.0.0.1\",\"port\":8888,\"item\":\"x\",\"client_id\":\"c\"}],\n");
    std::fprintf(f, "  \"aggregation\": {\n");
    std::fprintf(f, "    \"enabled\": true,\n");
    std::fprintf(f, "    \"window_ms\": 5000,\n");
    std::fprintf(f, "    \"output_format\": \"csv\",\n");
    std::fprintf(f, "    \"output_dir\": \"/tmp/output\",\n");
    std::fprintf(f, "    \"filename_prefix\": \"test\"\n");
    std::fprintf(f, "  }\n");
    std::fprintf(f, "}\n");
    std::fclose(f);
    
    MsgClientConfig config;
    std::string error;
    bool result = parseConfigFile(tmpfile, config, error);
    
    ASSERT_TRUE(result);
    ASSERT_TRUE(config.aggregation_config.enabled);
    ASSERT_EQ(5000, config.aggregation_config.window_ms);
    ASSERT_STREQ("/tmp/output", config.aggregation_config.output_dir);
    ASSERT_STREQ("test", config.aggregation_config.filename_prefix);
    
    std::remove(tmpfile);
    return true;
}

// ============================================================================
// Main Test Runner
// ============================================================================

int main() {
    printf("=================================================================\n");
    printf("Running Unit Tests\n");
    printf("=================================================================\n\n");
    
    for (const auto& test : g_tests) {
        printf("Running: %s ... ", test.name);
        fflush(stdout);
        
        if (test.func()) {
            printf("PASSED\n");
            ++g_passed;
        } else {
            printf("FAILED\n");
            ++g_failed;
        }
    }
    
    printf("\n=================================================================\n");
    printf("Results: %d passed, %d failed, %d total\n", 
           g_passed, g_failed, g_passed + g_failed);
    printf("=================================================================\n");
    
    return g_failed > 0 ? 1 : 0;
}
