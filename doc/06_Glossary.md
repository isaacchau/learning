# Glossary of Terms

## A

**Atomic Operation**
An operation that completes entirely or not at all, without any intermediate state visible to other threads. In C++, `std::atomic` provides these for simple types like integers.

**Acquire-Release Semantics**
Memory ordering constraints that ensure writes in one thread are visible to reads in another thread in a specific order. Used in lock-free programming.

## B

**Buffer**
A region of memory used to temporarily hold data while it's being moved from one place to another. In networking, buffers hold received data until processed.

**Busy-Wait (Spin-Wait)**
When a thread repeatedly checks a condition instead of sleeping. Faster for short waits but wastes CPU.

## C

**Cache Line**
The smallest unit of memory that CPUs move between cache and RAM (typically 64 bytes). False sharing occurs when two threads modify different variables on the same cache line.

**Constexpr**
A C++ keyword indicating an expression can be evaluated at compile time, not runtime.

**Context Switch**
When the CPU stops executing one thread and starts another. Expensive operation involving saving/restoring register state.

## D

**Deadlock**
When two or more threads are waiting for each other to release resources, causing all to block forever.

**Decoder Thread**
In this program, the thread that parses raw network bytes into structured messages.

## E

**Endianness**
The order bytes are stored in memory. Little-endian (x86): least significant byte first. Big-endian (network): most significant byte first.

## F

**False Sharing**
When two threads modify different variables that happen to be on the same cache line, causing unnecessary cache invalidations and poor performance.

## H

**Handler**
A function or callback that processes an event or message. In this program, the user-provided function that processes decoded messages.

## I

**IO Thread**
Input/Output thread. In this program, the thread that receives data from the network socket.

**Item**
The name of the data stream you want to subscribe to. Like a channel name or topic.

## L

**Lambda**
An anonymous function defined inline. Modern C++ feature that allows writing small functions where they're used.

**Lock-Free**
An algorithm that guarantees at least one thread makes progress without using locks (mutexes). Uses atomic operations instead.

**LTO (Link Time Optimization)**
Compiler optimization that happens during linking, allowing optimization across source file boundaries.

## M

**Memory Barrier**
An instruction that ensures memory operations before the barrier complete before operations after it. Used for thread synchronization.

**Memory Pool**
A pre-allocated block of memory managed by the program rather than the OS. Reduces allocation overhead.

**Memory Order**
Constraints on how memory operations can be reordered by the compiler or CPU. Options: `relaxed`, `acquire`, `release`, `seq_cst`.

**Move Semantics**
C++ feature allowing resources to be transferred from one object to another without copying. Uses r-value references (`&&`).

**MTU (Maximum Transmission Unit)**
The largest packet size that can be sent over a network. Ethernet MTU is typically 1500 bytes.

**Mutex**
Mutual exclusion lock. A synchronization primitive that allows only one thread to access a resource at a time.

## N

**Nagle's Algorithm**
TCP optimization that batches small messages to reduce network overhead. Disabled here (`TCP_NODELAY`) for low latency.

**nullptr**
C++11's type-safe null pointer constant. Replaces the problematic `NULL` macro.

## P

**Pipeline**
A processing pattern where data flows through a sequence of stages, each performing a specific transformation.

**pragma pack**
Compiler directive that controls structure padding. Used here to ensure network structures have no padding bytes.

**Protocol**
A set of rules governing the format and exchange of messages between systems.

## Q

**Queue**
A data structure that follows First-In-First-Out (FIFO) ordering. Used here to pass messages between threads.

## R

**RAII (Resource Acquisition Is Initialization)**
C++ idiom where resources are acquired in constructors and released in destructors. Ensures cleanup even if exceptions occur.

**Raw Message**
In this program, the uninterpreted bytes received from the network, before parsing.

**Reconnection**
Automatically re-establishing a network connection after it drops. This client does this automatically.

**Round-Robin**
A scheduling algorithm that cycles through items equally. Used here to distribute messages to workers.

## S

**Sequence Number**
A monotonically increasing number assigned to each message. Used for ordering and gap detection.

**Shared Pointer**
`std::shared_ptr` - a smart pointer that allows multiple owners of an object. Object deleted when last owner releases it.

**Size Class**
Memory pool concept: grouping allocations by size. This pool has 8 size classes from 64B to 256KB.

**Smart Pointer**
C++ objects that act like pointers but manage memory automatically. Types: `unique_ptr`, `shared_ptr`, `weak_ptr`.

**SPSC (Single Producer Single Consumer)**
A queue designed for exactly one thread writing and one thread reading. Can be implemented lock-free.

**std::atomic**
C++ template class providing atomic operations on a type. Essential for lock-free programming.

**std::move**
Casts an object to an r-value reference, enabling move semantics instead of copy.

## T

**TCP (Transmission Control Protocol)**
Reliable, ordered, connection-oriented network protocol. Used here for the client-server communication.

**Thread**
A separate execution path within a process. This program uses multiple threads for parallelism.

**Three-Stage Pipeline**
The architecture pattern used: IO thread → Decoder thread → Worker threads.

**Timeout**
A time limit for an operation. Used here in queue operations to prevent indefinite blocking.

## U

**Unique Pointer**
`std::unique_ptr` - a smart pointer with exclusive ownership. Cannot be copied, only moved.

**Unitialized Memory**
Memory that has been allocated but not yet constructed. Dangerous to use.

## W

**Worker Thread**
A thread that performs the actual processing work. This program can have multiple worker threads.

**Wire Format**
The exact byte layout of data as it travels over the network.

## Z

**Zero-Copy**
Technique where data is shared via references/pointers rather than being copied. Essential for high-performance networking.

---

## Common Abbreviations

| Abbreviation | Meaning |
|--------------|---------|
| CPU | Central Processing Unit |
| GB | Gigabyte (1024^3 bytes) |
| IO | Input/Output |
| KB | Kilobyte (1024 bytes) |
| MB | Megabyte (1024^2 bytes) |
| MTU | Maximum Transmission Unit |
| RAII | Resource Acquisition Is Initialization |
| SPSC | Single Producer Single Consumer |
| TCP | Transmission Control Protocol |
| TLS | Transport Layer Security |
| UDP | User Datagram Protocol |
