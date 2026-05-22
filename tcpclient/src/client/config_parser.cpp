// ============================================================================
// config_parser.cpp — INI configuration file parser implementation
// ============================================================================
// Wraps the lightweight IniFile parser to load MsgClientConfig from an
// INI-style file.  See doc/05_Build_and_Run.md for the INI schema.
//
// Validation strategy:
//   - Structural validation (types, required fields) happens during parsing.
//   - Semantic validation (ranges, consistency) happens in validateConfig()
//     after the full config object is built.
//   - This two-pass approach gives better error messages: we can report
//     the exact section/key that failed, not just a generic "invalid config".
// ============================================================================

#include "config_parser.h"
#include "ini_parser.h"
#include "log_msg.h"
#include <algorithm>

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
        return validateRange("workers", config.worker_thread_count,
                             Defaults::MIN_WORKER_THREADS, Defaults::MAX_WORKER_THREADS, error)
            && validateRange("raw_queue_size", config.raw_queue_size,
                             Defaults::MIN_QUEUE_SIZE, Defaults::MAX_QUEUE_SIZE, error)
            && validateRange("decoded_queue_size", config.decoded_queue_size,
                             Defaults::MIN_QUEUE_SIZE, Defaults::MAX_QUEUE_SIZE, error)
            && validateRangeInt("reconnect_interval_ms", config.reconnect_interval_ms,
                                Defaults::MIN_RECONNECT_MS, Defaults::MAX_RECONNECT_MS, error)
            && validateRangeInt("queue_push_timeout_ms", config.queue_push_timeout_ms,
                                Defaults::MIN_QUEUE_PUSH_TIMEOUT_MS, Defaults::MAX_QUEUE_PUSH_TIMEOUT_MS, error);
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

    // ------------------------------------------------------------------------
    // Parse global settings section
    // ------------------------------------------------------------------------
    bool parseGlobalSection(const IniFile& ini, MsgClientConfig& config, std::string& /*error*/) {
        if (ini.sectionCount("global") == 0) return true;

        if (ini.hasKey("global", 0, "workers")) {
            config.worker_thread_count = ini.getSizeT("global", 0, "workers");
        }
        if (ini.hasKey("global", 0, "raw_queue_size")) {
            config.raw_queue_size = ini.getSizeT("global", 0, "raw_queue_size");
        }
        if (ini.hasKey("global", 0, "decoded_queue_size")) {
            config.decoded_queue_size = ini.getSizeT("global", 0, "decoded_queue_size");
        }
        if (ini.hasKey("global", 0, "reconnect_interval_ms")) {
            config.reconnect_interval_ms = ini.getInt("global", 0, "reconnect_interval_ms");
        }
        if (ini.hasKey("global", 0, "queue_push_timeout_ms")) {
            config.queue_push_timeout_ms = ini.getInt("global", 0, "queue_push_timeout_ms");
        }
        return true;
    }

    // ------------------------------------------------------------------------
    // Parse connections section
    // ------------------------------------------------------------------------
    bool parseConnectionsSection(const IniFile& ini, MsgClientConfig& config, std::string& error) {
        size_t count = ini.sectionCount("connection");
        for (size_t i = 0; i < count; ++i) {
            ConnectionConfig conn_config;

            // Legacy single-endpoint format: host + port directly in [connection]
            if (ini.hasKey("connection", i, "host")) {
                std::string h = ini.getString("connection", i, "host");
                int port_val = ini.getInt("connection", i, "port", Defaults::PORT);
                if (!validateRangeInt("connections[" + std::to_string(i) + "].port",
                                      port_val, Defaults::MIN_PORT, Defaults::MAX_PORT, error)) {
                    return false;
                }
                conn_config.endpoints.push_back({h, static_cast<uint16_t>(port_val)});
            }

            // Multi-endpoint format: endpoints_host_N, endpoints_port_N
            // (INI doesn't support nested arrays, so we use indexed keys)
            for (size_t ep_idx = 0; ; ++ep_idx) {
                std::string host_key = "endpoints_host_" + std::to_string(ep_idx);
                std::string port_key = "endpoints_port_" + std::to_string(ep_idx);
                if (!ini.hasKey("connection", i, host_key)) break;

                std::string h = ini.getString("connection", i, host_key);
                int port_val = ini.getInt("connection", i, port_key, Defaults::PORT);
                if (!validateRangeInt("connections[" + std::to_string(i) + "].endpoints[" +
                                      std::to_string(ep_idx) + "].port",
                                      port_val, Defaults::MIN_PORT, Defaults::MAX_PORT, error)) {
                    return false;
                }
                conn_config.endpoints.push_back({h, static_cast<uint16_t>(port_val)});
            }

            if (ini.hasKey("connection", i, "failover_retries")) {
                conn_config.max_retries_per_endpoint = ini.getInt("connection", i, "failover_retries");
            }

            if (ini.hasKey("connection", i, "item")) {
                conn_config.item_name = ini.getString("connection", i, "item");
            }
            if (ini.hasKey("connection", i, "client_id")) {
                conn_config.client_id = ini.getString("connection", i, "client_id");
            }
            if (ini.hasKey("connection", i, "starting_seq")) {
                conn_config.starting_seq_num = ini.getUint64("connection", i, "starting_seq");
            }

            config.connections.push_back(conn_config);
        }
        return true;
    }

    // ------------------------------------------------------------------------
    // Parse aggregation section
    // ------------------------------------------------------------------------
    bool parseAggregationSection(const IniFile& ini, MsgClientConfig& config, std::string& error) {
        if (ini.sectionCount("aggregation") == 0) return true;

        if (ini.hasKey("aggregation", 0, "enabled")) {
            config.aggregation_config.enabled = ini.getBool("aggregation", 0, "enabled");
        }
        if (ini.hasKey("aggregation", 0, "window_ms")) {
            config.aggregation_config.window_ms = ini.getUint64("aggregation", 0, "window_ms");
        }
        if (ini.hasKey("aggregation", 0, "output_format")) {
            std::string fmt = ini.getString("aggregation", 0, "output_format");
            if (fmt == "csv" || fmt == "CSV") {
                config.aggregation_config.output_format = metrics::OutputFormat::CSV;
            } else {
                config.aggregation_config.output_format = metrics::OutputFormat::INFLUXDB_LINE;
            }
        }
        if (ini.hasKey("aggregation", 0, "output_dir")) {
            config.aggregation_config.output_dir = ini.getString("aggregation", 0, "output_dir");
        }
        if (ini.hasKey("aggregation", 0, "filename_prefix")) {
            config.aggregation_config.filename_prefix = ini.getString("aggregation", 0, "filename_prefix");
        }

        std::string agg_error = config.aggregation_config.validate();
        if (!agg_error.empty()) {
            error = "aggregation: " + agg_error;
            return false;
        }
        return true;
    }

    // ------------------------------------------------------------------------
    // Parse memory pool section
    // ------------------------------------------------------------------------
    bool parseMemoryPoolSection(const IniFile& ini, MsgClientConfig& config, std::string& /*error*/) {
        // Check if any memory_pool.class_N sections exist
        bool has_any = false;
        for (size_t i = 0; i < MemoryPool::NUM_SIZE_CLASSES; ++i) {
            std::string section = "memory_pool.class_" + std::to_string(i);
            if (ini.sectionCount(section) > 0) {
                has_any = true;
                break;
            }
        }
        if (!has_any) return true;

        config.pool_config.clear();

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

            std::string section = "memory_pool.class_" + std::to_string(i);
            if (ini.sectionCount(section) > 0) {
                if (ini.hasKey(section, 0, "initial")) {
                    cfg.initial_count = ini.getSizeT(section, 0, "initial");
                }
                if (ini.hasKey(section, 0, "max_free")) {
                    cfg.max_count = ini.getSizeT(section, 0, "max_free");
                }
                if (ini.hasKey(section, 0, "max_total")) {
                    cfg.max_total_allocated = ini.getSizeT(section, 0, "max_total");
                }
            }
            config.pool_config.push_back(cfg);
        }
        return true;
    }
}

bool parseConfigFile(const std::string& filepath,
                     MsgClientConfig& config,
                     std::string& error_message) {
    // Parse INI file
    IniFile ini;
    if (!ini.parseFile(filepath, error_message)) {
        return false;
    }

    // Parse each section
    if (!parseGlobalSection(ini, config, error_message)) return false;
    if (!parseConnectionsSection(ini, config, error_message)) return false;
    if (!parseAggregationSection(ini, config, error_message)) return false;
    if (!parseMemoryPoolSection(ini, config, error_message)) return false;

    // Final validation of the complete configuration
    if (!validateConfig(config, error_message)) {
        return false;
    }

    return true;
}

void printConfigFormat() {
    printf(
        "Configuration File Format (INI):\n"
        "================================\n"
        "\n"
        "; Global settings\n"
        "[global]\n"
        "workers = 4                    ; Number of worker threads (1-64)\n"
        "raw_queue_size = 16384         ; Raw queue size (64-1048576)\n"
        "decoded_queue_size = 16384     ; Decoded queue per worker (64-1048576)\n"
        "reconnect_interval_ms = 3000   ; Reconnect interval (100-300000)\n"
        "queue_push_timeout_ms = 5      ; Queue push timeout (-1-60000)\n"
        "\n"
        "; TCP connections (up to 64, repeat [connection] for each)\n"
        "[connection]\n"
        "host = primary.example.com     ; Server host (legacy single-endpoint)\n"
        "port = 8888                    ; Server port\n"
        "failover_retries = 2           ; Retries before failover\n"
        "item = AAPL                    ; Item name (max 32 chars)\n"
        "client_id = Client1            ; Client ID (max 32 chars)\n"
        "starting_seq = 0               ; Starting sequence number\n"
        "\n"
        "; Multi-endpoint connection (alternative to host/port above)\n"
        "[connection]\n"
        "endpoints_host_0 = primary.example.com\n"
        "endpoints_port_0 = 8888\n"
        "endpoints_host_1 = backup.example.com\n"
        "endpoints_port_1 = 8889\n"
        "failover_retries = 2\n"
        "item = AAPL\n"
        "client_id = Client1\n"
        "starting_seq = 0\n"
        "\n"
        "; Optional: Aggregation settings\n"
        "[aggregation]\n"
        "enabled = true\n"
        "window_ms = 1000\n"
        "output_format = csv            ; csv or influxdb_line\n"
        "output_dir = /tmp/metrics\n"
        "filename_prefix = agg\n"
        "\n"
        "; Optional: Memory pool tuning (override specific classes)\n"
        "[memory_pool.class_5]          ; 64KB class (recv buffers)\n"
        "initial = 512                  ; <= max_total\n"
        "max_free = 1024                ; <= max_total\n"
        "max_total = 8192               ; Maximum allocations\n"
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
