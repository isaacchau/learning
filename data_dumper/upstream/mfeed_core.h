#ifndef MFEED_CORE_H
#define MFEED_CORE_H

#include "mfeed_types.h"

namespace md {
namespace mfeed {

struct MFeedHeader {
    uint32_t message_len;
    MsgType msg_type;
    uint32_t sequence_number;
    uint64_t timestamp_ns;
};

struct OrderFlags {
    uint8_t is_imbalance : 1;
    uint8_t is_short_sale_exempt : 1;
    uint8_t is_test : 1;
    uint8_t reserved : 5;
};

struct TradeLeg {
    uint64_t leg_match_id;
    uint32_t quantity;
    double price;
    Side side;
    char counterparty[8];
};

} // namespace mfeed
} // namespace md

#endif // MFEED_CORE_H
