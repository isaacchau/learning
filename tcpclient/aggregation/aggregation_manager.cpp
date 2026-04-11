#include "aggregation_manager.h"

#include <chrono>
#include <cstring>

namespace aggregation {

AggregationManager::AggregationManager(const AggregationConfig& config)
    : config_(config)
    , window_duration_ns_(config.window_ms * 1000000ULL) {  // ms to ns
}

AggregationManager::~AggregationManager() {
    stop();
}

bool AggregationManager::init() {
    if (!config_.enabled) {
        return true;
    }
    
    // Initialize window boundaries
    auto now = std::chrono::system_clock::now();
    auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();
    
    current_window_start_ns_ = calculateNextWindowStart(now_ns);
    next_window_close_ns_ = current_window_start_ns_ + window_duration_ns_;
    
    return true;
}

void AggregationManager::start() {
    if (!config_.enabled || running_.exchange(true)) {
        return;
    }
    
    window_thread_ = std::thread(&AggregationManager::windowManagerLoop, this);
}

void AggregationManager::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    
    if (window_thread_.joinable()) {
        window_thread_.join();
    }
}

void AggregationManager::processMessage(const char* msg_buffer, size_t msg_len) {
    if (!config_.enabled) {
        return;
    }
    
    // Parse message type
    MarketDataType msg_type = parseMessageType(msg_buffer, msg_len);
    
    messages_processed_.fetch_add(1, std::memory_order_relaxed);
    
    switch (msg_type) {
        case MarketDataType::ORDER_NEW: {
            const auto* msg = castMessage<OrderNewMsg>(msg_buffer, msg_len);
            if (msg) processOrderNew(msg);
            break;
        }
        case MarketDataType::ORDER_UPDATE: {
            const auto* msg = castMessage<OrderUpdateMsg>(msg_buffer, msg_len);
            if (msg) processOrderUpdate(msg);
            break;
        }
        case MarketDataType::ORDER_CANCEL: {
            const auto* msg = castMessage<OrderCancelMsg>(msg_buffer, msg_len);
            if (msg) processOrderCancel(msg);
            break;
        }
        case MarketDataType::TRADE: {
            const auto* msg = castMessage<TradeMsg>(msg_buffer, msg_len);
            if (msg) processTrade(msg);
            break;
        }
        case MarketDataType::QUOTE_BID: {
            const auto* msg = castMessage<QuoteBidMsg>(msg_buffer, msg_len);
            if (msg) processQuoteBid(msg);
            break;
        }
        case MarketDataType::QUOTE_ASK: {
            const auto* msg = castMessage<QuoteAskMsg>(msg_buffer, msg_len);
            if (msg) processQuoteAsk(msg);
            break;
        }
        default:
            // Unknown message type - ignore or log
            break;
    }
}

void AggregationManager::flushCurrentWindow() {
    closeAndFlushWindow();
}

AggregationManager::Stats AggregationManager::getStats() const {
    Stats s;
    s.messages_processed = messages_processed_.load(std::memory_order_relaxed);
    s.windows_flushed = windows_flushed_.load(std::memory_order_relaxed);
    
    {
        std::lock_guard<std::mutex> lock(order_stats_mutex_);
        s.order_stats_entries = order_stats_map_.size();
    }
    {
        std::lock_guard<std::mutex> lock(trade_stats_mutex_);
        s.trade_stats_entries = trade_stats_map_.size();
    }
    {
        std::lock_guard<std::mutex> lock(quote_stats_mutex_);
        s.quote_stats_entries = quote_stats_map_.size();
    }
    
    return s;
}

// ============================================================================
// Window Management
// ============================================================================

void AggregationManager::windowManagerLoop() {
    while (running_.load(std::memory_order_relaxed)) {
        auto now = std::chrono::system_clock::now();
        auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()).count();
        
        if (shouldCloseWindow(now_ns)) {
            closeAndFlushWindow();
        }
        
        // Sleep for 10ms before checking again
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

bool AggregationManager::shouldCloseWindow(uint64_t now_ns) {
    return now_ns >= next_window_close_ns_.load(std::memory_order_relaxed);
}

void AggregationManager::closeAndFlushWindow() {
    if (!output_queue_) {
        // Just reset stats if no output queue
        resetAllStatsForNewWindow();
        return;
    }
    
    // Flush all stats to output queue
    flushOrderStats();
    flushTradeStats();
    flushQuoteStats();
    
    // Update window boundaries
    uint64_t next_start = next_window_close_ns_.load();
    current_window_start_ns_.store(next_start, std::memory_order_release);
    next_window_close_ns_.store(next_start + window_duration_ns_, std::memory_order_release);
    
    windows_flushed_.fetch_add(1, std::memory_order_relaxed);
    
    // Reset counters for new window
    resetAllStatsForNewWindow();
}

uint64_t AggregationManager::calculateNextWindowStart(uint64_t timestamp_ns) {
    // Align to window boundaries
    // e.g., if window is 1000ms and now is 1234567890123ns,
    // next boundary is at 1234567000000ns
    return (timestamp_ns / window_duration_ns_) * window_duration_ns_;
}

// ============================================================================
// Message Handlers
// ============================================================================

void AggregationManager::processOrderNew(const OrderNewMsg* msg) {
    OrderStatsKey key{msg->getMarket(), msg->getInstrument(), msg->getBroker()};
    
    std::lock_guard<std::mutex> lock(order_stats_mutex_);
    auto& stats = order_stats_map_[key];
    
    stats.newOrders.fetch_add(1, std::memory_order_relaxed);
    stats.openOrders.fetch_add(1, std::memory_order_relaxed);
    stats.totalOrderQty.fetch_add(msg->quantity, std::memory_order_relaxed);
}

void AggregationManager::processOrderUpdate(const OrderUpdateMsg* msg) {
    OrderStatsKey key{msg->getMarket(), msg->getInstrument(), msg->getBroker()};
    
    std::lock_guard<std::mutex> lock(order_stats_mutex_);
    auto& stats = order_stats_map_[key];
    
    stats.modifiedOrders.fetch_add(1, std::memory_order_relaxed);
}

void AggregationManager::processOrderCancel(const OrderCancelMsg* msg) {
    OrderStatsKey key{msg->getMarket(), msg->getInstrument(), msg->getBroker()};
    
    std::lock_guard<std::mutex> lock(order_stats_mutex_);
    auto& stats = order_stats_map_[key];
    
    stats.cancelledOrders.fetch_add(1, std::memory_order_relaxed);
    stats.totalCancelQty.fetch_add(msg->cancelled_qty, std::memory_order_relaxed);
    
    // Decrement open orders (but don't go below 0)
    uint64_t old_open = stats.openOrders.load(std::memory_order_relaxed);
    while (old_open > 0 && !stats.openOrders.compare_exchange_weak(
        old_open, old_open - 1, std::memory_order_relaxed)) {
        // Retry
    }
}

void AggregationManager::processTrade(const TradeMsg* msg) {
    TradeStatsKey key{msg->getMarket(), msg->getInstrument()};
    
    std::lock_guard<std::mutex> lock(trade_stats_mutex_);
    auto& stats = trade_stats_map_[key];
    
    stats.recordTrade(msg->price, msg->quantity);
}

void AggregationManager::processQuoteBid(const QuoteBidMsg* msg) {
    QuoteStatsKey key{msg->getMarket(), msg->getInstrument()};
    
    std::lock_guard<std::mutex> lock(quote_stats_mutex_);
    auto& stats = quote_stats_map_[key];
    
    stats.updateBid(msg->price, msg->quantity);
}

void AggregationManager::processQuoteAsk(const QuoteAskMsg* msg) {
    QuoteStatsKey key{msg->getMarket(), msg->getInstrument()};
    
    std::lock_guard<std::mutex> lock(quote_stats_mutex_);
    auto& stats = quote_stats_map_[key];
    
    stats.updateAsk(msg->price, msg->quantity);
}

// ============================================================================
// Flush to Output Queue
// ============================================================================

void AggregationManager::flushOrderStats() {
    std::lock_guard<std::mutex> lock(order_stats_mutex_);
    
    uint64_t window_start = current_window_start_ns_.load(std::memory_order_relaxed);
    
    for (const auto& entry : order_stats_map_) {
        const auto& key = entry.first;
        const auto& stats = entry.second;
        OutputRecord record;
        record.timestamp_ns = window_start;
        record.measurement = "orders";
        record.addTag("Market", key.market);
        record.addTag("Instrument", key.instrument);
        record.addTag("Broker", key.broker);
        record.addField("newOrders", stats.newOrders.load(std::memory_order_relaxed));
        record.addField("modifiedOrders", stats.modifiedOrders.load(std::memory_order_relaxed));
        record.addField("cancelledOrders", stats.cancelledOrders.load(std::memory_order_relaxed));
        record.addField("openOrders", stats.openOrders.load(std::memory_order_relaxed));
        record.addField("totalOrderQty", stats.totalOrderQty.load(std::memory_order_relaxed));
        record.addField("totalCancelQty", stats.totalCancelQty.load(std::memory_order_relaxed));
        
        output_queue_->push(std::move(record));
    }
}

void AggregationManager::flushTradeStats() {
    std::lock_guard<std::mutex> lock(trade_stats_mutex_);
    
    uint64_t window_start = current_window_start_ns_.load(std::memory_order_relaxed);
    
    for (const auto& entry : trade_stats_map_) {
        const auto& key = entry.first;
        const auto& stats = entry.second;
        OutputRecord record;
        record.timestamp_ns = window_start;
        record.measurement = "trades";
        record.addTag("Market", key.market);
        record.addTag("Instrument", key.instrument);
        record.addField("numTrades", stats.numTrades.load(std::memory_order_relaxed));
        record.addField("totalVolume", stats.totalVolume.load(std::memory_order_relaxed));
        record.addField("vwap", stats.getVWAP());
        record.addField("highPrice", stats.highPrice.load(std::memory_order_relaxed));
        record.addField("lowPrice", stats.lowPrice.load(std::memory_order_relaxed));
        record.addField("largeTrades", stats.largeTrades.load(std::memory_order_relaxed));
        record.addField("blockTrades", stats.blockTrades.load(std::memory_order_relaxed));
        
        output_queue_->push(std::move(record));
    }
}

void AggregationManager::flushQuoteStats() {
    std::lock_guard<std::mutex> lock(quote_stats_mutex_);
    
    uint64_t window_start = current_window_start_ns_.load(std::memory_order_relaxed);
    
    for (const auto& entry : quote_stats_map_) {
        const auto& key = entry.first;
        const auto& stats = entry.second;
        OutputRecord record;
        record.timestamp_ns = window_start;
        record.measurement = "quotes";
        record.addTag("Market", key.market);
        record.addTag("Instrument", key.instrument);
        record.addField("bidUpdates", stats.bidUpdates.load(std::memory_order_relaxed));
        record.addField("askUpdates", stats.askUpdates.load(std::memory_order_relaxed));
        record.addField("bestBid", stats.bestBid.load(std::memory_order_relaxed));
        record.addField("bestAsk", stats.bestAsk.load(std::memory_order_relaxed));
        record.addField("spread", stats.getSpread());
        record.addField("minSpread", stats.minSpread.load(std::memory_order_relaxed));
        record.addField("maxSpread", stats.maxSpread.load(std::memory_order_relaxed));
        
        output_queue_->push(std::move(record));
    }
}

void AggregationManager::resetAllStatsForNewWindow() {
    {
        std::lock_guard<std::mutex> lock(order_stats_mutex_);
        for (auto& entry : order_stats_map_) {
            entry.second.resetCountersForNewWindow();
        }
    }
    {
        std::lock_guard<std::mutex> lock(trade_stats_mutex_);
        for (auto& entry : trade_stats_map_) {
            entry.second.resetForNewWindow();
        }
    }
    {
        std::lock_guard<std::mutex> lock(quote_stats_mutex_);
        for (auto& entry : quote_stats_map_) {
            entry.second.resetCountersForNewWindow();
        }
    }
}

} // namespace aggregation
