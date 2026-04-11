#ifndef AGGREGATION_STATE_H
#define AGGREGATION_STATE_H

#include <atomic>
#include <cstdint>
#include <string>

namespace aggregation {

// ============================================================================
// Order Statistics
// Tracks order lifecycle metrics per (Market, Instrument, Broker)
// ============================================================================

struct OrderStats {
    // Counters - reset at window boundary
    std::atomic<uint64_t> newOrders{0};         // New orders created
    std::atomic<uint64_t> modifiedOrders{0};    // Orders modified
    std::atomic<uint64_t> cancelledOrders{0};   // Orders cancelled
    std::atomic<uint64_t> filledOrders{0};      // Orders fully filled
    
    // Gauges - persist across windows
    std::atomic<uint64_t> openOrders{0};        // Current open orders (increment on NEW, decrement on FILL/CANCEL)
    
    // Volume counters
    std::atomic<uint64_t> totalOrderQty{0};     // Sum of order quantities (new orders)
    std::atomic<uint64_t> totalCancelQty{0};    // Sum of cancelled quantities
    
    void resetCountersForNewWindow() {
        newOrders = 0;
        modifiedOrders = 0;
        cancelledOrders = 0;
        filledOrders = 0;
        totalOrderQty = 0;
        totalCancelQty = 0;
        // openOrders NOT reset - it's a gauge
    }
    
    // For debugging/logging
    std::string toString() const {
        return "OrderStats{new=" + std::to_string(newOrders.load()) +
               ", mod=" + std::to_string(modifiedOrders.load()) +
               ", cancel=" + std::to_string(cancelledOrders.load()) +
               ", open=" + std::to_string(openOrders.load()) + "}";
    }
};

// ============================================================================
// Trade Statistics  
// Tracks trading activity per (Market, Instrument)
// ============================================================================

struct TradeStats {
    // Counters - reset at window boundary
    std::atomic<uint64_t> numTrades{0};         // Number of trades
    std::atomic<uint64_t> totalVolume{0};       // Total quantity traded
    
    // Value tracking for VWAP
    std::atomic<double> totalValue{0.0};        // Sum of (price * quantity) for VWAP
    
    // Price extremes - reset at window boundary
    std::atomic<double> highPrice{0.0};         // Highest trade price
    std::atomic<double> lowPrice{0.0};          // Lowest trade price
    
    // Trade value distribution
    std::atomic<uint64_t> largeTrades{0};       // Trades > 10000 shares
    std::atomic<uint64_t> blockTrades{0};       // Trades > 100000 shares
    
    void recordTrade(double price, uint32_t quantity) {
        numTrades++;
        totalVolume += quantity;
        
        double tradeValue = price * quantity;
        // Atomic add for double (C++20 has atomic<double>::fetch_add, 
        // but for C++14 we use compare_exchange loop or just accept minor races)
        double oldValue = totalValue.load(std::memory_order_relaxed);
        while (!totalValue.compare_exchange_weak(
            oldValue, 
            oldValue + tradeValue,
            std::memory_order_relaxed)) {
            // Retry on failure
        }
        
        // Update high/low with compare_exchange
        double oldHigh = highPrice.load(std::memory_order_relaxed);
        while (price > oldHigh && !highPrice.compare_exchange_weak(
            oldHigh, price, std::memory_order_relaxed)) {
            // Retry
        }
        
        double oldLow = lowPrice.load(std::memory_order_relaxed);
        // Initialize low if zero
        if (oldLow == 0.0) {
            lowPrice.compare_exchange_strong(oldLow, price, std::memory_order_relaxed);
            oldLow = price;
        }
        while (price < oldLow && !lowPrice.compare_exchange_weak(
            oldLow, price, std::memory_order_relaxed)) {
            // Retry
        }
        
        // Count large/block trades
        if (quantity >= 100000) {
            blockTrades++;
            largeTrades++;
        } else if (quantity >= 10000) {
            largeTrades++;
        }
    }
    
    double getVWAP() const {
        uint64_t vol = totalVolume.load(std::memory_order_relaxed);
        if (vol == 0) return 0.0;
        return totalValue.load(std::memory_order_relaxed) / vol;
    }
    
    void resetForNewWindow() {
        numTrades = 0;
        totalVolume = 0;
        totalValue = 0.0;
        highPrice = 0.0;
        lowPrice = 0.0;
        largeTrades = 0;
        blockTrades = 0;
    }
};

// ============================================================================
// Quote Statistics
// Tracks quote/book activity per (Market, Instrument)
// ============================================================================

struct QuoteStats {
    std::atomic<uint64_t> bidUpdates{0};        // Number of bid updates
    std::atomic<uint64_t> askUpdates{0};        // Number of ask updates
    
    // Current best prices (gauges - persist)
    std::atomic<double> bestBid{0.0};
    std::atomic<double> bestAsk{0.0};
    std::atomic<uint32_t> bestBidQty{0};
    std::atomic<uint32_t> bestAskQty{0};
    
    // Spread tracking (computed from bid/ask)
    std::atomic<double> minSpread{0.0};
    std::atomic<double> maxSpread{0.0};
    
    void updateBid(double price, uint32_t qty) {
        bidUpdates++;
        bestBid.store(price, std::memory_order_relaxed);
        bestBidQty.store(qty, std::memory_order_relaxed);
        updateSpread();
    }
    
    void updateAsk(double price, uint32_t qty) {
        askUpdates++;
        bestAsk.store(price, std::memory_order_relaxed);
        bestAskQty.store(qty, std::memory_order_relaxed);
        updateSpread();
    }
    
    void updateSpread() {
        double bid = bestBid.load(std::memory_order_relaxed);
        double ask = bestAsk.load(std::memory_order_relaxed);
        if (bid > 0.0 && ask > 0.0) {
            double spread = ask - bid;
            
            double oldMin = minSpread.load(std::memory_order_relaxed);
            if (oldMin == 0.0 || spread < oldMin) {
                minSpread.compare_exchange_strong(oldMin, spread, std::memory_order_relaxed);
            }
            
            double oldMax = maxSpread.load(std::memory_order_relaxed);
            if (spread > oldMax) {
                maxSpread.compare_exchange_weak(oldMax, spread, std::memory_order_relaxed);
            }
        }
    }
    
    double getSpread() const {
        double ask = bestAsk.load(std::memory_order_relaxed);
        double bid = bestBid.load(std::memory_order_relaxed);
        if (ask > 0.0 && bid > 0.0) {
            return ask - bid;
        }
        return 0.0;
    }
    
    void resetCountersForNewWindow() {
        bidUpdates = 0;
        askUpdates = 0;
        minSpread = 0.0;
        maxSpread = 0.0;
        // bestBid, bestAsk, quantities persist as gauges
    }
};

// ============================================================================
// Composite Key Types
// ============================================================================

// OrderStats key: (Market, Instrument, Broker)
struct OrderStatsKey {
    std::string market;
    std::string instrument;
    std::string broker;
    
    bool operator==(const OrderStatsKey& other) const {
        return market == other.market && 
               instrument == other.instrument && 
               broker == other.broker;
    }
};

// TradeStats key: (Market, Instrument)
struct TradeStatsKey {
    std::string market;
    std::string instrument;
    
    bool operator==(const TradeStatsKey& other) const {
        return market == other.market && instrument == other.instrument;
    }
};

// QuoteStats key: (Market, Instrument)
struct QuoteStatsKey {
    std::string market;
    std::string instrument;
    
    bool operator==(const QuoteStatsKey& other) const {
        return market == other.market && instrument == other.instrument;
    }
};

// ============================================================================
// Hash Functions for Unordered Maps
// ============================================================================

struct OrderStatsKeyHash {
    size_t operator()(const OrderStatsKey& k) const {
        // Simple hash combining
        size_t h1 = std::hash<std::string>{}(k.market);
        size_t h2 = std::hash<std::string>{}(k.instrument);
        size_t h3 = std::hash<std::string>{}(k.broker);
        return h1 ^ (h2 << 1) ^ (h3 << 2);
    }
};

struct TradeStatsKeyHash {
    size_t operator()(const TradeStatsKey& k) const {
        size_t h1 = std::hash<std::string>{}(k.market);
        size_t h2 = std::hash<std::string>{}(k.instrument);
        return h1 ^ (h2 << 1);
    }
};

struct QuoteStatsKeyHash {
    size_t operator()(const QuoteStatsKey& k) const {
        size_t h1 = std::hash<std::string>{}(k.market);
        size_t h2 = std::hash<std::string>{}(k.instrument);
        return h1 ^ (h2 << 1);
    }
};

} // namespace aggregation

#endif // AGGREGATION_STATE_H
