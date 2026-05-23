// ============================================================================
// ini_parser.cpp — Lightweight INI-style configuration parser
// ============================================================================

#include "ini_parser.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>

// Helper: trim leading and trailing whitespace
static std::string trim(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
        ++start;
    }
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(start, end - start);
}

// Helper: lowercase string for case-insensitive comparison
static std::string toLower(const std::string& s) {
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return result;
}

// Helper: split content into lines
static std::vector<std::string> splitLines(const std::string& content) {
    std::vector<std::string> lines;
    std::string current;
    for (char c : content) {
        if (c == '\n') {
            lines.push_back(current);
            current.clear();
        } else if (c != '\r') {
            current.push_back(c);
        }
    }
    if (!current.empty() || !lines.empty()) {
        lines.push_back(current);
    }
    return lines;
}

// ============================================================================
// Public API
// ============================================================================

bool IniFile::parseFile(const std::string& filepath, std::string& error) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        error = "Cannot open config file: " + filepath;
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    return parseString(buffer.str(), error);
}

bool IniFile::parseString(const std::string& content, std::string& error) {
    clear();
    std::vector<std::string> lines = splitLines(content);
    return parseLines(lines, error);
}

void IniFile::clear() {
    sections_.clear();
    index_.clear();
}

size_t IniFile::sectionCount(const std::string& section) const {
    auto it = index_.find(section);
    if (it == index_.end()) return 0;
    return it->second.size();
}

bool IniFile::hasKey(const std::string& section, size_t occurrence,
                     const std::string& key) const {
    const Section* sec = findSection(section, occurrence);
    if (!sec) return false;
    return sec->values.find(key) != sec->values.end();
}

std::string IniFile::getString(const std::string& section, size_t occurrence,
                               const std::string& key,
                               const std::string& defaultValue) const {
    const Section* sec = findSection(section, occurrence);
    if (!sec) return defaultValue;
    auto it = sec->values.find(key);
    if (it == sec->values.end()) return defaultValue;
    return it->second;
}

int IniFile::getInt(const std::string& section, size_t occurrence,
                    const std::string& key, int defaultValue) const {
    const Section* sec = findSection(section, occurrence);
    if (!sec) return defaultValue;
    auto it = sec->values.find(key);
    if (it == sec->values.end()) return defaultValue;
    try {
        size_t pos = 0;
        long long val = std::stoll(it->second, &pos);
        if (pos != it->second.size()) return defaultValue;
        return static_cast<int>(val);
    } catch (...) {
        return defaultValue;
    }
}

uint64_t IniFile::getUint64(const std::string& section, size_t occurrence,
                            const std::string& key, uint64_t defaultValue) const {
    const Section* sec = findSection(section, occurrence);
    if (!sec) return defaultValue;
    auto it = sec->values.find(key);
    if (it == sec->values.end()) return defaultValue;
    try {
        size_t pos = 0;
        unsigned long long val = std::stoull(it->second, &pos);
        if (pos != it->second.size()) return defaultValue;
        return static_cast<uint64_t>(val);
    } catch (...) {
        return defaultValue;
    }
}

size_t IniFile::getSizeT(const std::string& section, size_t occurrence,
                         const std::string& key, size_t defaultValue) const {
    const Section* sec = findSection(section, occurrence);
    if (!sec) return defaultValue;
    auto it = sec->values.find(key);
    if (it == sec->values.end()) return defaultValue;
    try {
        size_t pos = 0;
        unsigned long long val = std::stoull(it->second, &pos);
        if (pos != it->second.size()) return defaultValue;
        return static_cast<size_t>(val);
    } catch (...) {
        return defaultValue;
    }
}

bool IniFile::getBool(const std::string& section, size_t occurrence,
                      const std::string& key, bool defaultValue) const {
    const Section* sec = findSection(section, occurrence);
    if (!sec) return defaultValue;
    auto it = sec->values.find(key);
    if (it == sec->values.end()) return defaultValue;
    std::string lower = toLower(trim(it->second));
    if (lower == "true" || lower == "yes" || lower == "1" || lower == "on") {
        return true;
    }
    if (lower == "false" || lower == "no" || lower == "0" || lower == "off") {
        return false;
    }
    return defaultValue;
}

std::vector<std::string> IniFile::getKeys(const std::string& section,
                                          size_t occurrence) const {
    std::vector<std::string> keys;
    const Section* sec = findSection(section, occurrence);
    if (!sec) return keys;
    keys.reserve(sec->values.size());
    std::transform(sec->values.begin(), sec->values.end(), std::back_inserter(keys),
                   [](const auto& kv) { return kv.first; });
    return keys;
}

// ============================================================================
// Internal helpers
// ============================================================================

const IniFile::Section* IniFile::findSection(const std::string& name,
                                             size_t occurrence) const {
    auto it = index_.find(name);
    if (it == index_.end()) return nullptr;
    if (occurrence >= it->second.size()) return nullptr;
    return &sections_[it->second[occurrence]];
}

bool IniFile::parseLines(const std::vector<std::string>& lines, std::string& error) {
    Section* currentSection = nullptr;

    for (size_t lineNum = 0; lineNum < lines.size(); ++lineNum) {
        std::string line = trim(lines[lineNum]);

        // Skip empty lines and comments
        if (line.empty() || line[0] == ';' || line[0] == '#') {
            continue;
        }

        // Section header: [section_name]
        if (line.front() == '[' && line.back() == ']') {
            std::string sectionName = trim(line.substr(1, line.size() - 2));
            if (sectionName.empty()) {
                error = "Line " + std::to_string(lineNum + 1) + ": empty section name";
                return false;
            }

            sections_.push_back({sectionName, {}});
            size_t idx = sections_.size() - 1;
            index_[sectionName].push_back(idx);
            currentSection = &sections_.back();
            continue;
        }

        // Key-value pair: key = value
        size_t eqPos = line.find('=');
        if (eqPos == std::string::npos) {
            error = "Line " + std::to_string(lineNum + 1) +
                    ": expected 'key = value' format, got: " + line;
            return false;
        }

        std::string key = trim(line.substr(0, eqPos));
        std::string value = trim(line.substr(eqPos + 1));

        if (key.empty()) {
            error = "Line " + std::to_string(lineNum + 1) + ": empty key";
            return false;
        }

        // Strip quotes from value if present
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        } else if (value.size() >= 2 && value.front() == '\'' && value.back() == '\'') {
            value = value.substr(1, value.size() - 2);
        }

        if (!currentSection) {
            error = "Line " + std::to_string(lineNum + 1) +
                    ": key-value pair outside of any section";
            return false;
        }

        currentSection->values[key] = value;
    }

    return true;
}
