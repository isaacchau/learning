#include "data_dumper.h"
#include <cstring>
#include <iostream>
#include <memory>

// ============================================================================
// Demo struct 1: Simple request-like struct
// ============================================================================

struct TcpRequest {
    uint32_t reqKey;
    char reqItem[32];
    uint64_t lastRespSeq;
    char clientID[32];

    DD_DUMPABLE() {
        DD_FIELD(reqKey);
        DD_FIELD(reqItem);
        DD_FIELD(lastRespSeq);
        DD_FIELD(clientID);
    }
};

// ============================================================================
// Demo struct 2: Nested header
// ============================================================================

struct MsgHeader {
    uint8_t msg_type;
    uint32_t msg_seq_num;
    uint64_t timestamp_ns;

    DD_DUMPABLE() {
        DD_FIELD(msg_type);
        DD_FIELD(msg_seq_num);
        DD_FIELD(timestamp_ns);
    }
};

// ============================================================================
// Demo enum: Market data types
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

DD_ENUM(MarketDataType, DD_ENUM_VAL(MarketDataType::UNKNOWN)
           DD_ENUM_VAL(MarketDataType::ORDER_NEW)
               DD_ENUM_VAL(MarketDataType::ORDER_UPDATE)
                   DD_ENUM_VAL(MarketDataType::ORDER_CANCEL)
                       DD_ENUM_VAL(MarketDataType::TRADE)
                           DD_ENUM_VAL(MarketDataType::QUOTE_BID)
                               DD_ENUM_VAL(MarketDataType::QUOTE_ASK)
                                   DD_ENUM_VAL(MarketDataType::INSTRUMENT_REF))

// ============================================================================
// Demo struct 3: Market data message with nested header and enum
// ============================================================================

struct OrderNewMsg {
    MsgHeader header;
    char market[16];
    char instrument[32];
    char broker[32];
    uint64_t order_id;
    double price;
    uint32_t quantity;
    char side;
    char order_type;

    DD_DUMPABLE() {
        DD_FIELD(header);
        DD_FIELD(market);
        DD_FIELD(instrument);
        DD_FIELD(broker);
        DD_FIELD(order_id);
        DD_FIELD(price);
        DD_FIELD(quantity);
        DD_FIELD(side);
        DD_FIELD(order_type);
    }
};

// ============================================================================
// Demo struct 4: Connection stats
// ============================================================================

struct ConnectionStats {
    uint64_t connection_id;
    uint64_t messages_received;
    bool connected;
    std::string endpoint;

    DD_DUMPABLE() {
        DD_FIELD(connection_id);
        DD_FIELD(messages_received);
        DD_FIELD(connected);
        DD_FIELD(endpoint);
    }
};

// ============================================================================
// Demo struct 5: Snapshot with vector of nested structs
// ============================================================================

struct StatsSnapshot {
    uint64_t messages_received;
    uint64_t messages_decoded;
    uint64_t messages_processed;
    uint64_t messages_dropped;
    std::vector<ConnectionStats> connection_stats;

    DD_DUMPABLE() {
        DD_FIELD(messages_received);
        DD_FIELD(messages_decoded);
        DD_FIELD(messages_processed);
        DD_FIELD(messages_dropped);
        DD_FIELD(connection_stats);
    }
};

// ============================================================================
// Helper to fill a request struct
// ============================================================================

static TcpRequest make_request() {
    TcpRequest req = {};
    req.reqKey = 0xCAFEBABE;
    std::strncpy(req.reqItem, "default", sizeof(req.reqItem));
    req.lastRespSeq = 42;
    std::strncpy(req.clientID, "MsgClient", sizeof(req.clientID));
    return req;
}

// ============================================================================
// Helper to fill an order struct
// ============================================================================

static OrderNewMsg make_order() {
    OrderNewMsg order = {};
    order.header.msg_type = static_cast<uint8_t>(MarketDataType::ORDER_NEW);
    order.header.msg_seq_num = 12345;
    order.header.timestamp_ns = 1714041600000000000ULL;
    std::strncpy(order.market, "NYSE", sizeof(order.market));
    std::strncpy(order.instrument, "AAPL", sizeof(order.instrument));
    std::strncpy(order.broker, "BROKER1", sizeof(order.broker));
    order.order_id = 9876543210ULL;
    order.price = 150.25;
    order.quantity = 100;
    order.side = 'B';
    order.order_type = 'L';
    return order;
}

// ============================================================================
// Demo main
// ============================================================================

int main() {
    std::cout << "=== Demo 1: Simple Struct ===\n";
    {
        TcpRequest req = make_request();
        std::cout << DataDumper::dump("request", req) << "\n";
    }

    std::cout << "=== Demo 2: Nested Struct ===\n";
    {
        OrderNewMsg order = make_order();
        std::cout << DataDumper::dump("order", order) << "\n";
    }

    std::cout << "=== Demo 3: Enum ===\n";
    {
        MarketDataType t1 = MarketDataType::TRADE;
        MarketDataType t2 = MarketDataType::UNKNOWN;
        std::cout << DataDumper::dump("type_trade", t1) << "\n";
        std::cout << DataDumper::dump("type_unknown", t2) << "\n";
    }

    std::cout << "=== Demo 4: Vector ===\n";
    {
        std::vector<OrderNewMsg> orders;
        orders.push_back(make_order());
        orders.push_back(make_order());
        std::cout << DataDumper::dump("orders", orders) << "\n";
    }

    std::cout << "=== Demo 5: Shared Ptr (non-null and null) ===\n";
    {
        auto ptr = std::make_shared<OrderNewMsg>(make_order());
        std::shared_ptr<OrderNewMsg> null_ptr;
        std::cout << DataDumper::dump("shared_order", ptr) << "\n";
        std::cout << DataDumper::dump("null_order", null_ptr) << "\n";
    }

    std::cout << "=== Demo 6: Map ===\n";
    {
        std::map<std::string, int> prices;
        prices["AAPL"] = 150;
        prices["GOOGL"] = 2800;
        prices["MSFT"] = 420;
        std::cout << DataDumper::dump("prices", prices) << "\n";
    }

    std::cout << "=== Demo 7: Pair ===\n";
    {
        std::pair<std::string, double> quote("AAPL", 150.25);
        std::cout << DataDumper::dump("quote", quote) << "\n";
    }

    std::cout << "=== Demo 8: Complex Snapshot ===\n";
    {
        StatsSnapshot snap = {};
        snap.messages_received = 1000000;
        snap.messages_decoded = 999958;
        snap.messages_processed = 999958;
        snap.messages_dropped = 42;
        snap.connection_stats.push_back({1, 500000, true, "127.0.0.1:8888"});
        snap.connection_stats.push_back({2, 499958, true, "127.0.0.1:8889"});
        snap.connection_stats.push_back({3, 0, false, "127.0.0.1:8890"});
        std::cout << DataDumper::dump("snapshot", snap) << "\n";
    }

    std::cout << "=== Demo 9: Raw Pointer ===\n";
    {
        int x = 42;
        int* p = &x;
        int* np = nullptr;
        std::cout << DataDumper::dump("ptr", p) << "\n";
        std::cout << DataDumper::dump("null_ptr", np) << "\n";
    }

    std::cout << "=== Demo 10: Atomic ===\n";
    {
        std::atomic<uint64_t> counter(999);
        std::cout << DataDumper::dump("counter", counter) << "\n";
    }

    std::cout << "=== Demo 11: Unregistered Enum (fallback) ===\n";
    {
        enum class Color { RED = 0, GREEN = 1, BLUE = 2 };
        Color c = Color::GREEN;
        std::cout << DataDumper::dump("color", c) << "\n";
    }

    std::cout << "=== Demo 12: String with special chars ===\n";
    {
        struct Message {
            std::string text;
            DD_DUMPABLE() { DD_FIELD(text); }
        };
        Message msg = {"Hello\nWorld\t!\"Quote\""};
        std::cout << DataDumper::dump("message", msg) << "\n";
    }

    return 0;
}
