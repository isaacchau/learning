#include "upstream/market_data.h"
#include "generated/market_data_dumps.h"
#include <iostream>

int main() {
    std::cout << "=== Upstream Struct Demo ===\n";
    market::v2::Trade trade = {};
    trade.id = 12345;
    trade.price = 150.25;
    trade.quantity = 100;
    trade.side = 'B';
    std::cout << DataDumper::dump("trade", trade) << "\n";

    std::cout << "=== Upstream Nested Struct Demo ===\n";
    market::v2::Order order = {};
    order.order_id = 9876543210ULL;
    order.limit_price = 200.50;
    order.qty = 500;
    order.is_active = true;
    std::cout << DataDumper::dump("order", order) << "\n";

    std::cout << "=== Upstream Enum Demo ===\n";
    market::v2::ExecType et = market::v2::ExecType::TRADE;
    std::cout << DataDumper::dump("exec_type", et) << "\n";

    return 0;
}
