#ifndef OMNET_TYPES_H
#define OMNET_TYPES_H

#include <cstdint>

namespace nasdaq {
namespace omnet {

enum class MsgType : uint16_t {
    LOGIN_REQUEST = 101,
    LOGIN_RESPONSE = 102,
    HEARTBEAT = 103,
    MARKET_DEFINITION = 201,
    INSTRUMENT_DEFINITION = 202,
    ORDER_BOOK_STATE = 301,
    TRADE_REPORT = 302
};

enum class Side : char {
    BUY = 'B',
    SELL = 'S',
    SHORT_SELL = 'T'
};

enum OrderType : uint8_t {
    MARKET = 1,
    LIMIT = 2,
    STOP = 3,
    ICEBERG = 4
};

enum class TimeInForce : uint8_t {
    DAY = 0,
    GTC = 1, // Good 'Til Cancelled
    IOC = 3, // Immediate or Cancel
    FOK = 4  // Fill or Kill
};

enum class InstrumentType : uint8_t {
    STOCK = 1,
    OPTION = 2,
    FUTURE = 3
};

enum class MarketState : uint8_t {
    PRE_OPEN = 1,
    CONTINUOUS = 2,
    CLOSED = 3
};

} // namespace omnet
} // namespace nasdaq

#endif // OMNET_TYPES_H
