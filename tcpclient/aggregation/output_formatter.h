#ifndef OUTPUT_FORMATTER_H
#define OUTPUT_FORMATTER_H

#include "output_record.h"
#include "aggregation_config.h"

#include <string>
#include <sstream>

namespace aggregation {

// ============================================================================
// Output Formatter
// Formats OutputRecord to InfluxDB Line or CSV format
// ============================================================================

class OutputFormatter {
public:
    explicit OutputFormatter(OutputFormat format);
    
    // Format a single record
    std::string format(const OutputRecord& record) const;
    
    // Generate CSV header for a measurement (only for CSV format)
    std::string csvHeader(const std::string& measurement,
                          const std::vector<std::string>& tag_keys,
                          const std::vector<std::string>& field_keys) const;

private:
    std::string formatInfluxDB(const OutputRecord& record) const;
    std::string formatCSV(const OutputRecord& record) const;
    
    // Escape special characters for InfluxDB
    static std::string escapeTagValue(const std::string& value);
    static std::string escapeFieldValue(const std::string& value);
    
    OutputFormat format_;
};

} // namespace aggregation

#endif // OUTPUT_FORMATTER_H
