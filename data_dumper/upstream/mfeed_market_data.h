#ifndef MFEED_MARKET_DATA_H
#define MFEED_MARKET_DATA_H

#include "mfeed_core.h"

namespace md {
namespace mfeed {

struct MarketDefinition {
    MFeedHeader header;
    char market_id[4];
    char market_name[32];
    MarketState state;
    uint8_t decimals;
};

struct InstrumentDefinition {
    MFeedHeader header;
    uint64_t instrument_id;
    char symbol[16];
    char market_id[4];
    uint32_t lot_size;
    double tick_size;
    InstrumentType instrument_type;
};

struct OrderBookState {
    MFeedHeader header;
    uint64_t instrument_id;
    uint32_t bid_qty[3];
    double bid_price[3];
    uint32_t ask_qty[3];
    double ask_price[3];
};

struct TradeReport {
    MFeedHeader header;
    uint64_t match_id;
    char symbol[16];
    uint16_t num_legs;
    TradeLeg legs[4];
};

} // namespace mfeed
} // namespace md

#endif // MFEED_MARKET_DATA_H
