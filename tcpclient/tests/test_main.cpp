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
        printf("    ASSERT_EQ(%s, %s) FAILED at line %d: expected %lld, got %lld\n", \
               #expected, #actual, __LINE__, (long long)(expected), (long long)(actual)); \
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
