#ifndef MESSAGE_TYPES_H
#define MESSAGE_TYPES_H

#include <cstdint>
#include <cstring>

// ============================================================================
// Market Data Message Type Enumeration
// ============================================================================

enum class MarketDataType : uint8_t {
    UNKNOWN = 0,
    ORDER_NEW,
    ORDER_UPDATE,
    ORDER_CANCEL,
    TRADE,
    QUOTE_BID,
    QUOTE_ASK,
    INSTRUMENT_REF
};

// ============================================================================
// Base Message Header (common to all messages)
// This assumes each message starts with a type discriminator
// ============================================================================

struct MsgHeader {
    uint8_t msg_type;           // MarketDataType as uint8_t
    uint8_t reserved[3];        // Padding for alignment
    uint32_t msg_seq_num;       // Sequence number
    uint64_t timestamp_ns;      // Nanosecond timestamp (Unix epoch)
};

// ============================================================================
// Order Messages
// ============================================================================

// New order created
struct OrderNewMsg {
    static constexpr MarketDataType TYPE = MarketDataType::ORDER_NEW;
    
    MsgHeader header;
    char market[16];            // Exchange/Market (e.g., "NYSE", "NASDAQ")
    char instrument[32];        // Symbol (e.g., "AAPL", "MSFT")
    char broker[32];            // Broker ID
    uint64_t order_id;          // Unique order identifier
    double price;               // Order price
    uint32_t quantity;          // Order quantity
    char side;                  // 'B' = Buy, 'S' = Sell
    char order_type;            // 'L' = Limit, 'M' = Market, etc.
    uint8_t padding[5];         // Align to 8 bytes
    
    // Helper methods
    const char* getMarket() const { return market; }
    const char* getInstrument() const { return instrument; }
    const char* getBroker() const { return broker; }
    uint64_t getTimestampNs() const { return header.timestamp_ns; }
};

// Order updated (e.g., quantity modified)
struct OrderUpdateMsg {
    static constexpr MarketDataType TYPE = MarketDataType::ORDER_UPDATE;
    
    MsgHeader header;
    char market[16];
    char instrument[32];
    char broker[32];
    uint64_t order_id;          // Order being updated
    double new_price;           // New price (0 if unchanged)
    uint32_t new_quantity;      // New quantity (0 if unchanged)
    uint32_t old_quantity;      // Previous quantity (for delta calc)
    char side;                  // 'B' or 'S'
    uint8_t padding[7];
    
    const char* getMarket() const { return market; }
    const char* getInstrument() const { return instrument; }
    const char* getBroker() const { return broker; }
    uint64_t getTimestampNs() const { return header.timestamp_ns; }
};

// Order cancelled
struct OrderCancelMsg {
    static constexpr MarketDataType TYPE = MarketDataType::ORDER_CANCEL;
    
    MsgHeader header;
    char market[16];
    char instrument[32];
    char broker[32];
    uint64_t order_id;          // Order being cancelled
    uint32_t cancelled_qty;     // Quantity cancelled
    char side;                  // 'B' or 'S'
    char cancel_reason;         // 'U' = User, 'S' = System, etc.
    uint8_t padding[6];
    
    const char* getMarket() const { return market; }
    const char* getInstrument() const { return instrument; }
    const char* getBroker() const { return broker; }
    uint64_t getTimestampNs() const { return header.timestamp_ns; }
};

// ============================================================================
// Trade Message
// ============================================================================

struct TradeMsg {
    static constexpr MarketDataType TYPE = MarketDataType::TRADE;
    
    MsgHeader header;
    char market[16];            // Exchange where trade occurred
    char instrument[32];        // Symbol
    uint64_t trade_id;          // Unique trade ID
    uint64_t bid_order_id;      // Buy order ID (optional, 0 if unknown)
    uint64_t ask_order_id;      // Sell order ID (optional, 0 if unknown)
    double price;               // Trade price
    uint32_t quantity;          // Trade quantity
    char aggressor_side;        // 'B' = Buy side aggressor, 'S' = Sell side
    char trade_condition;       // 'R' = Regular, 'O' = Opening, 'C' = Closing, etc.
    uint8_t padding[6];
    
    const char* getMarket() const { return market; }
    const char* getInstrument() const { return instrument; }
    uint64_t getTimestampNs() const { return header.timestamp_ns; }
    double getValue() const { return price * quantity; }
};

// ============================================================================
// Quote Messages (Level 1)
// ============================================================================

struct QuoteBidMsg {
    static constexpr MarketDataType TYPE = MarketDataType::QUOTE_BID;
    
    MsgHeader header;
    char market[16];
    char instrument[32];
    double price;
    uint32_t quantity;
    uint32_t num_orders;        // Number of orders at this level
    uint8_t padding[4];
    
    const char* getMarket() const { return market; }
    const char* getInstrument() const { return instrument; }
    uint64_t getTimestampNs() const { return header.timestamp_ns; }
};

struct QuoteAskMsg {
    static constexpr MarketDataType TYPE = MarketDataType::QUOTE_ASK;
    
    MsgHeader header;
    char market[16];
    char instrument[32];
    double price;
    uint32_t quantity;
    uint32_t num_orders;
    uint8_t padding[4];
    
    const char* getMarket() const { return market; }
    const char* getInstrument() const { return instrument; }
    uint64_t getTimestampNs() const { return header.timestamp_ns; }
};

// ============================================================================
// Message Parsing Helpers
// ============================================================================

// Extract message type from raw buffer
inline MarketDataType parseMessageType(const char* buffer, size_t len) {
    if (len < sizeof(MsgHeader)) {
        return MarketDataType::UNKNOWN;
    }
    const MsgHeader* hdr = reinterpret_cast<const MsgHeader*>(buffer);
    if (hdr->msg_type > static_cast<uint8_t>(MarketDataType::INSTRUMENT_REF)) {
        return MarketDataType::UNKNOWN;
    }
    return static_cast<MarketDataType>(hdr->msg_type);
}

// Template to cast buffer to specific message type
template<typename T>
const T* castMessage(const char* buffer, size_t len) {
    if (len < sizeof(T)) {
        return nullptr;
    }
    return reinterpret_cast<const T*>(buffer);
}

#endif // MESSAGE_TYPES_H
