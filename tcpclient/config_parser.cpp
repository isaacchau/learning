// ============================================================================
// config_parser.cpp — JSON configuration file parser implementation
// ============================================================================
// Wraps nlohmann/json (vendored single-header json.hpp) to load
// MsgClientConfig from a JSON file.  See doc/05_Build_and_Run.md
// for the JSON schema and examples.
//
// Validation strategy:
//   - Structural validation (types, required fields) happens during parsing.
//   - Semantic validation (ranges, consistency) happens in validateConfig()
//     after the full config object is built.
//   - This two-pass approach gives better error messages: we can report
//     the exact JSON path that failed, not just a generic "invalid config".
// ============================================================================

#include "config_parser.h"
#include "json.hpp"
#include "log_msg.h"
// aggregation config is now part of msg_client.h
#include <fstream>
#include <sstream>
#include <algorithm>

using json = nlohmann::json;

// Helper validation functions
namespace {
    
    bool validateRange(const std::string& name, size_t value, size_t min, size_t max, 
                       std::string& error) {
        if (value < min || value > max) {
            error = name + " must be between " + std::to_string(min) + 
                    " and " + std::to_string(max) + " (got " + std::to_string(value) + ")";
            return false;
        }
        return true;
    }
    
    bool validateRangeInt(const std::string& name, int value, int min, int max, 
                          std::string& error) {
        if (value < min || value > max) {
            error = name + " must be between " + std::to_string(min) + 
                    " and " + std::to_string(max) + " (got " + std::to_string(value) + ")";
            return false;
        }
        return true;
    }
    
    bool validateNotEmpty(const std::string& name, const std::string& value, 
                          std::string& error) {
        if (value.empty()) {
            error = name + " cannot be empty";
            return false;
        }
        return true;
    }
    
    bool validateMaxLength(const std::string& name, const std::string& value, 
                           size_t max_len, std::string& error) {
        if (value.length() > max_len) {
            error = name + " too long (max " + std::to_string(max_len) + 
                    " chars, got " + std::to_string(value.length()) + ")";
            return false;
        }
        return true;
    }
    
    // ------------------------------------------------------------------------
    // Global config validation
    // ------------------------------------------------------------------------
    bool validateGlobalSettings(const MsgClientConfig& config, std::string& error) {
        if (!validateRange("workers", config.worker_thread_count,
                           Defaults::MIN_WORKER_THREADS, Defaults::MAX_WORKER_THREADS, error)) {
            return false;
        }
        if (!validateRange("raw_queue_size", config.raw_queue_size,
                           Defaults::MIN_QUEUE_SIZE, Defaults::MAX_QUEUE_SIZE, error)) {
            return false;
        }
        if (!validateRange("decoded_queue_size", config.decoded_queue_size,
                           Defaults::MIN_QUEUE_SIZE, Defaults::MAX_QUEUE_SIZE, error)) {
            return false;
        }
        if (!validateRangeInt("reconnect_interval_ms", config.reconnect_interval_ms,
                              Defaults::MIN_RECONNECT_MS, Defaults::MAX_RECONNECT_MS, error)) {
            return false;
        }
        if (!validateRangeInt("queue_push_timeout_ms", config.queue_push_timeout_ms,
                              Defaults::MIN_QUEUE_PUSH_TIMEOUT_MS, Defaults::MAX_QUEUE_PUSH_TIMEOUT_MS, error)) {
            return false;
        }
        return true;
    }

    // ------------------------------------------------------------------------
    // Connection-level validation
    // ------------------------------------------------------------------------
    bool validateConnection(const ConnectionConfig& conn, size_t conn_idx, std::string& error) {
        std::string prefix = "connections[" + std::to_string(conn_idx) + "]";

        if (conn.endpoints.empty()) {
            error = prefix + ": at least one endpoint required";
            return false;
        }

        for (size_t j = 0; j < conn.endpoints.size(); ++j) {
            const auto& ep = conn.endpoints[j];
            std::string ep_prefix = prefix + ".endpoints[" + std::to_string(j) + "]";
            if (!validateNotEmpty(ep_prefix + ".host", ep.host, error)) return false;
            if (!validateRangeInt(ep_prefix + ".port", ep.port,
                                  Defaults::MIN_PORT, Defaults::MAX_PORT, error)) return false;
        }

        if (!validateNotEmpty(prefix + ".item_name", conn.item_name, error)) return false;
        if (!validateMaxLength(prefix + ".item_name", conn.item_name,
                               Defaults::MAX_ITEM_NAME_LEN, error)) return false;
        if (!validateNotEmpty(prefix + ".client_id", conn.client_id, error)) return false;
        if (!validateMaxLength(prefix + ".client_id", conn.client_id,
                               Defaults::MAX_CLIENT_ID_LEN, error)) return false;

        return true;
    }

    // ------------------------------------------------------------------------
    // Memory pool config validation
    // ------------------------------------------------------------------------
    bool validatePoolConfig(const MsgClientConfig& config, std::string& error) {
        if (config.pool_config.empty()) return true;

        if (config.pool_config.size() != MemoryPool::NUM_SIZE_CLASSES) {
            error = "Memory pool must have exactly " +
                    std::to_string(MemoryPool::NUM_SIZE_CLASSES) + " size classes";
            return false;
        }

        for (size_t i = 0; i < config.pool_config.size(); ++i) {
            const auto& cls = config.pool_config[i];
            std::string prefix = "memory_pool.class_" + std::to_string(i);

            if (cls.initial_count > cls.max_total_allocated) {
                error = prefix + ".initial (" + std::to_string(cls.initial_count) +
                        ") cannot exceed max_total (" + std::to_string(cls.max_total_allocated) + ")";
                return false;
            }
            if (cls.max_count > cls.max_total_allocated) {
                error = prefix + ".max_free (" + std::to_string(cls.max_count) +
                        ") cannot exceed max_total (" + std::to_string(cls.max_total_allocated) + ")";
                return false;
            }
        }
        return true;
    }

    // ------------------------------------------------------------------------
    // Top-level config validation
    // ------------------------------------------------------------------------
    bool validateConfig(const MsgClientConfig& config, std::string& error) {
        if (!validateGlobalSettings(config, error)) return false;

        if (config.connections.empty()) {
            error = "At least one connection must be configured";
            return false;
        }
        if (config.connections.size() > Defaults::MAX_CONNECTIONS) {
            error = "Too many connections (max " + std::to_string(Defaults::MAX_CONNECTIONS) +
                    ", got " + std::to_string(config.connections.size()) + ")";
            return false;
        }

        for (size_t i = 0; i < config.connections.size(); ++i) {
            if (!validateConnection(config.connections[i], i, error)) return false;
        }

        if (!validatePoolConfig(config, error)) return false;

        return true;
    }
}

bool parseConfigFile(const std::string& filepath, 
                     MsgClientConfig& config, 
                     std::string& error_message) {
    // Read file
    std::ifstream file(filepath);
    if (!file.is_open()) {
        error_message = "Cannot open config file: " + filepath;
        return false;
    }

    // Parse JSON
    json j;
    try {
        file >> j;
    } catch (const json::parse_error& e) {
        error_message = std::string("JSON parse error: ") + e.what();
        return false;
    }

    // Validate JSON types before parsing
    if (j.contains("global") && !j["global"].is_object()) {
        error_message = "'global' must be an object";
        return false;
    }
    
    if (j.contains("connections") && !j["connections"].is_array()) {
        error_message = "'connections' must be an array";
        return false;
    }
    
    if (j.contains("memory_pool") && !j["memory_pool"].is_object()) {
        error_message = "'memory_pool' must be an object";
        return false;
    }

    // Parse global settings
    if (j.contains("global")) {
        const auto& global = j["global"];
        
        try {
            if (global.contains("workers")) {
                config.worker_thread_count = global["workers"].get<size_t>();
            }
            if (global.contains("raw_queue_size")) {
                config.raw_queue_size = global["raw_queue_size"].get<size_t>();
            }
            if (global.contains("decoded_queue_size")) {
                config.decoded_queue_size = global["decoded_queue_size"].get<size_t>();
            }
            if (global.contains("reconnect_interval_ms")) {
                config.reconnect_interval_ms = global["reconnect_interval_ms"].get<int>();
            }
            if (global.contains("queue_push_timeout_ms")) {
                config.queue_push_timeout_ms = global["queue_push_timeout_ms"].get<int>();
            }
        } catch (const json::type_error& e) {
            error_message = std::string("Type error in 'global' section: ") + e.what();
            return false;
        }
    }

    // Parse connections
    if (j.contains("connections")) {
        const auto& connections = j["connections"];

        for (size_t i = 0; i < connections.size(); ++i) {
            const auto& conn = connections[i];
            
            if (!conn.is_object()) {
                error_message = "connections[" + std::to_string(i) + "] must be an object";
                return false;
            }
            
            ConnectionConfig conn_config;

            try {
                auto it_endpoints = conn.find("endpoints");
                if (it_endpoints != conn.end()) {
                    const auto& endpoints = *it_endpoints;
                    if (!endpoints.is_array()) {
                        error_message = "connections[" + std::to_string(i) + "].endpoints must be an array";
                        return false;
                    }
                    for (size_t j = 0; j < endpoints.size(); ++j) {
                        const auto& ep = endpoints[j];
                        if (!ep.is_object()) {
                            error_message = "connections[" + std::to_string(i) + "].endpoints[" +
                                           std::to_string(j) + "] must be an object";
                            return false;
                        }
                        EndpointConfig ep_cfg;
                        auto it_host = ep.find("host");
                        if (it_host != ep.end()) {
                            if (!it_host->is_string()) {
                                error_message = "connections[" + std::to_string(i) + "].endpoints[" +
                                               std::to_string(j) + "].host must be a string";
                                return false;
                            }
                            ep_cfg.host = it_host->get<std::string>();
                        }
                        auto it_port = ep.find("port");
                        if (it_port != ep.end()) {
                            int port_val = it_port->get<int>();
                            if (!validateRangeInt("connections[" + std::to_string(i) + "].endpoints[" +
                                                 std::to_string(j) + "].port",
                                                 port_val, Defaults::MIN_PORT, Defaults::MAX_PORT, error_message)) {
                                return false;
                            }
                            ep_cfg.port = static_cast<uint16_t>(port_val);
                        }
                        conn_config.endpoints.push_back(ep_cfg);
                    }
                }

                if (conn_config.endpoints.empty()) {
                    auto it_host = conn.find("host");
                    if (it_host != conn.end()) {
                        if (!it_host->is_string()) {
                            error_message = "connections[" + std::to_string(i) + "].host must be a string";
                            return false;
                        }
                        std::string h = it_host->get<std::string>();
                        uint16_t p = Defaults::PORT;
                        auto it_port = conn.find("port");
                        if (it_port != conn.end()) {
                            int port_val = it_port->get<int>();
                            if (!validateRangeInt("connections[" + std::to_string(i) + "].port",
                                                 port_val, Defaults::MIN_PORT, Defaults::MAX_PORT, error_message)) {
                                return false;
                            }
                            p = static_cast<uint16_t>(port_val);
                        }
                        conn_config.endpoints.push_back({h, p});
                    }
                }

                auto it_failover = conn.find("failover_retries");
                if (it_failover != conn.end()) {
                    conn_config.max_retries_per_endpoint = it_failover->get<int>();
                }
                auto it_item = conn.find("item");
                if (it_item != conn.end()) {
                    if (!it_item->is_string()) {
                        error_message = "connections[" + std::to_string(i) + "].item must be a string";
                        return false;
                    }
                    conn_config.item_name = it_item->get<std::string>();
                }
                auto it_client_id = conn.find("client_id");
                if (it_client_id != conn.end()) {
                    if (!it_client_id->is_string()) {
                        error_message = "connections[" + std::to_string(i) + "].client_id must be a string";
                        return false;
                    }
                    conn_config.client_id = it_client_id->get<std::string>();
                }
                auto it_seq = conn.find("starting_seq");
                if (it_seq != conn.end()) {
                    conn_config.starting_seq_num = it_seq->get<uint64_t>();
                }
            } catch (const json::type_error& e) {
                error_message = "Type error in connections[" + std::to_string(i) + "]: " + e.what();
                return false;
            }

            config.connections.push_back(conn_config);
        }
    }

    // Parse aggregation configuration (optional)
    if (j.contains("aggregation")) {
        const auto& agg = j["aggregation"];
        
        if (!agg.is_object()) {
            error_message = "'aggregation' must be an object";
            return false;
        }
        
        try {
            if (agg.contains("enabled")) {
                config.aggregation_config.enabled = agg["enabled"].get<bool>();
            }
            if (agg.contains("window_ms")) {
                config.aggregation_config.window_ms = agg["window_ms"].get<uint64_t>();
            }
            if (agg.contains("output_format")) {
                if (!agg["output_format"].is_string()) {
                    error_message = "aggregation.output_format must be a string";
                    return false;
                }
                std::string fmt = agg["output_format"].get<std::string>();
                if (fmt == "csv" || fmt == "CSV") {
                    config.aggregation_config.output_format = metrics::OutputFormat::CSV;
                } else {
                    config.aggregation_config.output_format = metrics::OutputFormat::INFLUXDB_LINE;
                }
            }
            if (agg.contains("output_dir")) {
                if (!agg["output_dir"].is_string()) {
                    error_message = "aggregation.output_dir must be a string";
                    return false;
                }
                config.aggregation_config.output_dir = agg["output_dir"].get<std::string>();
            }
            if (agg.contains("filename_prefix")) {
                if (!agg["filename_prefix"].is_string()) {
                    error_message = "aggregation.filename_prefix must be a string";
                    return false;
                }
                config.aggregation_config.filename_prefix = agg["filename_prefix"].get<std::string>();
            }
        } catch (const json::type_error& e) {
            error_message = std::string("Type error in 'aggregation' section: ") + e.what();
            return false;
        }
        
        // Validate aggregation config
        std::string agg_error = config.aggregation_config.validate();
        if (!agg_error.empty()) {
            error_message = "aggregation: " + agg_error;
            return false;
        }
    }

    // Parse memory pool configuration (optional)
    if (j.contains("memory_pool")) {
        const auto& pool = j["memory_pool"];
        
        config.pool_config.clear();
        // Define default size classes
        struct DefaultClass { size_t size; size_t initial; size_t max_free; size_t max_total; };
        std::vector<DefaultClass> defaults = {
            {64, 128, 1024, 4096},
            {256, 128, 1024, 4096},
            {1024, 128, 1024, 4096},
            {4096, 256, 1024, 8192},
            {16384, 256, 512, 4096},
            {65536, 512, 1024, 8192},
            {131072, 256, 512, 4096},
            {262144, 128, 256, 2048}
        };

        for (size_t i = 0; i < 8; ++i) {
            SizeClassConfig cfg;
            cfg.block_size = defaults[i].size;
            cfg.initial_count = defaults[i].initial;
            cfg.max_count = defaults[i].max_free;
            cfg.max_total_allocated = defaults[i].max_total;

            // Allow overriding per size class
            std::string key = "class_" + std::to_string(i);
            if (pool.contains(key)) {
                const auto& cls = pool[key];
                if (!cls.is_object()) {
                    error_message = "memory_pool." + key + " must be an object";
                    return false;
                }
                
                try {
                    if (cls.contains("initial")) cfg.initial_count = cls["initial"].get<size_t>();
                    if (cls.contains("max_free")) cfg.max_count = cls["max_free"].get<size_t>();
                    if (cls.contains("max_total")) cfg.max_total_allocated = cls["max_total"].get<size_t>();
                } catch (const json::type_error& e) {
                    error_message = "Type error in memory_pool." + key + ": " + e.what();
                    return false;
                }
            }

            config.pool_config.push_back(cfg);
        }
    }

    // Final validation of the complete configuration
    if (!validateConfig(config, error_message)) {
        return false;
    }

    return true;
}

void printConfigFormat() {
    printf(
        "Configuration File Format (JSON):\n"
        "=================================\n"
        "\n"
        "{\n"
        "  // Global settings\n"
        "  \"global\": {\n"
        "    \"workers\": 4,                    // Number of worker threads (1-64)\n"
        "    \"raw_queue_size\": 16384,         // Raw queue size (64-1048576)\n"
        "    \"decoded_queue_size\": 16384,     // Decoded queue per worker (64-1048576)\n"
        "    \"reconnect_interval_ms\": 3000,   // Reconnect interval (100-300000)\n"
        "    \"queue_push_timeout_ms\": 5       // Queue push timeout (-1-60000)\n"
        "  },\n"
        "\n"
        "  // TCP connections (up to 64)\n"
        "  \"connections\": [\n"
        "    {\n"
        "      // Multi-endpoint with failover (new format)\n"
        "      \"endpoints\": [\n"
        "        {\"host\": \"primary.example.com\", \"port\": 8888},\n"
        "        {\"host\": \"backup.example.com\",  \"port\": 8888}\n"
        "      ],\n"
        "      \"failover_retries\": 2,          // Retries before failover\n"
        "      \"item\": \"AAPL\",              // Item name (max 32 chars)\n"
        "      \"client_id\": \"Client1\",      // Client ID (max 32 chars)\n"
        "      \"starting_seq\": 0             // Starting sequence number\n"
        "    },\n"
        "    {\n"
        "      // Single endpoint (legacy format, still supported)\n"
        "      \"host\": \"server2.example.com\",\n"
        "      \"port\": 8889,\n"
        "      \"item\": \"MSFT\",\n"
        "      \"client_id\": \"Client2\",\n"
        "      \"starting_seq\": 0\n"
        "    }\n"
        "  ],\n"
        "\n"
        "  // Optional: Memory pool tuning\n"
        "  \"memory_pool\": {\n"
        "    \"class_5\": {                    // 64KB class (recv buffers)\n"
        "      \"initial\": 512,               // <= max_total\n"
        "      \"max_free\": 1024,             // <= max_total\n"
        "      \"max_total\": 8192             // Maximum allocations\n"
        "    }\n"
        "  }\n"
        "}\n"
        "\n"
        "All fields are optional. Missing fields use defaults.\n"
        "Connections can also be specified via command line.\n"
        "\n"
        "Validation Rules:\n"
        "- workers: 1-64\n"
        "- raw_queue_size, decoded_queue_size: 64-1048576\n"
        "- reconnect_interval_ms: 100-300000 (0.1s to 5min)\n"
        "- queue_push_timeout_ms: -1 to 60000 (-1=no wait, 0=forever)\n"
        "- port: 1-65535\n"
        "- item_name, client_id: non-empty, max 32 chars\n"
        "- max_connections: 1-64\n"
        "- max_retries_per_endpoint: >= 1\n"
    );
}
