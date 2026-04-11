#ifndef OUTPUT_RECORD_H
#define OUTPUT_RECORD_H

#include <string>
#include <vector>
#include <utility>
#include <cstdint>

namespace aggregation {

// ============================================================================
// Output Record
// Represents one aggregated data point ready for output
// ============================================================================

struct OutputRecord {
    // Measurement name (e.g., "orders", "trades", "quotes")
    std::string measurement;
    
    // Tags (dimensions) - will be output as tag=value pairs
    // e.g., {"Market", "NYSE"}, {"Instrument", "AAPL"}, {"Broker", "IB"}
    std::vector<std::pair<std::string, std::string>> tags;
    
    // Fields (values) - will be output as field=value pairs
    // e.g., {"newOrders", "15"}, {"openOrders", "42"}
    std::vector<std::pair<std::string, std::string>> fields;
    
    // Timestamp in nanoseconds (Unix epoch)
    uint64_t timestamp_ns;
    
    // Helper to add a tag
    void addTag(const std::string& key, const std::string& value) {
        tags.emplace_back(key, value);
    }
    
    // Helper to add a field
    void addField(const std::string& key, const std::string& value) {
        fields.emplace_back(key, value);
    }
    
    void addField(const std::string& key, uint64_t value) {
        fields.emplace_back(key, std::to_string(value));
    }
    
    void addField(const std::string& key, double value) {
        // Format double with appropriate precision
        char buf[64];
        snprintf(buf, sizeof(buf), "%.6f", value);
        fields.emplace_back(key, buf);
    }
    
    void addField(const std::string& key, int64_t value) {
        fields.emplace_back(key, std::to_string(value));
    }
};

} // namespace aggregation

#endif // OUTPUT_RECORD_H
