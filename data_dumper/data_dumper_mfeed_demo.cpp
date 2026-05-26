#include "upstream/mfeed_types.h"
#include "upstream/mfeed_core.h"
#include "upstream/mfeed_market_data.h"
#include "upstream/mfeed_protocol.h"
#include "generated/md_mfeed_dumps.h"
#include <iostream>
#include <cstring>

using namespace md::mfeed;

int main() {
    std::cout << "=========================================================\n";
    std::cout << "        MD MFeed API DataDumper Demo Application         \n";
    std::cout << "=========================================================\n\n";

    // 1. Session / Protocol Level: Login Request
    std::cout << "--- 1. Session Level: Login Request ---\n";
    LoginRequest login{};
    login.header.message_len = sizeof(LoginRequest);
    login.header.msg_type = MsgType::LOGIN_REQUEST;
    login.header.sequence_number = 1;
    login.header.timestamp_ns = 1716584000000000000ULL; // Simulation timestamp
    std::strncpy(login.username, "trader_alpha", sizeof(login.username) - 1);
    std::strncpy(login.password, "super_secure_password_123", sizeof(login.password) - 1);
    login.heartbeat_interval_sec = 30;

    std::cout << DataDumper::dump("login_request", login) << "\n";

    // 2. Reference / Static Data: Market Definition
    std::cout << "--- 2. Reference Data: Market Definition ---\n";
    MarketDefinition market{};
    market.header.message_len = sizeof(MarketDefinition);
    market.header.msg_type = MsgType::MARKET_DEFINITION;
    market.header.sequence_number = 2;
    market.header.timestamp_ns = 1716584000001000000ULL;
    std::memcpy(market.market_id, "XNYS", 4);
    std::strncpy(market.market_name, "New York Stock Exchange", sizeof(market.market_name) - 1);
    market.state = MarketState::CONTINUOUS;
    market.decimals = 4;

    std::cout << DataDumper::dump("market_definition", market) << "\n";

    // 3. Reference / Static Data: Instrument Definition
    std::cout << "--- 3. Reference Data: Instrument Definition ---\n";
    InstrumentDefinition inst{};
    inst.header.message_len = sizeof(InstrumentDefinition);
    inst.header.msg_type = MsgType::INSTRUMENT_DEFINITION;
    inst.header.sequence_number = 3;
    inst.header.timestamp_ns = 1716584000002000000ULL;
    inst.instrument_id = 998877;
    std::strncpy(inst.symbol, "AAPL", sizeof(inst.symbol) - 1);
    std::memcpy(inst.market_id, "XNYS", 4);
    inst.lot_size = 100;
    inst.tick_size = 0.01;
    inst.instrument_type = InstrumentType::STOCK;

    std::cout << DataDumper::dump("instrument_definition", inst) << "\n";

    // 4. Market Data Level: Order Book State
    std::cout << "--- 4. Market Data Level: Order Book State ---\n";
    OrderBookState book{};
    book.header.message_len = sizeof(OrderBookState);
    book.header.msg_type = MsgType::ORDER_BOOK_STATE;
    book.header.sequence_number = 42;
    book.header.timestamp_ns = 1716584005123456789ULL;
    book.instrument_id = 998877;
    
    // Fill Bid book levels
    book.bid_qty[0] = 500;  book.bid_price[0] = 185.50;
    book.bid_qty[1] = 1200; book.bid_price[1] = 185.45;
    book.bid_qty[2] = 2500; book.bid_price[2] = 185.30;

    // Fill Ask book levels
    book.ask_qty[0] = 800;  book.ask_price[0] = 185.55;
    book.ask_qty[1] = 1500; book.ask_price[1] = 185.60;
    book.ask_qty[2] = 3000; book.ask_price[2] = 185.75;

    std::cout << DataDumper::dump("order_book_state", book) << "\n";

    // 5. Transaction Level: Trade Report (Multi-Leg Combo Trade)
    std::cout << "--- 5. Transaction Level: Multi-Leg Trade Report ---\n";
    TradeReport trade{};
    trade.header.message_len = sizeof(TradeReport);
    trade.header.msg_type = MsgType::TRADE_REPORT;
    trade.header.sequence_number = 1001;
    trade.header.timestamp_ns = 1716584120987654321ULL;
    trade.match_id = 5544332211ULL;
    std::strncpy(trade.symbol, "AAPL-SPREAD", sizeof(trade.symbol) - 1);
    trade.num_legs = 2;

    // Leg 1
    trade.legs[0].leg_match_id = 5544332212ULL;
    trade.legs[0].quantity = 10;
    trade.legs[0].price = 186.00;
    trade.legs[0].side = Side::BUY;
    std::strncpy(trade.legs[0].counterparty, "MSCO", sizeof(trade.legs[0].counterparty) - 1);

    // Leg 2
    trade.legs[1].leg_match_id = 5544332213ULL;
    trade.legs[1].quantity = 10;
    trade.legs[1].price = 185.00;
    trade.legs[1].side = Side::SELL;
    std::strncpy(trade.legs[1].counterparty, "GSCO", sizeof(trade.legs[1].counterparty) - 1);

    std::cout << DataDumper::dump("trade_report", trade) << "\n";

    std::cout << "=========================================================\n";
    std::cout << "        Demo completed successfully!                     \n";
    std::cout << "=========================================================\n";

    return 0;
}
