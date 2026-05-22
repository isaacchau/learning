// ============================================================================
// ini_parser.h — Lightweight INI-style configuration parser
// ============================================================================
// Zero-dependency parser for simple key-value configuration files.
// Supports sections, line comments (; and #), and repeated sections.
//
// Format:
//   ; comment
//   [section_name]
//   key = value
//
// Repeated sections (e.g. [connection]) are tracked in order.
//
// Usage:
//   IniFile ini;
//   std::string error;
//   if (!ini.parseFile("config.ini", error)) { ... }
//   int workers = ini.getInt("global", "workers", 4);
//   std::string host = ini.getString("connection", "host", "127.0.0.1");
//   size_t connCount = ini.sectionCount("connection");
// ============================================================================

#ifndef INI_PARSER_H
#define INI_PARSER_H

#include <string>
#include <vector>
#include <map>
#include <cstdint>

class IniFile {
public:
    IniFile() = default;

    // Parse from file. Returns true on success, false with error set.
    bool parseFile(const std::string& filepath, std::string& error);

    // Parse from string (useful for tests). Returns true on success.
    bool parseString(const std::string& content, std::string& error);

    // --- Query methods ---

    // Number of occurrences of a section name (supports repeated sections)
    size_t sectionCount(const std::string& section) const;

    // Check if a key exists in a specific section occurrence
    bool hasKey(const std::string& section, size_t occurrence,
                const std::string& key) const;

    // Get string value. Returns defaultValue if not found.
    std::string getString(const std::string& section, size_t occurrence,
                          const std::string& key,
                          const std::string& defaultValue = "") const;

    // Get integer value. Returns defaultValue if not found or invalid.
    int getInt(const std::string& section, size_t occurrence,
               const std::string& key, int defaultValue = 0) const;

    // Get unsigned 64-bit value. Returns defaultValue if not found or invalid.
    uint64_t getUint64(const std::string& section, size_t occurrence,
                       const std::string& key, uint64_t defaultValue = 0) const;

    // Get size_t value. Returns defaultValue if not found or invalid.
    size_t getSizeT(const std::string& section, size_t occurrence,
                    const std::string& key, size_t defaultValue = 0) const;

    // Get bool value. Supports "true"/"false", "yes"/"no", "1"/"0".
    // Returns defaultValue if not found or invalid.
    bool getBool(const std::string& section, size_t occurrence,
                 const std::string& key, bool defaultValue = false) const;

    // Get all keys in a section occurrence (for debugging/diagnostics)
    std::vector<std::string> getKeys(const std::string& section,
                                      size_t occurrence) const;

    // Clear all parsed data
    void clear();

private:
    // Each section occurrence is a map of key->value
    // We store sections in a vector to preserve order, and also index by name
    struct Section {
        std::string name;
        std::map<std::string, std::string> values;
    };

    std::vector<Section> sections_;

    // Build index: section name -> list of indices into sections_
    std::map<std::string, std::vector<size_t>> index_;

    // Internal parse from lines
    bool parseLines(const std::vector<std::string>& lines, std::string& error);

    // Helper: get the Section* for a given (name, occurrence)
    const Section* findSection(const std::string& name, size_t occurrence) const;
};

#endif // INI_PARSER_H
