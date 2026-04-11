#ifndef AGGREGATION_CONFIG_H
#define AGGREGATION_CONFIG_H

#include <string>
#include <cstdint>

namespace aggregation {

// ============================================================================
// Output Format Enumeration
// ============================================================================

enum class OutputFormat {
    INFLUXDB_LINE,  // InfluxDB Line Protocol format
    CSV             // CSV with header
};

// ============================================================================
// Aggregation Configuration
// ============================================================================

struct AggregationConfig {
    // Master switch
    bool enabled = false;
    
    // Time window for aggregation (milliseconds)
    // Examples: 1000 = 1 second, 500 = 500ms, 60000 = 1 minute
    uint64_t window_ms = 1000;
    
    // Output format
    OutputFormat output_format = OutputFormat::INFLUXDB_LINE;
    
    // Output directory (will be created if not exists)
    std::string output_dir = "./output";
    
    // Filename prefix
    std::string filename_prefix = "marketdata";
    
    // Maximum file size before rotation (MB, 0 = no size limit)
    size_t max_file_size_mb = 100;
    
    // Validate configuration
    // Returns empty string if valid, error message otherwise
    std::string validate() const {
        if (!enabled) {
            return "";  // Disabled is always valid
        }
        
        if (window_ms == 0) {
            return "window_ms must be greater than 0";
        }
        
        if (window_ms < 100) {
            return "window_ms must be at least 100ms (100)";
        }
        
        if (window_ms > 3600000) {  // 1 hour
            return "window_ms must be less than 1 hour (3600000)";
        }
        
        if (output_dir.empty()) {
            return "output_dir cannot be empty";
        }
        
        if (filename_prefix.empty()) {
            return "filename_prefix cannot be empty";
        }
        
        return "";
    }
};

// Helper to convert string to OutputFormat
inline OutputFormat parseOutputFormat(const std::string& str) {
    if (str == "csv" || str == "CSV") {
        return OutputFormat::CSV;
    }
    // Default to InfluxDB Line
    return OutputFormat::INFLUXDB_LINE;
}

// Helper to convert OutputFormat to string
inline const char* outputFormatToString(OutputFormat fmt) {
    switch (fmt) {
        case OutputFormat::INFLUXDB_LINE:
            return "influxdb_line";
        case OutputFormat::CSV:
            return "csv";
        default:
            return "unknown";
    }
}

} // namespace aggregation

#endif // AGGREGATION_CONFIG_H
