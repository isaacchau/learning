#ifndef MARKET_DATA_H
#define MARKET_DATA_H

#include <cstdint>

namespace market {
namespace v2 {

struct Trade {
    uint64_t id;
    double price;
    uint32_t quantity;
    char side;
};

struct Order {
    uint64_t order_id;
    char symbol[16];
    double limit_price;
    uint32_t qty;
    bool is_active;
};

enum class ExecType : uint8_t {
    NEW = 1,
    CANCEL = 2,
    TRADE = 3
};

} // namespace v2
} // namespace market

#endif // MARKET_DATA_H
