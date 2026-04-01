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
          "  --config <file>      JSON configuration file (optional)\n"
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
          "  --host <addr>        Server hostname/IP\n"
          "  --port <port>        Server port\n"
          "  --item <name>        Subscription item name\n"
          "  --client-id <id>     Client identifier\n"
          "  --seq <num>          Starting sequence number\n"
          "\n"
          "Examples:\n"
          "  # Single connection (backward compatible)\n"
          "  %s --host 127.0.0.1 --port 8888 --item default\n"
          "\n"
          "  # Multiple connections to same server with different items\n"
          "  %s --host 127.0.0.1 --port 8888 --item A \\\n"
          "     --host 127.0.0.1 --port 8888 --item B\n"
          "\n"
          "  # Multiple connections to different servers\n"
          "  %s --host server1 --port 8888 --item data1 --client-id Client1 \\\n"
          "     --host server2 --port 8889 --item data2 --client-id Client2\n"
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
          prog, prog, prog);
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char *argv[]) {
  // Environment parsing helpers
  auto getEnvStr = [](const char *name, const char *def) -> std::string {
    const char *val = std::getenv(name);
    return val ? val : def;
  };
  auto getEnvInt = [](const char *name, int def) -> int {
    const char *val = std::getenv(name);
    if (val) {
      try { return std::stoi(val); } catch (...) {}
    }
    return def;
  };
  auto getEnvUll = [](const char *name, uint64_t def) -> uint64_t {
    const char *val = std::getenv(name);
    if (val) {
      try { return std::stoull(val); } catch (...) {}
    }
    return def;
  };

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
      config.queue_push_timeout_ms != 0 ? config.queue_push_timeout_ms : Defaults::QUEUE_PUSH_TIMEOUT_MS);

  int stats_interval_sec = getEnvInt("APP_TCP_CLIENT_STATS_INTERVAL", Defaults::STATS_INTERVAL_SEC);
  int pool_stats_interval_sec = 0;
  std::string log_dir = getEnvStr("APP_LOG_DIR", "");
  int log_stdout = -1;
  int log_file = -1;
  int log_syslog = -1;

  // Connection being built (CLI can add to config file connections)
  ConnectionConfig current_conn;
  current_conn.host = getEnvStr("APP_TCP_CLIENT_HOST", Defaults::HOST);
  current_conn.port = static_cast<uint16_t>(getEnvInt("APP_TCP_CLIENT_PORT", Defaults::PORT));
  current_conn.item_name = getEnvStr("APP_TCP_CLIENT_ITEM", Defaults::ITEM_NAME);
  current_conn.client_id = getEnvStr("APP_TCP_CLIENT_CLIENT_ID", Defaults::CLIENT_ID);
  current_conn.starting_seq_num = getEnvUll("APP_TCP_CLIENT_SEQ", Defaults::STARTING_SEQ_NUM);
  bool has_connection = false;

  // Parse command-line arguments
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
      // Already handled in first pass
      ++i;
    } else if (strcmp(argv[i], "--config-help") == 0) {
      printConfigFormat();
      return 0;
    } else if ((strcmp(argv[i], "--host") == 0) && i + 1 < argc) {
      // If we already have a connection building, save it and start a new one
      if (has_connection) {
        config.connections.push_back(current_conn);
        // Reset with defaults from environment
        current_conn.host = getEnvStr("APP_TCP_CLIENT_HOST", Defaults::HOST);
        current_conn.port = static_cast<uint16_t>(getEnvInt("APP_TCP_CLIENT_PORT", Defaults::PORT));
        current_conn.item_name = getEnvStr("APP_TCP_CLIENT_ITEM", Defaults::ITEM_NAME);
        current_conn.client_id = getEnvStr("APP_TCP_CLIENT_CLIENT_ID", Defaults::CLIENT_ID);
        current_conn.starting_seq_num = getEnvUll("APP_TCP_CLIENT_SEQ", Defaults::STARTING_SEQ_NUM);
      }
      current_conn.host = argv[++i];
      has_connection = true;
    } else if ((strcmp(argv[i], "--port") == 0) && i + 1 < argc) {
      try {
        int port = std::stoi(argv[++i]);
        if (port < Defaults::MIN_PORT || port > Defaults::MAX_PORT) {
          fprintf(stderr, "Error: Port must be between %d and %d\n", 
                  Defaults::MIN_PORT, Defaults::MAX_PORT);
          return 1;
        }
        current_conn.port = static_cast<uint16_t>(port);
        has_connection = true;
      } catch (...) {
        fprintf(stderr, "Error: Invalid port number: %s\n", argv[i]);
        return 1;
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
        return 1;
      }
    } else if ((strcmp(argv[i], "--workers") == 0) && i + 1 < argc) {
      try {
        config.worker_thread_count = static_cast<size_t>(std::stoi(argv[++i]));
      } catch (...) {
        fprintf(stderr, "Error: Invalid worker count: %s\n", argv[i]);
        return 1;
      }
    } else if ((strcmp(argv[i], "--raw-queue") == 0) && i + 1 < argc) {
      try {
        config.raw_queue_size = static_cast<size_t>(std::stoi(argv[++i]));
      } catch (...) {
        fprintf(stderr, "Error: Invalid queue size: %s\n", argv[i]);
        return 1;
      }
    } else if ((strcmp(argv[i], "--dec-queue") == 0) && i + 1 < argc) {
      try {
        config.decoded_queue_size = static_cast<size_t>(std::stoi(argv[++i]));
      } catch (...) {
        fprintf(stderr, "Error: Invalid queue size: %s\n", argv[i]);
        return 1;
      }
    } else if ((strcmp(argv[i], "--reconnect") == 0) && i + 1 < argc) {
      try {
        config.reconnect_interval_ms = std::stoi(argv[++i]);
      } catch (...) {
        fprintf(stderr, "Error: Invalid reconnect interval: %s\n", argv[i]);
        return 1;
      }
    } else if ((strcmp(argv[i], "--queue-timeout") == 0) && i + 1 < argc) {
      try {
        config.queue_push_timeout_ms = std::stoi(argv[++i]);
      } catch (...) {
        fprintf(stderr, "Error: Invalid queue timeout: %s\n", argv[i]);
        return 1;
      }
    } else if ((strcmp(argv[i], "--stats-interval") == 0) && i + 1 < argc) {
      try {
        stats_interval_sec = std::stoi(argv[++i]);
      } catch (...) {
        fprintf(stderr, "Error: Invalid stats interval: %s\n", argv[i]);
        return 1;
      }
    } else if ((strcmp(argv[i], "--pool-stats-interval") == 0) && i + 1 < argc) {
      try {
        pool_stats_interval_sec = std::stoi(argv[++i]);
        if (pool_stats_interval_sec < 0) {
          fprintf(stderr, "Error: Pool stats interval must be >= 0\n");
          return 1;
        }
      } catch (...) {
        fprintf(stderr, "Error: Invalid pool stats interval: %s\n", argv[i]);
        return 1;
      }
    } else if ((strcmp(argv[i], "--log-dir") == 0) && i + 1 < argc) {
      log_dir = argv[++i];
    } else if ((strcmp(argv[i], "--log-stdout") == 0) && i + 1 < argc) {
      try {
        log_stdout = std::stoi(argv[++i]);
      } catch (...) {
        fprintf(stderr, "Error: Invalid log stdout level: %s\n", argv[i]);
        return 1;
      }
    } else if ((strcmp(argv[i], "--log-file") == 0) && i + 1 < argc) {
      try {
        log_file = std::stoi(argv[++i]);
      } catch (...) {
        fprintf(stderr, "Error: Invalid log file level: %s\n", argv[i]);
        return 1;
      }
    } else if ((strcmp(argv[i], "--log-syslog") == 0) && i + 1 < argc) {
      try {
        log_syslog = std::stoi(argv[++i]);
      } catch (...) {
        fprintf(stderr, "Error: Invalid log syslog level: %s\n", argv[i]);
        return 1;
      }
    } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      printUsage(argv[0]);
      return 0;
    } else {
      fprintf(stderr, "Unknown option: %s\n", argv[i]);
      printUsage(argv[0]);
      return 1;
    }
  }

  // Add the last connection if one was being built
  if (has_connection) {
    config.connections.push_back(current_conn);
  }

  // If no connections specified, create a default one
  if (config.connections.empty()) {
    ConnectionConfig default_conn;
    default_conn.host = getEnvStr("APP_TCP_CLIENT_HOST", Defaults::HOST);
    default_conn.port = static_cast<uint16_t>(getEnvInt("APP_TCP_CLIENT_PORT", Defaults::PORT));
    default_conn.item_name = getEnvStr("APP_TCP_CLIENT_ITEM", Defaults::ITEM_NAME);
    default_conn.client_id = getEnvStr("APP_TCP_CLIENT_CLIENT_ID", Defaults::CLIENT_ID);
    default_conn.starting_seq_num = getEnvUll("APP_TCP_CLIENT_SEQ", Defaults::STARTING_SEQ_NUM);
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
    LOG_INFO("    [%zu] %s:%u (item='%s', client='%s', seq=%lu)",
             i, conn.host.c_str(), conn.port, conn.item_name.c_str(),
             conn.client_id.c_str(), conn.starting_seq_num);
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
