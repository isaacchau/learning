#include "config_parser.h"
#include "json.hpp"
#include "log_msg.h"
#include <fstream>
#include <sstream>

using json = nlohmann::json;

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

    // Parse global settings
    if (j.contains("global")) {
        const auto& global = j["global"];
        
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
    }

    // Parse connections
    if (j.contains("connections")) {
        const auto& connections = j["connections"];
        if (!connections.is_array()) {
            error_message = "'connections' must be an array";
            return false;
        }

        for (const auto& conn : connections) {
            ConnectionConfig conn_config;
            
            if (conn.contains("host")) {
                conn_config.host = conn["host"].get<std::string>();
            }
            if (conn.contains("port")) {
                conn_config.port = conn["port"].get<uint16_t>();
            }
            if (conn.contains("item")) {
                conn_config.item_name = conn["item"].get<std::string>();
            }
            if (conn.contains("client_id")) {
                conn_config.client_id = conn["client_id"].get<std::string>();
            }
            if (conn.contains("starting_seq")) {
                conn_config.starting_seq_num = conn["starting_seq"].get<uint64_t>();
            }

            config.connections.push_back(conn_config);
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
                if (cls.contains("initial")) cfg.initial_count = cls["initial"].get<size_t>();
                if (cls.contains("max_free")) cfg.max_count = cls["max_free"].get<size_t>();
                if (cls.contains("max_total")) cfg.max_total_allocated = cls["max_total"].get<size_t>();
            }

            config.pool_config.push_back(cfg);
        }
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
        "    \"workers\": 4,                    // Number of worker threads\n"
        "    \"raw_queue_size\": 16384,         // Raw queue size\n"
        "    \"decoded_queue_size\": 16384,     // Decoded queue per worker\n"
        "    \"reconnect_interval_ms\": 3000,   // Reconnect interval\n"
        "    \"queue_push_timeout_ms\": 5       // Queue push timeout\n"
        "  },\n"
        "\n"
        "  // TCP connections (up to 64)\n"
        "  \"connections\": [\n"
        "    {\n"
        "      \"host\": \"server1.example.com\",\n"
        "      \"port\": 8888,\n"
        "      \"item\": \"AAPL\",\n"
        "      \"client_id\": \"Client1\",\n"
        "      \"starting_seq\": 0\n"
        "    },\n"
        "    {\n"
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
        "      \"initial\": 512,\n"
        "      \"max_free\": 1024,\n"
        "      \"max_total\": 8192\n"
        "    }\n"
        "  }\n"
        "}\n"
        "\n"
        "All fields are optional. Missing fields use defaults.\n"
        "Connections can also be specified via command line.\n"
    );
}
