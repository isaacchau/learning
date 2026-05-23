// ============================================================================
// main.cpp — CLI entry point for msg_client
// ============================================================================
// Parses command-line arguments and environment variables, loads optional
// INI configuration, sets up signal handling, and runs the MsgClient.
//
// Configuration hierarchy (highest to lowest precedence):
//   1. Command-line arguments
//   2. Environment variables (APP_TCP_CLIENT_*, APP_LOG_*)
//   3. INI configuration file (--config <file>)
//   4. Hardcoded defaults in msg_client.h
//
// Why this precedence order?
//   - CLI args are most explicit (user typed them right now).
//   - Environment variables allow container/orchestrator overrides without
//     changing command lines.
//   - INI config files are version-controlled and shared across environments.
//   - Defaults are the safety net.
//
// See doc/05_Build_and_Run.md for usage examples.
// ============================================================================

#include "log_msg.h"
#include "msg_client.h"
#include "config_parser.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

// ============================================================================
// Global shutdown flag for signal handling
// ============================================================================

static std::atomic<bool> g_shutdown(false);

static void signalHandler(int signum) {
  (void)signum;
  g_shutdown.store(true, std::memory_order_release);
}

// ============================================================================
// Command-line usage
// ============================================================================

static void printUsage(const char *prog) {
  fprintf(stderr,
          "Usage: %s [options] [connection ...]\n"
          "\n"
          "Options:\n"
          "  --config <file>      INI configuration file (optional)\n"
          "  --workers <num>      Worker thread count   (default: %zu)\n"
          "  --raw-queue <size>   Raw queue size        (default: %zu)\n"
          "  --dec-queue <size>   Decoded queue size    (default: %zu)\n"
          "  --reconnect <ms>     Reconnect interval    (default: %d)\n"
          "  --queue-timeout <ms> Queue push timeout    (default: %d)\n"
          "                       (0=wait forever, -1=no wait, >0=timeout ms)\n"
          "  --stats-interval <s> Stats print interval  (default: %d)\n"
          "  --pool-stats-interval <s>  Pool stats print interval (default: 0=off)\n"
          "  --log-dir <path>     Directory for logs    (default: ./log)\n"
          "  --log-stdout <lvl>   STDOUT log level      (default: 6/INFO)\n"
          "  --log-file <lvl>     FILE log level        (default: 7/DEBUG)\n"
          "  --log-syslog <lvl>   SYSLOG log level      (default: 5/NOTICE)\n"
          "  --config-help        Show configuration file format\n"
          "  -h, --help           Show this help\n"
          "\n"
          "Connection Specification (can be specified multiple times):\n"
          "  --host <addr>[,...]  Server hostname/IP (comma-separated for failover)\n"
          "                       Each entry can include :port override\n"
          "  --port <port>        Default server port for hosts without :port\n"
          "  --failover-retries <n>  Retries on same endpoint before switching\n"
          "                          (default: %d)\n"
          "  --item <name>        Subscription item name\n"
          "  --client-id <id>     Client identifier\n"
          "  --seq <num>          Starting sequence number\n"
          "\n"
          "Examples:\n"
          "  # Single connection (backward compatible)\n"
          "  %s --host 127.0.0.1 --port 8888 --item default\n"
          "\n"
          "  # Single connection with failover endpoints\n"
          "  %s --host server1:8888,server2:8888,server3:8888 --item AAPL\n"
          "\n"
          "  # Multiple connections with different failover sets\n"
          "  %s --host primary1:8888,backup1:8888 --item A \\\n"
          "     --host primary2:8889,backup2:8889 --item B\n"
          "\n"
          "Environment Variables:\n"
          "  APP_TCP_CLIENT_HOST      Default host\n"
          "  APP_TCP_CLIENT_PORT      Default port\n"
          "  APP_TCP_CLIENT_ITEM      Default item name\n"
          "  APP_TCP_CLIENT_CLIENT_ID Default client ID\n"
          "  APP_TCP_CLIENT_SEQ       Default starting sequence\n"
          "  APP_TCP_CLIENT_WORKERS   Worker thread count\n"
          "  APP_TCP_CLIENT_RAW_QUEUE Raw queue size\n"
          "  APP_TCP_CLIENT_DEC_QUEUE Decoded queue size\n"
          "  APP_TCP_CLIENT_RECONNECT Reconnect interval (ms)\n"
          "  APP_TCP_CLIENT_QUEUE_TIMEOUT Queue push timeout (ms)\n"
          ,
          prog,
          Defaults::WORKER_THREAD_COUNT,
          Defaults::RAW_QUEUE_SIZE,
          Defaults::DECODED_QUEUE_SIZE,
          Defaults::RECONNECT_INTERVAL_MS,
          Defaults::QUEUE_PUSH_TIMEOUT_MS,
          Defaults::STATS_INTERVAL_SEC,
          Defaults::MAX_RETRIES_PER_ENDPOINT,
          prog, prog, prog);
}

// ============================================================================
// Env helpers
// ============================================================================

static std::string getEnvStr(const char* name, const char* def) {
  const char* val = std::getenv(name);
  return val ? val : def;
}

static int getEnvInt(const char* name, int def) {
  const char* val = std::getenv(name);
  if (val) {
    try { return std::stoi(val); } catch (const std::exception&) {}
  }
  return def;
}

static uint64_t getEnvUll(const char* name, uint64_t def) {
  const char* val = std::getenv(name);
  if (val) {
    try { return std::stoull(val); } catch (const std::exception&) {}
  }
  return def;
}

// ============================================================================
// CLI helpers
// ============================================================================

static void parseHostArg(const std::string& arg, ConnectionConfig& conn) {
  conn.endpoints.clear();
  size_t start = 0;
  while (start < arg.length()) {
    size_t comma = arg.find(',', start);
    std::string part = (comma == std::string::npos)
        ? arg.substr(start)
        : arg.substr(start, comma - start);

    std::string host;
    uint16_t port = 0;  // 0 = use default_port later

    size_t colon = part.rfind(':');
    if (colon != std::string::npos) {
      // Check that it's actually a port (not IPv6)
      // For simplicity, we only support IPv4 here
      host = part.substr(0, colon);
      try {
        int p = std::stoi(part.substr(colon + 1));
        if (p >= Defaults::MIN_PORT && p <= Defaults::MAX_PORT) {
          port = static_cast<uint16_t>(p);
        }
      } catch (const std::exception&) {
        // Not a valid port, treat whole thing as host
        host = part;
        port = 0;
      }
    } else {
      host = part;
    }

    if (!host.empty()) {
      conn.endpoints.push_back({host, port});
    }
    start = (comma == std::string::npos) ? arg.length() : comma + 1;
  }
}

static void finalizeConnection(ConnectionConfig& conn) {
  for (auto& ep : conn.endpoints) {
    if (ep.port == 0) {
      ep.port = conn.default_port;
    }
  }
}

static ConnectionConfig makeDefaultConn() {
  ConnectionConfig c;
  c.default_port = static_cast<uint16_t>(
      getEnvInt("APP_TCP_CLIENT_PORT", Defaults::PORT));
  c.endpoints.push_back({
      getEnvStr("APP_TCP_CLIENT_HOST", Defaults::HOST),
      c.default_port
  });
  c.item_name = getEnvStr("APP_TCP_CLIENT_ITEM", Defaults::ITEM_NAME);
  c.client_id = getEnvStr("APP_TCP_CLIENT_CLIENT_ID", Defaults::CLIENT_ID);
  c.starting_seq_num = getEnvUll("APP_TCP_CLIENT_SEQ", Defaults::STARTING_SEQ_NUM);
  return c;
}

// ============================================================================
// CLI option parsing helpers
// ============================================================================
// These helpers extract the giant if-else chain from main() into focused
// functions that handle one category of options each.
// ============================================================================

// Parse a connection-specific CLI option.  Returns false on error.
static bool parseConnectionOption(int argc, const char* const argv[], int& i,
                                  ConnectionConfig& current_conn,
                                  MsgClientConfig& config,
                                  bool& has_connection) {
  if ((strcmp(argv[i], "--host") == 0) && i + 1 < argc) {
    if (has_connection) {
      finalizeConnection(current_conn);
      config.connections.push_back(current_conn);
      current_conn = makeDefaultConn();
    }
    parseHostArg(argv[++i], current_conn);
    has_connection = true;
  } else if ((strcmp(argv[i], "--port") == 0) && i + 1 < argc) {
    try {
      int port = std::stoi(argv[++i]);
      if (port < Defaults::MIN_PORT || port > Defaults::MAX_PORT) {
        fprintf(stderr, "Error: Port must be between %d and %d\n",
                Defaults::MIN_PORT, Defaults::MAX_PORT);
        return false;
      }
      current_conn.default_port = static_cast<uint16_t>(port);
      for (auto& ep : current_conn.endpoints) {
        if (ep.port == 0) ep.port = current_conn.default_port;
      }
      has_connection = true;
    } catch (...) {
      fprintf(stderr, "Error: Invalid port number: %s\n", argv[i]);
      return false;
    }
  } else if ((strcmp(argv[i], "--failover-retries") == 0) && i + 1 < argc) {
    try {
      int retries = std::stoi(argv[++i]);
      if (retries < 1) {
        fprintf(stderr, "Error: failover-retries must be >= 1\n");
        return false;
      }
      current_conn.max_retries_per_endpoint = retries;
      has_connection = true;
    } catch (...) {
      fprintf(stderr, "Error: Invalid failover-retries: %s\n", argv[i]);
      return false;
    }
  } else if ((strcmp(argv[i], "--item") == 0) && i + 1 < argc) {
    current_conn.item_name = argv[++i];
    has_connection = true;
  } else if ((strcmp(argv[i], "--client-id") == 0) && i + 1 < argc) {
    current_conn.client_id = argv[++i];
    has_connection = true;
  } else if ((strcmp(argv[i], "--seq") == 0) && i + 1 < argc) {
    try {
      current_conn.starting_seq_num = std::stoull(argv[++i]);
      has_connection = true;
    } catch (...) {
      fprintf(stderr, "Error: Invalid sequence number: %s\n", argv[i]);
      return false;
    }
  }
  return true;
}

// Parse a global (non-connection) CLI option.  Returns false on error.
static bool parseGlobalOption(int argc, const char* const argv[], int& i,
                              MsgClientConfig& config,
                              int& stats_interval_sec,
                              int& pool_stats_interval_sec,
                              std::string& log_dir,
                              int& log_stdout, int& log_file, int& log_syslog) {
  if ((strcmp(argv[i], "--workers") == 0) && i + 1 < argc) {
    try {
      config.worker_thread_count = static_cast<size_t>(std::stoi(argv[++i]));
    } catch (...) {
      fprintf(stderr, "Error: Invalid worker count: %s\n", argv[i]);
      return false;
    }
  } else if ((strcmp(argv[i], "--raw-queue") == 0) && i + 1 < argc) {
    try {
      config.raw_queue_size = static_cast<size_t>(std::stoi(argv[++i]));
    } catch (...) {
      fprintf(stderr, "Error: Invalid queue size: %s\n", argv[i]);
      return false;
    }
  } else if ((strcmp(argv[i], "--dec-queue") == 0) && i + 1 < argc) {
    try {
      config.decoded_queue_size = static_cast<size_t>(std::stoi(argv[++i]));
    } catch (...) {
      fprintf(stderr, "Error: Invalid queue size: %s\n", argv[i]);
      return false;
    }
  } else if ((strcmp(argv[i], "--reconnect") == 0) && i + 1 < argc) {
    try {
      config.reconnect_interval_ms = std::stoi(argv[++i]);
    } catch (...) {
      fprintf(stderr, "Error: Invalid reconnect interval: %s\n", argv[i]);
      return false;
    }
  } else if ((strcmp(argv[i], "--queue-timeout") == 0) && i + 1 < argc) {
    try {
      config.queue_push_timeout_ms = std::stoi(argv[++i]);
    } catch (...) {
      fprintf(stderr, "Error: Invalid queue timeout: %s\n", argv[i]);
      return false;
    }
  } else if ((strcmp(argv[i], "--stats-interval") == 0) && i + 1 < argc) {
    try {
      stats_interval_sec = std::stoi(argv[++i]);
    } catch (...) {
      fprintf(stderr, "Error: Invalid stats interval: %s\n", argv[i]);
      return false;
    }
  } else if ((strcmp(argv[i], "--pool-stats-interval") == 0) && i + 1 < argc) {
    try {
      pool_stats_interval_sec = std::stoi(argv[++i]);
      if (pool_stats_interval_sec < 0) {
        fprintf(stderr, "Error: Pool stats interval must be >= 0\n");
        return false;
      }
    } catch (...) {
      fprintf(stderr, "Error: Invalid pool stats interval: %s\n", argv[i]);
      return false;
    }
  } else if ((strcmp(argv[i], "--log-dir") == 0) && i + 1 < argc) {
    log_dir = argv[++i];
  } else if ((strcmp(argv[i], "--log-stdout") == 0) && i + 1 < argc) {
    try {
      log_stdout = std::stoi(argv[++i]);
    } catch (...) {
      fprintf(stderr, "Error: Invalid log stdout level: %s\n", argv[i]);
      return false;
    }
  } else if ((strcmp(argv[i], "--log-file") == 0) && i + 1 < argc) {
    try {
      log_file = std::stoi(argv[++i]);
    } catch (...) {
      fprintf(stderr, "Error: Invalid log file level: %s\n", argv[i]);
      return false;
    }
  } else if ((strcmp(argv[i], "--log-syslog") == 0) && i + 1 < argc) {
    try {
      log_syslog = std::stoi(argv[++i]);
    } catch (...) {
      fprintf(stderr, "Error: Invalid log syslog level: %s\n", argv[i]);
      return false;
    }
  }
  return true;
}

// Parse an aggregation-related CLI option.  Returns false on error.
static bool parseAggregationOption(int argc, const char* const argv[], int& i,
                                   MsgClientConfig& config) {
  if (strcmp(argv[i], "--aggregation") == 0) {
    config.aggregation_config.enabled = true;
  } else if ((strcmp(argv[i], "--agg-window") == 0) && i + 1 < argc) {
    try {
      config.aggregation_config.window_ms = std::stoull(argv[++i]);
      config.aggregation_config.enabled = true;
    } catch (...) {
      fprintf(stderr, "Error: Invalid aggregation window: %s\n", argv[i]);
      return false;
    }
  } else if ((strcmp(argv[i], "--agg-format") == 0) && i + 1 < argc) {
    std::string fmt = argv[++i];
    if (fmt == "csv" || fmt == "CSV") {
      config.aggregation_config.output_format = metrics::OutputFormat::CSV;
    } else {
      config.aggregation_config.output_format = metrics::OutputFormat::INFLUXDB_LINE;
    }
  } else if ((strcmp(argv[i], "--agg-output") == 0) && i + 1 < argc) {
    config.aggregation_config.output_dir = argv[++i];
  } else if ((strcmp(argv[i], "--agg-prefix") == 0) && i + 1 < argc) {
    config.aggregation_config.filename_prefix = argv[++i];
  }
  return true;
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char *argv[]) {
  // First pass: check for --config argument
  std::string config_file;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
      config_file = argv[i + 1];
      break;
    }
  }

  // Build client configuration
  MsgClientConfig config;
  
  // Load from config file if specified
  if (!config_file.empty()) {
    std::string error;
    if (!parseConfigFile(config_file, config, error)) {
      fprintf(stderr, "Error loading config file: %s\n", error.c_str());
      return 1;
    }
    printf("Loaded configuration from: %s\n", config_file.c_str());
  }
  
  // Global settings from environment (override config file defaults)
  config.worker_thread_count  = static_cast<size_t>(getEnvInt("APP_TCP_CLIENT_WORKERS", 
      config.worker_thread_count > 0 ? config.worker_thread_count : Defaults::WORKER_THREAD_COUNT));
  config.raw_queue_size       = static_cast<size_t>(getEnvInt("APP_TCP_CLIENT_RAW_QUEUE", 
      config.raw_queue_size > 0 ? config.raw_queue_size : Defaults::RAW_QUEUE_SIZE));
  config.decoded_queue_size   = static_cast<size_t>(getEnvInt("APP_TCP_CLIENT_DEC_QUEUE", 
      config.decoded_queue_size > 0 ? config.decoded_queue_size : Defaults::DECODED_QUEUE_SIZE));
  config.reconnect_interval_ms= getEnvInt("APP_TCP_CLIENT_RECONNECT", 
      config.reconnect_interval_ms > 0 ? config.reconnect_interval_ms : Defaults::RECONNECT_INTERVAL_MS);
  config.queue_push_timeout_ms= getEnvInt("APP_TCP_CLIENT_QUEUE_TIMEOUT",
      config.queue_push_timeout_ms);

  int stats_interval_sec = getEnvInt("APP_TCP_CLIENT_STATS_INTERVAL", Defaults::STATS_INTERVAL_SEC);
  int pool_stats_interval_sec = 0;
  std::string log_dir = getEnvStr("APP_LOG_DIR", "");
  int log_stdout = -1;
  int log_file = -1;
  int log_syslog = -1;

  // Connection being built (CLI can add to config file connections)
  ConnectionConfig current_conn = makeDefaultConn();
  bool has_connection = false;

  // Parse command-line arguments
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
      // Already handled in first pass
      ++i;
    } else if (strcmp(argv[i], "--config-help") == 0) {
      printConfigFormat();
      return 0;
    } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      printUsage(argv[0]);
      return 0;
    } else if (strncmp(argv[i], "--host", 6) == 0 ||
               strncmp(argv[i], "--port", 6) == 0 ||
               strncmp(argv[i], "--failover", 10) == 0 ||
               strcmp(argv[i], "--item") == 0 ||
               strcmp(argv[i], "--client-id") == 0 ||
               strcmp(argv[i], "--seq") == 0) {
      if (!parseConnectionOption(argc, argv, i, current_conn, config, has_connection)) {
        return 1;
      }
    } else if (strncmp(argv[i], "--agg", 5) == 0) {
      if (!parseAggregationOption(argc, argv, i, config)) {
        return 1;
      }
    } else {
      int orig_i = i;
      if (!parseGlobalOption(argc, argv, i, config,
                             stats_interval_sec, pool_stats_interval_sec,
                             log_dir, log_stdout, log_file, log_syslog)) {
        return 1;
      }
      // If parseGlobalOption didn't consume anything, it was an unknown option.
      // We detect this by checking if 'i' was unchanged (but our helpers always
      // consume at least the current arg).  Actually, parseGlobalOption only
      // handles known globals; unknown options fall through silently.
      // To catch unknowns, we verify the option was actually handled.
      // Simpler: check if it's a known global prefix.
      bool is_known_global =
          strncmp(argv[orig_i], "--workers", 9) == 0 ||
          strncmp(argv[orig_i], "--raw-queue", 11) == 0 ||
          strncmp(argv[orig_i], "--dec-queue", 11) == 0 ||
          strncmp(argv[orig_i], "--reconnect", 11) == 0 ||
          strncmp(argv[orig_i], "--queue-timeout", 15) == 0 ||
          strncmp(argv[orig_i], "--stats-interval", 16) == 0 ||
          strncmp(argv[orig_i], "--pool-stats", 12) == 0 ||
          strncmp(argv[orig_i], "--log", 5) == 0;
      if (!is_known_global) {
        fprintf(stderr, "Unknown option: %s\n", argv[orig_i]);
        printUsage(argv[0]);
        return 1;
      }
    }
  }

  // Add the last connection if one was being built
  if (has_connection) {
    finalizeConnection(current_conn);
    config.connections.push_back(current_conn);
  }

  // If no connections specified, create a default one
  if (config.connections.empty()) {
    ConnectionConfig default_conn = makeDefaultConn();
    config.connections.push_back(default_conn);
  }

  // Validate configuration
  std::string validation_error = config.validate();
  if (!validation_error.empty()) {
    fprintf(stderr, "Configuration error: %s\n", validation_error.c_str());
    return 1;
  }

  // Initialize high-performance logger
  LogMsg::getInstance().init(argv[0], log_dir.empty() ? nullptr : log_dir.c_str(),
                             log_stdout, log_file, log_syslog);

  // Install signal handlers for graceful shutdown
  struct sigaction sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.sa_handler = signalHandler;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  // Ignore SIGPIPE (broken pipe on send)
  signal(SIGPIPE, SIG_IGN);

  // Validate aggregation config if enabled
  if (config.aggregation_config.enabled) {
    std::string agg_error = config.aggregation_config.validate();
    if (!agg_error.empty()) {
      fprintf(stderr, "Aggregation configuration error: %s\n", agg_error.c_str());
      return 1;
    }
  }

  // Print configuration
  LOG_INFO("=== MsgClient Configuration ===");
  LOG_INFO("  Workers:        %zu", config.worker_thread_count);
  LOG_INFO("  Raw Queue:      %zu", config.raw_queue_size);
  LOG_INFO("  Decoded Queue:  %zu (per worker)", config.decoded_queue_size);
  LOG_INFO("  Reconnect:      %d ms", config.reconnect_interval_ms);
  LOG_INFO("  Queue Timeout:  %d ms", config.queue_push_timeout_ms);
  LOG_INFO("  Stats Interval: %d s", stats_interval_sec);
  LOG_INFO("  Connections:    %zu", config.connections.size());
  for (size_t i = 0; i < config.connections.size(); ++i) {
    const auto& conn = config.connections[i];
    std::string ep_list;
    for (size_t j = 0; j < conn.endpoints.size(); ++j) {
      if (j > 0) ep_list += ", ";
      ep_list += conn.endpoints[j].host + ":" + std::to_string(conn.endpoints[j].port);
    }
    LOG_INFO("    [%zu] %s (item='%s', client='%s', seq=%lu, retries=%d)",
             i, ep_list.c_str(), conn.item_name.c_str(),
             conn.client_id.c_str(), conn.starting_seq_num,
             conn.max_retries_per_endpoint);
  }
  
  // Print aggregation config
  LOG_INFO("  Aggregation:    %s", config.aggregation_config.enabled ? "enabled" : "disabled");
  if (config.aggregation_config.enabled) {
    LOG_INFO("    Window:       %lu ms", config.aggregation_config.window_ms);
    LOG_INFO("    Format:       %s", config.aggregation_config.output_format == metrics::OutputFormat::CSV ? "csv" : "influxdb_line");
    LOG_INFO("    Output Dir:   %s", config.aggregation_config.output_dir.c_str());
    LOG_INFO("    Prefix:       %s", config.aggregation_config.filename_prefix.c_str());
  }
  LOG_INFO("================================");

  // Create and start client
  MsgClient client(config);

  client.setMessageHandler([](const SubMessage &msg, size_t worker_index, size_t connection_id) {
    // Default handler: silent processing.
    // In production, replace with actual business logic.
    (void)msg;
    (void)worker_index;
    (void)connection_id;
  });

  client.start();
  LOG_INFO("[Main] Client started. Press Ctrl+C to stop.");

  // Statistics reporting loop
  auto last_print = std::chrono::steady_clock::now();
  auto last_pool_print = std::chrono::steady_clock::now();
  StatsSnapshot prev_snap = {};

  while (!g_shutdown.load(std::memory_order_acquire)) {
    sleep(1);

    auto now = std::chrono::steady_clock::now();
    auto elapsed =
        std::chrono::duration_cast<std::chrono::seconds>(now - last_print).count();

    if (elapsed >= stats_interval_sec) {
      StatsSnapshot snap = client.getStats();

      uint64_t delta_recv =
          snap.messages_received - prev_snap.messages_received;
      uint64_t delta_proc =
          snap.messages_processed - prev_snap.messages_processed;
      uint64_t delta_bytes = snap.bytes_received - prev_snap.bytes_received;
      double mbps = (delta_bytes * 8.0) / (elapsed * 1000000.0);

      LOG_INFO("[Stats] recv=%lu(+%lu) decoded=%lu proc=%lu(+%lu) "
               "dropped=%lu bytes=%lu(%.2f Mbps) reconnects=%lu "
               "parse_err=%lu conns=%zu",
               snap.messages_received, delta_recv, snap.messages_decoded,
               snap.messages_processed, delta_proc, snap.messages_dropped,
               snap.bytes_received, mbps,
               snap.reconnect_count, snap.parse_errors,
               snap.connection_stats.size());

      // Print per-connection stats
      for (const auto& cs : snap.connection_stats) {
        LOG_INFO("[Conn %lu] %s item='%s' recv=%lu bytes=%lu reconnects=%lu %s",
                 cs.connection_id, cs.endpoint.c_str(), cs.item_name.c_str(),
                 cs.messages_received, cs.bytes_received, cs.reconnect_count,
                 cs.connected ? "(connected)" : "(disconnected)");
      }

      prev_snap = snap;
      last_print = now;
    }

    // Pool statistics (optional, default off)
    if (pool_stats_interval_sec > 0) {
      auto pool_elapsed = std::chrono::duration_cast<std::chrono::seconds>(
          now - last_pool_print).count();
      if (pool_elapsed >= pool_stats_interval_sec) {
        auto pool_stats = client.getPoolStats();
        LOG_INFO("[PoolStats] === Memory Pool Statistics ===");
        for (const auto& s : pool_stats) {
          LOG_INFO("[PoolStats] Size=%zuB allocated=%llu free=%zu in_use=%llu",
                   s.block_size,
                   (unsigned long long)s.total_allocated,
                   s.free_count,
                   (unsigned long long)s.current_allocated);
        }
        last_pool_print = now;
      }
    }
  }

  // Graceful shutdown
  LOG_INFO("[Main] Shutting down...");
  client.stop();

  // Final statistics
  StatsSnapshot final_snap = client.getStats();
  LOG_INFO("\n=== Final Statistics ===\n"
           "  Messages Received:  %lu\n"
           "  Messages Decoded:   %lu\n"
           "  Messages Processed: %lu\n"
           "  Messages Dropped:   %lu\n"
           "  Bytes Received:     %lu\n"
           "  Reconnects:         %lu\n"
           "  Parse Errors:       %lu\n"
           "========================",
           final_snap.messages_received, final_snap.messages_decoded,
           final_snap.messages_processed, final_snap.messages_dropped,
           final_snap.bytes_received, final_snap.reconnect_count,
           final_snap.parse_errors);

  LogMsg::getInstance().shutdown();
  return 0;
}
