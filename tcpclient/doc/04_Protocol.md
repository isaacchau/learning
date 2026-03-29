# Wire Protocol Explained

## What Is a Wire Protocol?

A **wire protocol** is the format of data as it travels over the network. Both client and server must agree on:
1. How big each field is
2. In what order fields appear
3. How to interpret the bytes

Think of it like a letter format:
- Return address
- Destination address
- Message body

## Protocol Overview

This protocol has two directions:
- **Client → Server:** Subscription request ("I want to receive data")
- **Server → Client:** Response messages (the actual data)

## Binary vs Text Protocols

**Text Protocol (like HTTP):**
```
GET /index.html HTTP/1.1\r\n
Host: example.com\r\n
\r\n
```
- Human readable
- Larger size
- Slower to parse

**Binary Protocol (this one):**
```
[4 bytes: 0xCAFEBABE][32 bytes: item name][8 bytes: sequence]
```
- Compact
- Fast to parse
- Not human readable

## The Structures (from protocol.h)

### Client → Server: TcpRequest

```cpp
#pragma pack(push, 1)  // No padding between fields!

struct TcpRequest {
    uint32_t reqKey;        // 4 bytes: Must be 0xCAFEBABE
    char     reqItem[32];   // 32 bytes: Item name (e.g., "default")
    uint64_t lastRespSeq;   // 8 bytes: Last sequence received (0 = new)
    char     clientID[32];  // 32 bytes: Client identifier
};
// Total: 76 bytes

#pragma pack(pop)
```

**Visual layout:**
```
Offset  Size  Field
   0     4    reqKey      [CA FE BA BE]
   4    32    reqItem     ["default" + nulls to fill 32 bytes]
  36     8    lastRespSeq [00 00 00 00 00 00 00 00] (8 bytes, little-endian)
  44    32    clientID    ["MsgClient" + nulls to fill 32 bytes]
  76    ---   end of structure
```

**Why `#pragma pack`?**
Normally, compilers add padding for alignment:
```cpp
struct Bad {
    uint32_t a;   // 4 bytes
    // 4 bytes padding!
    uint64_t b;   // 8 bytes (wants 8-byte alignment)
};
// Size would be 16 bytes, not 12!
```

With `#pragma pack(1)`, no padding is added - critical for network protocols where every byte matters.

### Server → Client: TcpResponse (Envelope)

```cpp
struct TcpResponse {
    uint16_t respLen;   // 2 bytes: Total message length
    uint64_t respSeq;   // 8 bytes: Response sequence number
};
// Total: 10 bytes
```

This is like an envelope - it tells you how big the entire message is.

### Server → Client: MsgHdr (Message Header)

```cpp
struct MsgHdr {
    uint64_t msgSeqNum;  // 8 bytes: Message sequence number
    uint32_t timestamp;  // 4 bytes: Unix timestamp
    uint16_t flags;      // 2 bytes: Flags for future use
};
// Total: 14 bytes
```

## Complete Message Format

A complete message from server to client looks like this:

```
┌─────────────────────────────────────────────────────────────┐
│  TcpResponse (10 bytes)                                      │
│  ├── respLen: Total length of everything after this         │
│  └── respSeq: Sequence number for this response             │
├─────────────────────────────────────────────────────────────┤
│  MsgHdr (14 bytes)                                           │
│  ├── msgSeqNum: Unique ID for this message                  │
│  ├── timestamp: When message was created                    │
│  └── flags: Extension bits                                  │
├─────────────────────────────────────────────────────────────┤
│  Body (variable, respLen - 24 bytes)                         │
│  └── Your actual data (opaque bytes)                        │
└─────────────────────────────────────────────────────────────┘
```

**Example with 256 byte body:**
```
Total message size: 10 + 14 + 256 = 280 bytes
respLen would be: 280
```

## Sequence Numbers Explained

Sequence numbers serve multiple purposes:

### 1. Gap Detection
```
Received: 1, 2, 3, 5, 6
Missing:  4  <-- Gap detected!
```

### 2. Resuming After Disconnect
```
Client: "I last received sequence 1000"
Server: "OK, I'll start from 1001"
```

### 3. Deduplication
If the same message arrives twice (network retry), you can detect it:
```
Received: 1, 2, 2, 3
Duplicate: ^ 2 is duplicate, ignore
```

## Magic Key: 0xCAFEBABE

This is a "magic number" that identifies the protocol.

**Why 0xCAFEBABE?**
- Easy to spot in hex dumps
- Unlikely to appear randomly
- Used in Java class files too (familiar to many developers)

In hex: `CA FE BA BE`

**Usage:**
```cpp
if (req.reqKey != MAGIC_KEY) {
    // This isn't our protocol - reject connection
    close(client_fd);
}
```

## Endianness

**Problem:** Different computers store multi-byte numbers differently.

**Big Endian (network byte order):**
```
Value: 0x12345678
Bytes: [12] [34] [56] [78]  (most significant first)
```

**Little Endian (x86 computers):**
```
Value: 0x12345678
Bytes: [78] [56] [34] [12]  (least significant first)
```

**This protocol uses:** Native endianness (assumes both client and server are x86/little-endian). For production, you might want explicit network byte order conversions using `htons()`, `htonl()`, etc.

## Protocol Constants

```cpp
static const uint32_t MAGIC_KEY = 0xCAFEBABE;
static const size_t MIN_MSG_LEN = sizeof(TcpResponse) + sizeof(MsgHdr);  // 24
static const size_t MAX_MSG_LEN = 65535;  // Fits in uint16_t
static const size_t RECV_BUFFER_SIZE = 2097152;  // 2MB default (configurable)
```

**Why MAX_MSG_LEN = 65535?**
Because `respLen` is `uint16_t` (16 bits), which maxes out at 65535.

### Defensive Programming Note

The validation code checks:
```cpp
if (resp.respLen < MIN_MSG_LEN || resp.respLen > MAX_MSG_LEN) { ... }
```

While the `> MAX_MSG_LEN` check is currently redundant (a `uint16_t` cannot exceed 65535), it is intentionally kept as **future-proofing**. If the protocol is later updated to use `uint32_t` for larger messages, this check becomes meaningful immediately. Modern compilers optimize away the always-false branch anyway, so there's no runtime cost.

**Why 2MB receive buffer?**
- Reduces partial message copies for large messages (>16KB)
- Amortizes syscall overhead over more data
- Memory cost is acceptable on modern systems (2MB per connection)
- Configurable via `APP_TCP_RECV_BUFFER_SIZE` env var if memory-constrained

## Example Walkthrough

**Step 1: Client connects and sends subscription:**
```
Bytes sent (76 total):
[BE BA FE CA]                 // reqKey (little-endian)
[64 65 66 61 75 6C 74 00 ...] // "default\0" padded to 32 bytes
[00 00 00 00 00 00 00 00]     // lastRespSeq = 0 (start from beginning)
[4D 73 67 43 6C 69 65 6E 74 00 ...] // "MsgClient\0" padded to 32 bytes
```

**Step 2: Server sends a message:**
```
Bytes sent (280 total for 256-byte body):
[18 01]                       // respLen = 280 (0x0118)
[01 00 00 00 00 00 00 00]     // respSeq = 1
[01 00 00 00 00 00 00 00]     // msgSeqNum = 1
[78 56 34 12]                 // timestamp = 0x12345678
[00 00]                       // flags = 0
[41 42 43 44 ...]             // 256 bytes of body data
```

**Step 3: Client parses the message:**
1. Read TcpResponse (10 bytes)
2. Check respLen - we expect 280 more bytes
3. Read MsgHdr (14 bytes)
4. Read body (respLen - 24 = 256 bytes)
5. Process the message
6. Acknowledge sequence 1 received

## Security Considerations

This is a **test/demo protocol**, not production-ready:

1. **No encryption** - Data sent in plain text
2. **No authentication** - Anyone can connect
3. **No integrity checks** - No checksums or signatures
4. **No rate limiting** - Server could be overwhelmed
5. **Fixed-size strings** - Buffer overflow if not careful (we use snprintf)

For production, consider:
- TLS/SSL for encryption
- Authentication tokens
- Message authentication codes (MAC)
- Rate limiting
- Variable-length strings with length prefixes
