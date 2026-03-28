#ifndef PROTOCOL_H
#define PROTOCOL_H

#include "shared_ptr_pool.h"
#include <cstddef>
#include <cstdint>
#include <memory>

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

static const uint32_t MAGIC_KEY = 0xCAFEBABE;
static const size_t MIN_MSG_LEN = sizeof(TcpResponse) + sizeof(MsgHdr); // 24
static const size_t MAX_MSG_LEN = 65535;
static const size_t RECV_BUFFER_SIZE = 65536; // 64KB

// ============================================================================
// Internal message types passed through the pipeline
// ============================================================================

// Raw message: produced by IO thread, consumed by decoder thread.
// Holds a shared reference to the receive buffer.
struct RawMessage {
  std::shared_ptr<Buffer> buffer; // Shared reference to recv buffer
  size_t offset;                  // Start of this message within buffer->data
  size_t length;    // Total message length (== TcpResponse.respLen)
  uint64_t seq_num; // From TcpResponse.respSeq

  RawMessage() : offset(0), length(0), seq_num(0) {}
};

// Decoded sub-message: produced by decoder thread, consumed by worker threads.
// Holds a shared reference to the original buffer (zero-copy).
struct SubMessage {
  std::shared_ptr<Buffer> buffer; // Keeps underlying buffer alive
  uint64_t seq_num;               // From MsgHdr.msgSeqNum
  uint32_t timestamp;             // From MsgHdr.timestamp
  uint16_t flags;                 // From MsgHdr.flags
  const char *body;               // Pointer into buffer->data (after MsgHdr)
  size_t body_length;             // Body size in bytes

  SubMessage()
      : seq_num(0), timestamp(0), flags(0), body(nullptr), body_length(0) {}

  const char *data() const { return body; }
  size_t length() const { return body_length; }
};

#endif // PROTOCOL_H
