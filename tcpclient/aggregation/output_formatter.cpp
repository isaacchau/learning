#include "output_formatter.h"

#include <iomanip>

namespace aggregation {

OutputFormatter::OutputFormatter(OutputFormat format) : format_(format) {}

std::string OutputFormatter::format(const OutputRecord& record) const {
    switch (format_) {
        case OutputFormat::INFLUXDB_LINE:
            return formatInfluxDB(record);
        case OutputFormat::CSV:
            return formatCSV(record);
        default:
            return formatInfluxDB(record);
    }
}

std::string OutputFormatter::csvHeader(const std::string& /*measurement*/,
                                        const std::vector<std::string>& tag_keys,
                                        const std::vector<std::string>& field_keys) const {
    if (format_ != OutputFormat::CSV) {
        return "";
    }
    
    std::ostringstream oss;
    oss << "measurement";
    
    for (const auto& key : tag_keys) {
        oss << ",tag_" << key;
    }
    
    for (const auto& key : field_keys) {
        oss << ",field_" << key;
    }
    
    oss << ",timestamp";
    return oss.str();
}

std::string OutputFormatter::formatInfluxDB(const OutputRecord& record) const {
    // InfluxDB Line Protocol:
    // measurement,tag1=value1,tag2=value2 field1=value1,field2=value2 timestamp
    
    std::ostringstream oss;
    
    // Measurement (escape commas, spaces)
    oss << record.measurement;
    
    // Tags
    for (const auto& tag : record.tags) {
        oss << "," << tag.first << "=" << escapeTagValue(tag.second);
    }
    
    // Space separator between tags and fields
    oss << " ";
    
    // Fields (at least one required)
    bool first_field = true;
    for (const auto& field : record.fields) {
        if (!first_field) {
            oss << ",";
        }
        oss << field.first << "=" << escapeFieldValue(field.second);
        first_field = false;
    }
    
    // Timestamp (nanoseconds since epoch)
    oss << " " << record.timestamp_ns;
    
    return oss.str();
}

std::string OutputFormatter::formatCSV(const OutputRecord& record) const {
    // CSV format:
    // measurement,tag_tag1,tag_tag2,field_field1,field_field2,timestamp
    
    std::ostringstream oss;
    
    // Measurement
    oss << record.measurement;
    
    // Tags with "tag_" prefix
    for (const auto& tag : record.tags) {
        oss << ",tag_" << tag.first << "=" << tag.second;
    }
    
    // Fields with "field_" prefix
    for (const auto& field : record.fields) {
        oss << ",field_" << field.first << "=" << field.second;
    }
    
    // Timestamp
    oss << "," << record.timestamp_ns;
    
    return oss.str();
}

std::string OutputFormatter::escapeTagValue(const std::string& value) {
    // InfluxDB tag values need escaping for: comma, equals, space
    std::string result;
    result.reserve(value.size());
    
    for (char c : value) {
        switch (c) {
            case ',':
            case '=':
            case ' ':
                // Escape with backslash
                result += '\\';
                result += c;
                break;
            default:
                result += c;
        }
    }
    
    return result;
}

std::string OutputFormatter::escapeFieldValue(const std::string& value) {
    // Field values in InfluxDB:
    // - Strings need to be quoted: "value"
    // - Numbers and booleans are written as-is
    // - For simplicity, we treat everything as string if it contains non-numeric chars
    
    // Check if it's a number (int or float)
    bool is_number = true;
    bool has_dot = false;
    
    for (size_t i = 0; i < value.size() && is_number; ++i) {
        char c = value[i];
        if (c >= '0' && c <= '9') {
            continue;
        }
        if (c == '.' && !has_dot) {
            has_dot = true;
            continue;
        }
        if (c == '-' && i == 0) {
            continue;  // Negative sign at start
        }
        is_number = false;
    }
    
    if (is_number && !value.empty()) {
        // Numeric value - return as-is
        return value;
    }
    
    // Check for boolean
    if (value == "true" || value == "false" ||
        value == "t" || value == "f" ||
        value == "TRUE" || value == "FALSE") {
        return value;
    }
    
    // String value - quote and escape
    std::string result = "\"";
    for (char c : value) {
        if (c == '"' || c == '\\') {
            result += '\\';
        }
        result += c;
    }
    result += '"';
    
    return result;
}

} // namespace aggregation
