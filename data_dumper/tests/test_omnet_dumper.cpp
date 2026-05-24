#include "upstream/omnet_types.h"
#include "upstream/omnet_core.h"
#include "upstream/omnet_market_data.h"
#include "upstream/omnet_protocol.h"
#include "generated/nasdaq_omnet_dumps.h"
#include <iostream>
#include <cstring>
#include <cassert>

using namespace nasdaq::omnet;

// Helper to remove any potential carriage returns for cross-platform comparison
std::string clean_string(const std::string& str) {
    std::string result;
    for (char c : str) {
        if (c != '\r') {
            result += c;
        }
    }
    return result;
}

void test_login_request() {
    LoginRequest login{};
    login.header.message_len = sizeof(LoginRequest);
    login.header.msg_type = MsgType::LOGIN_REQUEST;
    login.header.sequence_number = 1;
    login.header.timestamp_ns = 1716584000000000000ULL;
    std::strncpy(login.username, "trader_alpha", sizeof(login.username) - 1);
    std::strncpy(login.password, "super_secure_password_123", sizeof(login.password) - 1);
    login.heartbeat_interval_sec = 30;

    std::string out = clean_string(DataDumper::dump("login_request", login));
    
    std::string expected = 
        "login_request = nasdaq::omnet::LoginRequest {\n"
        "  header = nasdaq::omnet::OMnetHeader {\n"
        "    message_len = 80\n"
        "    msg_type = nasdaq::omnet::MsgType::LOGIN_REQUEST\n"
        "    sequence_number = 1\n"
        "    timestamp_ns = 1716584000000000000\n"
        "  }\n"
        "  username = \"trader_alpha\" (12/16)\n"
        "  password = \"super_secure_password_123\" (25/32)\n"
        "  heartbeat_interval_sec = 30\n"
        "}\n";

    assert(out == expected);
}

void test_market_definition() {
    MarketDefinition market{};
    market.header.message_len = sizeof(MarketDefinition);
    market.header.msg_type = MsgType::MARKET_DEFINITION;
    market.header.sequence_number = 2;
    market.header.timestamp_ns = 1716584000001000000ULL;
    // Copy exactly 4 chars (fully occupied, no null term, typical in binary formats)
    std::memcpy(market.market_id, "XNYS", 4);
    std::strncpy(market.market_name, "New York Stock Exchange", sizeof(market.market_name) - 1);
    market.state = MarketState::CONTINUOUS;
    market.decimals = 4;

    std::string out = clean_string(DataDumper::dump("market_definition", market));
    
    std::string expected = 
        "market_definition = nasdaq::omnet::MarketDefinition {\n"
        "  header = nasdaq::omnet::OMnetHeader {\n"
        "    message_len = 64\n"
        "    msg_type = nasdaq::omnet::MsgType::MARKET_DEFINITION\n"
        "    sequence_number = 2\n"
        "    timestamp_ns = 1716584000001000000\n"
        "  }\n"
        "  market_id = \"XNYS\" (4/4)\n"
        "  market_name = \"New York Stock Exchange\" (23/32)\n"
        "  state = nasdaq::omnet::MarketState::CONTINUOUS\n"
        "  decimals = 4\n"
        "}\n";

    assert(out == expected);
}

void test_order_book_state() {
    OrderBookState book{};
    book.header.message_len = sizeof(OrderBookState);
    book.header.msg_type = MsgType::ORDER_BOOK_STATE;
    book.header.sequence_number = 42;
    book.header.timestamp_ns = 1716584005123456789ULL;
    book.instrument_id = 998877;
    
    book.bid_qty[0] = 500;  book.bid_price[0] = 185.50;
    book.bid_qty[1] = 1200; book.bid_price[1] = 185.45;
    book.bid_qty[2] = 2500; book.bid_price[2] = 185.30;

    book.ask_qty[0] = 800;  book.ask_price[0] = 185.55;
    book.ask_qty[1] = 1500; book.ask_price[1] = 185.60;
    book.ask_qty[2] = 3000; book.ask_price[2] = 185.75;

    std::string out = clean_string(DataDumper::dump("order_book_state", book));
    
    std::string expected = 
        "order_book_state = nasdaq::omnet::OrderBookState {\n"
        "  header = nasdaq::omnet::OMnetHeader {\n"
        "    message_len = 112\n"
        "    msg_type = nasdaq::omnet::MsgType::ORDER_BOOK_STATE\n"
        "    sequence_number = 42\n"
        "    timestamp_ns = 1716584005123456789\n"
        "  }\n"
        "  instrument_id = 998877\n"
        "  bid_qty = [3] {\n"
        "    [0] = 500\n"
        "    [1] = 1200\n"
        "    [2] = 2500\n"
        "  }\n"
        "  bid_price = [3] {\n"
        "    [0] = 185.5\n"
        "    [1] = 185.45\n"
        "    [2] = 185.3\n"
        "  }\n"
        "  ask_qty = [3] {\n"
        "    [0] = 800\n"
        "    [1] = 1500\n"
        "    [2] = 3000\n"
        "  }\n"
        "  ask_price = [3] {\n"
        "    [0] = 185.55\n"
        "    [1] = 185.6\n"
        "    [2] = 185.75\n"
        "  }\n"
        "}\n";

    assert(out == expected);
}

int main() {
    std::cout << "Running test_login_request()...\n";
    test_login_request();
    std::cout << "Running test_market_definition()...\n";
    test_market_definition();
    std::cout << "Running test_order_book_state()...\n";
    test_order_book_state();

    std::cout << "All OMnet printout verification tests PASSED!\n";
    return 0;
}
