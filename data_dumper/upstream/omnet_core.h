#ifndef OMNET_CORE_H
#define OMNET_CORE_H

#include "omnet_types.h"

namespace nasdaq {
namespace omnet {

struct OMnetHeader {
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

} // namespace omnet
} // namespace nasdaq

#endif // OMNET_CORE_H
