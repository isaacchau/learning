#ifndef AGGREGATION_MANAGER_H
#define AGGREGATION_MANAGER_H

#include "aggregation_state.h"
#include "aggregation_config.h"
#include "output_queue.h"
#include "output_record.h"

#include "../market_data/message_types.h"

#include <unordered_map>
#include <mutex>
#include <atomic>
#include <thread>
#include <vector>
#include <functional>

namespace aggregation {

// ============================================================================
// Aggregation Manager
// Central manager for all aggregation state
// - Workers call processMessage() to update stats
// - Window manager thread flushes to output queue periodically
// ============================================================================

class AggregationManager {
public:
    explicit AggregationManager(const AggregationConfig& config);
    ~AggregationManager();
    
    // Initialize - must be called before use
    bool init();
    
    // Start the window manager thread
    void start();
    
    // Stop all processing
    void stop();
    
    // Process a market data message (called by worker threads)
    void processMessage(const char* msg_buffer, size_t msg_len);
    
    // Set output queue (must be called before start)
    void setOutputQueue(OutputQueue* queue) { output_queue_ = queue; }
    
    // Get current window start time
    uint64_t currentWindowStartNs() const { return current_window_start_ns_.load(); }
    
    // Manual flush (for testing or forced window close)
    void flushCurrentWindow();
    
    // Get stats for debugging
    struct Stats {
        uint64_t messages_processed;
        uint64_t windows_flushed;
        uint64_t order_stats_entries;
        uint64_t trade_stats_entries;
        uint64_t quote_stats_entries;
    };
    Stats getStats() const;

private:
    // Window manager thread loop
    void windowManagerLoop();
    
    // Check if window should close
    bool shouldCloseWindow(uint64_t now_ns);
    
    // Close current window and flush to queue
    void closeAndFlushWindow();
    
    // Calculate next window boundary
    uint64_t calculateNextWindowStart(uint64_t timestamp_ns);
    
    // Message type handlers
    void processOrderNew(const OrderNewMsg* msg);
    void processOrderUpdate(const OrderUpdateMsg* msg);
    void processOrderCancel(const OrderCancelMsg* msg);
    void processTrade(const TradeMsg* msg);
    void processQuoteBid(const QuoteBidMsg* msg);
    void processQuoteAsk(const QuoteAskMsg* msg);
    
    // Flush helpers
    void flushOrderStats();
    void flushTradeStats();
    void flushQuoteStats();
    
    // Reset stats for new window
    void resetAllStatsForNewWindow();
    
    // Configuration
    AggregationConfig config_;
    
    // Output queue
    OutputQueue* output_queue_ = nullptr;
    
    // Window timing
    std::atomic<uint64_t> current_window_start_ns_{0};
    std::atomic<uint64_t> next_window_close_ns_{0};
    uint64_t window_duration_ns_;
    
    // Threading
    std::thread window_thread_;
    std::atomic<bool> running_{false};
    
    // Aggregation state maps
    // OrderStats: keyed by (market, instrument, broker)
    std::unordered_map<OrderStatsKey, OrderStats, OrderStatsKeyHash> order_stats_map_;
    mutable std::mutex order_stats_mutex_;
    
    // TradeStats: keyed by (market, instrument)
    std::unordered_map<TradeStatsKey, TradeStats, TradeStatsKeyHash> trade_stats_map_;
    mutable std::mutex trade_stats_mutex_;
    
    // QuoteStats: keyed by (market, instrument)
    std::unordered_map<QuoteStatsKey, QuoteStats, QuoteStatsKeyHash> quote_stats_map_;
    mutable std::mutex quote_stats_mutex_;
    
    // Stats counters
    std::atomic<uint64_t> messages_processed_{0};
    std::atomic<uint64_t> windows_flushed_{0};
};

} // namespace aggregation

#endif // AGGREGATION_MANAGER_H
