#ifndef OMNET_PROTOCOL_H
#define OMNET_PROTOCOL_H

#include "omnet_core.h"

namespace nasdaq {
namespace omnet {

struct LoginRequest {
    OMnetHeader header;
    char username[16];
    char password[32];
    uint16_t heartbeat_interval_sec;
};

struct LoginResponse {
    OMnetHeader header;
    uint8_t status_code;
    char status_text[64];
};

struct Heartbeat {
    OMnetHeader header;
    uint8_t status_indicator;
};

} // namespace omnet
} // namespace nasdaq

#endif // OMNET_PROTOCOL_H
