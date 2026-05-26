#ifndef MFEED_PROTOCOL_H
#define MFEED_PROTOCOL_H

#include "mfeed_core.h"

namespace md {
namespace mfeed {

struct LoginRequest {
    MFeedHeader header;
    char username[16];
    char password[32];
    uint16_t heartbeat_interval_sec;
};

struct LoginResponse {
    MFeedHeader header;
    uint8_t status_code;
    char status_text[64];
};

struct Heartbeat {
    MFeedHeader header;
    uint8_t status_indicator;
};

} // namespace mfeed
} // namespace md

#endif // MFEED_PROTOCOL_H
