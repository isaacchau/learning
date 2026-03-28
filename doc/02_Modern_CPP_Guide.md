# Modern C++ Guide (C++11/14 Features)

If you haven't used C++ since before 2011, many things have changed. This guide explains the "alien" syntax you'll see in this codebase.

## Table of Contents
1. [Auto Keyword](#auto-keyword)
2. [Lambda Functions](#lambda-functions)
3. [Smart Pointers](#smart-pointers)
4. [Move Semantics](#move-semantics)
5. [nullptr](#nullptr)
6. [Range-based For Loops](#range-based-for-loops)
7. [Initializer Lists](#initializer-lists)
8. [Atomic Operations](#atomic-operations)
9. [Constexpr](#constexpr)

---

## Auto Keyword

**Old C++:**
```cpp
std::vector<std::string>::iterator it = myVector.begin();
```

**Modern C++:**
```cpp
auto it = myVector.begin();  // Compiler figures out the type
```

**Why:** Less typing, easier to maintain. The compiler knows the type, so why write it?

---

## Lambda Functions

Anonymous functions you can write inline.

**Old C++:**
```cpp
// Had to define a separate function or functor class
bool compare(int a, int b) { return a < b; }
std::sort(vec.begin(), vec.end(), compare);
```

**Modern C++:**
```cpp
// Define the function right where you use it
std::sort(vec.begin(), vec.end(), [](int a, int b) { return a < b; });
```

**Syntax breakdown:**
```cpp
[capture](parameters) -> return_type { body }

[capture]     - What variables from outside can the lambda see/use?
(parameters)  - Function parameters
-> return_type - Optional: what it returns (often inferred)
{ body }      - The code
```

**Capture examples:**
```cpp
int x = 10;

[x]      // Copy x into the lambda (x is read-only inside)
[&x]     // Reference to x (changes to x affect the original)
[&]      // Capture ALL variables by reference
[=]      // Capture ALL variables by copy
[this]   // Capture the current object
```

**In this codebase:**
```cpp
// In main.cpp - capturing nothing, just a simple helper
auto getEnvStr = [](const char *name, const char *def) -> std::string {
    const char *val = std::getenv(name);
    return val ? val : def;
};

// In msg_client.cpp - lambda used as custom deleter
return std::shared_ptr<Buffer>(
    buf, [this, cls](Buffer *b) { this->returnBuffer(b, cls); }
);
```

---

## Smart Pointers

**The Problem with Old C++:**
```cpp
void oldStyle() {
    MyClass* ptr = new MyClass();  // Allocate
    // ... lots of code ...
    delete ptr;  // Must remember to free! What if exception thrown?
}
```

**Modern C++ - Smart Pointers:**
Automatic memory management (like Java/Python garbage collection, but deterministic).

### unique_ptr
Owns an object exclusively. When `unique_ptr` goes out of scope, it deletes the object.

```cpp
{
    auto ptr = std::make_unique<MyClass>();  // Create
    ptr->doSomething();
}  // Automatically deleted here - no memory leak!
```

**In this codebase:**
```cpp
std::unique_ptr<MemoryPool> pool_;           // Owns the memory pool
std::unique_ptr<LockFreeRingBuffer<RawMessage>> raw_queue_;  // Owns the queue
```

### shared_ptr
Multiple owners share one object. Object is deleted when last owner releases it.

```cpp
{
    auto shared1 = std::make_shared<MyClass>();
    {
        auto shared2 = shared1;  // Now two owners
    }  // shared2 destroyed, but object lives on
}  // shared1 destroyed, object finally deleted
```

**In this codebase (zero-copy message passing):**
```cpp
struct RawMessage {
    std::shared_ptr<Buffer> buffer;  // Shared reference to receive buffer
    // Multiple RawMessages can share the same buffer
};
```

---

## Move Semantics

**The Problem:** Copying large objects is expensive.

**Old C++:**
```cpp
std::vector<int> createBigVector();
std::vector<int> v = createBigVector();  // Copies all elements! Expensive!
```

**Modern C++ - Move:**
Instead of copying, "steal" the resources.

```cpp
std::vector<int> v = createBigVector();  // Moves (steals) the data - O(1)!
```

**Syntax:**
```cpp
Type&&  // R-value reference - can bind to temporaries

std::move(x)  // Cast to r-value reference, enabling move
```

**In this codebase:**
```cpp
// Push a message without copying it
bool push(T&& item) {  // T&& accepts movable objects
    buffer_[head & mask_] = std::move(item);  // Move, don't copy
    return true;
}

// Usage
RawMessage raw;
raw_queue_->push_wait(std::move(raw), 5);  // Moves raw into queue
// After this, 'raw' is in a valid but unspecified state (don't use it)
```

---

## nullptr

**Old C++:**
```cpp
void* ptr = NULL;  // NULL is just #define NULL 0 - an integer!
```

**Modern C++:**
```cpp
void* ptr = nullptr;  // A real null pointer type
```

**Why it matters:**
```cpp
void foo(int x);      // #1
void foo(char* x);    // #2

foo(NULL);      // Calls #1 (NULL is 0, which is an int) - BUG!
foo(nullptr);   // Calls #2 (nullptr is a pointer) - Correct!
```

---

## Range-based For Loops

**Old C++:**
```cpp
for (std::vector<int>::iterator it = vec.begin(); it != vec.end(); ++it) {
    std::cout << *it;
}
```

**Modern C++:**
```cpp
for (int x : vec) {           // Simple!
    std::cout << x;
}

for (const auto& x : vec) {   // By const reference (no copy)
    std::cout << x;
}

for (auto& t : worker_threads_) {  // In this codebase
    if (t.joinable()) t.join();
}
```

---

## Initializer Lists

**Old C++:**
```cpp
std::vector<int> v;
v.push_back(1);
v.push_back(2);
v.push_back(3);
```

**Modern C++:**
```cpp
std::vector<int> v = {1, 2, 3};  // Uniform initialization

// In this codebase - size class configs
// Format: {block_size, initial_count, max_free_list, max_total_allocated}
return {{64, 64, 512, 1024}, {256, 64, 512, 1024}, {1024, 64, 512, 1024}};
```

---

## Atomic Operations

For thread-safe variables without locks.

```cpp
std::atomic<uint64_t> counter{0};

counter.fetch_add(1, std::memory_order_relaxed);  // Thread-safe increment
uint64_t val = counter.load(std::memory_order_relaxed);  // Thread-safe read
```

**Memory orders:**
- `relaxed` - Fastest, weakest guarantees (used here for stats)
- `acquire/release` - Stronger ordering (used in lock-free queue)
- `seq_cst` - Strongest (default, slowest)

**In this codebase:**
```cpp
struct MsgClientStats {
    std::atomic<uint64_t> messages_received{0};  // Thread-safe counter
    std::atomic<uint64_t> messages_decoded{0};
    // ...
};

// Increment without locks
stats_.messages_received.fetch_add(1, std::memory_order_relaxed);
```

---

## Constexpr

Compile-time computation.

```cpp
constexpr int max_size = 100;  // Known at compile time
constexpr int double_size = max_size * 2;  // Also compile time

// Old way
#define MAX_SIZE 100  // No type safety, preprocessor hack
```

---

## Summary Table

| Feature | Old C++ | Modern C++ | Benefit |
|---------|---------|------------|---------|
| Type inference | `std::vector<int>::iterator it` | `auto it` | Less typing |
| Functions | Named function elsewhere | Lambda inline | Local logic |
| Memory | `new/delete` | `unique_ptr/shared_ptr` | No leaks |
| Copying | Always copy | Can move | Performance |
| Null | `NULL` (integer 0) | `nullptr` (pointer) | Type safety |
| Loops | Iterator-based | Range-based | Readable |
| Initialization | Constructor calls | `{}` lists | Consistent |
| Threading | Locks everywhere | Atomics + lock-free | Speed |
