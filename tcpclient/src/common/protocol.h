// ============================================================================
// protocol.h — Wire-format protocol and internal pipeline message types
// ============================================================================
// Defines the packed binary format exchanged between client and server,
// plus the internal RawMessage / SubMessage structs used in the pipeline.
//
// All wire structs use #pragma pack(push, 1) to eliminate padding.
// This guarantees exact sizes and compatible layout across compilers.
//
// Environment overrides:
//   APP_TCP_MAGIC_KEY        — override protocol magic key (default 0xCAFEBABE)
//   APP_TCP_RECV_BUFFER_SIZE — override user-space recv buffer (default 64KB)
// ============================================================================

#ifndef PROTOCOL_H
#define PROTOCOL_H

#include "shared_ptr_pool.h"
#include <cstddef>
#include <cstdint>
#include <cstdlib>  // std::getenv
#include <memory>
#include <string>   // std::stoul

// ============================================================================
// Wire-format protocol structures (packed, no padding)
// ============================================================================

#pragma pack(push, 1)

// Client → Server subscription request
struct TcpRequest {
  uint32_t reqKey;      // Must be 0xCAFEBABE
  char reqItem[32];     // Name of the request Item; null padding
  uint64_t lastRespSeq; // Last received response sequence number (0 = start)
  char clientID[32];    // Name of the client; null padding
};

// Server → Client response envelope header
struct TcpResponse {
  uint16_t respLen; // Total response message length (envelope + MsgHdr + body)
  uint64_t respSeq; // Response sequence number
};

// Per-message header (follows TcpResponse in the wire data)
struct MsgHdr {
  uint64_t msgSeqNum; // Message sequence number
  uint32_t timestamp; // Unix timestamp (seconds)
  uint16_t flags;     // Message flags (extensible)
};

#pragma pack(pop)

// ============================================================================
// Protocol constants
// ============================================================================

// Magic key can be overridden via environment variable for testing/customization.
// Default: 0xCAFEBABE (classic Java magic number, easy to spot in hex dumps).
// To override: export APP_TCP_MAGIC_KEY="0xDEADBEEF"
inline uint32_t getMagicKey() {
    static const uint32_t key = []() -> uint32_t {
        const char* env = std::getenv("APP_TCP_MAGIC_KEY");
        if (env) {
            try {
                return static_cast<uint32_t>(std::stoul(env, nullptr, 0));
            } catch (...) {
                // Fall through to default on parse error
            }
        }
        return 0xCAFEBABE;
    }();
    return key;
}

// ============================================================================
// Protocol constants and limits
// ============================================================================

namespace ProtocolLimits {
    constexpr size_t MIN_MSG_LEN = sizeof(TcpResponse) + sizeof(MsgHdr); // 24
    constexpr size_t MAX_MSG_LEN = 65535;
}

using ProtocolLimits::MIN_MSG_LEN;
using ProtocolLimits::MAX_MSG_LEN;

// Application receive buffer size (configurable via APP_TCP_RECV_BUFFER_SIZE env var).
// Default: 65536 (64KB). Must be >= MAX_MSG_LEN.
// This is the user-space buffer for recv(), NOT the TCP socket buffer.
inline size_t getRecvBufferSize() {
    static const size_t size = []() -> size_t {
        const char* env = std::getenv("APP_TCP_RECV_BUFFER_SIZE");
        if (env) {
            try {
                size_t s = static_cast<size_t>(std::stoul(env));
                if (s >= MAX_MSG_LEN) return s;
            } catch (...) {
                // Fall through to default on error
            }
        }
        return 65536; // 64KB default (good for batching multiple small messages)
    }();
    return size;
}

// ============================================================================
// Internal message types passed through the pipeline
// ============================================================================

// Raw message: produced by IO thread, consumed by decoder thread.
// Holds a shared reference to the receive buffer.
//
// Why shared_ptr instead of a raw pointer + length?
//   - The recv buffer may contain multiple messages.  Each RawMessage
//     references the same underlying buffer at different offsets.
//   - shared_ptr ensures the buffer stays alive until the LAST consumer
//     (decoder or worker) finishes with it.  Without this, a fast decoder
//     could free the buffer while a slow worker still reads from it.
//   - The custom deleter returns the buffer to MemoryPool for reuse.
struct RawMessage {
  std::shared_ptr<Buffer> buffer; // Shared reference to recv buffer
  size_t offset;                  // Start of this message within buffer->data
  size_t length;                  // Total message length (== TcpResponse.respLen)
  uint64_t seq_num;               // From TcpResponse.respSeq
  size_t connection_id;           // Which connection this message came from

  RawMessage() : offset(0), length(0), seq_num(0), connection_id(0) {}
};

// Decoded sub-message: produced by decoder thread, consumed by worker threads.
// Holds a shared reference to the original buffer (zero-copy).
//
// Why does SubMessage also hold a shared_ptr?
//   - The decoder moves the shared_ptr from RawMessage to SubMessage.
//     This is a single atomic refcount bump (or none, if moved).
//   - Workers may outlive the decoder's processing of the next message,
//     so the buffer must remain pinned until ALL workers are done.
//   - Zero-copy: no message body bytes are copied between stages.
struct SubMessage {
  std::shared_ptr<Buffer> buffer; // Keeps underlying buffer alive
  uint64_t seq_num;               // From MsgHdr.msgSeqNum
  uint32_t timestamp;             // From MsgHdr.timestamp
  uint16_t flags;                 // From MsgHdr.flags
  const char *body;               // Pointer into buffer->data (after MsgHdr)
  size_t body_length;             // Body size in bytes
  size_t connection_id;           // Which connection this message came from

  SubMessage()
      : seq_num(0), timestamp(0), flags(0), body(nullptr), body_length(0), connection_id(0) {}

  const char *data() const { return body; }
  size_t length() const { return body_length; }
};

#endif // PROTOCOL_H
